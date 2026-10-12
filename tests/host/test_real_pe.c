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
/* Runner                                                                     */
/* -------------------------------------------------------------------------- */
static const mt_case_t cases[] = {
    MT_CASE(test_minimal_validates),
    MT_CASE(test_minimal_headers),
    MT_CASE(test_minimal_sections),
    MT_CASE(test_minimal_directories),
    MT_CASE(test_minimal_rva_translation),
    MT_CASE(test_minimal_walkers),
};

int main(void)
{
    return MT_RUN("real_pe", cases);
}
