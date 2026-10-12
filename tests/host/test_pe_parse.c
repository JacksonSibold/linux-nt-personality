/* test_pe_parse.c - host tests for src/loader/pe_parse.c.
 *
 * Images are built in memory (pe_image.c) and then copied into exact-size
 * heap buffers before parsing, so AddressSanitizer reports any read past the
 * end of the input when the toolchain supports it.
 *
 * pe_validate() prints its reason for each rejection to stderr. The Makefile
 * sends stderr to out/pe_diag.log; test results go to stdout.
 */
#include "minitest.h"
#include "pe_image.h"
#include "pe_parse.h"

#include <stdlib.h>
#include <string.h>

#define FUZZ_ITERS 3000

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */
typedef struct loaded {
    uint8_t *p;
    pe_context_t ctx;
} loaded_t;

static int load(loaded_t *L, const uint8_t *img, size_t len)
{
    L->p = malloc(len ? len : 1);
    if (!L->p)
        return 0;
    if (len)
        memcpy(L->p, img, len);
    return pe_context_init(&L->ctx, L->p, len) == PE_OK;
}

static void unload(loaded_t *L)
{
    free(L->p);
    L->p = NULL;
}

/* Validates a copy of the first len bytes of img. */
static pe_status_t validate_bytes(const uint8_t *img, size_t len)
{
    loaded_t L;
    pe_status_t s;
    if (!load(&L, img, len))
        return PE_ERR_BOUNDS;
    s = pe_validate(&L.ctx);
    unload(&L);
    return s;
}

typedef struct walk_stats {
    unsigned dlls;
    unsigned syms;
    int dll_is_kernel32;
    int sym_is_exitprocess;
    unsigned exports;
    unsigned relocs;
    uint16_t reloc_type[2];
    uint16_t reloc_offset[2];
    uint32_t reloc_page[2];
    unsigned excs;
    uint32_t exc_begin;
    uint32_t exc_end;
    uint32_t exc_unwind;
} walk_stats_t;

static pe_status_t cb_dll(void *u, const char *name, size_t len,
                          uint32_t iat_rva, uint32_t ilt_rva)
{
    walk_stats_t *st = u;
    (void)iat_rva;
    (void)ilt_rva;
    st->dlls++;
    st->dll_is_kernel32 = (len == 12 && memcmp(name, "KERNEL32.dll", 12) == 0);
    return PE_OK;
}

static pe_status_t cb_sym(void *u, const pe_import_symbol_t *sym)
{
    walk_stats_t *st = u;
    st->syms++;
    st->sym_is_exitprocess = sym->name != NULL && sym->name_len == 11 &&
                             memcmp(sym->name, "ExitProcess", 11) == 0;
    return PE_OK;
}

static pe_status_t cb_export(void *u, const pe_export_entry_t *e)
{
    walk_stats_t *st = u;
    (void)e;
    st->exports++;
    return PE_OK;
}

static pe_status_t cb_reloc(void *u, const pe_reloc_entry_t *e)
{
    walk_stats_t *st = u;
    if (st->relocs < 2) {
        st->reloc_type[st->relocs] = e->type;
        st->reloc_offset[st->relocs] = e->offset;
        st->reloc_page[st->relocs] = e->page_rva;
    }
    st->relocs++;
    return PE_OK;
}

static pe_status_t cb_exc(void *u, const pe_runtime_function_t *rf)
{
    walk_stats_t *st = u;
    st->excs++;
    st->exc_begin = rf->begin;
    st->exc_end = rf->end;
    st->exc_unwind = rf->unwind;
    return PE_OK;
}

static uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* Calls every walker and accessor on a context. Results are discarded: the
 * point is that nothing reads outside the input or loops forever. */
static void run_all_accessors(pe_context_t *ctx)
{
    walk_stats_t st;
    pe_data_directory_t dir;
    IMAGE_TLS_DIRECTORY64 tls;
    pe_section_view_t sv;
    uint16_t n = 0;
    uint32_t off = 0;

    memset(&st, 0, sizeof st);
    (void)pe_imports_walk(ctx, cb_dll, cb_sym, &st);
    (void)pe_exports_walk(ctx, cb_export, &st);
    (void)pe_relocs_walk(ctx, cb_reloc, &st);
    (void)pe_exception_walk(ctx, cb_exc, &st);
    (void)pe_tls_directory(ctx, &tls);
    if (pe_section_count(ctx, &n) == PE_OK) {
        for (uint16_t i = 0; i < n; ++i)
            (void)pe_section_get(ctx, i, &sv);
    }
    for (uint32_t i = 0; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++i)
        (void)pe_directory_get(ctx, i, &dir);
    (void)pe_rva_to_offset(ctx, 0x2010, &off);
    (void)pe_rva_to_offset(ctx, 0xFFFFFFF0u, &off);
}

/* -------------------------------------------------------------------------- */
/* Valid input and basic rejection                                            */
/* -------------------------------------------------------------------------- */
static void test_valid_image_validates(void)
{
    uint8_t img[IMG_CAP];
    loaded_t L;
    uint16_t n = 0;
    pe_section_view_t sv;
    uint32_t off = 0;

    img_build_valid(img);
    if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }

    MT_EXPECT_EQ(pe_validate(&L.ctx), PE_OK);
    MT_EXPECT_EQ(L.ctx.num_sections, 2);
    MT_EXPECT_EQ(L.ctx.entry_point, 0x1000);
    MT_EXPECT_EQ(L.ctx.size_of_image, 0x3000);
    MT_EXPECT_EQ(L.ctx.size_of_headers, 0x400);
    MT_EXPECT(L.ctx.image_base == 0x140000000ULL);

    MT_EXPECT_EQ(pe_section_count(&L.ctx, &n), PE_OK);
    MT_EXPECT_EQ(n, 2);
    MT_EXPECT_EQ(pe_section_get(&L.ctx, 1, &sv), PE_OK);
    MT_EXPECT_EQ(sv.virtual_address, 0x2000);
    MT_EXPECT_EQ(sv.file_offset, IMG_RDATA_RAW);

    MT_EXPECT_EQ(pe_rva_to_offset(&L.ctx, 0x2010, &off), PE_OK);
    MT_EXPECT_EQ(off, IMG_RDATA_RAW + 0x10);
    MT_EXPECT_EQ(pe_rva_to_offset(&L.ctx, 0x3000, &off), PE_ERR_NOT_FOUND);

    /* A valid image without data directories walks cleanly. */
    {
        walk_stats_t st;
        memset(&st, 0, sizeof st);
        MT_EXPECT_EQ(pe_imports_walk(&L.ctx, cb_dll, cb_sym, &st), PE_OK);
        MT_EXPECT_EQ(st.dlls, 0);
        MT_EXPECT_EQ(pe_relocs_walk(&L.ctx, cb_reloc, &st), PE_OK);
        MT_EXPECT_EQ(st.relocs, 0);
    }
    unload(&L);
}

static void test_null_and_empty_inputs_rejected(void)
{
    uint8_t img[IMG_CAP];
    pe_context_t ctx;

    MT_EXPECT_EQ(pe_validate(NULL), PE_ERR_BOUNDS);

    memset(&ctx, 0, sizeof ctx);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_ERR_BOUNDS);

    img_build_valid(img);
    MT_EXPECT_EQ(validate_bytes(img, 0), PE_ERR_BOUNDS);
    MT_EXPECT_EQ(pe_context_init(NULL, img, 0), PE_ERR_BOUNDS);
}

static void test_every_truncation_rejected(void)
{
    uint8_t img[IMG_CAP];
    unsigned accepted = 0;

    img_build_valid(img);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_OK);

    /* The last section's raw data ends at IMG_LEN, so every shorter prefix
     * must be rejected, whichever field first runs past the end. */
    for (size_t len = 0; len < IMG_LEN; ++len) {
        if (validate_bytes(img, len) == PE_OK)
            ++accepted;
    }
    MT_EXPECT_EQ(accepted, 0);
}

static void test_truncated_headers_rejected(void)
{
    uint8_t img[IMG_CAP];
    img_build_valid(img);

    /* Short DOS header: either code is acceptable, both mean "too small". */
    pe_status_t s = validate_bytes(img, 10);
    MT_EXPECT(s == PE_ERR_TRUNCATED || s == PE_ERR_BOUNDS);

    /* Ends inside the COFF header. */
    MT_EXPECT_EQ(validate_bytes(img, IMG_NT + 10), PE_ERR_TRUNCATED);
    /* Ends inside the optional header. */
    MT_EXPECT_EQ(validate_bytes(img, IMG_OPT + 100), PE_ERR_TRUNCATED);
}

/* A PE32+ optional header must be exactly 240 bytes. A shorter declared size
 * is corrupt, even when the file is too short to hold 240 bytes. */
static void test_short_optional_header_is_corrupt(void)
{
    uint8_t img[IMG_CAP];

    img_build_valid(img);
    img_w16(img, IMG_NT + 20, 200);      /* SizeOfOptionalHeader = 200 */
    MT_EXPECT_EQ(validate_bytes(img, IMG_NT + 24 + 200), PE_ERR_CORRUPT);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_CORRUPT);
}

static void test_bad_signatures_rejected(void)
{
    uint8_t img[IMG_CAP];

    img_build_valid(img);
    img[0] = 'X';
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_MZ);

    img_build_valid(img);
    img_w32(img, IMG_NT, 0);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_PE_SIG);

    /* e_lfanew pointing inside the DOS header: the bytes there are not "PE". */
    img_build_valid(img);
    img_w32(img, 60, 0x10);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_PE_SIG);

    img_build_valid(img);
    img_w16(img, IMG_NT + 4, 0x014C);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_MACHINE);

    img_build_valid(img);
    img_w16(img, IMG_OPT, 0x10B);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_MAGIC);
}

/* -------------------------------------------------------------------------- */
/* e_lfanew near 4 GiB                                                        */
/* -------------------------------------------------------------------------- */
static void test_e_lfanew_near_4gib_rejected(void)
{
    static const uint32_t values[] = {
        0xFFFFFFFFu, 0xFFFFFFF0u, 0x80000000u, 0x7FFFFFF0u,
        0x7F8u, 0x7F4u,
    };
    uint8_t img[IMG_CAP];

    for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        img_build_valid(img);
        img_w32(img, 60, values[i]);
        MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_TRUNCATED);
    }
}

/* -------------------------------------------------------------------------- */
/* Section table bounds                                                       */
/* -------------------------------------------------------------------------- */
static void test_section_raw_bounds_rejected(void)
{
    uint8_t img[IMG_CAP];

    img_build_valid(img);
    img_w32(img, IMG_SEC(1) + 20, 0xFFFFFFF0u);      /* PointerToRawData */
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_BOUNDS);

    img_build_valid(img);
    img_w32(img, IMG_SEC(1) + 16, 0x400);            /* SizeOfRawData: past EOF */
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_BOUNDS);
}

static void test_section_va_bounds_rejected(void)
{
    uint8_t img[IMG_CAP];

    img_build_valid(img);
    img_w32(img, IMG_SEC(0) + 12, 0x3000);           /* VA == SizeOfImage */
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_SECTIONS);

    img_build_valid(img);
    img_w32(img, IMG_SEC(1) + 8, 0xFFFF0000u);       /* VirtualSize huge */
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_SECTIONS);
}

static void test_overlapping_sections_rejected(void)
{
    uint8_t img[IMG_CAP];

    img_build_valid(img);
    img_w32(img, IMG_SEC(0) + 8, 0x2000);            /* .text now [1000,3000) */
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_SECTIONS);
}

static void test_size_of_headers_bounds_rejected(void)
{
    uint8_t img[IMG_CAP];

    /* Larger than the file. */
    img_build_valid(img);
    img_w32(img, IMG_OPT + 60, 0x10000);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_SECTIONS);

    img_build_valid(img);
    img_w32(img, IMG_OPT + 60, 0xFFFFFFFFu);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_SECTIONS);

    /* Too small to hold the section table (ends at 0x1D8). */
    img_build_valid(img);
    img_w32(img, IMG_OPT + 60, 0x100);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_SECTIONS);
}

/* -------------------------------------------------------------------------- */
/* Data directories                                                           */
/* -------------------------------------------------------------------------- */
static void test_bad_data_directory_rejected(void)
{
    uint8_t img[IMG_CAP];

    img_build_valid(img);
    img_set_dir(img, DIR_BASERELOC, 0xFFFF0000u, 8);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_DIRECTORY);

    /* Starts inside the image but runs past SizeOfImage. */
    img_build_valid(img);
    img_set_dir(img, DIR_EXCEPTION, 0x2000, 0x2000);
    MT_EXPECT_EQ(validate_bytes(img, IMG_LEN), PE_ERR_DIRECTORY);
}

static void test_imports_walk_valid_table(void)
{
    uint8_t img[IMG_CAP];
    loaded_t L;
    walk_stats_t st;

    img_build_valid(img);
    img_add_import(img);
    if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }

    memset(&st, 0, sizeof st);
    MT_EXPECT_EQ(pe_validate(&L.ctx), PE_OK);
    MT_EXPECT_EQ(pe_imports_walk(&L.ctx, cb_dll, cb_sym, &st), PE_OK);
    MT_EXPECT_EQ(st.dlls, 1);
    MT_EXPECT(st.dll_is_kernel32);
    MT_EXPECT_EQ(st.syms, 1);
    MT_EXPECT(st.sym_is_exitprocess);
    unload(&L);
}

static void test_relocs_walk_valid_block(void)
{
    uint8_t img[IMG_CAP];
    loaded_t L;
    walk_stats_t st;

    img_build_valid(img);
    img_add_reloc(img);
    if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }

    memset(&st, 0, sizeof st);
    MT_EXPECT_EQ(pe_validate(&L.ctx), PE_OK);
    MT_EXPECT_EQ(pe_relocs_walk(&L.ctx, cb_reloc, &st), PE_OK);
    MT_EXPECT_EQ(st.relocs, 2);
    MT_EXPECT_EQ(st.reloc_type[0], 10);
    MT_EXPECT_EQ(st.reloc_offset[0], 0x010);
    MT_EXPECT_EQ(st.reloc_page[0], 0x1000);
    MT_EXPECT_EQ(st.reloc_type[1], 3);
    MT_EXPECT_EQ(st.reloc_offset[1], 0x020);
    unload(&L);
}

static void test_relocs_bad_block_sizes_rejected(void)
{
    /* SizeOfBlock values: below the 8-byte header, not a multiple of 4,
     * and larger than the directory. */
    static const uint32_t sizes[] = { 6, 10, 0xFFFFFFFCu };
    uint8_t img[IMG_CAP];

    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        loaded_t L;
        walk_stats_t st;

        img_build_valid(img);
        img_add_reloc(img);
        img_w32(img, 0x704, sizes[i]);
        if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }

        memset(&st, 0, sizeof st);
        MT_EXPECT_EQ(pe_validate(&L.ctx), PE_OK);
        MT_EXPECT_EQ(pe_relocs_walk(&L.ctx, cb_reloc, &st), PE_ERR_CORRUPT);
        unload(&L);
    }
}

static void test_relocs_directory_past_file_rejected(void)
{
    uint8_t img[IMG_CAP];
    loaded_t L;
    walk_stats_t st;

    /* The directory stays inside SizeOfImage, so validation accepts it, but
     * it extends past the end of the file. */
    img_build_valid(img);
    img_add_reloc(img);
    img_set_dir(img, DIR_BASERELOC, 0x2100, 0x800);
    if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }

    memset(&st, 0, sizeof st);
    MT_EXPECT_EQ(pe_validate(&L.ctx), PE_OK);
    MT_EXPECT_EQ(pe_relocs_walk(&L.ctx, cb_reloc, &st), PE_ERR_BOUNDS);
    unload(&L);
}

/* Reserved base-relocation types (6 and 11-15) are not defined by the PE/COFF
 * specification and must be rejected by the walker. */
static void test_relocs_reserved_type_rejected(void)
{
    static const uint16_t types[] = { 6, 11, 15 };
    uint8_t img[IMG_CAP];

    for (size_t i = 0; i < sizeof types / sizeof types[0]; ++i) {
        loaded_t L;
        walk_stats_t st;

        img_build_valid(img);
        img_add_reloc(img);
        img_w16(img, 0x708, (uint16_t)((types[i] << 12) | 0x010));
        if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }

        memset(&st, 0, sizeof st);
        MT_EXPECT_EQ(pe_validate(&L.ctx), PE_OK);
        MT_EXPECT_EQ(pe_relocs_walk(&L.ctx, cb_reloc, &st), PE_ERR_CORRUPT);
        unload(&L);
    }
}

static void test_exception_bad_size_rejected(void)
{
    uint8_t img[IMG_CAP];
    loaded_t L;
    walk_stats_t st;

    img_build_valid(img);
    img_add_exception(img);
    img_set_dir(img, DIR_EXCEPTION, 0x2120, 13);     /* not a multiple of 12 */
    if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }

    memset(&st, 0, sizeof st);
    MT_EXPECT_EQ(pe_validate(&L.ctx), PE_OK);
    MT_EXPECT_EQ(pe_exception_walk(&L.ctx, cb_exc, &st), PE_ERR_CORRUPT);
    unload(&L);
}

/* x64 RUNTIME_FUNCTION.EndAddress is an exclusive RVA with no flag bits, so
 * a function that ends on an odd byte must keep its exact end. */
static void test_exception_end_address_not_truncated(void)
{
    uint8_t img[IMG_CAP];
    loaded_t L;
    walk_stats_t st;

    img_build_valid(img);
    img_add_exception(img);             /* EndAddress = 0x1007 */
    if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }

    memset(&st, 0, sizeof st);
    MT_EXPECT_EQ(pe_validate(&L.ctx), PE_OK);
    MT_EXPECT_EQ(pe_exception_walk(&L.ctx, cb_exc, &st), PE_OK);
    MT_EXPECT_EQ(st.excs, 1);
    MT_EXPECT_EQ(st.exc_begin, 0x1000);
    MT_EXPECT_EQ(st.exc_end, 0x1007);
    MT_EXPECT_EQ(st.exc_unwind, 0x2140);
    unload(&L);
}

/* A directory that stops validating after pe_validate() must not look like an
 * empty directory to the walkers. */
static void test_walkers_report_bad_directory(void)
{
    uint8_t img[IMG_CAP];
    loaded_t L;
    walk_stats_t st;

    img_build_valid(img);
    img_add_import(img);
    img_add_reloc(img);
    img_add_exception(img);
    if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }
    MT_EXPECT_EQ(pe_validate(&L.ctx), PE_OK);

    /* Corrupt the directories in place, after validation. */
    img_set_dir(L.p, DIR_EXPORT, 0xFFFF0000u, 40);
    img_set_dir(L.p, DIR_IMPORT, 0xFFFF0000u, 40);
    img_set_dir(L.p, DIR_EXCEPTION, 0xFFFF0000u, 12);
    img_set_dir(L.p, DIR_BASERELOC, 0xFFFF0000u, 8);

    memset(&st, 0, sizeof st);
    MT_EXPECT(pe_imports_walk(&L.ctx, cb_dll, cb_sym, &st) != PE_OK);
    MT_EXPECT(pe_exports_walk(&L.ctx, cb_export, &st) != PE_OK);
    MT_EXPECT(pe_relocs_walk(&L.ctx, cb_reloc, &st) != PE_OK);
    MT_EXPECT(pe_exception_walk(&L.ctx, cb_exc, &st) != PE_OK);
    unload(&L);
}

/* -------------------------------------------------------------------------- */
/* Fuzzing: random byte mutations of a valid image                            */
/* -------------------------------------------------------------------------- */
static void test_fuzzed_images_do_not_crash(void)
{
    uint8_t base[IMG_CAP];
    uint64_t rng = 0x243F6A8885A308D3ULL;
    unsigned validated = 0;

    img_build_valid(base);
    img_add_import(base);
    img_add_reloc(base);
    img_add_exception(base);

    for (unsigned it = 0; it < FUZZ_ITERS; ++it) {
        uint8_t img[IMG_LEN];
        loaded_t L;
        unsigned nmut = 1 + (unsigned)(rng_next(&rng) % 4);

        memcpy(img, base, IMG_LEN);
        for (unsigned m = 0; m < nmut; ++m) {
            size_t off = (size_t)(rng_next(&rng) % IMG_LEN);
            img[off] = (uint8_t)rng_next(&rng);
        }

        if (!load(&L, img, IMG_LEN)) { MT_EXPECT(0); return; }
        if (pe_validate(&L.ctx) == PE_OK) {
            ++validated;
            run_all_accessors(&L.ctx);
        }
        unload(&L);
    }
    /* Single-byte changes in unchecked fields keep many images valid, so the
     * walkers get exercised too. */
    MT_EXPECT(validated > 0);
}

/* -------------------------------------------------------------------------- */
/* Export and import layouts with room for long names                         */
/* -------------------------------------------------------------------------- */
/* The export directory and its tables live in .rdata, which maps file offset
 * EXP_DIR_OFF to RVA EXP_RDATA_RVA. Tests call exp_plan() for the file layout,
 * fill the tables and strings, then parse the whole buffer. */
#define EXP_DIR_OFF    0x600u
#define EXP_RDATA_RVA  0x2000u
#define EXP_FUNCS_OFF  0x700u

typedef struct exp_layout {
    size_t funcs;          /* file offset of AddressOfFunctions             */
    size_t names;          /* file offset of AddressOfNames                 */
    size_t ords;           /* file offset of AddressOfNameOrdinals          */
    size_t strs;           /* file offset of free string space              */
    size_t len;            /* whole file length                             */
    uint32_t rdata_len;    /* .rdata raw and virtual size                   */
    uint32_t size_of_image;
} exp_layout_t;

static uint32_t exp_rva(size_t off)
{
    return EXP_RDATA_RVA + (uint32_t)(off - EXP_DIR_OFF);
}

static exp_layout_t exp_plan(uint32_t nf, uint32_t nn, size_t str_bytes)
{
    exp_layout_t L;
    size_t end;
    L.funcs = EXP_FUNCS_OFF;
    L.names = L.funcs + 4 * (size_t)nf;
    L.ords = L.names + 4 * (size_t)nn;
    L.strs = L.ords + 2 * (size_t)nn;
    end = L.strs + str_bytes;
    if (end < IMG_LEN)
        end = IMG_LEN;
    L.rdata_len = (uint32_t)(((end - EXP_DIR_OFF) + 0x1FF) & ~(size_t)0x1FF);
    L.len = EXP_DIR_OFF + L.rdata_len;
    L.size_of_image = (uint32_t)((EXP_RDATA_RVA + L.rdata_len + 0xFFF) & ~0xFFFu);
    return L;
}

/* Allocates an exact-size image with a valid header and export directory. */
static uint8_t *exp_image(const exp_layout_t *L, uint32_t nf, uint32_t nn)
{
    uint8_t *b = calloc(1, L->len);
    if (!b)
        return NULL;
    img_build_valid(b);
    img_w32(b, IMG_SEC(1) + 8, L->rdata_len);
    img_w32(b, IMG_SEC(1) + 16, L->rdata_len);
    img_w32(b, IMG_OPT + 56, L->size_of_image);
    img_set_dir(b, DIR_EXPORT, EXP_RDATA_RVA, L->rdata_len);
    img_w32(b, EXP_DIR_OFF + 16, 1);                      /* Base             */
    img_w32(b, EXP_DIR_OFF + 20, nf);                     /* NumberOfFunctions */
    img_w32(b, EXP_DIR_OFF + 24, nn);                     /* NumberOfNames     */
    img_w32(b, EXP_DIR_OFF + 28, exp_rva(L->funcs));
    img_w32(b, EXP_DIR_OFF + 32, exp_rva(L->names));
    img_w32(b, EXP_DIR_OFF + 36, exp_rva(L->ords));
    return b;
}

static void exp_set_func(uint8_t *b, const exp_layout_t *L, uint32_t i,
                         uint32_t rva)
{
    img_w32(b, L->funcs + 4 * (size_t)i, rva);
}

static void exp_set_name(uint8_t *b, const exp_layout_t *L, uint32_t n,
                         uint32_t str_rva, uint16_t ord)
{
    img_w32(b, L->names + 4 * (size_t)n, str_rva);
    img_w16(b, L->ords + 2 * (size_t)n, ord);
}

#define EXP_KEEP 4
typedef struct exp_seen {
    unsigned count;
    uint32_t ord[EXP_KEEP];
    uint32_t rva[EXP_KEEP];
    size_t name_len[EXP_KEEP];
    char name[EXP_KEEP][64];
    size_t fwd_len;
    char fwd[64];
} exp_seen_t;

static pe_status_t cb_exp_keep(void *u, const pe_export_entry_t *e)
{
    exp_seen_t *s = u;
    if (s->count < EXP_KEEP) {
        unsigned k = s->count;
        size_t n = e->name_len < 63 ? e->name_len : 63;
        s->ord[k] = e->ordinal;
        s->rva[k] = e->rva;
        s->name_len[k] = e->name_len;
        if (e->name)
            memcpy(s->name[k], e->name, n);
        s->name[k][e->name ? n : 0] = '\0';
    }
    if (e->forwarder) {
        s->fwd_len = e->forwarder_len;
        size_t n = e->forwarder_len < 63 ? e->forwarder_len : 63;
        memcpy(s->fwd, e->forwarder, n);
        s->fwd[n] = '\0';
    }
    s->count++;
    return PE_OK;
}

/* -------------------------------------------------------------------------- */
/* Symbol name length limits (L-2)                                            */
/* -------------------------------------------------------------------------- */
static void test_export_name_length_cap(void)
{
    static const size_t lens[2] = { 4096, 4097 };
    for (unsigned i = 0; i < 2; ++i) {
        exp_layout_t L = exp_plan(1, 1, lens[i] + 1);
        uint8_t *b = exp_image(&L, 1, 1);
        exp_seen_t seen;
        pe_context_t ctx;
        if (!b) { MT_EXPECT(0); return; }

        memset(b + L.strs, 'A', lens[i]);
        exp_set_func(b, &L, 0, 0x1000);
        exp_set_name(b, &L, 0, exp_rva(L.strs), 0);

        MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
        MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
        memset(&seen, 0, sizeof seen);
        pe_status_t r = pe_exports_walk(&ctx, cb_exp_keep, &seen);
        if (i == 0) {
            MT_EXPECT_EQ(r, PE_OK);
            MT_EXPECT_EQ(seen.count, 1);
            MT_EXPECT_EQ(seen.name_len[0], 4096);
        } else {
            MT_EXPECT_EQ(r, PE_ERR_CORRUPT);
        }
        free(b);
    }
}

/* A name whose terminating NUL is missing before EOF must not be dropped
 * silently; the export is corrupt. */
static void test_export_name_unterminated_rejected(void)
{
    exp_layout_t L = exp_plan(1, 1, 16);
    uint8_t *b = exp_image(&L, 1, 1);
    exp_seen_t seen;
    pe_context_t ctx;
    if (!b) { MT_EXPECT(0); return; }

    /* Fill the rest of the file with non-NUL bytes: the name runs to EOF. */
    memset(b + L.strs, 'A', L.len - L.strs);
    exp_set_func(b, &L, 0, 0x1000);
    exp_set_name(b, &L, 0, exp_rva(L.strs), 0);

    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
    memset(&seen, 0, sizeof seen);
    MT_EXPECT_EQ(pe_exports_walk(&ctx, cb_exp_keep, &seen), PE_ERR_CORRUPT);
    free(b);
}

/* Forwarded export: the function RVA points inside the export directory and
 * the bytes there are the forwarder string. */
static void test_export_forwarder_resolves(void)
{
    static const char fwd[] = "NTDLL.RtlFoo";
    exp_layout_t L = exp_plan(1, 0, sizeof fwd);
    uint8_t *b = exp_image(&L, 1, 0);
    exp_seen_t seen;
    pe_context_t ctx;
    if (!b) { MT_EXPECT(0); return; }

    memcpy(b + L.strs, fwd, sizeof fwd);
    exp_set_func(b, &L, 0, exp_rva(L.strs));

    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
    memset(&seen, 0, sizeof seen);
    MT_EXPECT_EQ(pe_exports_walk(&ctx, cb_exp_keep, &seen), PE_OK);
    MT_EXPECT_EQ(seen.count, 1);
    MT_EXPECT_EQ(seen.rva[0], exp_rva(L.strs));
    MT_EXPECT_EQ(seen.fwd_len, sizeof fwd - 1);
    MT_EXPECT(memcmp(seen.fwd, fwd, sizeof fwd) == 0);
    free(b);
}

static void test_export_forwarder_length_cap(void)
{
    static const size_t lens[2] = { 4096, 4097 };
    for (unsigned i = 0; i < 2; ++i) {
        exp_layout_t L = exp_plan(1, 0, lens[i] + 1);
        uint8_t *b = exp_image(&L, 1, 0);
        exp_seen_t seen;
        pe_context_t ctx;
        if (!b) { MT_EXPECT(0); return; }

        memset(b + L.strs, 'F', lens[i]);
        exp_set_func(b, &L, 0, exp_rva(L.strs));

        MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
        MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
        memset(&seen, 0, sizeof seen);
        pe_status_t r = pe_exports_walk(&ctx, cb_exp_keep, &seen);
        MT_EXPECT_EQ(r, i == 0 ? PE_OK : PE_ERR_CORRUPT);
        free(b);
    }
}

/* Import by-name entry with a 4096- or 4097-byte name. */
static void test_import_name_length_cap(void)
{
    static const size_t lens[2] = { 4096, 4097 };
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *b = calloc(1, 0x2000);
        walk_stats_t st;
        pe_context_t ctx;
        if (!b) { MT_EXPECT(0); return; }

        img_build_valid(b);
        img_w32(b, IMG_SEC(1) + 8, 0x1A00);
        img_w32(b, IMG_SEC(1) + 16, 0x1A00);
        img_w32(b, IMG_OPT + 56, 0x4000);
        /* Descriptor at file 0x600 (RVA 0x2000): ILT/IAT 0x640, DLL name 0x1900. */
        img_w32(b, 0x600 + 0, 0x2040);
        img_w32(b, 0x600 + 12, 0x3300);
        img_w32(b, 0x600 + 16, 0x2040);
        img_w64(b, 0x640, 0x2100);         /* by-name at RVA 0x2100 (file 0x700) */
        img_w64(b, 0x648, 0);
        img_w16(b, 0x700, 0);              /* hint */
        memset(b + 0x702, 'I', lens[i]);
        memcpy(b + 0x1900, "KERNEL32.dll", 13);
        img_set_dir(b, DIR_IMPORT, 0x2000, 40);

        MT_EXPECT_EQ(pe_context_init(&ctx, b, 0x2000), PE_OK);
        MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
        memset(&st, 0, sizeof st);
        pe_status_t r = pe_imports_walk(&ctx, cb_dll, cb_sym, &st);
        MT_EXPECT_EQ(r, i == 0 ? PE_OK : PE_ERR_CORRUPT);
        free(b);
    }
}

/* -------------------------------------------------------------------------- */
/* Export table size and lookup (L-1)                                         */
/* -------------------------------------------------------------------------- */
/* Writes name strings (NUL-terminated, back to back) at L->strs and returns
 * each string's RVA through out[]. */
static void exp_put_strings(uint8_t *b, const exp_layout_t *L,
                            const char *const *strs, unsigned n, uint32_t *rva)
{
    size_t at = L->strs;
    for (unsigned i = 0; i < n; ++i) {
        size_t len = strlen(strs[i]) + 1;
        memcpy(b + at, strs[i], len);
        rva[i] = exp_rva(at);
        at += len;
    }
}

/* NumberOfFunctions and NumberOfNames above the documented limit, or a huge
 * NumberOfNames on a tiny file, are rejected as corrupt before any table is
 * touched. */
static void test_export_counts_over_limit_rejected(void)
{
    exp_layout_t L;
    uint8_t *b;
    pe_context_t ctx;
    exp_seen_t seen;

    L = exp_plan(PE_MAX_EXPORT_ENTRIES + 1, 0, 0);
    b = exp_image(&L, PE_MAX_EXPORT_ENTRIES + 1, 0);
    if (!b) { MT_EXPECT(0); return; }
    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
    memset(&seen, 0, sizeof seen);
    MT_EXPECT_EQ(pe_exports_walk(&ctx, cb_exp_keep, &seen), PE_ERR_CORRUPT);
    free(b);

    L = exp_plan(1, PE_MAX_EXPORT_ENTRIES + 1, 0);
    b = exp_image(&L, 1, PE_MAX_EXPORT_ENTRIES + 1);
    if (!b) { MT_EXPECT(0); return; }
    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
    memset(&seen, 0, sizeof seen);
    MT_EXPECT_EQ(pe_exports_walk(&ctx, cb_exp_keep, &seen), PE_ERR_CORRUPT);
    free(b);

    /* NumberOfNames = 0xFFFFFFFF on a file that is only a few kilobytes. */
    L = exp_plan(1, 0, 0);
    b = exp_image(&L, 1, 0);
    if (!b) { MT_EXPECT(0); return; }
    img_w32(b, EXP_DIR_OFF + 24, 0xFFFFFFFFu);
    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
    memset(&seen, 0, sizeof seen);
    MT_EXPECT_EQ(pe_exports_walk(&ctx, cb_exp_keep, &seen), PE_ERR_CORRUPT);
    free(b);
}

/* A name whose ordinal indexes past NumberOfFunctions has no function to
 * name. That is corrupt, not an ignored entry. */
static void test_export_ordinal_out_of_range_rejected(void)
{
    static const char *const names[] = { "Ab" };
    exp_layout_t L = exp_plan(1, 1, 3);
    uint8_t *b = exp_image(&L, 1, 1);
    uint32_t rva[1];
    exp_seen_t seen;
    pe_context_t ctx;
    if (!b) { MT_EXPECT(0); return; }

    exp_put_strings(b, &L, names, 1, rva);
    exp_set_func(b, &L, 0, 0x1000);
    exp_set_name(b, &L, 0, rva[0], 1);

    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
    memset(&seen, 0, sizeof seen);
    MT_EXPECT_EQ(pe_exports_walk(&ctx, cb_exp_keep, &seen), PE_ERR_CORRUPT);
    free(b);
}

/* Largest legal table: 65536 functions and 65536 names, about 640 KiB. Every
 * function has an RVA and every name points at the same string. The walker
 * must visit each export exactly once in bounded time. */
static void test_export_max_entries_walk(void)
{
    static const uint32_t max = PE_MAX_EXPORT_ENTRIES;
    static const char *const names[] = { "A" };
    exp_layout_t L = exp_plan(max, max, 2);
    uint8_t *b = exp_image(&L, max, max);
    uint32_t rva[1];
    exp_seen_t seen;
    pe_context_t ctx;
    if (!b) { MT_EXPECT(0); return; }

    exp_put_strings(b, &L, names, 1, rva);
    for (uint32_t i = 0; i < max; ++i) {
        exp_set_func(b, &L, i, 0x1000);
        exp_set_name(b, &L, i, rva[0], (uint16_t)i);
    }

    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);
    memset(&seen, 0, sizeof seen);
    MT_EXPECT_EQ(pe_exports_walk(&ctx, cb_exp_keep, &seen), PE_OK);
    MT_EXPECT_EQ(seen.count, max);
    free(b);
}

/* Binary search over a sorted name table. */
static void test_export_find_sorted(void)
{
    static const char *const names[] = { "Alpha", "Beta", "Gamma" };
    exp_layout_t L = exp_plan(3, 3, 32);
    uint8_t *b = exp_image(&L, 3, 3);
    uint32_t rva[3];
    pe_export_entry_t e;
    pe_context_t ctx;
    if (!b) { MT_EXPECT(0); return; }

    exp_put_strings(b, &L, names, 3, rva);
    for (uint32_t i = 0; i < 3; ++i) {
        exp_set_func(b, &L, i, 0x1000 + i);
        exp_set_name(b, &L, i, rva[i], (uint16_t)i);
    }

    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);

    MT_EXPECT_EQ(pe_export_find(&ctx, "Alpha", 5, &e), PE_OK);
    MT_EXPECT_EQ(e.ordinal, 1);
    MT_EXPECT_EQ(e.rva, 0x1000);
    MT_EXPECT_EQ(pe_export_find(&ctx, "Beta", 4, &e), PE_OK);
    MT_EXPECT_EQ(e.ordinal, 2);
    MT_EXPECT_EQ(pe_export_find(&ctx, "Gamma", 5, &e), PE_OK);
    MT_EXPECT_EQ(e.ordinal, 3);
    MT_EXPECT_EQ(e.rva, 0x1002);

    /* Missing names, including prefixes of present names. */
    MT_EXPECT_EQ(pe_export_find(&ctx, "Delta", 5, &e), PE_ERR_NOT_FOUND);
    MT_EXPECT_EQ(pe_export_find(&ctx, "Alp", 3, &e), PE_ERR_NOT_FOUND);
    MT_EXPECT_EQ(pe_export_find(&ctx, "Gamma!", 6, &e), PE_ERR_NOT_FOUND);
    MT_EXPECT_EQ(pe_export_find(&ctx, "", 0, &e), PE_ERR_NOT_FOUND);
    free(b);
}

/* The spec requires sorted names, but a table that is not sorted must still
 * resolve every name, through the linear fallback. */
static void test_export_find_unsorted_falls_back(void)
{
    static const char *const names[] = { "Gamma", "Alpha", "Beta" };
    exp_layout_t L = exp_plan(3, 3, 24);
    uint8_t *b = exp_image(&L, 3, 3);
    uint32_t rva[3];
    pe_export_entry_t e;
    pe_context_t ctx;
    if (!b) { MT_EXPECT(0); return; }

    exp_put_strings(b, &L, names, 3, rva);
    /* Name n maps to function ords[n]. */
    static const uint16_t ords[3] = { 2, 0, 1 };
    for (uint32_t i = 0; i < 3; ++i)
        exp_set_func(b, &L, i, 0x1000 + i);
    for (uint32_t n = 0; n < 3; ++n)
        exp_set_name(b, &L, n, rva[n], ords[n]);

    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);

    MT_EXPECT_EQ(pe_export_find(&ctx, "Alpha", 5, &e), PE_OK);
    MT_EXPECT_EQ(e.ordinal, 1);
    MT_EXPECT_EQ(pe_export_find(&ctx, "Beta", 4, &e), PE_OK);
    MT_EXPECT_EQ(e.ordinal, 2);
    MT_EXPECT_EQ(pe_export_find(&ctx, "Gamma", 5, &e), PE_OK);
    MT_EXPECT_EQ(e.ordinal, 3);
    MT_EXPECT_EQ(pe_export_find(&ctx, "Missing", 7, &e), PE_ERR_NOT_FOUND);
    free(b);
}

/* A lookup key longer than PE_MAX_SYMBOL_NAME cannot match a stored name. */
static void test_export_find_long_key_not_found(void)
{
    static const char *const names[] = { "Alpha" };
    exp_layout_t L = exp_plan(1, 1, 8);
    uint8_t *b = exp_image(&L, 1, 1);
    uint32_t rva[1];
    pe_export_entry_t e;
    pe_context_t ctx;
    char *key;
    if (!b) { MT_EXPECT(0); return; }

    exp_put_strings(b, &L, names, 1, rva);
    exp_set_func(b, &L, 0, 0x1000);
    exp_set_name(b, &L, 0, rva[0], 0);
    MT_EXPECT_EQ(pe_context_init(&ctx, b, L.len), PE_OK);
    MT_EXPECT_EQ(pe_validate(&ctx), PE_OK);

    key = malloc(PE_MAX_SYMBOL_NAME + 2);
    if (!key) { MT_EXPECT(0); free(b); return; }
    memset(key, 'A', PE_MAX_SYMBOL_NAME + 1);
    key[PE_MAX_SYMBOL_NAME + 1] = '\0';
    MT_EXPECT_EQ(pe_export_find(&ctx, key, PE_MAX_SYMBOL_NAME + 1, &e),
                 PE_ERR_NOT_FOUND);
    free(key);
    free(b);
}

/* -------------------------------------------------------------------------- */
/* Runner                                                                     */
/* -------------------------------------------------------------------------- */
static const mt_case_t cases[] = {
    MT_CASE(test_valid_image_validates),
    MT_CASE(test_null_and_empty_inputs_rejected),
    MT_CASE(test_every_truncation_rejected),
    MT_CASE(test_truncated_headers_rejected),
    MT_CASE(test_bad_signatures_rejected),
    MT_CASE(test_short_optional_header_is_corrupt),
    MT_CASE(test_e_lfanew_near_4gib_rejected),
    MT_CASE(test_section_raw_bounds_rejected),
    MT_CASE(test_section_va_bounds_rejected),
    MT_CASE(test_overlapping_sections_rejected),
    MT_CASE(test_size_of_headers_bounds_rejected),
    MT_CASE(test_bad_data_directory_rejected),
    MT_CASE(test_imports_walk_valid_table),
    MT_CASE(test_relocs_walk_valid_block),
    MT_CASE(test_relocs_bad_block_sizes_rejected),
    MT_CASE(test_relocs_directory_past_file_rejected),
    MT_CASE(test_exception_bad_size_rejected),
    MT_CASE(test_exception_end_address_not_truncated),
    MT_CASE(test_walkers_report_bad_directory),
    MT_CASE(test_export_name_length_cap),
    MT_CASE(test_export_name_unterminated_rejected),
    MT_CASE(test_export_forwarder_resolves),
    MT_CASE(test_export_forwarder_length_cap),
    MT_CASE(test_import_name_length_cap),
    MT_CASE(test_export_counts_over_limit_rejected),
    MT_CASE(test_export_ordinal_out_of_range_rejected),
    MT_CASE(test_export_max_entries_walk),
    MT_CASE(test_export_find_sorted),
    MT_CASE(test_export_find_unsorted_falls_back),
    MT_CASE(test_export_find_long_key_not_found),
    MT_CASE(test_relocs_reserved_type_rejected),
    MT_CASE(test_fuzzed_images_do_not_crash),
};

int main(void)
{
    return MT_RUN("pe_parse", cases);
}
