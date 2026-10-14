/*
 * test_advapi_pure.c - host tests for the pure mapping in src/ntdll/advapi_pure.h,
 * used by the advapi32 shim (src/ntdll/advapi32.c).
 */
#include "minitest.h"
#include "advapi_pure.h"
#include <string.h>

/* Only HKEY_LOCAL_MACHINE and HKEY_CURRENT_USER map to a kernel root. Other
 * predefined keys have no mapping, so the shim fails them instead of guessing. */
static void predefined_roots_map_only_supported_keys(void)
{
    MT_EXPECT(strcmp(adv_predef_root_path(ADV_HKEY_LOCAL_MACHINE), "\\Registry\\Machine") == 0);
    MT_EXPECT(strcmp(adv_predef_root_path(ADV_HKEY_CURRENT_USER), "\\Registry\\User") == 0);
    MT_EXPECT(adv_predef_root_path(ADV_HKEY_CLASSES_ROOT) == NULL);
    MT_EXPECT(adv_predef_root_path(ADV_HKEY_USERS) == NULL);
    MT_EXPECT(adv_predef_root_path(0x1234UL) == NULL);
}

static void predefined_range_is_0x80000000_to_0x80000003(void)
{
    MT_EXPECT(adv_is_predef_key(0x80000000UL) == 1);
    MT_EXPECT(adv_is_predef_key(0x80000003UL) == 1);
    MT_EXPECT(adv_is_predef_key(0x80000004UL) == 0);
    MT_EXPECT(adv_is_predef_key(0x7FFFFFFFUL) == 0);
    MT_EXPECT(adv_is_predef_key(0UL) == 0);
}

/* The WOW64 view flags are removed; the rest of the access mask is kept. */
static void wow64_flags_are_stripped_from_access(void)
{
    MT_EXPECT_EQ(adv_key_access(0x20019UL), 0x20019UL);
    MT_EXPECT_EQ(adv_key_access(0x20019UL | ADV_KEY_WOW64_64KEY), 0x20019UL);
    MT_EXPECT_EQ(adv_key_access(0x20019UL | ADV_KEY_WOW64_32KEY), 0x20019UL);
    MT_EXPECT_EQ(adv_key_access(0xF003FUL | ADV_KEY_WOW64_32KEY), 0xF003FUL);
}

/* No buffer: success, and the length is reported. Buffer too small: MORE_DATA,
 * length reported. Buffer big enough: success. The output is always set. */
static void query_plan_reports_size_and_more_data(void)
{
    unsigned long cb = 0xDEAD;
    MT_EXPECT_EQ(adv_query_plan(16, 0, 0, &cb), ADV_ERROR_SUCCESS);
    MT_EXPECT_EQ(cb, 16UL);

    cb = 0;
    MT_EXPECT_EQ(adv_query_plan(16, 1, 8, &cb), ADV_ERROR_MORE_DATA);
    MT_EXPECT_EQ(cb, 16UL);

    cb = 0;
    MT_EXPECT_EQ(adv_query_plan(16, 1, 16, &cb), ADV_ERROR_SUCCESS);
    MT_EXPECT_EQ(cb, 16UL);

    cb = 0;
    MT_EXPECT_EQ(adv_query_plan(0, 1, 0, &cb), ADV_ERROR_SUCCESS);
    MT_EXPECT_EQ(cb, 0UL);
}

/* KEY_VALUE_PARTIAL_INFORMATION: the data begins at offset 12 and must fit in
 * the record. A record shorter than its header never fits. */
static void partial_info_data_must_fit_record(void)
{
    MT_EXPECT_EQ(ADV_PARTIAL_DATA_OFFSET, 12UL);
    MT_EXPECT(adv_partial_data_fits(12, 0) == 1);
    MT_EXPECT(adv_partial_data_fits(20, 8) == 1);
    MT_EXPECT(adv_partial_data_fits(20, 9) == 0);
    MT_EXPECT(adv_partial_data_fits(11, 0) == 0);
    MT_EXPECT(adv_partial_data_fits(0, 0) == 0);
}

/* ASCII names become UTF-16 code units with a NUL. Names that do not fit in
 * cap, or that hold a non-ASCII byte, are rejected. */
static void ascii_to_utf16_bounds_and_ascii_only(void)
{
    unsigned short buf[8];
    MT_EXPECT_EQ(adv_ascii_to_utf16(buf, 8, "Abc"), 3L);
    MT_EXPECT(buf[0] == 'A' && buf[1] == 'b' && buf[2] == 'c' && buf[3] == 0);

    MT_EXPECT_EQ(adv_ascii_to_utf16(buf, 8, ""), 0L);
    MT_EXPECT(buf[0] == 0);

    /* 7 characters fit with the NUL in cap 8; 8 characters do not. */
    MT_EXPECT_EQ(adv_ascii_to_utf16(buf, 8, "1234567"), 7L);
    MT_EXPECT_EQ(adv_ascii_to_utf16(buf, 8, "12345678"), -1L);

    MT_EXPECT_EQ(adv_ascii_to_utf16(buf, 8, "\xC3\xA9"), -1L);
    MT_EXPECT_EQ(adv_ascii_to_utf16(buf, 0, "A"), -1L);
    MT_EXPECT_EQ(adv_ascii_to_utf16(buf, 8, NULL), 0L);
}

/* UNICODE_STRING.Length is a USHORT in bytes: the byte count must not
 * exceed 0xFFFE, and a too-long count is refused. */
static void utf16_bytes_respect_usshort_limit(void)
{
    MT_EXPECT_EQ(adv_utf16_bytes(0), 0UL);
    MT_EXPECT_EQ(adv_utf16_bytes(3), 6UL);
    MT_EXPECT_EQ(adv_utf16_bytes(0x7FFF), 0xFFFEUL);
    MT_EXPECT_EQ(adv_utf16_bytes(0x8000), 0UL);
}

static const mt_case_t cases[] = {
    MT_CASE(predefined_roots_map_only_supported_keys),
    MT_CASE(predefined_range_is_0x80000000_to_0x80000003),
    MT_CASE(wow64_flags_are_stripped_from_access),
    MT_CASE(query_plan_reports_size_and_more_data),
    MT_CASE(partial_info_data_must_fit_record),
    MT_CASE(ascii_to_utf16_bounds_and_ascii_only),
    MT_CASE(utf16_bytes_respect_usshort_limit),
};

int main(void)
{
    return MT_RUN("advapi_pure", cases);
}
