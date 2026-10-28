/*
 * ucrt_math.h - the C math library, freestanding.
 *
 * The exactly specified functions (fabs, floor, ceil, trunc, round, fmod, remainder, ldexp, frexp, modf,
 * nextafter, sqrt, the comparisons) are exact bit-level code. The transcendental functions evaluate in the
 * x87 extended format (64-bit significand, which every x86-64 CPU has and MinGW's long double maps to), with
 * argument reduction exact enough for the full double range, and round once to double. That gives results
 * within one unit in the last place of the true value, and almost always the correctly rounded one.
 *
 * Domain and range errors set errno (EDOM, ERANGE) as the Microsoft runtime does, and a domain error returns
 * the default NaN of the x87 and SSE units (the "indefinite", sign bit set), which the printf family shows as
 * "-nan(ind)". Host tests: tests/crt-host/test_math.c compares with glibc.
 */
#ifndef UCRT_MATH_H
#define UCRT_MATH_H

#include "ucrt_state.h"
#include "ucrt_math_tab.h"

/* ---- Bits and classification ----------------------------------------------------------------------- */

#define UCRT_DSIGN 0x8000000000000000ull
#define UCRT_DEXPMASK 0x7FF0000000000000ull
#define UCRT_DFRACMASK 0x000FFFFFFFFFFFFFull

static inline uint64_t ucrt_d2u(double d)
{
    uint64_t u;
    __builtin_memcpy(&u, &d, 8);
    return u;
}

static inline double ucrt_u2d(uint64_t u)
{
    double d;
    __builtin_memcpy(&d, &u, 8);
    return d;
}

static inline double ucrt_indefinite(void)
{
    return ucrt_u2d(0xFFF8000000000000ull);
}

static inline double ucrt_infinity(void)
{
    return ucrt_u2d(UCRT_DEXPMASK);
}

static inline int ucrt_isnan(double x)
{
    return (ucrt_d2u(x) << 1) > (UCRT_DEXPMASK << 1);
}

static inline int ucrt_isinf(double x)
{
    return (ucrt_d2u(x) << 1) == (UCRT_DEXPMASK << 1);
}

static inline int ucrt_isfinite(double x)
{
    return (ucrt_d2u(x) & UCRT_DEXPMASK) != UCRT_DEXPMASK;
}

static inline int ucrt_signbit(double x)
{
    return (int)(ucrt_d2u(x) >> 63);
}

/* The values _fpclass returns, and fpclassify's C99 classes. */
#define UCRT_FP_NAN 0
#define UCRT_FP_INFINITE 1
#define UCRT_FP_NORMAL (-1)
#define UCRT_FP_SUBNORMAL (-2)
#define UCRT_FP_ZERO 2

static inline int ucrt_fpclassify(double x)
{
    uint64_t u = ucrt_d2u(x) & ~UCRT_DSIGN;
    if (u >= UCRT_DEXPMASK)
        return u > UCRT_DEXPMASK ? UCRT_FP_NAN : UCRT_FP_INFINITE;
    if (u == 0)
        return UCRT_FP_ZERO;
    return u < 0x0010000000000000ull ? UCRT_FP_SUBNORMAL : UCRT_FP_NORMAL;
}

static inline double ucrt_fabs(double x)
{
    return ucrt_u2d(ucrt_d2u(x) & ~UCRT_DSIGN);
}

static inline double ucrt_copysign(double x, double y)
{
    return ucrt_u2d((ucrt_d2u(x) & ~UCRT_DSIGN) | (ucrt_d2u(y) & UCRT_DSIGN));
}

static inline double ucrt_domain(void)
{
    ucrt_set_errno(UCRT_EDOM);
    return ucrt_indefinite();
}

static inline double ucrt_range(double v)
{
    ucrt_set_errno(UCRT_ERANGE);
    return v;
}

/* ---- Rounding to integers (exact) ------------------------------------------------------------------ */

static inline double ucrt_trunc(double x)
{
    uint64_t u = ucrt_d2u(x);
    int e = (int)((u >> 52) & 0x7FF) - 1023;
    if (e >= 52)
        return x; /* already integral, infinity or NaN */
    if (e < 0)
        return ucrt_u2d(u & UCRT_DSIGN);
    return ucrt_u2d(u & ~(UCRT_DFRACMASK >> e));
}

static inline double ucrt_floor(double x)
{
    double t = ucrt_trunc(x);
    return (x < 0 && t != x) ? t - 1.0 : t;
}

static inline double ucrt_ceil(double x)
{
    double t = ucrt_trunc(x);
    return (x > 0 && t != x) ? t + 1.0 : t;
}

/* Halves round away from zero. */
static inline double ucrt_round(double x)
{
    double t = ucrt_trunc(x);
    if (ucrt_fabs(x - t) >= 0.5)
        t += ucrt_copysign(1.0, x);
    return t;
}

/* Halves round to even (the default rounding mode, which Windows processes run in). */
static inline double ucrt_rint(double x)
{
    static const double two52 = 4503599627370496.0;
    volatile double y;
    double a = ucrt_fabs(x);
    if (!(a < two52))
        return x;
    y = a + two52;
    y = y - two52;
    return ucrt_copysign(y, x);
}

static inline double ucrt_modf(double x, double *ip)
{
    double t = ucrt_trunc(x);
    *ip = t;
    if (ucrt_isinf(x))
        return ucrt_copysign(0.0, x);
    return ucrt_copysign(x - t, x);
}

/* ---- Scaling --------------------------------------------------------------------------------------- */

static inline double ucrt_scalbn(double x, int n)
{
    if (n > 1023) {
        x *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            x *= 0x1p1023;
            n -= 1023;
            if (n > 1023)
                n = 1023;
        }
    } else if (n < -1022) {
        /* scale in two steps, with 53 bits of headroom, so the final rounding into the subnormals happens once */
        x *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            x *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022)
                n = -1022;
        }
    }
    return x * ucrt_u2d((uint64_t)(0x3FF + n) << 52);
}

static inline double ucrt_ldexp(double x, int n)
{
    double r = ucrt_scalbn(x, n);
    if (ucrt_isinf(r) && !ucrt_isinf(x))
        ucrt_set_errno(UCRT_ERANGE);
    else if (r == 0 && x != 0 && ucrt_isfinite(x))
        ucrt_set_errno(UCRT_ERANGE);
    return r;
}

/* frexp: x = m * 2^e with 0.5 <= |m| < 1. */
static inline double ucrt_frexp(double x, int *e)
{
    uint64_t u = ucrt_d2u(x);
    int ex = (int)((u >> 52) & 0x7FF);
    *e = 0;
    if (ex == 0x7FF || (u << 1) == 0)
        return x;
    if (ex == 0) { /* subnormal: normalise first */
        x *= 0x1p64;
        u = ucrt_d2u(x);
        ex = (int)((u >> 52) & 0x7FF);
        *e = -64;
    }
    *e += ex - 1022;
    return ucrt_u2d((u & ~UCRT_DEXPMASK) | 0x3FE0000000000000ull);
}

/* logb and ilogb: the unbiased exponent. */
static inline double ucrt_logb(double x)
{
    int e;
    if (ucrt_isnan(x))
        return x;
    if (ucrt_isinf(x))
        return ucrt_fabs(x);
    if (x == 0) {
        ucrt_set_errno(UCRT_ERANGE);
        return -ucrt_infinity();
    }
    ucrt_frexp(x, &e);
    return (double)(e - 1);
}

static inline int ucrt_ilogb(double x)
{
    int e;
    if (ucrt_isnan(x) || x == 0)
        return (int)0x80000000u;
    if (ucrt_isinf(x))
        return 0x7FFFFFFF;
    ucrt_frexp(x, &e);
    return e - 1;
}

static inline double ucrt_nextafter(double x, double y)
{
    uint64_t ux, ax, ay;
    if (ucrt_isnan(x) || ucrt_isnan(y))
        return x + y;
    if (x == y)
        return y;
    ux = ucrt_d2u(x);
    ax = ux & ~UCRT_DSIGN;
    ay = ucrt_d2u(y) & ~UCRT_DSIGN;
    if (ax == 0) {
        if (ay == 0)
            return y;
        return ucrt_u2d(1 | (ucrt_d2u(y) & UCRT_DSIGN));
    }
    if (ax > ay || ((ux ^ ucrt_d2u(y)) & UCRT_DSIGN))
        --ux;
    else
        ++ux;
    return ucrt_u2d(ux);
}

/* ---- Remainders (exact) ---------------------------------------------------------------------------- */

static inline void ucrt_unpack(double x, uint64_t *m, int *e)
{
    uint64_t u = ucrt_d2u(x) & ~UCRT_DSIGN;
    int ex = (int)(u >> 52);
    *m = u & UCRT_DFRACMASK;
    if (ex == 0) {
        ex = 1; /* subnormal: no implicit bit */
    } else {
        *m |= 1ull << 52;
    }
    *e = ex - 1075; /* x = m * 2^e */
}

static inline double ucrt_fmod(double x, double y)
{
    uint64_t mx, my;
    int ex, ey;
    uint64_t sign = ucrt_d2u(x) & UCRT_DSIGN;
    if (ucrt_isnan(x) || ucrt_isnan(y))
        return x + y;
    if (ucrt_isinf(x) || y == 0)
        return ucrt_domain();
    if (ucrt_isinf(y) || ucrt_fabs(x) < ucrt_fabs(y))
        return x;
    if (x == 0)
        return x;
    ucrt_unpack(x, &mx, &ex);
    ucrt_unpack(y, &my, &ey);
    /* long division one bit at a time; a subnormal y can be far smaller than x's significand, so reduce with
     * a remainder whenever the quotient bit is not 0 or 1 */
    for (; ex > ey; --ex) {
        if (mx >= my)
            mx = mx >= 2 * my ? mx % my : mx - my;
        mx <<= 1;
    }
    if (mx >= my)
        mx %= my;
    if (mx == 0)
        return ucrt_u2d(sign);
    /* result = mx * 2^ey; renormalise into a double */
    while (mx < (1ull << 52) && ey > -1074) {
        mx <<= 1;
        --ey;
    }
    if (mx >= (1ull << 52))
        return ucrt_u2d(sign | ((uint64_t)(ey + 1075) << 52) | (mx & UCRT_DFRACMASK));
    return ucrt_u2d(sign | mx); /* subnormal result */
}

/* IEEE remainder: x - n*y with n the integer nearest x/y, ties to even. */
static inline double ucrt_remainder(double x, double y)
{
    double ay, ax, r, s;
    int odd;
    if (ucrt_isnan(x) || ucrt_isnan(y))
        return x + y;
    if (ucrt_isinf(x) || y == 0)
        return ucrt_domain();
    if (ucrt_isinf(y))
        return x;
    ax = ucrt_fabs(x);
    ay = ucrt_fabs(y);
    r = ay > 0x1p1022 ? ax : ucrt_fmod(ax, 2 * ay);
    odd = r >= ay; /* the quotient floor(ax/ay) is odd */
    s = odd ? r - ay : r;
    if (s > ay - s || (s == ay - s && odd))
        s -= ay;
    if (s == 0)
        return ucrt_copysign(0.0, x);
    return ucrt_signbit(x) ? -s : s; /* s may be negative: the result takes x's sign times s's */
}

/* ---- Square roots and the like ---------------------------------------------------------------------- */

static inline double ucrt_sqrt(double x)
{
    double r;
    if (x < 0)
        return ucrt_domain();
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline long double ucrt_sqrtl_(long double x)
{
    long double r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

static inline double ucrt_hypot(double x, double y)
{
    long double r;
    if (ucrt_isinf(x) || ucrt_isinf(y))
        return ucrt_infinity();
    if (ucrt_isnan(x) || ucrt_isnan(y))
        return x + y;
    r = ucrt_sqrtl_((long double)x * x + (long double)y * y);
    if (r > 1.7976931348623157e308L)
        return ucrt_range(ucrt_infinity());
    return (double)r;
}

static inline double ucrt_cbrt(double x)
{
    long double y, a;
    int e;
    if (x == 0 || !ucrt_isfinite(x))
        return x;
    a = ucrt_fabs(x);
    ucrt_frexp(a, &e);
    /* initial guess from the exponent, then Newton iterations in extended precision */
    y = (long double)ucrt_scalbn(1.0, e / 3);
    for (int i = 0; i < 12; ++i)
        y = y - (y * y * y - a) / (3 * y * y);
    return ucrt_copysign((double)y, x);
}

/* ---- Exponentials and logarithms (extended precision) ----------------------------------------------- */

#define UCRT_LN2_L 0.693147180559945309417232121458176568L
#define UCRT_LOG2E_L 1.442695040888963407359924681001892137L
#define UCRT_LOG10E_L 0.434294481903251827651128918916605082L
#define UCRT_LOG10_2_L 0.301029995663981195213738894724493027L

/* 2^k as an extended value, for |k| < 16000. */
static inline long double ucrt_pow2l(int k)
{
    union {
        long double v;
        struct {
            uint64_t mant;
            uint16_t se;
        } p;
    } u;
    u.v = 0;
    u.p.mant = 1ull << 63;
    u.p.se = (uint16_t)(16383 + k);
    return u.v;
}

static inline long double ucrt_expl_(long double x)
{
    long double kf, r, p;
    int k;
    if (x > 11000)
        return ucrt_pow2l(15000) * ucrt_pow2l(15000);
    if (x < -11000)
        return 0;
    kf = x * UCRT_LOG2E_L;
    k = (int)(kf < 0 ? kf - 0.5L : kf + 0.5L);
    r = (x - (long double)k * UCRT_LN2_HI_L) - (long double)k * UCRT_LN2_LO_L; /* k*hi is exact */
    /* Taylor series of exp(r), |r| <= 0.347 */
    p = 1 + r / 19;
    for (int n = 18; n >= 1; --n)
        p = 1 + r / n * p;
    return p * ucrt_pow2l(k);
}

static inline long double ucrt_expm1l_(long double x)
{
    long double p;
    if (x > -0.5L && x < 0.5L) {
        p = 1;
        for (int n = 24; n >= 2; --n)
            p = 1 + x / n * p;
        return x * p;
    }
    return ucrt_expl_(x) - 1;
}

static inline double ucrt_exp(double x)
{
    long double r;
    if (ucrt_isnan(x))
        return x;
    if (x > 709.782712893384)
        return ucrt_range(ucrt_infinity());
    if (x < -745.2)
        return x == -ucrt_infinity() ? 0.0 : ucrt_range(0.0);
    r = ucrt_expl_(x);
    return (double)r;
}

static inline double ucrt_exp2(double x)
{
    long double r;
    if (ucrt_isnan(x))
        return x;
    if (x >= 1024)
        return ucrt_range(ucrt_infinity());
    if (x < -1080)
        return x == -ucrt_infinity() ? 0.0 : ucrt_range(0.0);
    {
        /* split x = n + f so that the scaling by 2^n is exact and only f*ln2 is rounded */
        long double n = x < 0 ? -(long double)(int)(-x + 0.5) : (long double)(int)(x + 0.5);
        r = ucrt_expl_(((long double)x - n) * UCRT_LN2_L) * ucrt_pow2l((int)n);
    }
    return (double)r;
}

static inline double ucrt_expm1(double x)
{
    if (ucrt_isnan(x))
        return x;
    if (x > 709.782712893384)
        return ucrt_range(ucrt_infinity());
    if (x < -40)
        return -1.0;
    return (double)ucrt_expm1l_(x);
}

/* The natural logarithm of a positive finite extended value. */
static inline long double ucrt_logl_(long double a)
{
    union {
        long double v;
        struct {
            uint64_t mant;
            uint16_t se;
        } p;
    } u;
    long double m, f, s, z, sum;
    int k;
    u.v = a;
    k = (int)(u.p.se & 0x7FFF) - 16383;
    if (k == -16383) { /* a denormal extended value cannot arise from a double */
        return -11356;
    }
    u.p.se = 16383;
    m = u.v; /* in [1, 2) */
    if (m > 1.41421356237309504880L) {
        m *= 0.5L;
        ++k;
    }
    f = m - 1;
    s = f / (2 + f);
    z = s * s;
    sum = 0;
    for (int i = 17; i >= 0; --i)
        sum = sum * z + 1.0L / (2 * i + 1);
    return (long double)k * UCRT_LN2_HI_L + (2 * s * sum + (long double)k * UCRT_LN2_LO_L);
}

static inline double ucrt_log(double x)
{
    if (ucrt_isnan(x))
        return x;
    if (x < 0)
        return ucrt_domain();
    if (x == 0)
        return ucrt_range(-ucrt_infinity());
    if (ucrt_isinf(x))
        return x;
    if (x == 1.0)
        return 0.0;
    return (double)ucrt_logl_(ucrt_fpclassify(x) == UCRT_FP_SUBNORMAL ? (long double)x : (long double)x);
}

static inline double ucrt_log2(double x)
{
    if (ucrt_isnan(x))
        return x;
    if (x < 0)
        return ucrt_domain();
    if (x == 0)
        return ucrt_range(-ucrt_infinity());
    if (ucrt_isinf(x))
        return x;
    return (double)(ucrt_logl_((long double)x) * UCRT_LOG2E_L);
}

static inline double ucrt_log10(double x)
{
    if (ucrt_isnan(x))
        return x;
    if (x < 0)
        return ucrt_domain();
    if (x == 0)
        return ucrt_range(-ucrt_infinity());
    if (ucrt_isinf(x))
        return x;
    return (double)(ucrt_logl_((long double)x) * UCRT_LOG10E_L);
}

/* log(1 + t) for an extended t > -1, accurate for small t. */
static inline long double ucrt_log1pl_(long double t)
{
    if (t > -0.25L && t < 0.25L) {
        long double s = t / (2 + t), z = s * s, sum = 0;
        for (int i = 17; i >= 0; --i)
            sum = sum * z + 1.0L / (2 * i + 1);
        return 2 * s * sum;
    }
    return ucrt_logl_(1 + t);
}

static inline double ucrt_log1p(double x)
{
    if (ucrt_isnan(x))
        return x;
    if (x < -1)
        return ucrt_domain();
    if (x == -1)
        return ucrt_range(-ucrt_infinity());
    if (ucrt_isinf(x))
        return x;
    return (double)ucrt_log1pl_(x);
}

/* ---- pow ------------------------------------------------------------------------------------------- */

/* 1 when y is an odd integer, 0 for an even one, -1 when it is not an integer. */
static inline int ucrt_int_parity(double y)
{
    double a = ucrt_fabs(y);
    if (a >= 0x1p53)
        return 0; /* every double that large is even */
    if (ucrt_trunc(a) != a)
        return -1;
    return ucrt_fmod(a, 2.0) != 0.0;
}

static inline double ucrt_pow(double x, double y)
{
    int par;
    long double l, r;
    double res;
    if (y == 0)
        return 1.0;
    if (x == 1.0)
        return 1.0;
    if (ucrt_isnan(x) || ucrt_isnan(y))
        return x + y;
    par = ucrt_int_parity(y);
    if (ucrt_isinf(y)) {
        double ax = ucrt_fabs(x);
        if (ax == 1.0)
            return 1.0;
        return ((ax > 1.0) == (y > 0)) ? ucrt_infinity() : 0.0;
    }
    if (x == 0) {
        if (y < 0) {
            ucrt_set_errno(UCRT_ERANGE);
            return (par == 1) ? ucrt_copysign(ucrt_infinity(), x) : ucrt_infinity();
        }
        return (par == 1) ? x : 0.0;
    }
    if (ucrt_isinf(x)) {
        if (x > 0)
            return y > 0 ? x : 0.0;
        if (y > 0)
            return par == 1 ? x : ucrt_infinity();
        return par == 1 ? -0.0 : 0.0;
    }
    if (x < 0 && par < 0)
        return ucrt_domain();
    l = ucrt_logl_((long double)ucrt_fabs(x)) * (long double)y;
    if (l > 710) {
        ucrt_set_errno(UCRT_ERANGE);
        return (x < 0 && par == 1) ? -ucrt_infinity() : ucrt_infinity();
    }
    if (l < -746) {
        ucrt_set_errno(UCRT_ERANGE);
        return (x < 0 && par == 1) ? -0.0 : 0.0;
    }
    r = ucrt_expl_(l);
    res = (double)r;
    if (ucrt_isinf(res))
        ucrt_set_errno(UCRT_ERANGE);
    else if (res == 0)
        ucrt_set_errno(UCRT_ERANGE);
    return (x < 0 && par == 1) ? -res : res;
}

/* ---- Hyperbolic functions -------------------------------------------------------------------------- */

static inline double ucrt_sinh(double x)
{
    long double em, r;
    double a = ucrt_fabs(x);
    if (ucrt_isnan(x) || ucrt_isinf(x) || x == 0)
        return x;
    if (a > 710.4758600739439)
        return ucrt_range(ucrt_copysign(ucrt_infinity(), x));
    em = ucrt_expm1l_(a);
    r = a < 0.5 ? (em + em / (em + 1)) / 2 : (ucrt_expl_(a) - 1 / ucrt_expl_(a)) / 2;
    return ucrt_copysign((double)r, x);
}

static inline double ucrt_cosh(double x)
{
    long double e;
    double a = ucrt_fabs(x);
    if (ucrt_isnan(x))
        return x;
    if (ucrt_isinf(x))
        return ucrt_infinity();
    if (a > 710.4758600739439)
        return ucrt_range(ucrt_infinity());
    e = ucrt_expl_(a);
    return (double)((e + 1 / e) / 2);
}

static inline double ucrt_tanh(double x)
{
    long double em;
    double a = ucrt_fabs(x);
    if (ucrt_isnan(x) || x == 0)
        return x;
    if (a > 22)
        return ucrt_copysign(1.0, x);
    em = ucrt_expm1l_(2 * (long double)a);
    return ucrt_copysign((double)(em / (em + 2)), x);
}

static inline double ucrt_asinh(double x)
{
    long double a, r;
    if (ucrt_isnan(x) || ucrt_isinf(x) || x == 0)
        return x;
    a = ucrt_fabs(x);
    if (a > 1e18L)
        r = ucrt_logl_(a) + UCRT_LN2_L;
    else /* asinh(a) = log1p(a + a^2 / (1 + sqrt(1 + a^2))) */
        r = ucrt_log1pl_(a + a * a / (1 + ucrt_sqrtl_(1 + a * a)));
    return ucrt_copysign((double)r, x);
}

static inline double ucrt_acosh(double x)
{
    long double a;
    if (ucrt_isnan(x))
        return x;
    if (x < 1)
        return ucrt_domain();
    if (ucrt_isinf(x))
        return x;
    a = x;
    if (a > 1e18L)
        return (double)(ucrt_logl_(a) + UCRT_LN2_L);
    {
        long double t = a - 1; /* exact */
        return (double)ucrt_log1pl_(t + ucrt_sqrtl_(t * (t + 2)));
    }
}

static inline double ucrt_atanh(double x)
{
    long double a, r;
    if (ucrt_isnan(x))
        return x;
    a = ucrt_fabs(x);
    if (a > 1)
        return ucrt_domain();
    if (a == 1)
        return ucrt_range(ucrt_copysign(ucrt_infinity(), x));
    if (x == 0)
        return x;
    /* atanh(a) = 0.5 * log1p(2a / (1 - a)) */
    r = ucrt_log1pl_(2 * a / (1 - a)) / 2;
    return ucrt_copysign((double)r, x);
}

/* ---- Trigonometric functions ------------------------------------------------------------------------ */

#define UCRT_TWO_OVER_PI_L 0.636619772367581343075535053490057448L

/* Bits [pos, pos + count) of a little-endian array of 32-bit limbs, count <= 64. */
static inline uint64_t ucrt_get_bits(const uint32_t *limbs, int pos, int count)
{
    uint64_t v = 0;
    for (int i = 0; i < count; ++i) {
        int b = pos + i;
        v |= (uint64_t)((limbs[b >> 5] >> (b & 31)) & 1u) << i;
    }
    return v;
}

/* Payne-Hanek: the quadrant (x * 2/pi rounded to an integer, mod 4) and the remainder y = x - n*pi/2 for a double
 * of any size. x*2/pi is formed exactly from enough bits of 2/pi that the fraction keeps 64 significant bits even
 * when it is close to zero. */
static inline int ucrt_rem_pio2_big(double x, long double *y)
{
    enum { WINDOW = 7 };                  /* limbs of 2/pi used: 224 bits */
    uint64_t m;
    int e, i0, s, n;
    uint32_t al[WINDOW];                  /* the same, little-endian */
    uint32_t pl[WINDOW + 2];
    uint64_t carry;
    uint32_t mlo, mhi;
    long double f;
    ucrt_unpack(x, &m, &e);               /* |x| = m * 2^e, e >= -32 for the arguments that reach here */
    i0 = e - 1 < 1 ? 1 : e - 1;
    for (int j = 0; j < WINDOW; ++j) {
        int p = (i0 - 1) + 32 * j, wi = p >> 5, sh = p & 31;
        uint32_t v = ucrt_two_over_pi[wi] << sh;
        if (sh)
            v |= ucrt_two_over_pi[wi + 1] >> (32 - sh);
        al[WINDOW - 1 - j] = v;
    }
    /* pl = al * m */
    mlo = (uint32_t)m;
    mhi = (uint32_t)(m >> 32);
    for (int k = 0; k < WINDOW + 2; ++k)
        pl[k] = 0;
    carry = 0;
    for (int k = 0; k < WINDOW; ++k) {
        uint64_t t = (uint64_t)al[k] * mlo + carry;
        pl[k] = (uint32_t)t;
        carry = t >> 32;
    }
    pl[WINDOW] = (uint32_t)carry;
    carry = 0;
    for (int k = 0; k < WINDOW; ++k) {
        uint64_t t = (uint64_t)al[k] * mhi + pl[k + 1] + carry;
        pl[k + 1] = (uint32_t)t;
        carry = t >> 32;
    }
    pl[WINDOW + 1] = (uint32_t)carry;
    s = i0 + 32 * WINDOW - 1 - e;         /* bit s of the product is the units bit of x * 2/pi */
    n = (int)ucrt_get_bits(pl, s, 2);
    {
        /* The fraction as a 128-bit integer. When it is at least one half the nearest integer is n + 1 and the
         * remainder is negative; take the magnitude from the integer, so a result near zero keeps all its bits. */
        unsigned __int128 v = ((unsigned __int128)ucrt_get_bits(pl, s - 64, 64) << 64) | ucrt_get_bits(pl, s - 128, 64);
        int neg = 0, lz = 0;
        uint64_t top;
        if (v >> 127) {
            neg = 1;
            ++n;
            v = (unsigned __int128)0 - v;
        }
        while (v && !(v >> 127)) {
            v <<= 1;
            ++lz;
        }
        top = (uint64_t)(v >> 64);
        f = v ? (long double)top * ucrt_pow2l(-64 - lz) : 0;
        if (neg)
            f = -f;
    }
    *y = f * UCRT_PIO2_LD;
    if (ucrt_signbit(x)) {
        n = -n;
        *y = -*y;
    }
    return n & 3;
}

/* x = n*pi/2 + y with |y| <= pi/4; returns n mod 4. */
static inline int ucrt_rem_pio2(double x, long double *y)
{
    long double xl, kf, r;
    int64_t n;
    if (ucrt_fabs(x) >= 1048576.0)
        return ucrt_rem_pio2_big(x, y);
    xl = x;
    kf = xl * UCRT_TWO_OVER_PI_L;
    n = (int64_t)(kf < 0 ? kf - 0.5L : kf + 0.5L);
    r = xl - (long double)n * UCRT_PIO2_1;
    r -= (long double)n * UCRT_PIO2_2;
    r -= (long double)n * UCRT_PIO2_3;
    r -= (long double)n * UCRT_PIO2_4;
    *y = r;
    return (int)(n & 3);
}

static inline long double ucrt_sin_k(long double y)
{
    long double y2 = y * y, p = 1;
    for (int k = 13; k >= 1; --k)
        p = 1 - y2 / ((2 * k) * (2 * k + 1)) * p;
    return y * p;
}

static inline long double ucrt_cos_k(long double y)
{
    long double y2 = y * y, p = 1;
    for (int k = 13; k >= 1; --k)
        p = 1 - y2 / ((2 * k - 1) * (2 * k)) * p;
    return p;
}

static inline double ucrt_sin(double x)
{
    long double y, r;
    int q;
    if (ucrt_isnan(x))
        return x;
    if (ucrt_isinf(x))
        return ucrt_domain();
    if (ucrt_fabs(x) < 0x1p-27)
        return x;
    q = ucrt_rem_pio2(x, &y);
    r = (q & 1) ? ucrt_cos_k(y) : ucrt_sin_k(y);
    return (double)((q & 2) ? -r : r);
}

static inline double ucrt_cos(double x)
{
    long double y, r;
    int q;
    if (ucrt_isnan(x))
        return x;
    if (ucrt_isinf(x))
        return ucrt_domain();
    if (ucrt_fabs(x) < 0x1p-27)
        return 1.0;
    q = ucrt_rem_pio2(x, &y);
    r = (q & 1) ? ucrt_sin_k(y) : ucrt_cos_k(y);
    return (double)(((q + 1) & 2) ? -r : r);
}

static inline double ucrt_tan(double x)
{
    long double y, r;
    int q;
    if (ucrt_isnan(x))
        return x;
    if (ucrt_isinf(x))
        return ucrt_domain();
    if (ucrt_fabs(x) < 0x1p-27)
        return x;
    q = ucrt_rem_pio2(x, &y);
    r = (q & 1) ? -ucrt_cos_k(y) / ucrt_sin_k(y) : ucrt_sin_k(y) / ucrt_cos_k(y);
    return (double)r;
}

/* atan of a non-negative extended value. */
static inline long double ucrt_atanl_(long double a)
{
    long double a2, p, r;
    int inv = 0;
    if (a > 1) {
        a = 1 / a;
        inv = 1;
    }
    /* two halvings of the angle: atan(a) = 2 atan(a / (1 + sqrt(1 + a^2))) */
    a = a / (1 + ucrt_sqrtl_(1 + a * a));
    a = a / (1 + ucrt_sqrtl_(1 + a * a));
    a2 = a * a;
    p = 1;
    for (int k = 16; k >= 1; --k)
        p = 1 - a2 * (2 * k - 1) / (2 * k + 1) * p;
    r = 4 * a * p;
    return inv ? UCRT_PIO2_LD - r : r;
}

static inline double ucrt_atan(double x)
{
    if (ucrt_isnan(x) || x == 0)
        return x;
    if (ucrt_isinf(x))
        return (double)ucrt_copysign((double)UCRT_PIO2_LD, x);
    return ucrt_copysign((double)ucrt_atanl_(ucrt_fabs(x)), x);
}

static inline double ucrt_atan2(double y, double x)
{
    long double t;
    double ay = ucrt_fabs(y), ax = ucrt_fabs(x);
    int xneg = ucrt_signbit(x);
    if (ucrt_isnan(x) || ucrt_isnan(y))
        return x + y;
    if (y == 0)
        return xneg ? ucrt_copysign((double)UCRT_PI_LD, y) : y;
    if (x == 0)
        return ucrt_copysign((double)UCRT_PIO2_LD, y);
    if (ucrt_isinf(x)) {
        if (ucrt_isinf(y))
            return ucrt_copysign((double)(xneg ? 3 * UCRT_PIO2_LD / 2 : UCRT_PIO2_LD / 2), y);
        return xneg ? ucrt_copysign((double)UCRT_PI_LD, y) : ucrt_copysign(0.0, y);
    }
    if (ucrt_isinf(y))
        return ucrt_copysign((double)UCRT_PIO2_LD, y);
    t = ucrt_atanl_((long double)ay / (long double)ax);
    if (xneg)
        t = UCRT_PI_LD - t;
    return ucrt_copysign((double)t, y);
}

static inline double ucrt_asin(double x)
{
    long double a, c;
    if (ucrt_isnan(x))
        return x;
    a = ucrt_fabs(x);
    if (a > 1)
        return ucrt_domain();
    if (x == 0)
        return x;
    if (a == 1)
        return ucrt_copysign((double)UCRT_PIO2_LD, x);
    c = ucrt_sqrtl_((1 - a) * (1 + a));
    return ucrt_copysign((double)ucrt_atanl_(a / c), x);
}

static inline double ucrt_acos(double x)
{
    long double a, c, t;
    if (ucrt_isnan(x))
        return x;
    a = ucrt_fabs(x);
    if (a > 1)
        return ucrt_domain();
    if (x == 1)
        return 0.0;
    if (x == -1)
        return (double)UCRT_PI_LD;
    if (x == 0)
        return (double)UCRT_PIO2_LD;
    c = ucrt_sqrtl_((1 - a) * (1 + a));
    t = ucrt_atanl_(c / a);
    return (double)(x < 0 ? UCRT_PI_LD - t : t);
}

#endif /* UCRT_MATH_H */
