/*
 * test_ldr_pure.c - host tests for the address translation and bounds logic
 * in src/ntdll/ldr_pure.h, used by ldr.c for the IAT patch (LDR-2).
 *
 * The image buffers here stand in for a mapped module laid out by RVA. Each
 * test keeps the file-offset location of the same bytes in view: the section
 * raw data sits at 0x400 in the file and at VirtualAddress 0x2000 in the
 * image, so an RVA write must never change the bytes at 0x400.
 */
#include "minitest.h"
#include "ldr_pure.h"
#include <stdlib.h>
#include <string.h>

#define IMG_SIZE 0x3000ULL
#define IDATA_RVA 0x2000u
#define IDATA_RAW_OFF 0x400u
#define MARK 0xABu

static unsigned long long read_u64(const unsigned char *p)
{
    unsigned long long v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* LDR-2: the IAT base is FirstThunk. A zero FirstThunk falls back to the ILT
 * RVA (OriginalFirstThunk), also as an RVA. */
static void iat_base_is_rva_with_ilt_fallback(void)
{
    MT_EXPECT_EQ(ldr_iat_base_rva(0x2000, 0x1000), 0x2000);
    MT_EXPECT_EQ(ldr_iat_base_rva(0x2000, 0), 0x2000);
    MT_EXPECT_EQ(ldr_iat_base_rva(0, 0x1800), 0x1800);
}

/* LDR-2: the resolved address lands at IDATA_RVA + index * 8 in the image,
 * and the bytes at the file offset of the same section stay as they were. */
static void iat_write_lands_at_rva_not_file_offset(void)
{
    unsigned char image[IMG_SIZE];
    memset(image, MARK, sizeof(image));

    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, IDATA_RVA, 1, 0x1122334455667788ULL) == 1);
    MT_EXPECT(read_u64(image + IDATA_RVA + 8) == 0x1122334455667788ULL);
    MT_EXPECT(read_u64(image + IDATA_RVA) == 0xABABABABABABABABULL);

    for (unsigned i = 0; i < 8; ++i)
        MT_EXPECT(image[IDATA_RAW_OFF + i] == MARK);
}

/* The last slot that fits ends exactly at the image size. */
static void iat_write_exact_fit_at_end(void)
{
    unsigned char image[IMG_SIZE];
    memset(image, MARK, sizeof(image));

    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0x2FF8, 0, 0x55ULL) == 1);
    MT_EXPECT(read_u64(image + 0x2FF8) == 0x55ULL);
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0x2FFC, 0, 0x66ULL) == 0);
}

/* A rejected write changes nothing, including the last valid slot. */
static void iat_write_rejected_writes_nothing(void)
{
    unsigned char image[IMG_SIZE];
    unsigned char copy[IMG_SIZE];
    memset(image, MARK, sizeof(image));
    memcpy(copy, image, sizeof(image));

    /* Fixture shape from the LDR-2 test plan: FirstThunk at image_size - 4. */
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, IMG_SIZE - 4, 0, 0x77ULL) == 0);
    MT_EXPECT(memcmp(image, copy, sizeof(image)) == 0);
}

/* The slot address is computed without wrapping at 32 bits, and a zero-size
 * image accepts no write at all. */
static void iat_write_no_wrap(void)
{
    unsigned char image[IMG_SIZE];
    memset(image, MARK, sizeof(image));

    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0xFFFFFFFCULL, 0, 1) == 0);
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0xFFFFFFF8ULL, 1, 1) == 0);
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, 0, 0xFFFFFFFFFFFFULL, 1) == 0);
    MT_EXPECT(ldr_iat_write(image, 0, IDATA_RVA, 0, 1) == 0);
}

/* LDR-4: AddressOfCallBacks is a VA built on the preferred base. The table is
 * read from mod->image at VA - OriginalBase, not dereferenced as a VA. */
#define PREF_BASE 0x140000000ULL

static void tls_table_va_zero_is_none(void)
{
    unsigned int rva = 0xDEAD;
    MT_EXPECT(ldr_tls_cb_table_rva(0, PREF_BASE, IMG_SIZE, &rva) == LDR_TLS_CB_NONE);
    MT_EXPECT_EQ(rva, 0xDEAD);
}

/* A table VA below the preferred base is not in the image: rejected, and the
 * output is left alone. */
static void tls_table_below_base_is_bad(void)
{
    unsigned int rva = 0xDEAD;
    MT_EXPECT(ldr_tls_cb_table_rva(PREF_BASE - 0x1000, PREF_BASE, IMG_SIZE, &rva)
              == LDR_TLS_CB_BAD);
    MT_EXPECT_EQ(rva, 0xDEAD);
}

/* The RVA is the VA minus the preferred base, and the 8-byte table head must
 * fit. The image may sit at any real address; only OriginalBase matters. */
static void tls_table_rva_is_va_minus_original_base(void)
{
    unsigned int rva = 0;
    MT_EXPECT(ldr_tls_cb_table_rva(PREF_BASE + 0x2010, PREF_BASE, IMG_SIZE, &rva)
              == LDR_TLS_CB_OK);
    MT_EXPECT_EQ(rva, 0x2010);

    MT_EXPECT(ldr_tls_cb_table_rva(PREF_BASE + 0x2FF8, PREF_BASE, IMG_SIZE, &rva)
              == LDR_TLS_CB_OK);
    MT_EXPECT_EQ(rva, 0x2FF8);

    /* Table head at image_size - 4: an 8-byte read would run past the image. */
    MT_EXPECT(ldr_tls_cb_table_rva(PREF_BASE + 0x2FFC, PREF_BASE, IMG_SIZE, &rva)
              == LDR_TLS_CB_BAD);
}

/* A VA far above the image, or a zero-size image, never yields a table. */
static void tls_table_no_wrap(void)
{
    unsigned int rva = 0;
    MT_EXPECT(ldr_tls_cb_table_rva(0xFFFFFFFFFFFFFFF0ULL, PREF_BASE, IMG_SIZE, &rva)
              == LDR_TLS_CB_BAD);
    MT_EXPECT(ldr_tls_cb_table_rva(PREF_BASE + 0x2000, PREF_BASE, 0, &rva)
              == LDR_TLS_CB_BAD);
    MT_EXPECT(ldr_tls_cb_table_rva(PREF_BASE + 0x2000, 0xFFFFFFFFFFFFFFF8ULL, IMG_SIZE, &rva)
              == LDR_TLS_CB_BAD);
}

/* Entries are read in order and the zero entry ends the table. The entry
 * after the terminator is never reached. */
static void tls_callbacks_read_until_zero(void)
{
    unsigned char image[IMG_SIZE];
    memset(image, MARK, sizeof(image));
    const unsigned long long cb0 = PREF_BASE + 0x1000;
    const unsigned long long cb1 = PREF_BASE + 0x1100;
    memcpy(image + 0x2010, &cb0, 8);
    memcpy(image + 0x2018, &cb1, 8);
    memset(image + 0x2020, 0, 8);
    memcpy(image + 0x2028, &cb0, 8);

    unsigned long long v = 0;
    MT_EXPECT(ldr_tls_cb_entry(image, IMG_SIZE, 0x2010, 0, &v) == 1);
    MT_EXPECT(v == cb0);
    MT_EXPECT(ldr_tls_cb_entry(image, IMG_SIZE, 0x2010, 1, &v) == 1);
    MT_EXPECT(v == cb1);
    MT_EXPECT(ldr_tls_cb_entry(image, IMG_SIZE, 0x2010, 2, &v) == 0);
    MT_EXPECT(v == 0);
}

/* A table with no terminator stops at the end of the image. The last entry
 * ends exactly at image_size and nothing past it is read. */
static void tls_callbacks_stop_at_image_end(void)
{
    unsigned char *image = malloc(IMG_SIZE);
    MT_ASSERT(image != NULL);
    memset(image, MARK, IMG_SIZE);
    const unsigned long long cb0 = PREF_BASE + 0x1000;
    const unsigned long long cb1 = PREF_BASE + 0x1008;
    memcpy(image + 0x2FF0, &cb0, 8);
    memcpy(image + 0x2FF8, &cb1, 8);

    unsigned long long v = 0;
    MT_EXPECT(ldr_tls_cb_entry(image, IMG_SIZE, 0x2FF0, 0, &v) == 1);
    MT_EXPECT(v == cb0);
    MT_EXPECT(ldr_tls_cb_entry(image, IMG_SIZE, 0x2FF0, 1, &v) == 1);
    MT_EXPECT(v == cb1);
    MT_EXPECT(ldr_tls_cb_entry(image, IMG_SIZE, 0x2FF0, 2, &v) == 0);
    free(image);
}

/* An index whose entry lies outside the image reads nothing. */
static void tls_callbacks_out_of_range_reads_nothing(void)
{
    unsigned char image[IMG_SIZE];
    memset(image, MARK, sizeof(image));
    unsigned long long v = 0x5555;
    MT_EXPECT(ldr_tls_cb_entry(image, IMG_SIZE, 0x2FFC, 0, &v) == 0);
    MT_EXPECT(v == 0x5555);
    MT_EXPECT(ldr_tls_cb_entry(image, IMG_SIZE, 0xFFFFFFF0ULL, 0, &v) == 0);
    MT_EXPECT(v == 0x5555);
}

/* Strict binding: an import whose DLL is missing fails with
 * STATUS_DLL_NOT_FOUND, a found DLL that lacks the name fails with
 * STATUS_ENTRYPOINT_NOT_FOUND, and only a fully bound import succeeds. */
static void unbound_import_status_codes(void)
{
    MT_EXPECT_EQ(ldr_unbound_import_status(0, 0), 0xC0000135UL);
    MT_EXPECT_EQ(ldr_unbound_import_status(0, 1), 0xC0000135UL);
    MT_EXPECT_EQ(ldr_unbound_import_status(1, 0), 0xC0000139UL);
    MT_EXPECT_EQ(ldr_unbound_import_status(1, 1), 0UL);
}

/* Synthetic import table: an IAT at RVA 0x2000 with three slots. Slot 0 still
 * holds the hint/name RVA of an import the binder could not resolve. Slot 1
 * holds a bound VA (preferred base + RVA). Slot 2 is zero, the terminator. The
 * raw-RVA check must flag slot 0 only. */
static void synthetic_iat_detects_unbound_slot(void)
{
    unsigned char image[IMG_SIZE];
    memset(image, MARK, sizeof(image));
    const unsigned long long name_rva = 0x2100;
    const unsigned long long bound = PREF_BASE + 0x1010;

    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, IDATA_RVA, 0, name_rva) == 1);
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, IDATA_RVA, 1, bound) == 1);
    MT_EXPECT(ldr_iat_write(image, IMG_SIZE, IDATA_RVA, 2, 0) == 1);

    MT_EXPECT(ldr_iat_slot_is_raw_rva(read_u64(image + IDATA_RVA), IMG_SIZE) == 1);
    MT_EXPECT(ldr_iat_slot_is_raw_rva(read_u64(image + IDATA_RVA + 8), IMG_SIZE) == 0);
    MT_EXPECT(ldr_iat_slot_is_raw_rva(read_u64(image + IDATA_RVA + 16), IMG_SIZE) == 0);

    /* A zero slot is the terminator, not an unbound import. */
    MT_EXPECT(ldr_iat_slot_is_raw_rva(0, IMG_SIZE) == 0);
    /* A slot holding exactly the image size is a VA, not an RVA. */
    MT_EXPECT(ldr_iat_slot_is_raw_rva(IMG_SIZE, IMG_SIZE) == 0);
}

static int apiset_is(const char *name, const char *want)
{
    char out[64];
    memset(out, 'x', sizeof(out));
    if (!ldr_apiset_host(name, out, sizeof(out)))
        return want == NULL;
    return want && strcmp(out, want) == 0;
}

/* API set names resolve by family prefix, with or without .dll and in any case. */
static void apiset_families(void)
{
    MT_EXPECT(apiset_is("api-ms-win-crt-stdio-l1-1-0.dll", "ucrtbase.dll"));
    MT_EXPECT(apiset_is("API-MS-WIN-CRT-RUNTIME-L1-1-0.DLL", "ucrtbase.dll"));
    MT_EXPECT(apiset_is("api-ms-win-crt-math-l1-1-0", "ucrtbase.dll"));
    MT_EXPECT(apiset_is("api-ms-win-core-synch-l1-2-0.dll", "kernel32.dll"));
    MT_EXPECT(apiset_is("api-ms-win-core-heap-l1-1-0.dll", "kernel32.dll"));
    MT_EXPECT(apiset_is("api-ms-win-core-processthreads-l1-1-2.dll", "kernel32.dll"));
    MT_EXPECT(apiset_is("api-ms-win-core-rtlsupport-l1-2-0.dll", "ntdll.dll"));
    MT_EXPECT(apiset_is("api-ms-win-core-registry-l1-1-0.dll", "advapi32.dll"));
    MT_EXPECT(apiset_is("api-ms-win-security-base-l1-1-0.dll", "advapi32.dll"));
    MT_EXPECT(apiset_is("api-ms-win-eventing-provider-l1-1-0.dll", "advapi32.dll"));
    MT_EXPECT(apiset_is("ext-ms-win-ntuser-window-l1-1-0.dll", "user32.dll"));
    MT_EXPECT(apiset_is("ext-ms-win-gdi-dc-l1-1-0.dll", "gdi32.dll"));
    MT_EXPECT(apiset_is("api-ms-win-gdi-dpiinfo-l1-1-0.dll", "gdi32.dll"));
}

/* Names that are not API sets are left to the normal search, including near misses. */
static void apiset_non_sets(void)
{
    MT_EXPECT(apiset_is("kernel32.dll", NULL));
    MT_EXPECT(apiset_is("ucrtbase.dll", NULL));
    MT_EXPECT(apiset_is("api-ms-win-crt", NULL));        /* the prefix needs its trailing dash */
    MT_EXPECT(apiset_is("api-ms-win-core", NULL));
    MT_EXPECT(apiset_is("xapi-ms-win-core-synch-l1-1-0.dll", NULL));
    MT_EXPECT(apiset_is("api-ms-win-unknown-l1-1-0.dll", NULL));
    MT_EXPECT(apiset_is("", NULL));
}

static void apiset_buffer_rules(void)
{
    char tiny[8], exact[13];
    MT_EXPECT(!ldr_apiset_host("api-ms-win-crt-stdio-l1-1-0.dll", tiny, sizeof(tiny))); /* "ucrtbase.dll" needs 13 */
    MT_EXPECT(ldr_apiset_host("api-ms-win-crt-stdio-l1-1-0.dll", exact, sizeof(exact)));
    MT_EXPECT(strcmp(exact, "ucrtbase.dll") == 0);
    MT_EXPECT(!ldr_apiset_host(NULL, exact, sizeof(exact)));
    MT_EXPECT(!ldr_apiset_host("api-ms-win-crt-x", NULL, 4));
}

static const mt_case_t cases[] = {
    { "apiset_families", apiset_families },
    { "apiset_non_sets", apiset_non_sets },
    { "apiset_buffer_rules", apiset_buffer_rules },
    { "unbound_import_status_codes", unbound_import_status_codes },
    { "synthetic_iat_detects_unbound_slot", synthetic_iat_detects_unbound_slot },
    { "iat_base_is_rva_with_ilt_fallback", iat_base_is_rva_with_ilt_fallback },
    { "iat_write_lands_at_rva_not_file_offset", iat_write_lands_at_rva_not_file_offset },
    { "iat_write_exact_fit_at_end", iat_write_exact_fit_at_end },
    { "iat_write_rejected_writes_nothing", iat_write_rejected_writes_nothing },
    { "iat_write_no_wrap", iat_write_no_wrap },
    { "tls_table_va_zero_is_none", tls_table_va_zero_is_none },
    { "tls_table_below_base_is_bad", tls_table_below_base_is_bad },
    { "tls_table_rva_is_va_minus_original_base", tls_table_rva_is_va_minus_original_base },
    { "tls_table_no_wrap", tls_table_no_wrap },
    { "tls_callbacks_read_until_zero", tls_callbacks_read_until_zero },
    { "tls_callbacks_stop_at_image_end", tls_callbacks_stop_at_image_end },
    { "tls_callbacks_out_of_range_reads_nothing", tls_callbacks_out_of_range_reads_nothing },
};

int main(void)
{
    return MT_RUN("ldr_pure", cases);
}
