/*
 * test_k32_nls_pure.c - host tests for src/ntdll/k32_nls_pure.h: locale names and information, comparison, case mapping,
 * character types and number formatting.
 */
#include "minitest.h"
#include "k32_nls_pure.h"

#include <string.h>

static size_t wide(const char *s, WCHAR *w)
{
    size_t n = 0;
    while (s[n]) {
        w[n] = (WCHAR)(unsigned char)s[n];
        ++n;
    }
    w[n] = 0;
    return n;
}

static int cmp(DWORD flags, const char *a, const char *b)
{
    WCHAR x[128], y[128];
    int nx = (int)wide(a, x), ny = (int)wide(b, y);
    return k32n_compare(flags, x, nx, y, ny);
}

static void locale_names(void)
{
    WCHAR w[16], n[16];
    wide("en-us", w);
    MT_EXPECT_EQ((long)k32n_name_to_lcid(w), 0x409);
    wide("EN", w);
    MT_EXPECT_EQ((long)k32n_name_to_lcid(w), 0x409);
    MT_EXPECT_EQ((long)k32n_name_to_lcid(NULL), 0x409);
    wide("", w);
    MT_EXPECT_EQ((long)k32n_name_to_lcid(w), 0x7F);
    wide("de-DE", w);
    MT_EXPECT_EQ((long)k32n_name_to_lcid(w), 0);
    MT_EXPECT_EQ(k32n_lcid_name(0x409, n, 16), 6);
    MT_EXPECT(n[0] == 'e' && n[2] == '-' && n[4] == 'S');
    MT_EXPECT_EQ(k32n_lcid_name(0x409, n, 3), -1);
    MT_EXPECT_EQ(k32n_lcid_name(0x7F, n, 16), 1);
    MT_EXPECT_EQ(k32n_lcid_name(0x407, n, 16), 0);
}

static void locale_information(void)
{
    MT_EXPECT(strcmp(k32n_locale_string(0x0E), ".") == 0);
    MT_EXPECT(strcmp(k32n_locale_string(0x0F), ",") == 0);
    MT_EXPECT(strcmp(k32n_locale_string(0x5C), "en-US") == 0);
    MT_EXPECT(strcmp(k32n_locale_string(0x2A), "Monday") == 0 && strcmp(k32n_locale_string(0x30), "Sunday") == 0);
    MT_EXPECT(strcmp(k32n_locale_string(0x38), "January") == 0 && strcmp(k32n_locale_string(0x43), "December") == 0);
    MT_EXPECT(strcmp(k32n_locale_string(0x44), "Jan") == 0 && strcmp(k32n_locale_string(0x4F), "Dec") == 0);
    MT_EXPECT(strcmp(k32n_locale_string(0x1F), "M/d/yyyy") == 0);
    MT_EXPECT(k32n_locale_string(0x7777) == NULL);
    MT_EXPECT(k32n_locale_is_number(0x11) && k32n_locale_is_number(0x1004) && !k32n_locale_is_number(0x0E));
    MT_EXPECT_EQ((long)k32n_locale_number(0x01, k32n_locale_string(0x01)), 0x409); /* the language ID is hexadecimal */
    MT_EXPECT_EQ((long)k32n_locale_number(0x1004, k32n_locale_string(0x1004)), 1252);
    MT_EXPECT_EQ((long)k32n_locale_number(0x11, k32n_locale_string(0x11)), 2);
}

static void comparison(void)
{
    MT_EXPECT_EQ(cmp(0, "abc", "abc"), 2);
    MT_EXPECT_EQ(cmp(0, "abc", "abd"), 1);
    MT_EXPECT_EQ(cmp(0, "b", "a"), 3);
    MT_EXPECT_EQ(cmp(0, "abc", "ab"), 3);
    MT_EXPECT_EQ(cmp(0, "ab", "abc"), 1);
    /* the default sort: a < A < b, not ASCII order */
    MT_EXPECT_EQ(cmp(0, "a", "A"), 1);
    MT_EXPECT_EQ(cmp(0, "A", "b"), 1);
    MT_EXPECT_EQ(cmp(0, "B", "a"), 3);
    MT_EXPECT_EQ(cmp(K32N_NORM_IGNORECASE, "Hello", "hELLO"), 2);
    MT_EXPECT_EQ(cmp(K32N_NORM_IGNORECASE, "Hello", "hELLP"), 1);
    /* symbols before digits before letters */
    MT_EXPECT_EQ(cmp(0, "_", "0"), 1);
    MT_EXPECT_EQ(cmp(0, "9", "a"), 1);
    MT_EXPECT_EQ(cmp(0, "[", "a"), 1); /* '[' is above 'A' in ASCII but sorts as a symbol */
    MT_EXPECT_EQ(cmp(K32N_NORM_IGNORESYMBOLS, "a-b", "ab"), 2);
    /* a difference past the case difference decides first */
    MT_EXPECT_EQ(cmp(0, "aB", "Ab"), 1);
    MT_EXPECT_EQ(cmp(0, "Ab", "ac"), 1);
    {
        WCHAR a[3] = {'a', 'B', 0}, b[3] = {'A', 'b', 0};
        MT_EXPECT_EQ(k32n_compare_ordinal(a, 2, b, 2, 1), 2);
        MT_EXPECT_EQ(k32n_compare_ordinal(a, 2, b, 2, 0), 3); /* 'a' (0x61) above 'A' (0x41) in code unit order */
        MT_EXPECT_EQ(k32n_compare_ordinal(a, 1, a, 2, 0), 1);
    }
    {
        WCHAR a[3] = {0xE9, 0, 0}, b[3] = {0xC9, 0, 0};
        MT_EXPECT_EQ(k32n_compare(K32N_NORM_IGNORECASE, a, 1, b, 1), 2); /* é == É */
    }
}

static void case_mapping_and_types(void)
{
    WCHAR in[8], out[8];
    int n;
    wide("aBc\xE9", in);
    n = 4;
    MT_EXPECT_EQ(k32n_lcmap(K32N_LCMAP_UPPERCASE, in, n, out, 8), 4);
    MT_EXPECT(out[0] == 'A' && out[1] == 'B' && out[2] == 'C' && out[3] == 0xC9);
    MT_EXPECT_EQ(k32n_lcmap(K32N_LCMAP_LOWERCASE, in, n, out, 8), 4);
    MT_EXPECT(out[0] == 'a' && out[1] == 'b' && out[3] == 0xE9);
    MT_EXPECT_EQ(k32n_lcmap(K32N_LCMAP_UPPERCASE, in, n, NULL, 0), 4);
    MT_EXPECT_EQ(k32n_lcmap(K32N_LCMAP_UPPERCASE, in, n, out, 2), -2);
    MT_EXPECT_EQ(k32n_lcmap(K32N_LCMAP_UPPERCASE | K32N_LCMAP_LOWERCASE, in, n, out, 8), -1);
    MT_EXPECT_EQ(k32n_lcmap(K32N_LCMAP_SORTKEY, in, n, out, 8), -1);
    MT_EXPECT_EQ(k32n_lcmap(0, in, n, out, 8), 4); /* no case flag: a copy */
    /* character types */
    MT_EXPECT_EQ(k32n_ctype1('A') & 0x3FF, 0x0001 | 0x0080 | 0x0100 | 0x0200);
    MT_EXPECT_EQ(k32n_ctype1('a') & 0x3FF, 0x0002 | 0x0080 | 0x0100 | 0x0200);
    MT_EXPECT((k32n_ctype1('5') & 0x0004) && (k32n_ctype1('5') & 0x0080) && !(k32n_ctype1('5') & 0x0100));
    MT_EXPECT((k32n_ctype1(' ') & 0x0008) && (k32n_ctype1(' ') & 0x0040));
    MT_EXPECT((k32n_ctype1('!') & 0x0010) && !(k32n_ctype1('!') & 0x0100));
    MT_EXPECT((k32n_ctype1('\n') & 0x0020));
    MT_EXPECT_EQ(k32n_ctype1(0xD800), 0);
    MT_EXPECT_EQ(k32n_ctype2('5'), 3);
    MT_EXPECT_EQ(k32n_ctype2('a'), 1);
    MT_EXPECT_EQ(k32n_ctype2(' '), 10);
    MT_EXPECT(k32n_ctype3('a') & 0x8000);
    MT_EXPECT(k32n_ctype3('+') & 0x0008);
}

static void number_formatting(void)
{
    char out[64];
    MT_EXPECT_EQ(k32n_format_number("1234567.891", 2, 3, ".", ",", out, 64), 12);
    MT_EXPECT(strcmp(out, "1,234,567.89") == 0);
    MT_EXPECT_EQ(k32n_format_number("1234567.895", 2, 3, ".", ",", out, 64), 12);
    MT_EXPECT(strcmp(out, "1,234,567.90") == 0);
    MT_EXPECT_EQ(k32n_format_number("-1234.5", 2, 3, ".", ",", out, 64), 9);
    MT_EXPECT(strcmp(out, "-1,234.50") == 0);
    MT_EXPECT_EQ(k32n_format_number("0.5", 0, 3, ".", ",", out, 64), 1);
    MT_EXPECT(strcmp(out, "1") == 0); /* rounds half up into the integer part */
    MT_EXPECT_EQ(k32n_format_number("999.999", 2, 3, ".", ",", out, 64), 8);
    MT_EXPECT(strcmp(out, "1,000.00") == 0);
    MT_EXPECT_EQ(k32n_format_number("12", 0, 3, ".", ",", out, 64), 2);
    MT_EXPECT(strcmp(out, "12") == 0);
    MT_EXPECT_EQ(k32n_format_number(".25", 2, 3, ".", ",", out, 64), 4);
    MT_EXPECT(strcmp(out, "0.25") == 0);
    MT_EXPECT_EQ(k32n_format_number("-0.001", 2, 3, ".", ",", out, 64), 4);
    MT_EXPECT(strcmp(out, "0.00") == 0); /* a rounded-away negative is not negative */
    MT_EXPECT_EQ(k32n_format_number("abc", 2, 3, ".", ",", out, 64), -1);
    MT_EXPECT_EQ(k32n_format_number("1234567", 0, 3, ".", ",", out, 5), -1); /* short buffer */
    MT_EXPECT_EQ(k32n_format_number("1234", 0, 0, ".", ",", out, 64), 4); /* no grouping */
    MT_EXPECT(strcmp(out, "1234") == 0);
}

static const mt_case_t cases[] = {
    MT_CASE(locale_names), MT_CASE(locale_information), MT_CASE(comparison), MT_CASE(case_mapping_and_types), MT_CASE(number_formatting),
};

int main(void)
{
    return MT_RUN("k32_nls_pure", cases);
}
