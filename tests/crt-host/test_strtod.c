/*
 * test_strtod.c - tests for src/crt/ucrt_strtod.h against glibc's strtod and strtof: random decimal strings,
 * round trips of printed doubles, midpoints between adjacent doubles (the hard cases for rounding), denormals,
 * overflow, hexadecimal floats, infinity and nan, and the end pointer on malformed input.
 */
#include "minitest.h"
#include "ucrt_strtod.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0xD1B54A32D192ED03ull;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static int same_bits(double a, double b)
{
    uint64_t x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    return x == y;
}

static int same_bitsf(float a, float b)
{
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return x == y;
}

/* One string: the value, the end pointer, and ERANGE agree with glibc. */
static int agree(const char *s)
{
    const char *e1;
    char *e2;
    double a, b;
    int ue, ge;
    g_ucrt_state.errno_value = 0;
    errno = 0;
    a = ucrt_strtod(s, &e1);
    ue = ucrt_get_errno() == UCRT_ERANGE;
    b = strtod(s, &e2);
    ge = errno == ERANGE;
    if (isnan(a) && isnan(b))
        return e1 == e2;
    if (!same_bits(a, b) || e1 != e2) {
        printf("    strtod(\"%s\"): got %a end %td, want %a end %td\n", s, a, e1 - s, b, e2 - s);
        return 0;
    }
    /* glibc also sets ERANGE for inexact denormals, which the runtime does not: compare only overflow and zero. */
    if ((isinf(b) && !isinf(a)) || ((b == 0.0) != (a == 0.0))) {
        printf("    strtod(\"%s\"): value class differs\n", s);
        return 0;
    }
    if (ge && (isinf(b) || b == 0.0) && !ue) { /* an overflow or an underflow to zero must set ERANGE */
        printf("    strtod(\"%s\"): ERANGE missing\n", s);
        return 0;
    }
    return 1;
}

static void random_decimal_strings(void)
{
    char s[160];
    int i;
    for (i = 0; i < 60000; ++i) {
        int nint = (int)(rnd64() % 20), nfrac = (int)(rnd64() % 25), k, e;
        char *p = s;
        if (rnd64() & 1)
            *p++ = (rnd64() & 1) ? '-' : '+';
        for (k = 0; k < nint; ++k)
            *p++ = (char)('0' + rnd64() % 10);
        if (nfrac || (rnd64() & 1)) {
            *p++ = '.';
            for (k = 0; k < nfrac; ++k)
                *p++ = (char)('0' + rnd64() % 10);
        }
        if (rnd64() % 3) {
            e = (int)(rnd64() % 700) - 350;
            p += sprintf(p, "%c%d", (rnd64() & 1) ? 'e' : 'E', e);
        }
        *p = 0;
        if (!agree(s)) {
            MT_EXPECT(0);
            return;
        }
    }
}

static void round_trips(void)
{
    char s[64];
    int i;
    for (i = 0; i < 60000; ++i) {
        uint64_t b = rnd64() & 0x7FFFFFFFFFFFFFFFull;
        double v, r;
        if ((b >> 52) == 0x7FF)
            continue;
        memcpy(&v, &b, 8);
        if (rnd64() & 1)
            v = -v;
        snprintf(s, sizeof(s), "%.17g", v);
        r = ucrt_strtod(s, NULL);
        if (!same_bits(r, v)) {
            printf("    round trip of %s: got %a want %a\n", s, r, v);
            MT_EXPECT(0);
            return;
        }
        snprintf(s, sizeof(s), "%a", v);
        r = ucrt_strtod(s, NULL);
        if (!same_bits(r, v)) {
            printf("    hex round trip of %s: got %a want %a\n", s, r, v);
            MT_EXPECT(0);
            return;
        }
    }
}

/* The midpoint of two adjacent doubles is printed exactly; strings just below, at and just above it must round
 * down, to even, and up. */
static void midpoints(void)
{
    char *buf = malloc(4000), *buf2 = malloc(4000);
    int i;
    for (i = 0; i < 4000; ++i) {
        uint64_t b = rnd64() & 0x7FEFFFFFFFFFFFFFull;
        double a, n;
        long double mid;
        if (i < 600)
            b = rnd64() & 0x000FFFFFFFFFFFFFull; /* denormals */
        memcpy(&a, &b, 8);
        n = nextafter(a, INFINITY);
        mid = ((long double)a + (long double)n) / 2;
        snprintf(buf, 4000, "%.1100Lf", mid);
        if (!agree(buf)) {
            MT_EXPECT(0);
            break;
        }
        /* Nudge the last digit: still a hard case, now off the tie. */
        {
            size_t len = strlen(buf);
            strcpy(buf2, buf);
            for (size_t k = len; k-- > 0;) {
                if (buf2[k] == '.')
                    continue;
                if (buf2[k] != '0') {
                    buf2[k] = (char)(buf2[k] - 1);
                    break;
                }
                buf2[k] = '9';
            }
            if (!agree(buf2)) {
                MT_EXPECT(0);
                break;
            }
            strcpy(buf2, buf);
            strcat(buf2, "000000000000000000000000000000000000000000000000000001");
            if (!agree(buf2)) {
                MT_EXPECT(0);
                break;
            }
        }
    }
    free(buf);
    free(buf2);
}

static void named_cases(void)
{
    static const char *const cases[] = {
        "0", "-0", "0.0", "1", "1.5", ".5", "5.", "-.5e1", "1e308", "1.7976931348623157e308", "1.7976931348623159e308",
        "1e309", "-1e309", "4.9e-324", "2e-324", "2.4703282292062327e-324", "2.4703282292062328e-324", "1e-400",
        "2.2250738585072011e-308", "2.2250738585072014e-308", "0.1", "0.3", "123456789012345678901234567890",
        "9007199254740993", "9007199254740992.5", "9007199254740993.5", "  \t\n 12.5xyz", "12.5e", "12.5e+", "12.5e+3x",
        "1e+5", "1E5", ".e5", ".", "-", "+", "e5", "", "  ", "0x", "0xg", "0x1p3", "0X1.8P1", "0x.8", "0x1.fffffffffffffp1023",
        "0x1p-1074", "0x1p-1075", "0x1.0000000000001p0", "0x1.00000000000008p0", "0x1.00000000000018p0",
        "inf", "-inf", "INF", "infinity", "InFiNiTy", "infinit", "nan", "-nan", "NAN", "nan(123)", "nan(", "nan()",
        "1e99999999999", "1e-99999999999", "00000000000000000000000001", "0.0000000000000000000000001",
        "1_000", "1,5", "1 5",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        MT_EXPECT(agree(cases[i]));
    /* The 1100-digit string of 2^-1075 (a tie between zero and the smallest denormal): half-even gives zero. */
    {
        char *big = malloc(2000);
        snprintf(big, 2000, "%.1100Lf", ldexpl(1.0L, -1075));
        MT_EXPECT(agree(big));
        free(big);
    }
    /* Long digit strings beyond the kept digits still round correctly. */
    {
        char s[1200];
        memset(s, '0', sizeof(s));
        s[0] = '0';
        s[1] = '.';
        memcpy(s + 2, "5000000000000", 13);
        s[1100] = '1';
        s[1101] = 0;
        MT_EXPECT(agree(s));
    }
}

static void errno_behaviour(void)
{
    g_ucrt_state.errno_value = 0;
    MT_EXPECT(isinf(ucrt_strtod("1e400", NULL)));
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    g_ucrt_state.errno_value = 0;
    MT_EXPECT(ucrt_strtod("1e-400", NULL) == 0.0);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    g_ucrt_state.errno_value = 0;
    MT_EXPECT(ucrt_strtod("1e300", NULL) == 1e300);
    MT_EXPECT_EQ(ucrt_get_errno(), 0);
    g_ucrt_state.errno_value = 0;
    MT_EXPECT(ucrt_strtod("0e-999", NULL) == 0.0);
    MT_EXPECT_EQ(ucrt_get_errno(), 0); /* a true zero is not an underflow */
}

static void float_matches_glibc(void)
{
    char s[80];
    int i;
    for (i = 0; i < 40000; ++i) {
        const char *e1;
        char *e2;
        float a, b;
        int nint = (int)(rnd64() % 12), nfrac = (int)(rnd64() % 14), k;
        char *p = s;
        for (k = 0; k < nint; ++k)
            *p++ = (char)('0' + rnd64() % 10);
        *p++ = '.';
        for (k = 0; k < nfrac; ++k)
            *p++ = (char)('0' + rnd64() % 10);
        if (rnd64() & 1)
            p += sprintf(p, "e%d", (int)(rnd64() % 100) - 50);
        *p = 0;
        a = ucrt_strtof(s, &e1);
        b = strtof(s, &e2);
        if (!same_bitsf(a, b) || e1 != e2) {
            printf("    strtof(\"%s\"): got %a want %a\n", s, (double)a, (double)b);
            MT_EXPECT(0);
            return;
        }
    }
    /* A value between two floats must round once, not through double. */
    {
        const char *e;
        /* 1 + 2^-24 + tiny: just above the float midpoint between 1 and 1+2^-23. */
        float up = ucrt_strtof("1.000000059604644775390626", &e);
        float tie = ucrt_strtof("1.0000000596046447753906250", &e);
        MT_EXPECT(same_bitsf(up, strtof("1.000000059604644775390626", NULL)));
        MT_EXPECT(same_bitsf(tie, 1.0f)); /* an exact tie rounds to the even mantissa */
    }
}

static void wide_strings(void)
{
    static const ucrt_wchar a[] = {' ', '-', '1', '2', '.', '5', 'e', '2', 'x', 0};
    static const ucrt_wchar b[] = {'I', 'n', 'F', 0};
    static const ucrt_wchar c[] = {'0', 'x', '1', 'p', '4', 0};
    const ucrt_wchar *e;
    MT_EXPECT(ucrt_wcstod(a, &e) == -1250.0);
    MT_EXPECT_EQ(*e, 'x');
    MT_EXPECT(isinf(ucrt_wcstod(b, &e)));
    MT_EXPECT(ucrt_wcstod(c, &e) == 16.0);
    MT_EXPECT(ucrt_wcstof(a, &e) == -1250.0f);
}

static void nan_flavours(void)
{
    uint64_t bits;
    double v = ucrt_strtod("nan", NULL);
    memcpy(&bits, &v, 8);
    MT_EXPECT(bits == 0x7FF8000000000000ull);
    v = ucrt_strtod("-nan", NULL);
    memcpy(&bits, &v, 8);
    MT_EXPECT(bits == 0xFFF8000000000000ull);
    v = ucrt_strtod("nan(ind)", NULL); /* the "indefinite" NaN of x86 */
    memcpy(&bits, &v, 8);
    MT_EXPECT(bits == 0xFFF8000000000000ull);
    v = ucrt_strtod("nan(snan)", NULL);
    memcpy(&bits, &v, 8);
    MT_EXPECT((bits & 0x7FF8000000000000ull) == 0x7FF0000000000000ull && (bits & 0xFFFFFFFFFFFFFull) != 0);
}

static const mt_case_t cases[] = {
    MT_CASE(random_decimal_strings),
    MT_CASE(round_trips),
    MT_CASE(midpoints),
    MT_CASE(named_cases),
    MT_CASE(errno_behaviour),
    MT_CASE(float_matches_glibc),
    MT_CASE(wide_strings),
    MT_CASE(nan_flavours),
};

int main(void)
{
    return MT_RUN("crt_strtod", cases);
}
