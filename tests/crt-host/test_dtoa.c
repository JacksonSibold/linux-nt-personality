/*
 * test_dtoa.c - tests for src/crt/ucrt_dtoa.h against glibc's printf, which rounds the exact binary value
 * half to even as the runtime does: %.Nf for mode 3 and %.Ne for mode 2, on random doubles of every
 * magnitude, denormals, exact ties, powers of ten, and values near the rounding boundaries.
 */
#include "minitest.h"
#include "ucrt_dtoa.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* %.Nf from the digits of mode 3. */
static void build_fixed(char *out, double v, int nd)
{
    char dig[1500];
    int decpt = 0, n = ucrt_dtoa(fabs(v), 3, nd, dig, &decpt), i;
    char *p = out;
    if (signbit(v))
        *p++ = '-';
    if (decpt <= 0) {
        *p++ = '0';
    } else {
        for (i = 0; i < decpt; ++i)
            *p++ = i < n ? dig[i] : '0';
    }
    if (nd > 0) {
        *p++ = '.';
        for (i = 0; i < nd; ++i) {
            int idx = decpt + i; /* index into dig of this fraction digit; negative means a leading zero */
            *p++ = (idx >= 0 && idx < n) ? dig[idx] : '0';
        }
    }
    *p = 0;
}

/* %.Ne from the digits of mode 2: N+1 significant digits. */
static void build_exp(char *out, double v, int nd)
{
    char dig[1500];
    int decpt = 0, n = ucrt_dtoa(fabs(v), 2, nd + 1, dig, &decpt), i, e;
    char *p = out;
    if (signbit(v))
        *p++ = '-';
    *p++ = dig[0];
    if (nd > 0) {
        *p++ = '.';
        for (i = 1; i <= nd; ++i)
            *p++ = i < n ? dig[i] : '0';
    }
    e = (v == 0) ? 0 : decpt - 1;
    p += sprintf(p, "e%c%02d", e < 0 ? '-' : '+', abs(e));
}

static int check_value(double v)
{
    static const int nds[] = {0, 1, 2, 3, 5, 9, 10, 15, 16, 17, 20, 25, 40};
    char want[1600], got[1600];
    for (size_t k = 0; k < sizeof(nds) / sizeof(nds[0]); ++k) {
        int nd = nds[k];
        snprintf(want, sizeof(want), "%.*f", nd, v);
        build_fixed(got, v, nd);
        if (strcmp(want, got)) {
            printf("    fixed %%.%df of %a: want %s got %s\n", nd, v, want, got);
            return 0;
        }
        snprintf(want, sizeof(want), "%.*e", nd, v);
        build_exp(got, v, nd);
        if (strcmp(want, got)) {
            printf("    exp %%.%de of %a: want %s got %s\n", nd, v, want, got);
            return 0;
        }
    }
    return 1;
}

static void random_doubles(void)
{
    int i;
    for (i = 0; i < 40000; ++i) {
        uint64_t b = rnd64();
        double v;
        b &= 0x7FFFFFFFFFFFFFFFull;
        if ((b >> 52) == 0x7FF)
            continue; /* inf and nan are the caller's business */
        memcpy(&v, &b, sizeof(v));
        if (!check_value(v)) {
            MT_EXPECT(0);
            return;
        }
    }
}

static void moderate_magnitudes(void)
{
    int i;
    for (i = 0; i < 40000; ++i) {
        double v = (double)(int64_t)(rnd64() >> 12) / (double)(1u << (rnd64() % 30));
        if (rnd64() & 1)
            v = -v;
        if (!check_value(v)) {
            MT_EXPECT(0);
            return;
        }
    }
}

static void exact_ties_and_edges(void)
{
    static const double vals[] = {
        0.0, -0.0, 0.5, 1.5, 2.5, 3.5, 0.125, 0.375, 0.625, 2.675, 1.005, 0.045, 1e22, 1e23, 1e-5, 9.995, 99.5, 999.5,
        0.9999999999999999, 0.99999999999999989, 9.5, 0.05, 0.15, 0.25, 0.35, 4.35, 1e300, 1e-300, 4.9e-324,
        2.2250738585072014e-308, 1.7976931348623157e308, 123456789012345678.0, 5e-324, 0.1, 0.2, 0.3,
        100.0, 1000000.0, 4503599627370496.5, 4503599627370497.0, 9007199254740993.0,
    };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i)
        MT_EXPECT(check_value(vals[i]));
    for (int e = -320; e <= 300; ++e) { /* every power of ten */
        char buf[32];
        snprintf(buf, sizeof(buf), "1e%d", e);
        MT_EXPECT(check_value(strtod(buf, NULL)));
    }
    for (int e = -1074; e <= 1023; e += 7)
        MT_EXPECT(check_value(ldexp(1.0, e)));
}

static void large_precision_is_exact(void)
{
    /* The expansion of 2^-1074 has 751 significant digits; ask for more digits than that. */
    char want[2000], got[2000];
    double v = 4.9406564584124654e-324;
    snprintf(want, sizeof(want), "%.1100f", v);
    build_fixed(got, v, 1100);
    MT_EXPECT(strcmp(want, got) == 0);
    snprintf(want, sizeof(want), "%.760e", v);
    build_exp(got, v, 760);
    MT_EXPECT(strcmp(want, got) == 0);
    v = 1.7976931348623157e308;
    snprintf(want, sizeof(want), "%.30f", v);
    build_fixed(got, v, 30);
    MT_EXPECT(strcmp(want, got) == 0);
}

static void digit_string_properties(void)
{
    char dig[1500];
    int decpt, n;
    /* Zero. */
    n = ucrt_dtoa(0.0, 2, 5, dig, &decpt);
    MT_EXPECT_EQ(n, 1);
    MT_EXPECT(dig[0] == '0' && decpt == 1);
    n = ucrt_dtoa(0.0, 3, 5, dig, &decpt);
    MT_EXPECT_EQ(n, 0);
    /* A value that rounds to zero in fixed mode has no digits. */
    n = ucrt_dtoa(0.0004, 3, 2, dig, &decpt);
    MT_EXPECT_EQ(n, 0);
    /* Carry into a new leading digit. */
    n = ucrt_dtoa(9.996, 3, 2, dig, &decpt);
    MT_EXPECT(n == 4 && memcmp(dig, "1000", 4) == 0 && decpt == 2);
    n = ucrt_dtoa(9.996, 2, 3, dig, &decpt);
    MT_EXPECT(n == 3 && memcmp(dig, "100", 3) == 0 && decpt == 2);
    /* A tie rounds to even: 2.5 -> 2, 3.5 -> 4 with no fraction. */
    n = ucrt_dtoa(2.5, 3, 0, dig, &decpt);
    MT_EXPECT(n == 1 && dig[0] == '2' && decpt == 1);
    n = ucrt_dtoa(3.5, 3, 0, dig, &decpt);
    MT_EXPECT(n == 1 && dig[0] == '4' && decpt == 1);
    /* Out-of-range digit counts are clamped. */
    n = ucrt_dtoa(1.0, 2, 5000, dig, &decpt);
    MT_EXPECT_EQ(n, UCRT_DTOA_MAX_DIGITS);
    n = ucrt_dtoa(1.0, 2, 0, dig, &decpt); /* mode 2 keeps at least one digit */
    MT_EXPECT(n == 1 && dig[0] == '1');
}

static const mt_case_t cases[] = {
    MT_CASE(random_doubles),
    MT_CASE(moderate_magnitudes),
    MT_CASE(exact_ties_and_edges),
    MT_CASE(large_precision_is_exact),
    MT_CASE(digit_string_properties),
};

int main(void)
{
    return MT_RUN("crt_dtoa", cases);
}
