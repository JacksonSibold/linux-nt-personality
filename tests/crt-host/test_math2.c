/*
 * test_math2.c - tests for src/crt/ucrt_math2.h: fma (exact, against glibc bit for bit), the integer
 * conversions, fmin/fmax/fdim, erf/erfc and the gamma functions against glibc's long double versions, and the
 * float wrappers.
 */
#include "minitest.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include "ucrt_math2.h"

#define MT_FAIL(...) do { char fb_[256]; snprintf(fb_, sizeof fb_, __VA_ARGS__); mt_fail(__FILE__, __LINE__, fb_); } while (0)

static uint64_t rng = 0xD1B54A32D192ED03ull;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static double rnd_exp(int lo, int hi)
{
    uint64_t e = (uint64_t)(1023 + lo + (int)(rnd64() % (uint64_t)(hi - lo + 1)));
    return ucrt_u2d((rnd64() & UCRT_DSIGN) | (e << 52) | (rnd64() & UCRT_DFRACMASK));
}

static double rnd_unit(void)
{
    return (double)(rnd64() >> 11) / 9007199254740992.0;
}

static int same_bits(double a, double b)
{
    if (isnan(a) && isnan(b))
        return 1;
    return memcmp(&a, &b, 8) == 0;
}

static uint64_t ulp_dist(double a, double b)
{
    int64_t ia, ib;
    if (isnan(a) || isnan(b))
        return (isnan(a) && isnan(b)) ? 0 : ~0ull;
    if (a == b)
        return 0;
    memcpy(&ia, &a, 8);
    memcpy(&ib, &b, 8);
    if (ia < 0)
        ia = (int64_t)0x8000000000000000ull - ia;
    if (ib < 0)
        ib = (int64_t)0x8000000000000000ull - ib;
    return ia > ib ? (uint64_t)(ia - ib) : (uint64_t)(ib - ia);
}

static void fma_random(void)
{
    for (int i = 0; i < 400000; ++i) {
        double x, y, z;
        switch (rnd64() % 6) {
        case 0: x = rnd_exp(-20, 20); y = rnd_exp(-20, 20); z = rnd_exp(-45, 45); break;
        case 1: x = rnd_exp(-500, 500); y = rnd_exp(-500, 500); z = rnd_exp(-1000, 1000); break;
        case 2: /* heavy cancellation: z is minus the rounded product, plus a small change */
            x = rnd_exp(-30, 30); y = rnd_exp(-30, 30); z = -(x * y);
            if (rnd64() & 1)
                z = nextafter(z, rnd64() & 1 ? INFINITY : -INFINITY);
            break;
        case 3: /* subnormal results */
            x = rnd_exp(-540, -500); y = rnd_exp(-540, -500); z = rnd_exp(-1074, -1000); break;
        case 4: /* near overflow */
            x = rnd_exp(500, 520); y = rnd_exp(500, 520); z = rnd_exp(1000, 1023); break;
        default: x = rnd_exp(-3, 3); y = rnd_exp(-3, 3); z = (double)((int)(rnd64() % 9) - 4); break;
        }
        {
            double a = ucrt_fma(x, y, z), b = fma(x, y, z);
            if (!same_bits(a, b)) {
                MT_FAIL("fma(%a, %a, %a) = %a, glibc %a", x, y, z, a, b);
                return;
            }
        }
    }
}

static void fma_specials(void)
{
    static const double v[] = {0.0, -0.0, 1.0, -1.0, 3.0, INFINITY, -INFINITY, NAN, DBL_MAX, DBL_MIN, 4.9406564584124654e-324, -4.9406564584124654e-324};
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); ++i)
        for (size_t j = 0; j < sizeof(v) / sizeof(v[0]); ++j)
            for (size_t k = 0; k < sizeof(v) / sizeof(v[0]); ++k) {
                double a = ucrt_fma(v[i], v[j], v[k]), b = fma(v[i], v[j], v[k]);
                if (!same_bits(a, b)) {
                    MT_FAIL("fma(%a, %a, %a) = %a, glibc %a", v[i], v[j], v[k], a, b);
                    return;
                }
            }
}

static void min_max_dim(void)
{
    static const double v[] = {0.0, -0.0, 1.0, -1.0, 2.5, INFINITY, -INFINITY, NAN, 1e300, -1e300};
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); ++i)
        for (size_t j = 0; j < sizeof(v) / sizeof(v[0]); ++j) {
            double x = v[i], y = v[j];
            double a = ucrt_fmin(x, y), b = fmin(x, y);
            MT_ASSERT((isnan(a) && isnan(b)) || a == b);
            a = ucrt_fmax(x, y);
            b = fmax(x, y);
            MT_ASSERT((isnan(a) && isnan(b)) || a == b);
            a = ucrt_fdim(x, y);
            b = fdim(x, y);
            MT_ASSERT((isnan(a) && isnan(b)) || a == b);
        }
    MT_EXPECT(ucrt_signbit(ucrt_fmin(0.0, -0.0)) && ucrt_signbit(ucrt_fmin(-0.0, 0.0)));
    MT_EXPECT(!ucrt_signbit(ucrt_fmax(0.0, -0.0)) && !ucrt_signbit(ucrt_fmax(-0.0, 0.0)));
}

static void integer_conversions(void)
{
    static const double v[] = {0.0, 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 2147483647.0, 2147483647.5, 2147483648.0, -2147483648.0,
                               -2147483648.5, -2147483649.0, 9.2233720368547748e18, 9.2233720368547758e18, -9.2233720368547758e18,
                               -9.3e18, 1e300, -1e300, INFINITY, -INFINITY, NAN, 4503599627370497.0, 0.49999999999999994};
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
        double x = v[i];
        long long a = ucrt_llrint(x), b = llrint(x);
        MT_ASSERT(a == b);
        a = ucrt_llround(x);
        b = llround(x);
        if (!isnan(x) && round(x) >= -9223372036854775808.0 && round(x) < 9223372036854775808.0)
            MT_ASSERT(a == b);
        else
            MT_ASSERT(a == (long long)0x8000000000000000ull);
        {
            int32_t l = ucrt_lrint(x), m = ucrt_lround(x);
            if (fabs(rint(x)) <= 2147483647.0)
                MT_ASSERT(l == (int32_t)rint(x));
            else
                MT_ASSERT(l == (int32_t)0x80000000u);
            if (fabs(round(x)) <= 2147483647.0)
                MT_ASSERT(m == (int32_t)round(x));
            else
                MT_ASSERT(m == (int32_t)0x80000000u);
        }
    }
    for (int i = 0; i < 100000; ++i) {
        double x = (rnd_unit() - 0.5) * 4e9;
        MT_ASSERT(ucrt_llrint(x) == llrint(x) && ucrt_llround(x) == llround(x));
    }
}

static double ref_erf(double x) { return (double)erfl(x); }
static double ref_erfc(double x) { return (double)erfcl(x); }
static double ref_tgamma(double x) { return (double)tgammal(x); }
static double ref_lgamma(double x) { return (double)lgammal(x); }

typedef double (*fn1)(double);

/* at most one ulp from the rounded long double reference (or within `abs_tol` of it, for results near zero) */
static void check_rate(const char *name, fn1 mine, fn1 ref, double (*gen)(void), int iters, double abs_tol, int min_per_mille)
{
    int exact = 0;
    for (int i = 0; i < iters; ++i) {
        double x = gen(), a = mine(x), b = ref(x);
        uint64_t d = ulp_dist(a, b);
        if (d == 0) {
            ++exact;
        } else if (d > 1 && !(fabs(a - b) <= abs_tol)) {
            MT_FAIL("%s(%a): %a vs glibc %a (%llu ulp)", name, x, a, b, (unsigned long long)d);
            return;
        }
    }
    if (exact * 1000 < iters * min_per_mille)
        MT_FAIL("%s: only %d of %d results are identical", name, exact, iters);
}

static void check(const char *name, fn1 mine, fn1 ref, double (*gen)(void), int iters, double abs_tol)
{
    check_rate(name, mine, ref, gen, iters, abs_tol, 985);
}

static double g_erf(void) { return (rnd_unit() - 0.5) * 14; }
static double g_erf_small(void) { return rnd_exp(-40, 0); }
static double g_erfc_pos(void) { return rnd_unit() * 27; }
static double g_gamma_pos(void) { return rnd_unit() * 30 + 1e-3; }
static double g_gamma_big(void) { return 1 + rnd_unit() * 170.5; }
static double g_gamma_neg(void) { return -(rnd_unit() * 30) - 1e-3; }
static double g_gamma_tiny(void) { return rnd_exp(-60, -1); }
static double g_lgamma_wide(void) { return rnd_exp(-20, 60); }
static double g_near1(void) { return 1 + (rnd_unit() - 0.5) * 0.4; }
static double g_near2(void) { return 2 + (rnd_unit() - 0.5) * 0.4; }

static void erf_functions(void)
{
    check("erf", ucrt_erf, ref_erf, g_erf, 100000, 0);
    check("erf.small", ucrt_erf, ref_erf, g_erf_small, 100000, 0);
    check("erfc", ucrt_erfc, ref_erfc, g_erfc_pos, 100000, 0);
    check("erfc.neg", ucrt_erfc, ref_erfc, g_erf, 100000, 0);
    MT_EXPECT(ucrt_erf(0.0) == 0.0 && ucrt_erf(INFINITY) == 1.0 && ucrt_erf(-INFINITY) == -1.0);
    MT_EXPECT(ucrt_erfc(INFINITY) == 0.0 && ucrt_erfc(-INFINITY) == 2.0);
}

static void gamma_functions(void)
{
    check_rate("tgamma", ucrt_tgamma, ref_tgamma, g_gamma_pos, 100000, 0, 950);
    check_rate("tgamma.big", ucrt_tgamma, ref_tgamma, g_gamma_big, 100000, 0, 850);
    check_rate("tgamma.neg", ucrt_tgamma, ref_tgamma, g_gamma_neg, 100000, 0, 950);
    check("tgamma.tiny", ucrt_tgamma, ref_tgamma, g_gamma_tiny, 50000, 0);
    check("lgamma", ucrt_lgamma, ref_lgamma, g_gamma_pos, 100000, 2e-16);
    check("lgamma.wide", ucrt_lgamma, ref_lgamma, g_lgamma_wide, 100000, 1e-16);
    check("lgamma.neg", ucrt_lgamma, ref_lgamma, g_gamma_neg, 100000, 2e-16);
    check("lgamma.near1", ucrt_lgamma, ref_lgamma, g_near1, 100000, 0);
    check("lgamma.near2", ucrt_lgamma, ref_lgamma, g_near2, 100000, 0);
    for (int n = 1; n <= 20; ++n) {
        double f = 1;
        for (int k = 2; k < n; ++k)
            f *= k;
        MT_ASSERT(ucrt_tgamma((double)n) == f); /* (n-1)! is exact */
    }
    MT_EXPECT(ucrt_tgamma(0.5) == (double)tgammal(0.5L));
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_isnan(ucrt_tgamma(-2.0)) && ucrt_get_errno() == UCRT_EDOM);
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_tgamma(0.0) == INFINITY && ucrt_tgamma(-0.0) == -INFINITY && ucrt_get_errno() == UCRT_ERANGE);
    MT_EXPECT(ucrt_tgamma(172.0) == INFINITY);
    MT_EXPECT(ucrt_lgamma(0.0) == INFINITY && ucrt_lgamma(-3.0) == INFINITY);
    {
        int sign;
        MT_EXPECT(ucrt_lgamma_r(-0.5, &sign) > 1.2 && sign == -1);
        MT_EXPECT(ucrt_lgamma_r(-1.5, &sign) > 0 && sign == 1);
        MT_EXPECT(ucrt_lgamma_r(3.5, &sign) > 0 && sign == 1);
    }
}

static void float_versions(void)
{
    for (int i = 0; i < 100000; ++i) {
        float x = (float)((rnd_unit() - 0.5) * 20), y = (float)((rnd_unit() - 0.5) * 20);
        float a, b;
        if (ucrt_sinf(x) != (float)sinl(x) || ucrt_cosf(x) != (float)cosl(x) || ucrt_atanf(x) != (float)atanl(x)) {
            MT_FAIL("float trig mismatch at %a", (double)x);
            return;
        }
        if (ucrt_atan2f(x, y) != (float)atan2l(x, y) || ucrt_hypotf(x, y) != (float)hypotl(x, y)) {
            MT_FAIL("float atan2/hypot mismatch at %a %a", (double)x, (double)y);
            return;
        }
        a = ucrt_expf(x);
        b = (float)expl(x);
        if (a != b) {
            MT_FAIL("expf(%a) = %a vs %a", (double)x, (double)a, (double)b);
            return;
        }
        if (fabsf(y) > 1e-3f) {
            a = ucrt_powf(fabsf(x), y);
            b = (float)powl(fabsl(x), y);
            if (a != b) {
                MT_FAIL("powf(%a, %a) = %a vs %a", (double)fabsf(x), (double)y, (double)a, (double)b);
                return;
            }
        }
        if (ucrt_sqrtf(fabsf(x)) != sqrtf(fabsf(x)) || ucrt_floorf(x) != floorf(x) || ucrt_fmodf(x, y) != fmodf(x, y)) {
            MT_FAIL("float exact mismatch");
            return;
        }
    }
    {
        int e1, e2;
        float f1 = ucrt_frexpf(40.0f, &e1), f2 = frexpf(40.0f, &e2);
        MT_EXPECT(f1 == f2 && e1 == e2);
        MT_EXPECT(ucrt_ldexpf(1.5f, 4) == 24.0f);
        {
            float ip;
            MT_EXPECT(ucrt_modff(3.25f, &ip) == 0.25f && ip == 3.0f);
        }
        MT_EXPECT(ucrt_fmaf(2.0f, 3.0f, 1.0f) == 7.0f);
    }
}

static void nan_payload(void)
{
    double n = ucrt_nan("");
    MT_EXPECT(ucrt_isnan(n) && !ucrt_signbit(n));
    MT_EXPECT((ucrt_d2u(ucrt_nan("5")) & 0xFFFFFFFFull) == 5);
    MT_EXPECT(ucrt_isnan(ucrt_nan(NULL)));
}

/* A Windows process starts with the x87 precision control at 53 bits; the guarded entry points must give the same
 * answers anyway, and restore the control word. */
static void guarded_under_double_precision(void)
{
    unsigned short saved, narrow = 0x27F, after;
    __asm__ volatile("fnstcw %0" : "=m"(saved));
    __asm__ volatile("fldcw %0" : : "m"(narrow));
    /* the test is only meaningful if the narrow precision really does hurt the unguarded code */
    {
        int differ = 0;
        for (int i = 0; i < 20000; ++i) {
            double x = (rnd_unit() - 0.5) * 40, narrow_r, wide_r;
            __asm__ volatile("fldcw %0" : : "m"(narrow));
            narrow_r = ucrt_exp(x);
            __asm__ volatile("fldcw %0" : : "m"(saved));
            wide_r = ucrt_exp(x);
            differ += !same_bits(narrow_r, wide_r);
        }
        __asm__ volatile("fldcw %0" : : "m"(narrow));
        MT_EXPECT(differ > 100);
    }
    /* compare each guarded function (run under 0x27F) with the plain one run under the default word */
    for (int i = 0; i < 20000; ++i) {
        double x = (rnd_unit() - 0.5) * 40, y = (rnd_unit() - 0.5) * 8, ax = x < 0 ? -x : x, g[12], r[12];
        __asm__ volatile("fldcw %0" : : "m"(narrow));
        g[0] = ucrt_exp_g(x); g[1] = ucrt_log_g(ax + 1e-3); g[2] = ucrt_sin_g(x); g[3] = ucrt_cos_g(x); g[4] = ucrt_tan_g(x);
        g[5] = ucrt_atan_g(x); g[6] = ucrt_pow_g(ax + 0.01, y); g[7] = ucrt_sinh_g(y); g[8] = ucrt_atan2_g(x, y); g[9] = ucrt_tgamma_g(ax / 3 + 0.1);
        g[10] = ucrt_erf_g(y); g[11] = ucrt_hypot_g(x, y);
        __asm__ volatile("fldcw %0" : : "m"(saved));
        r[0] = ucrt_exp(x); r[1] = ucrt_log(ax + 1e-3); r[2] = ucrt_sin(x); r[3] = ucrt_cos(x); r[4] = ucrt_tan(x);
        r[5] = ucrt_atan(x); r[6] = ucrt_pow(ax + 0.01, y); r[7] = ucrt_sinh(y); r[8] = ucrt_atan2(x, y); r[9] = ucrt_tgamma(ax / 3 + 0.1);
        r[10] = ucrt_erf(y); r[11] = ucrt_hypot(x, y);
        for (int k = 0; k < 12; ++k)
            if (!same_bits(g[k], r[k])) {
                __asm__ volatile("fldcw %0" : : "m"(saved));
                MT_FAIL("guarded function %d differs for %a %a: %a vs %a", k, x, y, g[k], r[k]);
                return;
            }
    }
    /* the guard leaves the control word as it found it */
    __asm__ volatile("fldcw %0" : : "m"(narrow));
    (void)ucrt_sin_g(1.0);
    __asm__ volatile("fnstcw %0" : "=m"(after));
    __asm__ volatile("fldcw %0" : : "m"(saved));
    MT_EXPECT_EQ(after, narrow);
}

static const mt_case_t cases[] = {
    MT_CASE(guarded_under_double_precision),
    MT_CASE(fma_random), MT_CASE(fma_specials), MT_CASE(min_max_dim), MT_CASE(integer_conversions),
    MT_CASE(erf_functions), MT_CASE(gamma_functions), MT_CASE(float_versions), MT_CASE(nan_payload),
};

int main(void)
{
    return MT_RUN("crt_math2", cases);
}
