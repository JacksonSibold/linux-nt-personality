/*
 * test_conv.c - tests for the integer conversions in src/crt/ucrt_conv.h against glibc's strtoll and
 * strtoull on random and hand-picked strings, the 32-bit long forms, and the itoa family.
 */
#include "minitest.h"
#include "ucrt_conv.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0xB5297A4D3C6E1F27ull;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static void rand_numstr(char *out, size_t max)
{
    static const char *const alpha = "0123456789abcdefxXzZ+- \t_";
    size_t n = rnd() % max, i, a = strlen(alpha);
    for (i = 0; i < n; ++i)
        out[i] = alpha[rnd() % a];
    out[n] = 0;
}

static void signed64_matches_glibc(void)
{
    char s[40];
    int i;
    for (i = 0; i < 60000; ++i) {
        int base = (int)(rnd() % 38);
        const char *e1, *e2 = NULL;
        char *g_end;
        int64_t a, b;
        int ucrt_err, g_err;
        if (base == 1 || base == 37)
            base = 10;
        rand_numstr(s, 30);
        g_ucrt_state.errno_value = 0;
        errno = 0;
        a = ucrt_strtoi64(s, &e1, base);
        ucrt_err = ucrt_get_errno();
        b = strtoll(s, &g_end, base);
        g_err = errno;
        e2 = g_end;
        if (a != b || e1 != e2 || (ucrt_err == UCRT_ERANGE) != (g_err == ERANGE)) {
            printf("    strtoll(\"%s\", %d): got %lld end %td err %d, want %lld end %td err %d\n", s, base, (long long)a,
                   e1 - s, ucrt_err, (long long)b, e2 - s, g_err);
            MT_EXPECT(0);
            return;
        }
    }
}

static void unsigned64_matches_glibc(void)
{
    char s[40];
    int i;
    for (i = 0; i < 60000; ++i) {
        int base = (int)(rnd() % 38);
        const char *e1;
        char *g_end;
        uint64_t a, b;
        if (base == 1 || base == 37)
            base = 16;
        rand_numstr(s, 30);
        errno = 0;
        g_ucrt_state.errno_value = 0;
        a = ucrt_strtoui64(s, &e1, base);
        b = strtoull(s, &g_end, base);
        if (a != b || e1 != g_end) {
            printf("    strtoull(\"%s\", %d): got %llu end %td, want %llu end %td\n", s, base, (unsigned long long)a, e1 - s,
                   (unsigned long long)b, g_end - s);
            MT_EXPECT(0);
            return;
        }
    }
}

static void named_cases(void)
{
    const char *e;
    g_ucrt_state.errno_value = 0;
    MT_EXPECT_EQ(ucrt_strtoi64("  -123abc", &e, 10), -123);
    MT_EXPECT_EQ(*e, 'a');
    MT_EXPECT_EQ(ucrt_strtoi64("0x1F", &e, 0), 31);
    MT_EXPECT_EQ(ucrt_strtoi64("0x1F", &e, 16), 31);
    MT_EXPECT_EQ(ucrt_strtoi64("017", &e, 0), 15);
    MT_EXPECT_EQ(ucrt_strtoi64("0x", &e, 16), 0); /* a lone prefix reads the zero */
    MT_EXPECT_EQ(*e, 'x');
    MT_EXPECT_EQ(ucrt_strtoi64("  ", &e, 10), 0);
    MT_EXPECT_EQ(ucrt_strtoi64("zz", &e, 36), 35 * 36 + 35);
    /* No digits: the end pointer is the start of the input, including leading white space. */
    {
        const char *in = "  +x";
        ucrt_strtoi64(in, &e, 10);
        MT_EXPECT(e == in);
    }
    /* Overflow clamps and sets ERANGE. */
    g_ucrt_state.errno_value = 0;
    MT_EXPECT(ucrt_strtoi64("9223372036854775808", &e, 10) == INT64_MAX);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    g_ucrt_state.errno_value = 0;
    MT_EXPECT(ucrt_strtoi64("-9223372036854775809", &e, 10) == INT64_MIN);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    g_ucrt_state.errno_value = 0;
    MT_EXPECT(ucrt_strtoi64("-9223372036854775808", &e, 10) == INT64_MIN);
    MT_EXPECT_EQ(ucrt_get_errno(), 0);
    /* A bad base is an invalid parameter. */
    g_ucrt_state.errno_value = 0;
    MT_EXPECT_EQ(ucrt_strtoi64("12", &e, 1), 0);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EINVAL);
    MT_EXPECT_EQ(ucrt_strtoi64("12", &e, 37), 0);
    MT_EXPECT_EQ(ucrt_strtoi64("12", &e, -2), 0);
}

static void long_is_32_bits(void)
{
    const char *e;
    g_ucrt_state.errno_value = 0;
    MT_EXPECT_EQ(ucrt_strtol("2147483647", &e, 10), 2147483647);
    MT_EXPECT_EQ(ucrt_get_errno(), 0);
    MT_EXPECT_EQ(ucrt_strtol("2147483648", &e, 10), 2147483647); /* clamps to LONG_MAX of the 32-bit long */
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    g_ucrt_state.errno_value = 0;
    MT_EXPECT_EQ(ucrt_strtol("-2147483648", &e, 10), INT32_MIN);
    MT_EXPECT_EQ(ucrt_get_errno(), 0);
    MT_EXPECT_EQ(ucrt_strtol("-2147483649", &e, 10), INT32_MIN);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    g_ucrt_state.errno_value = 0;
    MT_EXPECT_EQ(ucrt_strtoul("4294967295", &e, 10), 4294967295u);
    MT_EXPECT_EQ(ucrt_get_errno(), 0);
    MT_EXPECT_EQ(ucrt_strtoul("4294967296", &e, 10), 4294967295u);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    g_ucrt_state.errno_value = 0;
    MT_EXPECT_EQ(ucrt_strtoul("-1", &e, 10), 4294967295u); /* negation modulo 2^32 */
    MT_EXPECT_EQ(ucrt_strtoul("-5", &e, 10), 4294967291u);
    MT_EXPECT_EQ(ucrt_get_errno(), 0);
    /* atoi follows strtol. */
    MT_EXPECT_EQ(ucrt_atoi("  42xyz"), 42);
    MT_EXPECT_EQ(ucrt_atoi("-7"), -7);
    MT_EXPECT_EQ(ucrt_atoi("99999999999"), 2147483647);
    MT_EXPECT_EQ(ucrt_atoi("abc"), 0);
    MT_EXPECT(ucrt_atoi64("99999999999") == 99999999999ll);
}

static void wide_forms(void)
{
    static const ucrt_wchar a[] = {' ', '-', '1', '2', '3', 'x', 0};
    static const ucrt_wchar b[] = {'0', 'x', 'f', 'F', 0};
    static const ucrt_wchar c[] = {0x3000, '7', 0}; /* an ideographic space is white space for wide strings */
    static const ucrt_wchar d[] = {0x0663, '5', 0}; /* an Arabic-Indic digit is not a digit */
    const ucrt_wchar *e;
    MT_EXPECT_EQ(ucrt_wcstoi64(a, &e, 10), -123);
    MT_EXPECT_EQ(*e, 'x');
    MT_EXPECT_EQ(ucrt_wcstoi64(b, &e, 0), 255);
    MT_EXPECT_EQ(ucrt_wcstoi64(c, &e, 10), 7);
    MT_EXPECT_EQ(ucrt_wcstoi64(d, &e, 10), 0);
    MT_EXPECT(e == d);
    MT_EXPECT_EQ(ucrt_wcsatoi(a), -123);
    MT_EXPECT_EQ(ucrt_wcstol(b, &e, 16), 255);
}

static void itoa_family(void)
{
    char buf[80];
    MT_EXPECT_EQ(ucrt_strutoa_raw(0, buf, 10, 0), 1);
    MT_EXPECT(strcmp(buf, "0") == 0);
    MT_EXPECT_EQ(ucrt_strutoa_raw(255, buf, 16, 0), 2);
    MT_EXPECT(strcmp(buf, "ff") == 0);
    ucrt_strutoa_raw(5, buf, 2, 0);
    MT_EXPECT(strcmp(buf, "101") == 0);
    ucrt_strutoa_raw(35, buf, 36, 0);
    MT_EXPECT(strcmp(buf, "z") == 0);
    ucrt_strutoa_raw(UINT64_MAX, buf, 10, 0);
    MT_EXPECT(strcmp(buf, "18446744073709551615") == 0);
    ucrt_strutoa_raw(UINT64_MAX, buf, 2, 0);
    MT_EXPECT_EQ(strlen(buf), 64);
    MT_EXPECT_EQ(ucrt_strutoa_raw(1, buf, 1, 0), 0); /* bad radix */
    MT_EXPECT_EQ(ucrt_strutoa_raw(1, buf, 37, 0), 0);
    /* _i64toa_s: negative decimal gets a sign, other radixes the unsigned pattern. */
    g_ucrt_state.errno_value = 0;
    MT_EXPECT_EQ(ucrt_stri64toa_s(-255, buf, sizeof(buf), 10, 0), 0);
    MT_EXPECT(strcmp(buf, "-255") == 0);
    MT_EXPECT_EQ(ucrt_stri64toa_s(-1, buf, sizeof(buf), 16, 0), 0);
    MT_EXPECT(strcmp(buf, "ffffffffffffffff") == 0);
    MT_EXPECT_EQ(ucrt_stri64toa_s(INT64_MIN, buf, sizeof(buf), 10, 0), 0);
    MT_EXPECT(strcmp(buf, "-9223372036854775808") == 0);
    MT_EXPECT_EQ(ucrt_stri64toa_s(-1, buf, sizeof(buf), 10, 1), 0); /* an unsigned value never gets a sign */
    MT_EXPECT(strcmp(buf, "18446744073709551615") == 0);
    /* Too small a buffer: ERANGE and an empty string. */
    MT_EXPECT_EQ(ucrt_stri64toa_s(12345, buf, 5, 10, 0), UCRT_ERANGE);
    MT_EXPECT_EQ(buf[0], 0);
    MT_EXPECT_EQ(ucrt_stri64toa_s(1234, buf, 5, 10, 0), 0);
    MT_EXPECT(strcmp(buf, "1234") == 0);
    MT_EXPECT_EQ(ucrt_stri64toa_s(1, NULL, 5, 10, 0), UCRT_EINVAL);
    MT_EXPECT_EQ(ucrt_stri64toa_s(1, buf, 0, 10, 0), UCRT_EINVAL);
    MT_EXPECT_EQ(ucrt_stri64toa_s(1, buf, 8, 40, 0), UCRT_EINVAL);
    /* A wide result. */
    {
        ucrt_wchar w[20];
        MT_EXPECT_EQ(ucrt_wcsi64toa_s(-4096, w, 20, 16, 0), 0);
        MT_EXPECT(w[0] == 'f' && w[12] == 'f' && w[13] == '0' && w[15] == '0' && w[16] == 0);
        MT_EXPECT_EQ(ucrt_wcsi64toa_s(-4096, w, 20, 10, 0), 0);
        MT_EXPECT(w[0] == '-' && w[1] == '4' && w[4] == '6' && w[5] == 0);
    }
}

static const mt_case_t cases[] = {
    MT_CASE(signed64_matches_glibc),
    MT_CASE(unsigned64_matches_glibc),
    MT_CASE(named_cases),
    MT_CASE(long_is_32_bits),
    MT_CASE(wide_forms),
    MT_CASE(itoa_family),
};

int main(void)
{
    return MT_RUN("crt_conv", cases);
}
