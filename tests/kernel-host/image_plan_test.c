/*
 * image_plan_test.c - host tests for the SEC_IMAGE layout planner
 * (src/kernel/nt/image_plan.h).
 *
 * Synthesized images come from pe_synth.h. Real images are the MinGW
 * fixtures in tests/host/out/fixtures, read as bytes and never executed. The
 * fixture tests print SKIP and pass when the files are absent.
 */
#include "image_plan.h"
#include "pe_synth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_fails;

#define CHECK(c) do { \
    g_checks++; \
    if (!(c)) { g_fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

#define FIX_DIR "../../host/out/fixtures/"

/* Expected segment values for the default synthesized image. */
static void check_default_plan(const ip_plan_t *p)
{
    CHECK(p->preferred_base == PES_BASE_DEFAULT);
    CHECK(p->size_of_image == 0x4000);
    CHECK(p->size_of_headers == 0x400);
    CHECK(p->section_alignment == 0x1000);
    CHECK(p->file_alignment == 0x200);
    CHECK(p->entry_rva == 0x1000);
    CHECK(p->relocatable == 0);
    CHECK(p->relocs_stripped == 1);
    CHECK(p->num_segments == 3);

    /* seg[0]: header region, read-only, zero tail to the page */
    CHECK(p->seg[0].is_header == 1);
    CHECK(p->seg[0].rva == 0);
    CHECK(p->seg[0].size == 0x1000);
    CHECK(p->seg[0].file_offset == 0);
    CHECK(p->seg[0].file_size == 0x400);
    CHECK(p->seg[0].zero_size == 0xC00);
    CHECK(p->seg[0].prot == IP_PROT_READONLY);

    /* .text: file gives VirtualSize bytes (0x100), rest of the page is zero */
    CHECK(p->seg[1].is_header == 0);
    CHECK(p->seg[1].rva == 0x1000);
    CHECK(p->seg[1].size == 0x1000);
    CHECK(p->seg[1].file_offset == 0x400);
    CHECK(p->seg[1].file_size == 0x100);
    CHECK(p->seg[1].zero_size == 0xF00);
    CHECK(p->seg[1].prot == IP_PROT_EXECUTE_READ);

    /* .data: raw 0x200 of VirtualSize 0x1800 page-rounded to 0x2000 */
    CHECK(p->seg[2].rva == 0x2000);
    CHECK(p->seg[2].size == 0x2000);
    CHECK(p->seg[2].file_offset == 0x600);
    CHECK(p->seg[2].file_size == 0x200);
    CHECK(p->seg[2].zero_size == 0x1E00);
    CHECK(p->seg[2].prot == IP_PROT_WRITECOPY);
}

static int test_default_plan(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    memset(&p, 0, sizeof p);
    pe_synth_build(buf, NULL);
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_OK);
    check_default_plan(&p);
    return 0;
}

static int test_relocatable_plan(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_opts_t o = { 0, 1, 0, 0 };
    pe_synth_build(buf, &o);
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_OK);
    CHECK(p.relocatable == 1);
    CHECK(p.relocs_stripped == 0);
    CHECK(p.reloc_rva == 0x1000);
    CHECK(p.reloc_size == 0x0C);
    return 0;
}

static int test_protection_table(void)
{
    CHECK(ip_prot_from_characteristics(0x00000000u) == IP_PROT_NOACCESS);
    CHECK(ip_prot_from_characteristics(0x40000000u) == IP_PROT_READONLY);
    CHECK(ip_prot_from_characteristics(0x20000000u) == IP_PROT_EXECUTE);
    CHECK(ip_prot_from_characteristics(0x60000000u) == IP_PROT_EXECUTE_READ);
    CHECK(ip_prot_from_characteristics(0x80000000u) == IP_PROT_WRITECOPY);
    CHECK(ip_prot_from_characteristics(0xC0000000u) == IP_PROT_WRITECOPY);
    CHECK(ip_prot_from_characteristics(0xA0000000u) == IP_PROT_EXECUTE_WRITECOPY);
    CHECK(ip_prot_from_characteristics(0xE0000000u) == IP_PROT_EXECUTE_WRITECOPY);
    return 0;
}

static int test_pe32_refused(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_opts_t o = { 1, 0, 0, 0 };
    pe_synth_build(buf, &o);
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_PE32);
    return 0;
}

static int test_not_a_pe(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_build(buf, NULL);
    buf[0] = 'X';
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_FORMAT);
    return 0;
}

static int test_truncated_headers(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_build(buf, NULL);
    /* Headers end at 0x1D8 and SizeOfHeaders is 0x400: 0x100 bytes is short */
    CHECK(ip_plan_build(buf, 0x100, &p) == IP_ERR_FORMAT);
    return 0;
}

static int test_param_errors(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_build(buf, NULL);
    CHECK(ip_plan_build(NULL, PES_FILE_SIZE, &p) == IP_ERR_PARAM);
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, NULL) == IP_ERR_PARAM);
    CHECK(ip_plan_build(buf, 0, &p) == IP_ERR_PARAM);
    return 0;
}

static int test_unsupported_section_alignment(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_opts_t o = { 0, 0, 0, 0x200 };
    pe_synth_build(buf, &o);
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_UNSUPPORTED_ALIGN);
    return 0;
}

static int test_base_alignment(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_opts_t o = { 0, 0, 0x140001000ULL, 0 };
    pe_synth_build(buf, &o);
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_ALIGN);
    pe_synth_build(buf, NULL);
    pes_put64(buf, PES_OPT_OFF + 24, 0);
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_ALIGN);
    return 0;
}

static int test_base_limit(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    /* The base is the user-space limit itself, 64 KiB aligned, so base + SizeOfImage is past it */
    pe_synth_opts_t o = { 0, 0, 0x00007FFFFFFF0000ULL, 0 };
    pe_synth_build(buf, &o);
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_BASE);
    return 0;
}

static int test_raw_pointer_alignment(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_build(buf, NULL);
    pes_put32(buf, PES_TEXT_HDR + 20, 0x401);
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_ALIGN);
    return 0;
}

static int test_raw_beyond_file(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_build(buf, NULL);
    pes_put32(buf, PES_DATA_HDR + 16, 0x1000);   /* SizeOfRawData: 0x600+0x1000 > 0x800 */
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_RAW_RANGE);
    return 0;
}

static int test_section_beyond_size_of_image(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_build(buf, NULL);
    pes_put32(buf, PES_SIZE_OF_IMAGE_OFF, 0x3000);  /* .data ends at 0x3800 */
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_LAYOUT);
    return 0;
}

static int test_overlapping_sections(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_build(buf, NULL);
    pes_put32(buf, PES_DATA_HDR + 12, 0x1000);      /* .data onto .text */
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_LAYOUT);
    return 0;
}

static int test_section_over_headers(void)
{
    u8 buf[PES_FILE_SIZE];
    ip_plan_t p;
    pe_synth_build(buf, NULL);
    pes_put32(buf, PES_TEXT_HDR + 12, 0);           /* .text at RVA 0 covers the header page */
    CHECK(ip_plan_build(buf, PES_FILE_SIZE, &p) == IP_ERR_LAYOUT);
    return 0;
}

/* Checks invariants that any valid plan must meet, for a real image. */
static void check_plan_invariants(const ip_plan_t *p, size_t file_len)
{
    u32 i, prev_end = 0;
    CHECK(p->num_segments >= 2 && p->num_segments <= IP_MAX_SECTIONS + 1);
    CHECK(p->size_of_image % p->section_alignment == 0);
    CHECK(p->preferred_base % IP_ALLOC_GRANULARITY == 0);
    CHECK(p->seg[0].is_header == 1 && p->seg[0].prot == IP_PROT_READONLY);
    for (i = 0; i < p->num_segments; i++) {
        const ip_segment_t *s = &p->seg[i];
        CHECK(s->rva % IP_PAGE_SIZE == 0);
        CHECK(s->size % p->section_alignment == 0);
        CHECK(s->file_size <= s->size);
        CHECK(s->zero_size == s->size - s->file_size);
        CHECK((u64)s->file_offset + s->file_size <= (u64)file_len);
        CHECK((u64)s->rva + s->size <= (u64)p->size_of_image);
        if (i > 0) CHECK(s->rva >= prev_end);   /* sorted and non-overlapping */
        prev_end = s->rva + s->size;
    }
}

static u8 *read_fixture(const char *name, size_t *len)
{
    char path[256];
    FILE *f;
    long n;
    u8 *buf;
    snprintf(path, sizeof path, FIX_DIR "%s", name);
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int test_real_fixtures(void)
{
    static const char *names[] = { "minimal.exe", "exports.dll" };
    unsigned k;
    for (k = 0; k < sizeof names / sizeof names[0]; k++) {
        size_t len = 0;
        u8 *buf = read_fixture(names[k], &len);
        ip_plan_t *p;
        if (!buf) {
            printf("  SKIP %s: fixture not built (run make in tests/host)\n", names[k]);
            continue;
        }
        p = calloc(1, sizeof *p);
        CHECK(p != NULL);
        if (p) {
            /* Invariants are checked only for a plan the planner accepted */
            if (ip_plan_build(buf, len, p) == IP_OK) {
                check_plan_invariants(p, len);
            } else {
                CHECK(0);
            }
            if (strcmp(names[k], "minimal.exe") == 0 && p->num_segments > 0) {
                /* .text is code: read and execute, not writable */
                int found_rx = 0;
                u32 i;
                for (i = 0; i < p->num_segments; i++)
                    if (p->seg[i].prot == IP_PROT_EXECUTE_READ) found_rx = 1;
                CHECK(found_rx);
            }
            free(p);
        }
        free(buf);
    }
    return 0;
}

typedef int (*test_fn)(void);

static const struct { const char *name; test_fn fn; } g_tests[] = {
    { "default_plan", test_default_plan },
    { "relocatable_plan", test_relocatable_plan },
    { "protection_table", test_protection_table },
    { "pe32_refused", test_pe32_refused },
    { "not_a_pe", test_not_a_pe },
    { "truncated_headers", test_truncated_headers },
    { "param_errors", test_param_errors },
    { "unsupported_section_alignment", test_unsupported_section_alignment },
    { "base_alignment", test_base_alignment },
    { "base_limit", test_base_limit },
    { "raw_pointer_alignment", test_raw_pointer_alignment },
    { "raw_beyond_file", test_raw_beyond_file },
    { "section_beyond_size_of_image", test_section_beyond_size_of_image },
    { "overlapping_sections", test_overlapping_sections },
    { "section_over_headers", test_section_over_headers },
    { "real_fixtures", test_real_fixtures },
};

int main(void)
{
    unsigned i;
    int before;
    for (i = 0; i < sizeof g_tests / sizeof g_tests[0]; i++) {
        before = g_fails;
        printf("TEST %s ... ", g_tests[i].name);
        g_tests[i].fn();
        printf("%s\n", g_fails == before ? "PASS" : "FAIL");
    }
    printf("image_plan_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
