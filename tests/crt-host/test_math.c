/*
 * test_math.c - tests for src/crt/ucrt_math.h against glibc. The exact functions must match bit for bit; the
 * transcendental ones may differ from glibc by one unit in the last place (both are within an ulp of the true
 * value) and the fraction of exact matches is required to be high.
 */
#include "minitest.h"
#include <math.h>
#include <string.h>
#include <float.h>
#include <errno.h>
#include "ucrt_math.h"
#include <stdio.h>

#define MT_FAIL(...) do { char fb_[256]; snprintf(fb_, sizeof fb_, __VA_ARGS__); mt_fail(__FILE__, __LINE__, fb_); } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* A random double with an exponent drawn from [lo, hi] (binary), random sign and significand. */
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

/* The distance in representable doubles between a and b (0 when equal); huge when one is NaN or the signs differ
 * across more than the zero. */
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

#define EXACT_CASE(NAME, ARGDECL, GEN, CALL_U, CALL_G)                            \
    static void NAME(void)                                                        \
    {                                                                             \
        for (int i = 0; i < 200000; ++i) {                                        \
            ARGDECL GEN;                                                          \
            double a = CALL_U, b = CALL_G;                                        \
            if (!same_bits(a, b)) {                                               \
                MT_FAIL("%s mismatch at iteration %d: %a vs %a", #NAME, i, a, b); \
                return;                                                           \
            }                                                                     \
        }                                                                         \
    }

static double special_value(uint64_t r)
{
    static const double v[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5, -2.5, 0x1p52, 0x1p53, -0x1p52, 4503599627370495.5,
                               DBL_MAX, -DBL_MAX, DBL_MIN, 4.9406564584124654e-324, -4.9406564584124654e-324, INFINITY, -INFINITY, NAN,
                               0.49999999999999994, -0.49999999999999994, 3.0, 1e300, 1e-300, 2.2250738585072009e-308};
    return v[r % (sizeof(v) / sizeof(v[0]))];
}

static double any_double(void)
{
    uint64_t r = rnd64();
    switch (r % 5) {
    case 0: return special_value(rnd64());
    case 1: return ucrt_u2d(rnd64());               /* any bit pattern */
    case 2: return rnd_exp(-60, 60);
    case 3: return rnd_exp(-4, 4);
    default: return (double)(int)(rnd64() % 2001) / 8.0 - 125.0; /* multiples of 1/8 */
    }
}

EXACT_CASE(floor_matches, double x =, any_double(), ucrt_floor(x), floor(x))
EXACT_CASE(ceil_matches, double x =, any_double(), ucrt_ceil(x), ceil(x))
EXACT_CASE(trunc_matches, double x =, any_double(), ucrt_trunc(x), trunc(x))
EXACT_CASE(round_matches, double x =, any_double(), ucrt_round(x), round(x))
EXACT_CASE(rint_matches, double x =, any_double(), ucrt_rint(x), rint(x))
EXACT_CASE(fabs_matches, double x =, any_double(), ucrt_fabs(x), fabs(x))
EXACT_CASE(sqrt_matches, double x =, ucrt_fabs(any_double()), ucrt_sqrt(x), sqrt(x))
EXACT_CASE(fmod_matches, double x = any_double(); double y =, any_double(), ucrt_fmod(x, y), fmod(x, y))
EXACT_CASE(remainder_matches, double x = any_double(); double y =, any_double(), ucrt_remainder(x, y), remainder(x, y))
EXACT_CASE(copysign_matches, double x = any_double(); double y =, any_double(), ucrt_copysign(x, y), copysign(x, y))
EXACT_CASE(nextafter_matches, double x = any_double(); double y =, any_double(), ucrt_nextafter(x, y), nextafter(x, y))
EXACT_CASE(logb_matches, double x =, any_double(), ucrt_logb(x), logb(x))

static void fmod_close_operands(void)
{
    /* operands of similar magnitude exercise the loop's last steps and the subnormal tail */
    for (int i = 0; i < 200000; ++i) {
        double y = rnd_exp(-1074 + 53, 1000);
        double x = y * (1.0 + rnd_unit() * 7.0) * ((rnd64() & 1) ? 1 : -1);
        if (!same_bits(ucrt_fmod(x, y), fmod(x, y)) || !same_bits(ucrt_remainder(x, y), remainder(x, y))) {
            MT_FAIL("fmod/remainder mismatch for %a %a", x, y);
            return;
        }
    }
    {
        double sub = 4.9406564584124654e-324, a = 3 * sub, b = 2 * sub;
        MT_EXPECT(same_bits(ucrt_fmod(a, b), fmod(a, b)));
        MT_EXPECT(same_bits(ucrt_remainder(a, b), remainder(a, b)));
    }
}

static void frexp_ldexp_modf_ilogb(void)
{
    for (int i = 0; i < 200000; ++i) {
        double x = any_double();
        int e1, e2, n = (int)(rnd64() % 4400) - 2200;
        double m1 = ucrt_frexp(x, &e1), m2 = frexp(x, &e2);
        if (!same_bits(m1, m2) || (isfinite(x) && x != 0 && e1 != e2)) {
            MT_FAIL("frexp mismatch for %a: %a/%d vs %a/%d", x, m1, e1, m2, e2);
            return;
        }
        if (!same_bits(ucrt_scalbn(x, n), scalbn(x, n))) {
            MT_FAIL("scalbn mismatch for %a, %d: %a vs %a", x, n, ucrt_scalbn(x, n), scalbn(x, n));
            return;
        }
        {
            double ip1, ip2;
            double f1 = ucrt_modf(x, &ip1), f2 = modf(x, &ip2);
            if (!same_bits(f1, f2) || !same_bits(ip1, ip2)) {
                MT_FAIL("modf mismatch for %a", x);
                return;
            }
        }
        if (isfinite(x) && x != 0 && ucrt_ilogb(x) != ilogb(x)) {
            MT_FAIL("ilogb mismatch for %a", x);
            return;
        }
    }
}

static void classification(void)
{
    for (int i = 0; i < 100000; ++i) {
        double x = any_double();
        int cls = fpclassify(x), mine = ucrt_fpclassify(x);
        int expect = cls == FP_NAN ? UCRT_FP_NAN : cls == FP_INFINITE ? UCRT_FP_INFINITE : cls == FP_ZERO ? UCRT_FP_ZERO
                     : cls == FP_SUBNORMAL ? UCRT_FP_SUBNORMAL : UCRT_FP_NORMAL;
        MT_ASSERT(mine == expect);
        MT_ASSERT(ucrt_isnan(x) == !!isnan(x));
        MT_ASSERT(ucrt_isinf(x) == !!isinf(x));
        MT_ASSERT(ucrt_isfinite(x) == !!isfinite(x));
        MT_ASSERT(ucrt_signbit(x) == !!signbit(x));
    }
}

/* The references are glibc's long double functions, rounded: more accurate than its double ones. */
static double ref_exp(double x) { return (double)expl(x); }
static double ref_exp2(double x) { return (double)exp2l(x); }
static double ref_expm1(double x) { return (double)expm1l(x); }
static double ref_log(double x) { return (double)logl(x); }
static double ref_log2(double x) { return (double)log2l(x); }
static double ref_log10(double x) { return (double)log10l(x); }
static double ref_log1p(double x) { return (double)log1pl(x); }
static double ref_sinh(double x) { return (double)sinhl(x); }
static double ref_cosh(double x) { return (double)coshl(x); }
static double ref_tanh(double x) { return (double)tanhl(x); }
static double ref_asinh(double x) { return (double)asinhl(x); }
static double ref_acosh(double x) { return (double)acoshl(x); }
static double ref_atanh(double x) { return (double)atanhl(x); }
static double ref_cbrt(double x) { return (double)cbrtl(x); }
static double ref_sin(double x) { return (double)sinl(x); }
static double ref_cos(double x) { return (double)cosl(x); }
static double ref_tan(double x) { return (double)tanl(x); }
static double ref_atan(double x) { return (double)atanl(x); }
static double ref_asin(double x) { return (double)asinl(x); }
static double ref_acos(double x) { return (double)acosl(x); }
static double ref_atan2(double y, double x) { return (double)atan2l(y, x); }
static double ref_pow(double x, double y) { return (double)powl(x, y); }
static double ref_hypot(double x, double y) { return (double)hypotl(x, y); }

/* Compares a transcendental function with glibc over many inputs; at most one ulp apart, and mostly identical. */
typedef double (*fn1)(double);
typedef double (*fn2)(double, double);

static void check_fn1(const char *name, fn1 mine, fn1 ref, double (*gen)(void), int iters)
{
    int exact = 0, total = 0;
    for (int i = 0; i < iters; ++i) {
        double x = gen();
        double a = mine(x), b = ref(x);
        uint64_t d = ulp_dist(a, b);
        ++total;
        if (d == 0) {
            ++exact;
        } else if (d > 1) {
            MT_FAIL("%s(%a): %a vs glibc %a (%llu ulp)", name, x, a, b, (unsigned long long)d);
            return;
        }
    }
    if (exact * 1000 < total * 990)
        MT_FAIL("%s: only %d of %d results match glibc exactly", name, exact, total);
}

static void check_fn2(const char *name, fn2 mine, fn2 ref, double (*gx)(void), double (*gy)(void), int iters)
{
    int exact = 0, total = 0;
    for (int i = 0; i < iters; ++i) {
        double x = gx(), y = gy();
        double a = mine(x, y), b = ref(x, y);
        uint64_t d = ulp_dist(a, b);
        ++total;
        if (d == 0) {
            ++exact;
        } else if (d > 1) {
            MT_FAIL("%s(%a, %a): %a vs glibc %a (%llu ulp)", name, x, y, a, b, (unsigned long long)d);
            return;
        }
    }
    if (exact * 1000 < total * 990)
        MT_FAIL("%s: only %d of %d results match glibc exactly", name, exact, total);
}

static double g_small(void) { return (rnd_unit() - 0.5) * 20; }
static double g_wide(void) { return rnd_exp(-40, 9); }
static double g_pos_wide(void) { return ucrt_fabs(rnd_exp(-1070, 1020)); }
static double g_pos_mid(void) { return ucrt_fabs(rnd_exp(-20, 20)); }
static double g_near1(void) { return 1.0 + (rnd_unit() - 0.5) * 1e-3; }
static double g_exp_range(void) { return (rnd_unit() - 0.5) * 1480; }
static double g_unit(void) { return (rnd_unit() - 0.5) * 2; }
static double g_ge1(void) { return 1.0 + ucrt_fabs(rnd_exp(-30, 30)); }
static double g_hyp(void) { return (rnd_unit() - 0.5) * 1430; }

static void exp_family(void)
{
    check_fn1("exp", ucrt_exp, ref_exp, g_exp_range, 200000);
    check_fn1("exp.small", ucrt_exp, ref_exp, g_small, 200000);
    check_fn1("exp2", ucrt_exp2, ref_exp2, g_exp_range, 200000);
    check_fn1("expm1", ucrt_expm1, ref_expm1, g_small, 200000);
    check_fn1("expm1.tiny", ucrt_expm1, ref_expm1, g_wide, 200000);
}

static void log_family(void)
{
    check_fn1("log", ucrt_log, ref_log, g_pos_wide, 200000);
    check_fn1("log.mid", ucrt_log, ref_log, g_pos_mid, 200000);
    check_fn1("log.near1", ucrt_log, ref_log, g_near1, 200000);
    check_fn1("log2", ucrt_log2, ref_log2, g_pos_wide, 200000);
    check_fn1("log10", ucrt_log10, ref_log10, g_pos_wide, 200000);
    check_fn1("log1p", ucrt_log1p, ref_log1p, g_small, 100000);
    check_fn1("log1p.tiny", ucrt_log1p, ref_log1p, g_wide, 100000);
    /* exact powers are exact */
    for (int e = -1074; e <= 1023; ++e) {
        double p = ucrt_scalbn(1.0, e);
        MT_ASSERT(ucrt_log2(p) == (double)e);
    }
    for (int e = -22; e <= 22; ++e) {
        double p = pow(10.0, e);
        if (e >= 0)
            MT_ASSERT(ucrt_log10(p) == (double)e);
    }
}

static void hyperbolic(void)
{
    check_fn1("sinh", ucrt_sinh, ref_sinh, g_hyp, 200000);
    check_fn1("sinh.small", ucrt_sinh, ref_sinh, g_small, 200000);
    check_fn1("cosh", ucrt_cosh, ref_cosh, g_hyp, 200000);
    check_fn1("tanh", ucrt_tanh, ref_tanh, g_small, 200000);
    check_fn1("tanh.tiny", ucrt_tanh, ref_tanh, g_wide, 100000);
    check_fn1("asinh", ucrt_asinh, ref_asinh, g_small, 200000);
    check_fn1("asinh.wide", ucrt_asinh, ref_asinh, g_wide, 100000);
    check_fn1("acosh", ucrt_acosh, ref_acosh, g_ge1, 200000);
    check_fn1("atanh", ucrt_atanh, ref_atanh, g_unit, 200000);
}

static double gy_small(void) { return (rnd_unit() - 0.5) * 40; }
static double gx_pos(void) { return rnd_unit() * 100; }
static double gx_any(void) { return (rnd_unit() - 0.5) * 100; }
static double gy_int(void) { return (double)((int)(rnd64() % 41) - 20); }
static double gx_bigr(void) { return ucrt_fabs(rnd_exp(-30, 30)); }
static double gy_frac(void) { return (rnd_unit() - 0.5) * 6; }

static void pow_random(void)
{
    check_fn2("pow", ucrt_pow, ref_pow, gx_pos, gy_small, 200000);
    check_fn2("pow.int", ucrt_pow, ref_pow, gx_any, gy_int, 200000);
    check_fn2("pow.wide", ucrt_pow, ref_pow, gx_bigr, gy_frac, 200000);
    /* integer powers of small integers are exact */
    for (int b = 2; b <= 12; ++b)
        for (int e = 0; e <= 15; ++e) {
            double want = 1;
            for (int k = 0; k < e; ++k)
                want *= b;
            MT_ASSERT(ucrt_pow(b, e) == want);
        }
}

static void pow_special_cases(void)
{
    static const double vals[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 3.0, -3.0, 0.25, 1e300, -1e300, 1e-300, INFINITY, -INFINITY, NAN, 1e10, 0x1p53, 0x1p54, 2.5, -2.5};
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i)
        for (size_t j = 0; j < sizeof(vals) / sizeof(vals[0]); ++j) {
            double x = vals[i], y = vals[j];
            double a = ucrt_pow(x, y), b = pow(x, y);
            /* the domain error result is a NaN either way; every other result matches within an ulp */
            if (ulp_dist(a, b) > 1) {
                MT_FAIL("pow(%a, %a) = %a, glibc %a", x, y, a, b);
                return;
            }
            if (!isnan(b) && signbit(a) != signbit(b)) {
                MT_FAIL("pow(%a, %a) sign differs: %a vs %a", x, y, a, b);
                return;
            }
        }
}

static void errno_reporting(void)
{
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_isnan(ucrt_sqrt(-1.0)));
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EDOM);
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_log(0.0) == -INFINITY);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_isnan(ucrt_log(-2.0)));
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EDOM);
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_exp(1000.0) == INFINITY);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_exp(1.0) > 2.7);
    MT_EXPECT_EQ(ucrt_get_errno(), 0);
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_isnan(ucrt_pow(-8.0, 1.0 / 3.0)));
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EDOM);
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_isnan(ucrt_fmod(1.0, 0.0)));
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EDOM);
    /* the NaN of a domain error is the indefinite: the sign bit is set */
    MT_EXPECT(ucrt_d2u(ucrt_sqrt(-1.0)) == 0xFFF8000000000000ull);
}

static void hypot_cbrt(void)
{
    check_fn2("hypot", ucrt_hypot, ref_hypot, g_wide, g_wide, 200000);
    check_fn1("cbrt", ucrt_cbrt, ref_cbrt, g_wide, 200000);
    for (int i = -100; i <= 100; ++i)
        MT_ASSERT(ucrt_cbrt((double)i * i * i) == (double)i);
    MT_EXPECT(ucrt_hypot(3.0, 4.0) == 5.0);
    MT_EXPECT(ucrt_hypot(DBL_MAX, DBL_MAX) == INFINITY);
    MT_EXPECT(ucrt_hypot(INFINITY, NAN) == INFINITY);
}

static double g_trig_small(void) { return (rnd_unit() - 0.5) * 20; }
static double g_trig_wide(void) { return rnd_exp(-30, 60); }
static double g_trig_huge(void) { return rnd_exp(20, 1023); }
/* the double nearest to k * pi/2, where the reduced argument is as small as the format allows */
static double g_trig_nearpi(void)
{
    long double k = (long double)(rnd64() % 20000000ull) + 1;
    return (double)(k * 1.57079632679489661923132169163975144L);
}
static double g_trig_nearpi_big(void)
{
    long double k = (long double)(rnd64() >> 12) * 4;
    return (double)(k * 1.57079632679489661923132169163975144L);
}

static void trig_functions(void)
{
    double (*gens[])(void) = {g_trig_small, g_trig_wide, g_trig_huge, g_trig_nearpi, g_trig_nearpi_big};
    static const char *const gname[] = {"small", "wide", "huge", "nearpi", "nearpi_big"};
    for (size_t g = 0; g < sizeof(gens) / sizeof(gens[0]); ++g) {
        char name[48];
        snprintf(name, sizeof name, "sin.%s", gname[g]);
        check_fn1(name, ucrt_sin, ref_sin, gens[g], 100000);
        snprintf(name, sizeof name, "cos.%s", gname[g]);
        check_fn1(name, ucrt_cos, ref_cos, gens[g], 100000);
        snprintf(name, sizeof name, "tan.%s", gname[g]);
        check_fn1(name, ucrt_tan, ref_tan, gens[g], 100000);
    }
    MT_EXPECT(ucrt_sin(0.0) == 0.0 && !ucrt_signbit(ucrt_sin(0.0)));
    MT_EXPECT(ucrt_signbit(ucrt_sin(-0.0)));
    MT_EXPECT(ucrt_cos(0.0) == 1.0);
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_isnan(ucrt_sin(INFINITY)) && ucrt_get_errno() == UCRT_EDOM);
    MT_EXPECT(ucrt_isnan(ucrt_cos(-INFINITY)));
    MT_EXPECT(ucrt_isnan(ucrt_tan(NAN)));
}

static double g_inv_wide(void) { return rnd_exp(-60, 60); }
static double g_unit1(void) { return (rnd_unit() - 0.5) * 2; }
static double g_unit_edge(void) { return (rnd64() & 1 ? 1 : -1) * (1.0 - rnd_unit() * 1e-6); }

static void inverse_trig(void)
{
    check_fn1("atan", ucrt_atan, ref_atan, g_inv_wide, 200000);
    check_fn1("atan.small", ucrt_atan, ref_atan, g_trig_small, 200000);
    check_fn1("asin", ucrt_asin, ref_asin, g_unit1, 200000);
    check_fn1("asin.edge", ucrt_asin, ref_asin, g_unit_edge, 100000);
    check_fn1("acos", ucrt_acos, ref_acos, g_unit1, 200000);
    check_fn1("acos.edge", ucrt_acos, ref_acos, g_unit_edge, 100000);
    check_fn2("atan2", ucrt_atan2, ref_atan2, g_inv_wide, g_inv_wide, 300000);
    check_fn2("atan2.near", ucrt_atan2, ref_atan2, g_trig_small, g_trig_small, 200000);
    {
        static const double v[] = {0.0, -0.0, 1.0, -1.0, 2.0, -2.0, INFINITY, -INFINITY, NAN, 1e300, 1e-300};
        for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); ++i)
            for (size_t j = 0; j < sizeof(v) / sizeof(v[0]); ++j) {
                double a = ucrt_atan2(v[i], v[j]), b = atan2(v[i], v[j]);
                if (ulp_dist(a, b) > 1 || (!isnan(b) && signbit(a) != signbit(b))) {
                    MT_FAIL("atan2(%a, %a) = %a, glibc %a", v[i], v[j], a, b);
                    return;
                }
            }
    }
    ucrt_set_errno(0);
    MT_EXPECT(ucrt_isnan(ucrt_asin(1.5)) && ucrt_get_errno() == UCRT_EDOM);
    MT_EXPECT(ucrt_isnan(ucrt_acos(-1.5)));
    MT_EXPECT(ucrt_atan(INFINITY) == atan(INFINITY));
}

static const mt_case_t cases[] = {
    MT_CASE(trig_functions),    MT_CASE(inverse_trig),
    MT_CASE(floor_matches),     MT_CASE(ceil_matches),    MT_CASE(trunc_matches),      MT_CASE(round_matches),
    MT_CASE(rint_matches),      MT_CASE(fabs_matches),    MT_CASE(sqrt_matches),       MT_CASE(fmod_matches),
    MT_CASE(remainder_matches), MT_CASE(copysign_matches), MT_CASE(nextafter_matches), MT_CASE(logb_matches),
    MT_CASE(fmod_close_operands), MT_CASE(frexp_ldexp_modf_ilogb), MT_CASE(classification),
    MT_CASE(exp_family),        MT_CASE(log_family),      MT_CASE(hyperbolic),         MT_CASE(pow_random),
    MT_CASE(pow_special_cases), MT_CASE(errno_reporting), MT_CASE(hypot_cbrt),
};

int main(void)
{
    return MT_RUN("crt_math", cases);
}
