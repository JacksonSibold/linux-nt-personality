/*
 * test_adv_reg_pure.c - tests for src/ntdll/adv_reg_pure.h: predefined keys, the RegGetValue type filter, string termination and
 * the buffer fit rules.
 */
#include "minitest.h"
#include "adv_reg_pure.h"

#include <string.h>

static void predefined_keys(void)
{
    MT_EXPECT_EQ(adv_predef_id(0xFFFFFFFF80000002ull), ADV_ID_LOCAL_MACHINE); /* the sign-extended handle */
    MT_EXPECT_EQ(adv_predef_id(0x80000001ull), ADV_ID_CURRENT_USER);          /* after a trip through a DWORD */
    MT_EXPECT_EQ(adv_predef_id(0x1234ull), 0);                                /* an ordinary handle */
    MT_EXPECT_EQ(adv_predef_id(0xFFFFFFFF7FFFFFFFull), 0);
    MT_EXPECT_EQ(adv_predef_id(0x0000000180000002ull), 0);
    MT_EXPECT(strcmp(adv_predef_path(ADV_ID_LOCAL_MACHINE), "\\Registry\\Machine") == 0);
    MT_EXPECT(strcmp(adv_predef_path(ADV_ID_CURRENT_USER), "\\Registry\\User") == 0);
    MT_EXPECT(strcmp(adv_predef_path(ADV_ID_CLASSES_ROOT), "\\Registry\\Machine\\Software\\Classes") == 0);
    MT_EXPECT(adv_predef_path(ADV_ID_PERFORMANCE_DATA) == NULL);
    MT_EXPECT_EQ(adv_check_subkey((const uint16_t *)u"\\Software"), ADV_R_BAD_PATHNAME);
    MT_EXPECT_EQ(adv_check_subkey((const uint16_t *)u"Software"), ADV_R_SUCCESS);
    MT_EXPECT_EQ(adv_check_subkey(NULL), ADV_R_SUCCESS);
}

static void type_filter(void)
{
    MT_EXPECT(adv_rrf_allows(ADV_REG_DWORD, ADV_RRF_RT_REG_DWORD));
    MT_EXPECT(!adv_rrf_allows(ADV_REG_DWORD, ADV_RRF_RT_REG_SZ));
    MT_EXPECT(adv_rrf_allows(ADV_REG_SZ, 0xFFFF)); /* RRF_RT_ANY */
    MT_EXPECT(adv_rrf_allows(ADV_REG_QWORD, ADV_RRF_RT_REG_QWORD | ADV_RRF_RT_REG_DWORD));
    MT_EXPECT(adv_rrf_allows(ADV_REG_BINARY, ADV_RRF_RT_REG_BINARY));
    MT_EXPECT(adv_rrf_allows(ADV_REG_MULTI_SZ, ADV_RRF_RT_REG_MULTI_SZ));
    MT_EXPECT(!adv_rrf_allows(ADV_REG_MULTI_SZ, ADV_RRF_RT_REG_SZ));
    MT_EXPECT(adv_rrf_allows(ADV_REG_NONE, ADV_RRF_RT_REG_NONE));
    MT_EXPECT(!adv_rrf_allows(5, 0xFFFF)); /* REG_DWORD_BIG_ENDIAN has no bit */
    /* an expandable string is expanded for a string restriction unless that is switched off */
    MT_EXPECT(adv_rrf_allows(ADV_REG_EXPAND_SZ, ADV_RRF_RT_REG_SZ));
    MT_EXPECT(!adv_rrf_allows(ADV_REG_EXPAND_SZ, ADV_RRF_RT_REG_SZ | ADV_RRF_NOEXPAND));
    MT_EXPECT(adv_rrf_allows(ADV_REG_EXPAND_SZ, ADV_RRF_RT_REG_EXPAND_SZ | ADV_RRF_NOEXPAND));
    MT_EXPECT(adv_is_string_type(ADV_REG_MULTI_SZ) && !adv_is_string_type(ADV_REG_DWORD));
}

static void terminators(void)
{
    static const uint8_t ab[] = {'a', 0, 'b', 0};
    static const uint8_t ab0[] = {'a', 0, 'b', 0, 0, 0};
    static const uint8_t multi[] = {'x', 0, 0, 0, 'y', 0, 0, 0};
    static const uint8_t multi_done[] = {'x', 0, 0, 0, 'y', 0, 0, 0, 0, 0};
    MT_EXPECT_EQ((long)adv_terminator_bytes(ADV_REG_SZ, ab, sizeof ab), 2L);
    MT_EXPECT_EQ((long)adv_terminator_bytes(ADV_REG_SZ, ab0, sizeof ab0), 0L);
    MT_EXPECT_EQ((long)adv_terminator_bytes(ADV_REG_SZ, ab, 0), 2L);
    MT_EXPECT_EQ((long)adv_terminator_bytes(ADV_REG_EXPAND_SZ, ab, sizeof ab), 2L);
    MT_EXPECT_EQ((long)adv_terminator_bytes(ADV_REG_MULTI_SZ, multi, sizeof multi), 2L); /* one more NUL character finishes it */
    MT_EXPECT_EQ((long)adv_terminator_bytes(ADV_REG_MULTI_SZ, multi_done, sizeof multi_done), 0L);
    MT_EXPECT_EQ((long)adv_terminator_bytes(ADV_REG_MULTI_SZ, ab, sizeof ab), 4L);
}

static void buffer_fit(void)
{
    MT_EXPECT(adv_name_fits(3, 4));
    MT_EXPECT(!adv_name_fits(4, 4)); /* no room for the NUL */
    MT_EXPECT(!adv_name_fits(0, 0));
    MT_EXPECT(adv_name_fits(0, 1));
    MT_EXPECT(adv_value_is_text(ADV_REG_SZ) && !adv_value_is_text(ADV_REG_BINARY));
}

static const mt_case_t cases[] = {
    MT_CASE(predefined_keys), MT_CASE(type_filter), MT_CASE(terminators), MT_CASE(buffer_fit),
};

int main(void)
{
    return MT_RUN("adv_reg_pure", cases);
}
