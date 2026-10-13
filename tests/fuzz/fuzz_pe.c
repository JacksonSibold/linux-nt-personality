/* fuzz_pe.c - deterministic mutation fuzzer for src/loader/pe_parse.c.
 *
 * Usage (see tests/fuzz/Makefile for the targets that call these):
 *   fuzz_pe --gen-seeds DIR              write the synthesized seed images
 *   fuzz_pe [--seed N] [--start I] [--iters N] FILE...
 *                                        mutate each seed file N times
 *   fuzz_pe --check-one FILE             run the invariant checks on one input
 *   fuzz_pe --minimise FILE              shrink a failing input to FILE.min
 *
 * Exit codes: 0 clean, 1 invariant violation, 3 memory fault, 2 usage or I/O.
 *
 * Determinism: iteration I of seed file F under base seed S always produces the
 * same input.  The PRNG is xorshift64*, seeded by splitmix64 from S, the FNV-1a
 * hash of the seed file's basename, and I.  A failure prints all three, and the
 * offending input is written to tests/fuzz/out/fail-*.bin.
 *
 * Memory safety without sanitizers: every input is copied so that it ends
 * exactly at a guard page that is PAGE_NOACCESS (PROT_NONE on POSIX).  Any read
 * past the declared size faults on that page.  A vectored handler (Windows) or
 * a SIGSEGV/SIGBUS handler (POSIX) reports the fault with the seed and
 * iteration, saves the input, and exits 3.  The handler only runs while an
 * input is under test.
 *
 * Invariants checked after pe_validate() accepts an image:
 *   - every status returned is a pe_status_t value;
 *   - validated section, directory and header ranges lie inside the buffer;
 *   - every name or forwarder pointer returned by a walker lies inside the
 *     buffer and is NUL-terminated there, and is at most PE_MAX_SYMBOL_NAME long;
 *   - every RVA returned by a walker is below SizeOfImage;
 *   - every walker makes at most a bounded number of callbacks, derived from
 *     the input size (the walk cannot do unbounded work).
 */
/* -std=c11 hides WIFEXITED and friends on glibc unless these are set. */
#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_plat.h"
#include "pe_image.h"
#include "pe_parse.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

#define MAX_INPUT      (1u << 20)
#define MAX_FIELDS     640
#define OUT_DIR        "out"
#define MUTLOG_CAP     512
#define NSTATUS        ((unsigned)PE_ERR_NOT_FOUND + 1u)

static const char *const status_name[NSTATUS] = {
    "OK", "TRUNCATED", "MZ", "PE_SIG", "MACHINE", "MAGIC",
    "BOUNDS", "SECTIONS", "DIRECTORY", "CORRUPT", "NOT_FOUND",
};

/* -------------------------------------------------------------------------- */
/* Guard-page input region (plat_alloc_guarded)                               */
/* -------------------------------------------------------------------------- */
static uint8_t *g_end;      /* first byte of the trailing guard page          */

static void die(const char *msg)
{
    fprintf(stderr, "fuzz_pe: %s\n", msg);
    exit(2);
}

static void region_init(void)
{
    uint8_t *data = plat_alloc_guarded(MAX_INPUT);
    if (!data)
        die("cannot allocate the guarded input region");
    g_end = data + MAX_INPUT;
}

/* Copies an input so that its last byte is the last byte before the guard. */
static const uint8_t *place(const uint8_t *src, size_t n)
{
    uint8_t *p = g_end - n;
    if (n)
        memcpy(p, src, n);
    return p;
}

/* -------------------------------------------------------------------------- */
/* Reporting and fault handling                                               */
/* -------------------------------------------------------------------------- */
static int g_save = 1;             /* 0 for --check-one children             */
static int g_armed;                /* 1 while an input is under test          */
static uint64_t g_seed;
static uint32_t g_iter;
static char g_file[128] = "(none)";
static char g_mutlog[MUTLOG_CAP] = "";
static const uint8_t *g_in;
static size_t g_inlen;

/* Prints a finding and saves its input.  Returns without exiting. */
static void describe(const char *kind, const char *msg)
{
    char path[320];
    FILE *f;

    snprintf(path, sizeof path, OUT_DIR "/fail-%s-s%llu-i%u.bin", g_file,
             (unsigned long long)g_seed, (unsigned)g_iter);
    printf("%s: file=%s seed=%llu iter=%u\n", kind, g_file,
           (unsigned long long)g_seed, (unsigned)g_iter);
    printf("  %s\n", msg);
    printf("  mutations: %s\n", g_mutlog[0] ? g_mutlog : "(none)");
    if (g_save) {
        f = fopen(path, "wb");
        if (f) {
            if (g_inlen)
                fwrite(g_in, 1, g_inlen, f);
            fclose(f);
            printf("  input saved: %s (%zu bytes)\n", path, g_inlen);
        }
        printf("  reproduce: fuzz_pe --seed %llu --start %u --iters 1 "
               OUT_DIR "/seeds/%s\n",
               (unsigned long long)g_seed, (unsigned)g_iter, g_file);
    }
    fflush(stdout);
}

static void report(int code, const char *kind, const char *msg)
{
    describe(kind, msg);
    exit(code);
}

/* --keep-going: each distinct violation message is described once (with the
 * first input that produced it) and counted after that, instead of stopping. */
#define MAX_CLASSES 32
static int g_keep_going;
static struct { const char *msg; unsigned long n; uint64_t seed; uint32_t iter; }
    g_class[MAX_CLASSES];
static unsigned g_nclass;

static void note_violation(const char *msg)
{
    unsigned k;
    if (!g_keep_going)
        report(1, "VIOLATION", msg);
    for (k = 0; k < g_nclass; ++k)
        if (g_class[k].msg == msg) {
            g_class[k].n++;
            return;
        }
    if (g_nclass == MAX_CLASSES)
        report(1, "VIOLATION", msg);
    g_class[g_nclass].msg = msg;
    g_class[g_nclass].n = 1;
    g_class[g_nclass].seed = g_seed;
    g_class[g_nclass].iter = g_iter;
    g_nclass++;
    describe("VIOLATION", msg);
}

static void on_fault_addr(const void *addr)
{
    char msg[160];
    /* The process exits inside report(), so the non-reentrant calls there
     * cannot affect other code. */
    snprintf(msg, sizeof msg, "memory fault at %p, %+lld bytes from the input end",
             addr, (long long)((const char *)addr - (const char *)g_end));
    report(3, "FAULT", msg);
}


/* -------------------------------------------------------------------------- */
/* PRNG: splitmix64 for seeding, xorshift64* for the stream                   */
/* -------------------------------------------------------------------------- */
typedef struct rng { uint64_t s; } rng_t;

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static uint64_t xs_next(rng_t *r)
{
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* Uniform-ish value in [0, n); n == 0 gives 0. */
static uint32_t rnd(rng_t *r, uint32_t n)
{
    return n ? (uint32_t)(xs_next(r) % n) : 0;
}

static rng_t iter_rng(uint64_t seed, uint64_t file_hash, uint32_t iter)
{
    rng_t r;
    r.s = splitmix64(seed ^ splitmix64(file_hash) ^
                     ((uint64_t)iter * 0x9E3779B97F4A7C15ULL));
    if (r.s == 0)
        r.s = 1;
    return r;
}

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

/* -------------------------------------------------------------------------- */
/* Little-endian field access on the mutable work buffer                      */
/* -------------------------------------------------------------------------- */
static uint64_t get_le(const uint8_t *p, unsigned w)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < w; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void put_le(uint8_t *p, unsigned w, uint64_t v)
{
    for (unsigned i = 0; i < w; ++i)
        p[i] = (uint8_t)(v >> (8 * i));
}

static int fits(uint64_t off, unsigned w, size_t len)
{
    return off + w <= (uint64_t)len;
}

/* Header layout of the (possibly already mutated) buffer.  Every field is
 * located with bounds checks; when e_lfanew is unusable the fixed offsets of
 * the synthesized base image are used instead, so mutations still hit headers. */
typedef struct layout {
    uint32_t nt;     /* offset of "PE\0\0"                                     */
    uint32_t oh;     /* offset of the optional header                          */
    uint32_t st;     /* offset of the section table (0 when not in the buffer) */
    uint32_t nsec;   /* sections whose 40-byte headers lie in the buffer       */
} layout_t;

static void layout_of(const uint8_t *b, size_t len, layout_t *L)
{
    uint64_t soh, st;
    uint32_t n;

    L->nt = IMG_NT;
    if (len >= 64) {
        uint64_t e = get_le(b + 60, 4);
        if (e + 24 <= len)
            L->nt = (uint32_t)e;
    }
    L->oh = L->nt + 24;
    soh = fits(L->nt + 20, 2, len) ? get_le(b + L->nt + 20, 2) : 240;
    st = (uint64_t)L->oh + soh;
    n = fits(L->nt + 6, 2, len) ? (uint32_t)get_le(b + L->nt + 6, 2) : 0;
    if (n > 96)
        n = 96;
    while (n && st + 40u * n > len)
        --n;
    L->st = st <= len ? (uint32_t)st : 0;
    L->nsec = L->st ? n : 0;
}

/* Field offsets worth mutating, computed from the current layout. */
typedef struct field { uint32_t off; unsigned w; } field_t;

static size_t header_fields(const uint8_t *b, size_t len, field_t *F)
{
    layout_t L;
    size_t n = 0;
    uint32_t i;

    layout_of(b, len, &L);

#define ADD_FIELD(o, wd) do {                                   \
        if (fits((o), (wd), len) && n < MAX_FIELDS) {           \
            F[n].off = (uint32_t)(o);                           \
            F[n].w = (wd);                                      \
            ++n;                                                \
        }                                                       \
    } while (0)

    ADD_FIELD(0, 2);                    /* e_magic                 */
    ADD_FIELD(60, 4);                   /* e_lfanew                */
    ADD_FIELD(L.nt + 4, 2);             /* Machine                 */
    ADD_FIELD(L.nt + 6, 2);             /* NumberOfSections        */
    ADD_FIELD(L.nt + 20, 2);            /* SizeOfOptionalHeader    */
    ADD_FIELD(L.nt + 22, 2);            /* Characteristics         */
    ADD_FIELD(L.oh + 0, 2);             /* Magic                   */
    ADD_FIELD(L.oh + 16, 4);            /* AddressOfEntryPoint     */
    ADD_FIELD(L.oh + 32, 4);            /* SectionAlignment        */
    ADD_FIELD(L.oh + 36, 4);            /* FileAlignment           */
    ADD_FIELD(L.oh + 56, 4);            /* SizeOfImage             */
    ADD_FIELD(L.oh + 60, 4);            /* SizeOfHeaders           */
    ADD_FIELD(L.oh + 68, 2);            /* Subsystem               */
    ADD_FIELD(L.oh + 108, 4);           /* NumberOfRvaAndSizes     */
    for (i = 0; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++i) {
        ADD_FIELD(L.oh + 112 + 8 * i, 4);
        ADD_FIELD(L.oh + 116 + 8 * i, 4);
    }
    for (i = 0; i < L.nsec; ++i) {
        uint32_t s = L.st + 40 * i;
        ADD_FIELD(s + 8, 4);            /* VirtualSize             */
        ADD_FIELD(s + 12, 4);           /* VirtualAddress          */
        ADD_FIELD(s + 16, 4);           /* SizeOfRawData           */
        ADD_FIELD(s + 20, 4);           /* PointerToRawData        */
        ADD_FIELD(s + 36, 4);           /* Characteristics         */
    }
#undef ADD_FIELD
    return n;
}

/* -------------------------------------------------------------------------- */
/* Mutators                                                                   */
/* -------------------------------------------------------------------------- */
static void logmut(const char *what, unsigned long long a, unsigned long long b)
{
    size_t n = strlen(g_mutlog);
    if (n + 1 < sizeof g_mutlog)
        snprintf(g_mutlog + n, sizeof g_mutlog - n, "%s%s(%llx,%llx)",
                 n ? " " : "", what, a, b);
}

static const uint32_t boundary_fixed[8] = {
    0u, 1u, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu, 0x10000u, 0x1000u,
};

/* A value for a w-byte field: boundary constants, size-1/size/size+1, or noise. */
static uint64_t pick_value(rng_t *r, size_t len, unsigned w)
{
    uint64_t v;
    switch (rnd(r, 10)) {
    case 0: case 1: case 2: case 3: case 4:
    case 5:
        v = boundary_fixed[rnd(r, 8)];
        break;
    case 6:
        v = len ? (uint64_t)len - 1 : 0;
        break;
    case 7:
        v = len;
        break;
    case 8:
        v = (uint64_t)len + 1;
        break;
    default:
        v = xs_next(r) & 0xffffffffu;
        break;
    }
    if (w == 2)
        v &= 0xffffu;
    return v;
}

static void mut_flip(uint8_t *b, size_t len, rng_t *r)
{
    uint32_t pos, bit;
    if (!len)
        return;
    pos = rnd(r, (uint32_t)len);
    bit = rnd(r, 8);
    b[pos] ^= (uint8_t)(1u << bit);
    logmut("flip", pos, bit);
}

static void mut_byte(uint8_t *b, size_t len, rng_t *r)
{
    uint32_t pos;
    if (!len)
        return;
    pos = rnd(r, (uint32_t)len);
    b[pos] = (uint8_t)xs_next(r);
    logmut("byte", pos, b[pos]);
}

static void mut_field(uint8_t *b, size_t len, rng_t *r)
{
    static field_t F[MAX_FIELDS];
    size_t n = header_fields(b, len, F);
    field_t f;
    uint64_t v;
    if (!n)
        return;
    f = F[rnd(r, (uint32_t)n)];
    v = pick_value(r, len, f.w);
    put_le(b + f.off, f.w, v);
    logmut("field", f.off, v);
}

static void mut_trunc(uint8_t *b, size_t *len, rng_t *r)
{
    layout_t L;
    uint32_t nl;
    (void)b;
    layout_of(b, *len, &L);
    if (rnd(r, 2)) {
        nl = rnd(r, (uint32_t)*len + 1);
    } else {
        /* Cut just before, at, or just after a structure boundary. */
        uint32_t cands[7] = {L.nt, L.nt + 24, L.oh + 16, L.oh + 108,
                             L.st, L.st + 40, 0x200};
        nl = cands[rnd(r, 7)] + rnd(r, 3) - 1u;
    }
    if (nl > *len)
        return;
    *len = nl;
    logmut("trunc", nl, 0);
}

static void mut_grow(uint8_t *b, size_t *len, rng_t *r)
{
    uint32_t k = 1 + rnd(r, 64);
    uint32_t i;
    if (*len + k > MAX_INPUT)
        return;
    for (i = 0; i < k; ++i)
        b[*len + i] = (uint8_t)xs_next(r);
    *len += k;
    logmut("grow", k, 0);
}

/* Swaps two section headers, or copies one over another. */
static void mut_secshuffle(uint8_t *b, size_t len, rng_t *r)
{
    layout_t L;
    uint8_t tmp[40];
    uint32_t i, j;
    layout_of(b, len, &L);
    if (L.nsec < 2)
        return;
    i = rnd(r, L.nsec);
    j = rnd(r, L.nsec);
    if (i == j)
        return;
    memcpy(tmp, b + L.st + 40 * i, 40);
    if (rnd(r, 2)) {
        memcpy(b + L.st + 40 * i, b + L.st + 40 * j, 40);
        memcpy(b + L.st + 40 * j, tmp, 40);
        logmut("secswap", i, j);
    } else {
        memcpy(b + L.st + 40 * i, b + L.st + 40 * j, 40);
        logmut("seccopy", i, j);
    }
}

/* Makes a section field or a data directory RVA alias another section. */
static void mut_overlap(uint8_t *b, size_t len, rng_t *r)
{
    static const int32_t deltas[8] = {0, 1, -1, 0x10, -0x10, 0x200, -0x200, 0x1000};
    static const uint32_t kinds[4] = {8, 12, 16, 20}; /* VSize, VA, RawSize, RawPtr */
    layout_t L;
    uint32_t i, j, k, d;
    int32_t delta;
    uint64_t src, dst;

    layout_of(b, len, &L);
    if (!L.nsec)
        return;
    i = rnd(r, L.nsec);
    j = rnd(r, L.nsec);
    delta = deltas[rnd(r, 8)];
    if (rnd(r, 3) != 0) {
        k = kinds[rnd(r, 4)];
        src = L.st + 40 * j + k;
        dst = L.st + 40 * i + k;
        if (!fits(src, 4, len) || !fits(dst, 4, len))
            return;
        put_le(b + dst, 4, (uint32_t)(get_le(b + src, 4) + (uint32_t)delta));
        logmut("overlap-sec", dst, (unsigned long long)delta);
    } else {
        d = rnd(r, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);
        dst = (uint64_t)L.oh + 112 + 8 * d;
        src = L.st + 40 * j + 12;
        if (!fits(dst, 8, len) || !fits(src, 4, len))
            return;
        put_le(b + dst, 4, (uint32_t)(get_le(b + src, 4) + (uint32_t)delta));
        logmut("overlap-dir", d, (unsigned long long)delta);
    }
}

static void mut_splice(uint8_t *b, size_t len, rng_t *r)
{
    uint32_t from, to, n;
    if (len < 2)
        return;
    from = rnd(r, (uint32_t)len);
    to = rnd(r, (uint32_t)len);
    n = 1 + rnd(r, 32);
    if (from + n > len || to + n > len)
        return;
    memmove(b + to, b + from, n);
    logmut("splice", from, to);
}

static void mutate(uint8_t *b, size_t *len, rng_t *r)
{
    switch (rnd(r, 16)) {
    case 0: case 1:             mut_flip(b, *len, r); break;
    case 2:                     mut_byte(b, *len, r); break;
    case 3: case 4: case 5: case 6:
                                mut_field(b, *len, r); break;
    case 7:                     mut_trunc(b, len, r); break;
    case 8:                     mut_grow(b, len, r); break;
    case 9: case 10:            mut_secshuffle(b, *len, r); break;
    case 11: case 12: case 13:  mut_overlap(b, *len, r); break;
    default:                    mut_splice(b, *len, r); break;
    }
}

/* -------------------------------------------------------------------------- */
/* Invariant checks                                                           */
/* -------------------------------------------------------------------------- */
typedef struct cbs {
    const uint8_t *data;
    size_t size;
    uint32_t soi;      /* SizeOfImage: every returned RVA must be below it   */
    size_t calls;
    size_t budget;     /* callbacks allowed for the walk in progress          */
    const char *bad;   /* first violation found, or NULL                      */
} cbs_t;

static void flag(cbs_t *c, const char *msg)
{
    if (!c->bad)
        c->bad = msg;
}

/* p .. p+n lies inside the input buffer. */
static int inside(const cbs_t *c, const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p;
    uintptr_t base = (uintptr_t)c->data;
    if (a < base)
        return 0;
    return n <= c->size && a - base <= c->size - n;
}

/* s has n characters followed by a NUL, all inside the buffer. */
static int cstr_inside(const cbs_t *c, const char *s, size_t n)
{
    if (!inside(c, s, n + 1))
        return 0;
    return s[n] == '\0';
}

/* Counts one callback; returns nonzero (and flags) once the budget is spent. */
static int tick(cbs_t *c)
{
    if (++c->calls > c->budget) {
        flag(c, "walker exceeded its input-derived callback bound (unbounded work)");
        return 1;
    }
    return 0;
}

static pe_status_t cb_dll(void *u, const char *name, size_t len,
                          uint32_t iat, uint32_t ilt)
{
    cbs_t *c = u;
    (void)iat;
    (void)ilt;
    if (tick(c))
        return PE_ERR_CORRUPT;
    if (len == 0 || !cstr_inside(c, name, len))
        flag(c, "import DLL name is not a NUL-terminated string inside the buffer");
    return PE_OK;
}

static pe_status_t cb_sym(void *u, const pe_import_symbol_t *s)
{
    cbs_t *c = u;
    if (tick(c))
        return PE_ERR_CORRUPT;
    if (s->name) {
        if (!cstr_inside(c, s->name, s->name_len))
            flag(c, "import name is not a NUL-terminated string inside the buffer");
        if (s->name_len > PE_MAX_SYMBOL_NAME)
            flag(c, "import name longer than PE_MAX_SYMBOL_NAME");
    }
    return PE_OK;
}

static pe_status_t cb_export(void *u, const pe_export_entry_t *e)
{
    cbs_t *c = u;
    if (tick(c))
        return PE_ERR_CORRUPT;
    if (e->rva >= c->soi)
        flag(c, "export RVA is not below SizeOfImage");
    if (e->name && !cstr_inside(c, e->name, e->name_len))
        flag(c, "export name is not a NUL-terminated string inside the buffer");
    if (e->name_len > PE_MAX_SYMBOL_NAME)
        flag(c, "export name longer than PE_MAX_SYMBOL_NAME");
    if (e->forwarder && !cstr_inside(c, e->forwarder, e->forwarder_len))
        flag(c, "forwarder string is not a NUL-terminated string inside the buffer");
    return PE_OK;
}

static pe_status_t cb_reloc(void *u, const pe_reloc_entry_t *r)
{
    cbs_t *c = u;
    if (tick(c))
        return PE_ERR_CORRUPT;
    if (r->page_rva >= c->soi)
        flag(c, "relocation page RVA is not below SizeOfImage");
    if (r->type == 6 || r->type > 10)
        flag(c, "relocation walker returned a reserved type");
    if (r->offset > 0xFFF)
        flag(c, "relocation offset exceeds 12 bits");
    return PE_OK;
}

static pe_status_t cb_rf(void *u, const pe_runtime_function_t *rf)
{
    cbs_t *c = u;
    if (tick(c))
        return PE_ERR_CORRUPT;
    if (rf->begin >= c->soi || rf->end > c->soi || rf->unwind >= c->soi)
        flag(c, "runtime function RVA is outside the image");
    return PE_OK;
}

#define CHECK(cond, msg) do { if (cond) { flag(&c, msg); goto done; } } while (0)

/* Runs the parser on data[0..size) and checks the invariants.  Returns NULL
 * when all hold, else a description of the first violation.  *status receives
 * the pe_validate() result. */
static const char *check_input(const uint8_t *data, size_t size, int *status)
{
    static const char probe_name[] = "ExitProcess";
    pe_context_t ctx;
    pe_status_t st;
    cbs_t c;
    uint32_t i;
    unsigned d;

    memset(&c, 0, sizeof c);
    c.data = data;
    c.size = size;
    *status = -1;

    if (pe_context_init(&ctx, data, size) != PE_OK)
        return "pe_context_init failed";
    st = pe_validate(&ctx);
    *status = (int)st;
    if ((unsigned)st > (unsigned)PE_ERR_NOT_FOUND)
        return "pe_validate returned an out-of-range status";
    if (st != PE_OK)
        return NULL;

    c.soi = ctx.size_of_image;
    CHECK((uint64_t)ctx.section_table_off +
          (uint64_t)ctx.num_sections * sizeof(IMAGE_SECTION_HEADER) > size,
          "validated section table extends past the buffer");
    CHECK(ctx.size_of_headers > size, "validated SizeOfHeaders is past the buffer");
    CHECK(ctx.num_data_directories > IMAGE_NUMBEROF_DIRECTORY_ENTRIES,
          "validated NumberOfRvaAndSizes exceeds 16");

    /* Sections: raw data inside the buffer, RVA inside the image. */
    for (i = 0; i < ctx.num_sections; ++i) {
        pe_section_view_t sv;
        uint32_t off;
        CHECK(pe_section_get(&ctx, (uint16_t)i, &sv) != PE_OK,
              "pe_section_get failed on a validated section");
        CHECK(sv.file_size &&
              (uint64_t)sv.file_offset + sv.file_size > size,
              "section raw data extends past the buffer");
        CHECK(sv.virtual_address >= c.soi, "section RVA is not below SizeOfImage");
        /* RVA -> offset: any success must land inside the buffer. */
        if (pe_rva_to_offset(&ctx, sv.virtual_address, &off) == PE_OK)
            CHECK(off >= size, "pe_rva_to_offset returned an offset past the buffer");
        if (sv.virtual_size &&
            pe_rva_to_offset(&ctx, sv.virtual_address + sv.virtual_size - 1, &off) == PE_OK)
            CHECK(off >= size, "pe_rva_to_offset returned an offset past the buffer");
    }
    {
        /* Edge RVAs: header start, the last image byte, and an out-of-range one. */
        const uint32_t probes[3] = {0u, c.soi ? c.soi - 1 : 0u, 0xFFFFFFFFu};
        for (i = 0; i < 3; ++i) {
            uint32_t off;
            if (pe_rva_to_offset(&ctx, probes[i], &off) == PE_OK)
                CHECK(off >= size, "pe_rva_to_offset returned an offset past the buffer");
        }
    }

    /* Data directories: any directory that resolves lies inside the image. */
    for (d = 0; d < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++d) {
        pe_data_directory_t dd;
        if (pe_directory_get(&ctx, d, &dd) != PE_OK)
            continue;
        CHECK(dd.rva && dd.rva >= c.soi, "data directory RVA is not below SizeOfImage");
        CHECK((uint64_t)dd.rva + dd.size > c.soi,
              "data directory extends past SizeOfImage");
    }

    /* Walkers.  Each one gets a budget derived from the input size. */
    c.budget = size / 8 + size / 20 + 64;
    st = pe_imports_walk(&ctx, cb_dll, cb_sym, &c);
    CHECK((unsigned)st > (unsigned)PE_ERR_NOT_FOUND, "imports walk returned a bad status");

    c.calls = 0;
    c.budget = 2u * PE_MAX_EXPORT_ENTRIES + 16;
    st = pe_exports_walk(&ctx, cb_export, &c);
    CHECK((unsigned)st > (unsigned)PE_ERR_NOT_FOUND, "exports walk returned a bad status");

    c.calls = 0;
    c.budget = size / 2 + 16;
    st = pe_relocs_walk(&ctx, cb_reloc, &c);
    CHECK((unsigned)st > (unsigned)PE_ERR_NOT_FOUND, "relocs walk returned a bad status");

    c.calls = 0;
    c.budget = size / 12 + 1;
    st = pe_exception_walk(&ctx, cb_rf, &c);
    CHECK((unsigned)st > (unsigned)PE_ERR_NOT_FOUND, "exception walk returned a bad status");

    {
        IMAGE_TLS_DIRECTORY64 tls;
        st = pe_tls_directory(&ctx, &tls);
        CHECK((unsigned)st > (unsigned)PE_ERR_NOT_FOUND, "TLS directory returned a bad status");
    }

    {
        pe_export_entry_t ent;
        st = pe_export_find(&ctx, probe_name, sizeof probe_name - 1, &ent);
        CHECK((unsigned)st > (unsigned)PE_ERR_NOT_FOUND, "export lookup returned a bad status");
        if (st == PE_OK) {
            CHECK(ent.rva >= c.soi, "export lookup RVA is not below SizeOfImage");
            CHECK(ent.name && !cstr_inside(&c, ent.name, ent.name_len),
                  "export lookup name is not inside the buffer");
            CHECK(ent.forwarder && !cstr_inside(&c, ent.forwarder, ent.forwarder_len),
                  "export lookup forwarder is not inside the buffer");
        }
    }

done:
    return c.bad;
}

/* -------------------------------------------------------------------------- */
/* Iteration driver                                                           */
/* -------------------------------------------------------------------------- */
static void check_and_report(const uint8_t *data, size_t n, int *status)
{
    const char *bad;
    const uint8_t *p = place(data, n);
    g_in = p;
    g_inlen = n;
    g_armed = 1;
    bad = check_input(p, n, status);
    g_armed = 0;
    if (bad)
        note_violation(bad);
}

static int read_file(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(buf, 1, cap + 1, f);
    fclose(f);
    if (n > cap)
        return -1;
    return (int)n;
}

static void set_file_name(const char *path)
{
    const char *base = path;
    const char *p;
    for (p = path; *p; ++p)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    snprintf(g_file, sizeof g_file, "%s", base);
}

static uint8_t g_seedbuf[MAX_INPUT];
static uint8_t g_work[MAX_INPUT];

static int fuzz_file(const char *path, uint64_t seed, uint32_t start, uint32_t iters)
{
    unsigned long hist[NSTATUS];
    uint64_t fh;
    int n;
    uint32_t it;
    unsigned k;

    n = read_file(path, g_seedbuf, MAX_INPUT - 128);
    if (n < 0) {
        fprintf(stderr, "fuzz_pe: cannot read seed %s\n", path);
        return 2;
    }
    set_file_name(path);
    fh = fnv1a(g_file);
    memset(hist, 0, sizeof hist);

    for (it = start; it < start + iters; ++it) {
        rng_t r = iter_rng(seed, fh, it);
        size_t len = (size_t)n;
        int st = -1;
        uint32_t nops;

        memcpy(g_work, g_seedbuf, (size_t)n);
        g_seed = seed;
        g_iter = it;
        g_mutlog[0] = '\0';
        /* Iteration 0 of every file checks the unmodified seed. */
        nops = it == 0 ? 0 : 1 + rnd(&r, 4) + (rnd(&r, 8) == 0 ? 1 + rnd(&r, 4) : 0);
        for (k = 0; k < nops; ++k)
            mutate(g_work, &len, &r);

        check_and_report(g_work, len, &st);
        if (st >= 0 && (unsigned)st < NSTATUS)
            hist[st]++;
    }

    printf("fuzz_pe: %s seed=%llu iters=%u accepted=%lu\n", g_file,
           (unsigned long long)seed, (unsigned)iters, hist[PE_OK]);
    printf("  rejected by status:");
    for (k = 1; k < NSTATUS; ++k)
        if (hist[k])
            printf(" %s=%lu", status_name[k], hist[k]);
    printf("\n  violation classes=%u faults=0\n", g_nclass);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Seed synthesis                                                             */
/* -------------------------------------------------------------------------- */
static int write_seed(const char *dir, const char *name, const uint8_t *b, size_t n)
{
    char path[300];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return 0;
    fwrite(b, 1, n, f);
    fclose(f);
    printf("seed: %s (%zu bytes)\n", path, n);
    return 1;
}

static int gen_seeds(const char *dir)
{
    static uint8_t img[IMG_CAP];
    int ok = 1;

    img_build_valid(img);
    ok &= write_seed(dir, "synth_base.bin", img, IMG_LEN);

    img_build_valid(img);
    img_add_import(img);
    ok &= write_seed(dir, "synth_import.bin", img, IMG_LEN);

    img_build_valid(img);
    img_add_reloc(img);
    ok &= write_seed(dir, "synth_reloc.bin", img, IMG_LEN);

    img_build_valid(img);
    img_add_exception(img);
    ok &= write_seed(dir, "synth_exception.bin", img, IMG_LEN);

    img_build_valid(img);
    img_add_import(img);
    img_add_reloc(img);
    img_add_exception(img);
    ok &= write_seed(dir, "synth_all.bin", img, IMG_LEN);

    return ok ? 0 : 2;
}

/* -------------------------------------------------------------------------- */
/* Minimiser: shrinks a failing input while the same finding reproduces.  Each
 * candidate runs in a child process (--check-one), so a memory fault in a
 * candidate cannot kill the minimiser.  Shrinking first cuts the tail, then
 * zeroes runs of bytes (zeroing keeps every offset in place, so the header
 * structure survives), and finally zeroes single bytes. */
static const char *g_self = "fuzz_pe";
static char g_need[256];

/* Runs the child on b[0..n).  Returns its exit code, or -1 when its output
 * does not contain g_need (the candidate fails in a different way). */
static int child_rc(const uint8_t *b, size_t n)
{
    static char out[4096];
    char tmp[300], outp[300], cmd[800];
    FILE *f;
    int st, rc;
    size_t got = 0;

    snprintf(tmp, sizeof tmp, OUT_DIR "/min-tmp.bin");
    snprintf(outp, sizeof outp, OUT_DIR "/min-tmp.out");
    f = fopen(tmp, "wb");
    if (!f)
        die("cannot write " OUT_DIR "/min-tmp.bin");
    if (n)
        fwrite(b, 1, n, f);
    fclose(f);
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "\"\"%s\" --check-one \"%s\" >\"%s\" 2>%s\"",
             g_self, tmp, outp, FUZZ_DEVNULL);
    st = system(cmd);
    rc = st;
#else
    snprintf(cmd, sizeof cmd, "'%s' --check-one '%s' >'%s' 2>%s",
             g_self, tmp, outp, FUZZ_DEVNULL);
    st = system(cmd);
    rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
#endif
    f = fopen(outp, "rb");
    if (f) {
        got = fread(out, 1, sizeof out - 1, f);
        fclose(f);
    }
    out[got] = '\0';
    if (g_need[0] && !strstr(out, g_need))
        return -1;
    return rc;
}

/* Extracts the finding text from the child's output: the line after the
 * VIOLATION or FAULT line.  Faults are matched by their first words only,
 * since the fault address moves with the input. */
static void learn_need(const char *out_path)
{
    static char out[4096];
    FILE *f = fopen(out_path, "rb");
    size_t got;
    char *p, *e;
    if (!f)
        return;
    got = fread(out, 1, sizeof out - 1, f);
    fclose(f);
    out[got] = '\0';
    p = strstr(out, "\n  ");
    if (!p)
        return;
    p += 3;
    e = strchr(p, '\n');
    if (e)
        *e = '\0';
    if (!strncmp(out, "FAULT", 5))
        snprintf(g_need, sizeof g_need, "memory fault");
    else
        snprintf(g_need, sizeof g_need, "%s", p);
}

static int minimise(const char *path)
{
    static uint8_t cur[MAX_INPUT];
    static uint8_t trial[MAX_INPUT];
    int n, want, progress = 1;
    size_t step, chunk, i;
    char outp[300];
    FILE *f;

    n = read_file(path, cur, MAX_INPUT - 128);
    if (n < 0)
        die("cannot read input to minimise");
    want = child_rc(cur, (size_t)n);
    if (want == 0)
        die("input does not fail; nothing to minimise");
    learn_need(OUT_DIR "/min-tmp.out");
    printf("minimise: %s: %d bytes, exit %d, finding: %s\n", path, n, want, g_need);

    /* Phase 1: cut the tail while the finding survives. */
    for (step = (size_t)n / 2; step >= 1; step /= 2) {
        while ((size_t)n > step && child_rc(cur, (size_t)n - step) == want)
            n -= (int)step;
    }

    /* Phases 2 and 3: zero runs of bytes, halving the run length. */
    while (progress) {
        progress = 0;
        for (chunk = (size_t)n / 2 ? (size_t)n / 2 : 1; chunk >= 1; chunk /= 2) {
            for (i = 0; i < (size_t)n; i += chunk) {
                size_t m = chunk < (size_t)n - i ? chunk : (size_t)n - i;
                size_t k, nonzero = 0;
                for (k = i; k < i + m; ++k)
                    nonzero += cur[k] != 0;
                if (!nonzero)
                    continue;
                memcpy(trial, cur, (size_t)n);
                memset(trial + i, 0, m);
                if (child_rc(trial, (size_t)n) == want) {
                    memcpy(cur, trial, (size_t)n);
                    progress = 1;
                }
            }
            if (chunk == 1)
                break;
        }
    }

    snprintf(outp, sizeof outp, "%s.min", path);
    f = fopen(outp, "wb");
    if (!f)
        die("cannot write minimised output");
    if (n)
        fwrite(cur, 1, (size_t)n, f);
    fclose(f);
    printf("minimised to %d bytes (nonzero bytes kept: see %s)\n", n, outp);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */
static void usage(void)
{
    fprintf(stderr,
            "usage: fuzz_pe --gen-seeds DIR\n"
            "       fuzz_pe [--seed N] [--start I] [--iters N] FILE...\n"
            "       fuzz_pe --check-one FILE\n"
            "       fuzz_pe --minimise FILE\n");
}

int main(int argc, char **argv)
{
    uint64_t seed = 1;
    uint32_t start = 0, iters = 2000;
    int i, first_file = 0;
    const char *mode = NULL, *arg = NULL;

    g_self = argv[0];
    plat_silence_stderr(); /* pe_validate logs every rejection */

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "--seed") && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--start") && i + 1 < argc)
            start = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--iters") && i + 1 < argc)
            iters = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--keep-going"))
            g_keep_going = 1;
        else if (!strcmp(a, "--gen-seeds") && i + 1 < argc)
            mode = "gen", arg = argv[++i];
        else if (!strcmp(a, "--check-one") && i + 1 < argc)
            mode = "check", arg = argv[++i];
        else if (!strcmp(a, "--minimise") && i + 1 < argc)
            mode = "min", arg = argv[++i];
        else if (a[0] == '-') {
            usage();
            return 2;
        } else {
            first_file = i;
            break;
        }
    }

    if (mode && !strcmp(mode, "gen"))
        return gen_seeds(arg);
    if (mode && !strcmp(mode, "min"))
        return minimise(arg);

    region_init();
    plat_install_fault_handler(on_fault_addr);

    if (mode && !strcmp(mode, "check")) {
        int n, st = -1;
        g_save = 0;
        set_file_name(arg);
        n = read_file(arg, g_seedbuf, MAX_INPUT - 128);
        if (n < 0)
            die("cannot read input");
        check_and_report(g_seedbuf, (size_t)n, &st);
        printf("ok: %s validate=%s (%d bytes)\n", arg,
               (unsigned)st < NSTATUS ? status_name[st] : "?", n);
        return 0;
    }

    if (!first_file) {
        usage();
        return 2;
    }
    for (i = first_file; i < argc; ++i) {
        int rc = fuzz_file(argv[i], seed, start, iters);
        if (rc)
            return rc;
    }
    return g_nclass ? 1 : 0;
}
