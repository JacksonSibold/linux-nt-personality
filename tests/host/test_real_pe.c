/* test_real_pe.c - pe_parse against real PE images produced by the MinGW gcc.
 *
 * The fixtures in out/fixtures/ are built by tests/host/Makefile from
 * the sources under tests/host/fixtures/. They are read into exact-size heap
 * buffers and parsed. They are never executed.
 *
 * Expected values are hard-coded from the sources and checked against
 * `objdump -p` output for the toolchain in TOOLCHAIN.md (gcc 16.2.0, binutils
 * 2.47). Layout values (RVAs, section counts) depend on the linker, so a
 * different binutils may need them updated.
 */
#include "minitest.h"
#include "pe_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX_DIR "out/fixtures/"

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */
typedef struct real_image {
    u8 *p;
    size_t len;
    pe_context_t ctx;
} real_image_t;

/* Reads a whole file into an exact-size buffer and initialises ctx. Returns 0
 * on any failure, after reporting it through the test log. */
static int real_load(real_image_t *img, const char *name)
{
    char path[256];
    FILE *f;
    long n;

    memset(img, 0, sizeof *img);
    snprintf(path, sizeof path, FIX_DIR "%s", name);
    f = fopen(path, "rb");
    if (!f) {
        printf("    cannot open %s (build it with make real-pe)\n", path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    img->len = (size_t)n;
    img->p = malloc(img->len);
    if (!img->p || fread(img->p, 1, img->len, f) != img->len) {
        fclose(f);
        free(img->p);
        img->p = NULL;
        return 0;
    }
    fclose(f);
    return pe_context_init(&img->ctx, img->p, img->len) == PE_OK;
}

static void real_unload(real_image_t *img)
{
    free(img->p);
    img->p = NULL;
}

/* Section name as a NUL-terminated string (PE names are at most 8 bytes and
 * need not be NUL-terminated). */
static int sec_name_is(const pe_section_view_t *v, const char *want)
{
    char buf[IMAGE_SIZEOF_SHORT_NAME + 1];
    memset(buf, 0, sizeof buf);
    memcpy(buf, v->hdr.Name, IMAGE_SIZEOF_SHORT_NAME);
    return strcmp(buf, want) == 0;
}

/* -------------------------------------------------------------------------- */
/* (a) minimal.exe: nostdlib, custom entry point                              */
/* -------------------------------------------------------------------------- */
static void test_minimal_validates(void)
{
    real_image_t img;
    MT_ASSERT(real_load(&img, "minimal.exe"));
    MT_EXPECT_EQ(pe_validate(&img.ctx), PE_OK);
    real_unload(&img);
}

static void test_minimal_headers(void)
{
    real_image_t img;
    MT_ASSERT(real_load(&img, "minimal.exe"));
    MT_ASSERT(pe_validate(&img.ctx) == PE_OK);

    MT_EXPECT_EQ(img.ctx.optional_magic, 0x20B);
    MT_EXPECT_EQ(img.ctx.entry_point, 0x1000);          /* minimal_entry */
    MT_EXPECT_EQ(img.ctx.image_base, 0x140000000ULL);
    MT_EXPECT_EQ(img.ctx.section_alignment, 0x1000);
    MT_EXPECT_EQ(img.ctx.file_alignment, 0x200);
    MT_EXPECT_EQ(img.ctx.size_of_image, 0x7000);
    MT_EXPECT_EQ(img.ctx.size_of_headers, 0x400);
    MT_EXPECT_EQ(img.ctx.subsystem, 3);                 /* IMAGE_SUBSYSTEM_WINDOWS_CUI */
    MT_EXPECT_EQ(img.ctx.num_data_directories, 16);
    MT_EXPECT_EQ(img.ctx.dll_characteristics & 0x40, 0x40);  /* DYNAMIC_BASE */
    real_unload(&img);
}

static void test_minimal_sections(void)
{
    real_image_t img;
    pe_section_view_t v;
    u16 n = 0;
    static const struct { const char *name; u32 va, vsize, raw, rawsize; } want[] = {
        { ".text",  0x1000, 0x10, 0x400, 0x200 },
        { ".data",  0x2000, 0x10, 0x600, 0x200 },
        { ".rdata", 0x3000, 0x20, 0x800, 0x200 },
        { ".pdata", 0x4000, 0x0c, 0xa00, 0x200 },
        { ".xdata", 0x5000, 0x04, 0xc00, 0x200 },
        { ".idata", 0x6000, 0x18, 0xe00, 0x200 },
    };

    MT_ASSERT(real_load(&img, "minimal.exe"));
    MT_ASSERT(pe_validate(&img.ctx) == PE_OK);
    MT_ASSERT(pe_section_count(&img.ctx, &n) == PE_OK);
    MT_EXPECT_EQ(n, sizeof want / sizeof want[0]);

    for (u16 i = 0; i < n && i < sizeof want / sizeof want[0]; ++i) {
        MT_ASSERT(pe_section_get(&img.ctx, i, &v) == PE_OK);
        MT_EXPECT(sec_name_is(&v, want[i].name));
        MT_EXPECT_EQ(v.virtual_address, want[i].va);
        MT_EXPECT_EQ(v.virtual_size, want[i].vsize);
        MT_EXPECT_EQ(v.file_offset, want[i].raw);
        MT_EXPECT_EQ(v.file_size, want[i].rawsize);
    }
    real_unload(&img);
}

static void test_minimal_directories(void)
{
    real_image_t img;
    pe_data_directory_t d;

    MT_ASSERT(real_load(&img, "minimal.exe"));
    MT_ASSERT(pe_validate(&img.ctx) == PE_OK);

    MT_ASSERT(pe_directory_get(&img.ctx, IMAGE_DIRECTORY_ENTRY_EXPORT, &d) == PE_OK);
    MT_EXPECT_EQ(d.rva, 0);
    MT_EXPECT_EQ(d.size, 0);

    /* ld emits an all-zero import descriptor for a nostdlib image. */
    MT_ASSERT(pe_directory_get(&img.ctx, IMAGE_DIRECTORY_ENTRY_IMPORT, &d) == PE_OK);
    MT_EXPECT_EQ(d.rva, 0x6000);
    MT_EXPECT_EQ(d.size, 0x18);

    MT_ASSERT(pe_directory_get(&img.ctx, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &d) == PE_OK);
    MT_EXPECT_EQ(d.rva, 0x4000);
    MT_EXPECT_EQ(d.size, 0x0c);

    MT_ASSERT(pe_directory_get(&img.ctx, IMAGE_DIRECTORY_ENTRY_BASERELOC, &d) == PE_OK);
    MT_EXPECT_EQ(d.rva, 0);
    MT_EXPECT_EQ(d.size, 0);

    MT_ASSERT(pe_directory_get(&img.ctx, IMAGE_DIRECTORY_ENTRY_TLS, &d) == PE_OK);
    MT_EXPECT_EQ(d.rva, 0);
    MT_EXPECT_EQ(d.size, 0);
    real_unload(&img);
}

static void test_minimal_rva_translation(void)
{
    real_image_t img;
    u32 off = 0;

    MT_ASSERT(real_load(&img, "minimal.exe"));
    MT_ASSERT(pe_validate(&img.ctx) == PE_OK);
    MT_EXPECT_EQ(pe_rva_to_offset(&img.ctx, 0x1000, &off), PE_OK);
    MT_EXPECT_EQ(off, 0x400);                 /* minimal_entry */
    MT_EXPECT_EQ(pe_rva_to_offset(&img.ctx, 0x2000, &off), PE_OK);
    MT_EXPECT_EQ(off, 0x600);                 /* minimal_counter */
    real_unload(&img);
}

static pe_status_t count_dll(void *user, const char *name, size_t len,
                             u32 iat, u32 ilt)
{
    (void)name; (void)len; (void)iat; (void)ilt;
    ++*(unsigned *)user;
    return PE_OK;
}

static pe_status_t count_sym(void *user, const pe_import_symbol_t *sym)
{
    (void)sym;
    ++*(unsigned *)user;
    return PE_OK;
}

static pe_status_t count_export(void *user, const pe_export_entry_t *e)
{
    (void)e;
    ++*(unsigned *)user;
    return PE_OK;
}

static pe_status_t count_reloc(void *user, const pe_reloc_entry_t *e)
{
    (void)e;
    ++*(unsigned *)user;
    return PE_OK;
}

static pe_status_t take_rf(void *user, const pe_runtime_function_t *rf)
{
    unsigned *n = user;
    if (*n == 0) {
        MT_EXPECT_EQ(rf->begin, 0x1000);
        MT_EXPECT_EQ(rf->end, 0x1009);
        MT_EXPECT_EQ(rf->unwind, 0x5000);
    }
    ++*n;
    return PE_OK;
}

static pe_status_t count_rf(void *user, const pe_runtime_function_t *rf)
{
    (void)rf;
    ++*(unsigned *)user;
    return PE_OK;
}

static void test_minimal_walkers(void)
{
    real_image_t img;
    unsigned n;
    pe_status_t s;

    MT_ASSERT(real_load(&img, "minimal.exe"));
    MT_ASSERT(pe_validate(&img.ctx) == PE_OK);

    /* Empty import descriptor array: success, nothing reported. */
    n = 0;
    s = pe_imports_walk(&img.ctx, count_dll, count_sym, &n);
    MT_EXPECT_EQ(s, PE_OK);
    MT_EXPECT_EQ(n, 0);

    n = 0;
    MT_EXPECT_EQ(pe_exports_walk(&img.ctx, count_export, &n), PE_OK);
    MT_EXPECT_EQ(n, 0);

    n = 0;
    MT_EXPECT_EQ(pe_relocs_walk(&img.ctx, count_reloc, &n), PE_OK);
    MT_EXPECT_EQ(n, 0);

    /* One RUNTIME_FUNCTION for minimal_entry (the loop is a leaf with no
     * prologue, so the unwind record is a bare 4-byte header). */
    n = 0;
    MT_EXPECT_EQ(pe_exception_walk(&img.ctx, take_rf, &n), PE_OK);
    MT_EXPECT_EQ(n, 1);

    /* No TLS directory: NOT_FOUND, not a bogus structure. */
    {
        IMAGE_TLS_DIRECTORY64 tls;
        MT_EXPECT_EQ(pe_tls_directory(&img.ctx, &tls), PE_ERR_NOT_FOUND);
    }
    real_unload(&img);
}

/* -------------------------------------------------------------------------- */
/* (b) exports.dll: named, ordinal-only, DATA exports and forwarders          */
/* -------------------------------------------------------------------------- */
#define MAX_SEEN 16

typedef struct seen_export {
    u32 ordinal;
    u32 rva;
    int has_name;
    char name[64];
    int has_fwd;
    char fwd[64];
} seen_export_t;

typedef struct export_log {
    unsigned n;
    seen_export_t e[MAX_SEEN];
} export_log_t;

static void copy_str(char *dst, size_t cap, const char *src, size_t len)
{
    size_t k = len < cap - 1 ? len : cap - 1;
    memcpy(dst, src, k);
    dst[k] = '\0';
}

static pe_status_t log_export(void *user, const pe_export_entry_t *e)
{
    export_log_t *log = user;
    seen_export_t *s;
    if (log->n >= MAX_SEEN)
        return PE_ERR_CORRUPT;
    s = &log->e[log->n++];
    s->ordinal = e->ordinal;
    s->rva = e->rva;
    s->has_name = e->name != NULL;
    if (e->name)
        copy_str(s->name, sizeof s->name, e->name, e->name_len);
    s->has_fwd = e->forwarder != NULL;
    if (e->forwarder)
        copy_str(s->fwd, sizeof s->fwd, e->forwarder, e->forwarder_len);
    return PE_OK;
}

static void expect_export(const export_log_t *log, unsigned i,
                          u32 ord, u32 rva, const char *name,
                          const char *fwd)
{
    const seen_export_t *s;
    if (i >= log->n) {
        MT_EXPECT(i < log->n);
        return;
    }
    s = &log->e[i];
    MT_EXPECT_EQ(s->ordinal, ord);
    if (!fwd)  /* a forwarder's RVA points at its string, not code */
        MT_EXPECT_EQ(s->rva, rva);
    MT_EXPECT_EQ(s->has_name, name != NULL);
    if (name)
        MT_EXPECT(strcmp(s->name, name) == 0);
    MT_EXPECT_EQ(s->has_fwd, fwd != NULL);
    if (fwd)
        MT_EXPECT(strcmp(s->fwd, fwd) == 0);
}

static void test_exports_dll_validates(void)
{
    real_image_t img;
    MT_ASSERT(real_load(&img, "exports.dll"));
    MT_EXPECT_EQ(pe_validate(&img.ctx), PE_OK);
    real_unload(&img);
}

/* Named exports come first in name-table order, then unnamed functions in
 * ordinal order. Ordinals are biased by the directory's Base (1). */
static void test_exports_dll_walk(void)
{
    real_image_t img;
    export_log_t log;

    MT_ASSERT(real_load(&img, "exports.dll"));
    MT_ASSERT(pe_validate(&img.ctx) == PE_OK);

    memset(&log, 0, sizeof log);
    MT_ASSERT(pe_exports_walk(&img.ctx, log_export, &log) == PE_OK);
    MT_EXPECT_EQ(log.n, 7);

    expect_export(&log, 0, 1, 0x1000, "alpha_fn", NULL);
    expect_export(&log, 1, 2, 0x1006, "beta_fn", NULL);
    expect_export(&log, 2, 4, 0, "delta_fwd", "kernel32.HeapAlloc");
    expect_export(&log, 3, 5, 0x2000, "exports_counter", NULL);
    expect_export(&log, 4, 3, 0, "gamma_fwd", "ntdll.RtlAllocateHeap");
    expect_export(&log, 5, 10, 0x100c, NULL, NULL);
    expect_export(&log, 6, 11, 0, NULL, "ntdll.RtlFreeHeap");
    real_unload(&img);
}

static void test_exports_dll_find(void)
{
    real_image_t img;
    pe_export_entry_t e;

    MT_ASSERT(real_load(&img, "exports.dll"));
    MT_ASSERT(pe_validate(&img.ctx) == PE_OK);

    MT_EXPECT_EQ(pe_export_find(&img.ctx, "alpha_fn", 8, &e), PE_OK);
    MT_EXPECT_EQ(e.ordinal, 1);
    MT_EXPECT_EQ(e.rva, 0x1000);
    MT_EXPECT(e.forwarder == NULL);

    MT_EXPECT_EQ(pe_export_find(&img.ctx, "gamma_fwd", 9, &e), PE_OK);
    MT_EXPECT_EQ(e.ordinal, 3);
    MT_ASSERT(e.forwarder != NULL);
    MT_EXPECT_EQ(e.forwarder_len, 21);          /* "ntdll.RtlAllocateHeap" */
    MT_EXPECT(memcmp(e.forwarder, "ntdll.RtlAllocateHeap", 21) == 0);

    /* Exported by ordinal only: no name to find. */
    MT_EXPECT_EQ(pe_export_find(&img.ctx, "hidden_fn", 9, &e), PE_ERR_NOT_FOUND);
    MT_EXPECT_EQ(pe_export_find(&img.ctx, "no_such", 7, &e), PE_ERR_NOT_FOUND);
    real_unload(&img);
}

static void test_exports_dll_directories(void)
{
    real_image_t img;
    pe_data_directory_t d;
    unsigned n = 0;

    MT_ASSERT(real_load(&img, "exports.dll"));
    MT_ASSERT(pe_validate(&img.ctx) == PE_OK);

    MT_ASSERT(pe_directory_get(&img.ctx, IMAGE_DIRECTORY_ENTRY_EXPORT, &d) == PE_OK);
    MT_EXPECT_EQ(d.rva, 0x6000);
    MT_EXPECT_EQ(d.size, 0x100);

    /* Four RUNTIME_FUNCTION entries, one per function that has code. */
    MT_EXPECT_EQ(pe_exception_walk(&img.ctx, count_rf, &n), PE_OK);
    MT_EXPECT_EQ(n, 4);
    real_unload(&img);
}

/* -------------------------------------------------------------------------- */
/* Runner                                                                     */
/* -------------------------------------------------------------------------- */
static const mt_case_t cases[] = {
    MT_CASE(test_minimal_validates),
    MT_CASE(test_minimal_headers),
    MT_CASE(test_minimal_sections),
    MT_CASE(test_minimal_directories),
    MT_CASE(test_minimal_rva_translation),
    MT_CASE(test_minimal_walkers),
    MT_CASE(test_exports_dll_validates),
    MT_CASE(test_exports_dll_walk),
    MT_CASE(test_exports_dll_find),
    MT_CASE(test_exports_dll_directories),
};

int main(void)
{
    return MT_RUN("real_pe", cases);
}
