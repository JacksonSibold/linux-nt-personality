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
/* Runner                                                                     */
/* -------------------------------------------------------------------------- */
static const mt_case_t cases[] = {
    MT_CASE(test_valid_image_validates),
    MT_CASE(test_null_and_empty_inputs_rejected),
    MT_CASE(test_every_truncation_rejected),
    MT_CASE(test_truncated_headers_rejected),
    MT_CASE(test_bad_signatures_rejected),
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
    MT_CASE(test_walkers_report_bad_directory),
    MT_CASE(test_fuzzed_images_do_not_crash),
};

int main(void)
{
    return MT_RUN("pe_parse", cases);
}
