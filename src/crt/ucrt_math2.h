/*
 * ucrt_math2.h - the rest of the C99 math library: fma, fmin/fmax/fdim, the integer conversions (lrint, lround
 * and the long long forms, with Windows's 32-bit long), erf, erfc, tgamma and lgamma, nan, and the float
 * versions. Host tests: tests/crt-host/test_math2.c.
 */
#ifndef UCRT_MATH2_H
#define UCRT_MATH2_H

#include "ucrt_math.h"

typedef unsigned __int128 ucrt_u128;

/* Rounds (-1)^sign * mag * 2^exp to the nearest double, ties to even, exactly, including subnormals, overflow
 * to infinity and underflow to zero. mag has its leading bit at 126 or below. */
static inline double ucrt_round_pack(int sign, ucrt_u128 mag, int exp)
{
    int msb = 127, drop;
    uint64_t m;
    double r;
    if (mag == 0)
        return sign ? -0.0 : 0.0;
    while (!(mag >> msb))
        --msb;
    if (exp + msb > 1023)
        return sign ? -ucrt_infinity() : ucrt_infinity();
    drop = msb - 52;                      /* low bits below the 53-bit significand */
    if (exp + drop < -1074)
        drop = -1074 - exp;               /* subnormal: the unit in the last place is 2^-1074 */
    if (drop <= 0) {
        m = (uint64_t)(mag << -drop);
    } else if (drop > 127) {
        return sign ? -0.0 : 0.0;
    } else {
        ucrt_u128 low = mag & (((ucrt_u128)1 << drop) - 1), half = (ucrt_u128)1 << (drop - 1);
        m = (uint64_t)(mag >> drop);
        if (low > half || (low == half && (m & 1)))
            ++m;
    }
    r = ucrt_scalbn((double)m, exp + drop); /* m <= 2^53, so the conversion is exact; the scaling is too */
    return sign ? -r : r;
}

/* x*y + z with a single rounding. */
static inline double ucrt_fma(double x, double y, double z)
{
    uint64_t mx, my, mz;
    int ex, ey, ez, ps, zs, shift, d, bs, ss, eb, sign;
    ucrt_u128 p, zm, big, small, r;
    if (!ucrt_isfinite(x) || !ucrt_isfinite(y) || x == 0 || y == 0)
        return x * y + z;
    if (!ucrt_isfinite(z))
        return z; /* a finite product, however large, cannot change an infinity or a NaN */
    ps = ucrt_signbit(x) ^ ucrt_signbit(y);
    ucrt_unpack(x, &mx, &ex);
    ucrt_unpack(y, &my, &ey);
    p = (ucrt_u128)mx * my;
    ex += ey; /* the product's exponent */
    if (z == 0)
        return ucrt_round_pack(ps, p, ex);
    zs = ucrt_signbit(z);
    ucrt_unpack(z, &mz, &ez);
    /* put both leading bits at bit 126 */
    shift = 126;
    while (!(p >> shift))
        --shift;
    p <<= 126 - shift;
    ex -= 126 - shift;
    zm = mz;
    shift = 126;
    while (!(zm >> shift))
        --shift;
    zm <<= 126 - shift;
    ez -= 126 - shift;
    if (ex >= ez) {
        big = p;
        bs = ps;
        eb = ex;
        small = zm;
        ss = zs;
        d = ex - ez;
    } else {
        big = zm;
        bs = zs;
        eb = ez;
        small = p;
        ss = ps;
        d = ez - ex;
    }
    if (d >= 127) {
        small = 1; /* only its presence matters: a sticky bit */
    } else if (d > 0) {
        ucrt_u128 lost = small & (((ucrt_u128)1 << d) - 1);
        small = (small >> d) | (lost != 0);
    }
    if (bs == ss) {
        r = big + small;
        sign = bs;
    } else if (big >= small) {
        r = big - small;
        sign = bs;
    } else {
        r = small - big;
        sign = ss;
    }
    if (r == 0)
        return 0.0;
    return ucrt_round_pack(sign, r, eb);
}

/* ---- Minimum, maximum, difference ------------------------------------------------------------------- */

static inline double ucrt_fmin(double x, double y)
{
    if (ucrt_isnan(x))
        return y;
    if (ucrt_isnan(y))
        return x;
    if (x == y)
        return ucrt_signbit(x) ? x : y;
    return x < y ? x : y;
}

static inline double ucrt_fmax(double x, double y)
{
    if (ucrt_isnan(x))
        return y;
    if (ucrt_isnan(y))
        return x;
    if (x == y)
        return ucrt_signbit(x) ? y : x;
    return x > y ? x : y;
}

static inline double ucrt_fdim(double x, double y)
{
    double d;
    if (ucrt_isnan(x) || ucrt_isnan(y))
        return x + y;
    if (!(x > y))
        return 0.0;
    d = x - y;
    if (ucrt_isinf(d) && !ucrt_isinf(x))
        ucrt_set_errno(UCRT_ERANGE);
    return d;
}

/* nan("payload"): a quiet NaN whose payload is the decimal number in the string, if it is one. */
static inline double ucrt_nan(const char *tag)
{
    uint64_t payload = 0;
    if (tag) {
        for (const char *p = tag; *p >= '0' && *p <= '9'; ++p)
            payload = payload * 10 + (uint64_t)(*p - '0');
    }
    return ucrt_u2d(0x7FF8000000000000ull | (payload & 0x0007FFFFFFFFFFFFull));
}

/* ---- Conversions to integers ------------------------------------------------------------------------ */

/* The values the hardware instruction gives when the result does not fit: the "integer indefinite". */
static inline int64_t ucrt_llrint(double x)
{
    double r = ucrt_rint(x);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return (int64_t)0x8000000000000000ull;
    return (int64_t)r;
}

static inline int32_t ucrt_lrint(double x)
{
    double r = ucrt_rint(x);
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return (int32_t)0x80000000u;
    return (int32_t)r;
}

static inline int64_t ucrt_llround(double x)
{
    double r = ucrt_round(x);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return (int64_t)0x8000000000000000ull;
    return (int64_t)r;
}

static inline int32_t ucrt_lround(double x)
{
    double r = ucrt_round(x);
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return (int32_t)0x80000000u;
    return (int32_t)r;
}

/* ---- Error function --------------------------------------------------------------------------------- */

#define UCRT_TWO_OVER_SQRTPI_L 1.12837916709551257389615890312154517L
#define UCRT_ONE_OVER_SQRTPI_L 0.564189583547756286948079451560772586L

/* exp(-x^2) with x^2 split exactly, so a large x does not lose bits to the squaring. */
static inline long double ucrt_exp_negsq(long double x)
{
    long double xh = (long double)(double)x, xl;
    /* keep 26 bits in xh so xh*xh is exact in extended precision */
    union {
        double d;
        uint64_t u;
    } c;
    c.d = (double)x;
    c.u &= ~0x7FFFFFFull;
    xh = c.d;
    xl = x - xh;
    return ucrt_expl_(-(xh * xh)) * ucrt_expl_(-(2 * xh * xl + xl * xl));
}

/* erf by the series with positive terms, accurate (relative) for |x| < 6. */
static inline long double ucrt_erf_series(long double x)
{
    long double x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 400; ++n) {
        term *= 2 * x2 / (2 * n + 1);
        sum += term;
        if (term < sum * 1e-22L)
            break;
    }
    return UCRT_TWO_OVER_SQRTPI_L * ucrt_exp_negsq(x) * sum;
}

/* erfc by its continued fraction, for x >= 1.5. */
static inline long double ucrt_erfc_cf(long double x)
{
    long double t = x;
    for (int k = 250; k >= 1; --k)
        t = x + (k * 0.5L) / t;
    return UCRT_ONE_OVER_SQRTPI_L * ucrt_exp_negsq(x) / t;
}

static inline double ucrt_erf(double x)
{
    double a = ucrt_fabs(x);
    if (ucrt_isnan(x) || x == 0)
        return x;
    if (a >= 6)
        return ucrt_copysign(1.0, x);
    return ucrt_copysign((double)ucrt_erf_series(a), x);
}

static inline double ucrt_erfc(double x)
{
    long double a;
    if (ucrt_isnan(x))
        return x;
    if (x >= 27.3)
        return ucrt_range(0.0);
    if (x <= -6)
        return 2.0;
    a = ucrt_fabs(x);
    if (a < 1.5L) {
        long double e = ucrt_erf_series(a);
        return (double)(x < 0 ? 1 + e : 1 - e);
    }
    {
        long double c = ucrt_erfc_cf(a);
        return (double)(x < 0 ? 2 - c : c);
    }
}

/* ---- Gamma function --------------------------------------------------------------------------------- */

#define UCRT_HALF_LN_2PI_L 0.918938533204672741780329736405617639L
#define UCRT_PI_L 3.14159265358979323846264338327950288L

/* ln gamma(z) for z >= 0.25 by the Stirling series after shifting z above 25 with the recurrence. */
static inline long double ucrt_lgamma_stirling(long double z)
{
    static const long double c[] = {
        0.0833333333333333333333333333333333L,      /* B2 / (2 * 1) */
        -0.00277777777777777777777777777777778L,    /* B4 / (4 * 3) */
        0.000793650793650793650793650793650794L,    /* B6 / (6 * 5) */
        -0.000595238095238095238095238095238095L,   /* B8 / (8 * 7) */
        0.000841750841750841750841750841750842L,    /* B10 / (10 * 9) */
        -0.00191752691752691752691752691752692L,    /* B12 / (12 * 11) */
        0.00641025641025641025641025641025641L,     /* B14 / (14 * 13) */
        -0.0295506535947712418300653594771242L,     /* B16 / (16 * 15) */
        0.179644372368830573164938490016825L,       /* B18 / (18 * 17) */
        -1.39243221690590111642743221690590L,       /* B20 / (20 * 19) */
        13.4028640441683919944789510006901L,        /* B22 / (22 * 21) */
    };
    long double prod = 1, w, series = 0, zi;
    while (z < 25) {
        prod *= z;
        z += 1;
    }
    zi = 1 / z;
    w = zi * zi;
    for (int k = (int)(sizeof(c) / sizeof(c[0])) - 1; k >= 0; --k)
        series = series * w + c[k];
    return (z - 0.5L) * ucrt_logl_(z) - z + UCRT_HALF_LN_2PI_L + series * zi - ucrt_logl_(prod);
}

/* ln gamma(1 + t) for |t| <= 0.25, and ln gamma(2 + t): Taylor series with the zeta values. */
static inline long double ucrt_lgamma_taylor(long double t, int about2)
{
    long double sum = 0, tk = t * t;
    long double lead = about2 ? 1 - UCRT_EULER_GAMMA_L : -UCRT_EULER_GAMMA_L;
    for (int k = 2; k <= 41; ++k) {
        long double zk = ucrt_zeta_m1[k - 2];
        long double term = (about2 ? zk : zk + 1) * tk / k;
        sum += (k & 1) ? -term : term;
        tk *= t;
    }
    return lead * t + sum;
}

/* ln |gamma(x)| for a finite x that is not a pole; *sign receives the sign of gamma(x). */
static inline long double ucrt_lgammal_(long double x, int *sign)
{
    int dummy;
    *sign = 1;
    if (x > 0) {
        if (x > 0.75L && x < 1.25L)
            return ucrt_lgamma_taylor(x - 1, 0);
        if (x > 1.75L && x < 2.25L)
            return ucrt_lgamma_taylor(x - 2, 1);
        return ucrt_lgamma_stirling(x);
    }
    /* reflection: gamma(x) gamma(1 - x) = pi / sin(pi x); on (n, n + 1) the sign of sin(pi x) is (-1)^n */
    {
        double fl = ucrt_floor((double)x);
        long double n = x - ucrt_rint((double)x); /* the distance to the nearest integer, exact */
        if (n < 0)
            n = -n;
        *sign = ucrt_fmod(ucrt_fabs(fl), 2.0) != 0 ? -1 : 1;
        return ucrt_logl_(UCRT_PI_L / ucrt_sin_k(n * UCRT_PI_L)) - ucrt_lgammal_(1 - x, &dummy);
    }
}

static inline double ucrt_lgamma_r(double x, int *sign)
{
    long double v;
    *sign = 1;
    if (ucrt_isnan(x))
        return x;
    if (ucrt_isinf(x))
        return ucrt_infinity();
    if (x == 0 || (x < 0 && ucrt_trunc(x) == x)) {
        *sign = ucrt_signbit(x) ? -1 : 1;
        if (x != 0)
            *sign = 1;
        return ucrt_range(ucrt_infinity());
    }
    if (x > 2.5e305)
        return ucrt_range(ucrt_infinity());
    v = ucrt_lgammal_(x, sign);
    return (double)v;
}

static inline double ucrt_lgamma(double x)
{
    int sign;
    return ucrt_lgamma_r(x, &sign);
}

static inline double ucrt_tgamma(double x)
{
    long double v;
    int sign;
    if (ucrt_isnan(x))
        return x;
    if (ucrt_isinf(x))
        return x > 0 ? x : ucrt_domain();
    if (x == 0)
        return ucrt_range(ucrt_copysign(ucrt_infinity(), x));
    if (x < 0 && ucrt_trunc(x) == x)
        return ucrt_domain();
    if (x > 171.7)
        return ucrt_range(ucrt_infinity());
    if (x < -190)
        return ucrt_range(ucrt_fmod(ucrt_fabs(ucrt_floor(x)), 2.0) != 0 ? -0.0 : 0.0);
    v = ucrt_lgammal_(x, &sign);
    v = ucrt_expl_(v);
    if (sign < 0)
        v = -v;
    if (ucrt_fabs((double)v) == ucrt_infinity() || (double)v == 0)
        ucrt_set_errno(UCRT_ERANGE);
    return (double)v;
}

/* ---- The float versions ----------------------------------------------------------------------------- */

/* Each is the double function rounded to float: sqrt, the rounding functions, fabs and so on are exact; the rest are
 * correctly rounded except in the rare case where the double result lies exactly between two floats. */
#define UCRT_FLOAT1(name) \
    static inline float ucrt_##name##f(float x) { return (float)ucrt_##name((double)x); }
#define UCRT_FLOAT2(name) \
    static inline float ucrt_##name##f(float x, float y) { return (float)ucrt_##name((double)x, (double)y); }

UCRT_FLOAT1(sin) UCRT_FLOAT1(cos) UCRT_FLOAT1(tan) UCRT_FLOAT1(asin) UCRT_FLOAT1(acos) UCRT_FLOAT1(atan)
UCRT_FLOAT1(sinh) UCRT_FLOAT1(cosh) UCRT_FLOAT1(tanh) UCRT_FLOAT1(asinh) UCRT_FLOAT1(acosh) UCRT_FLOAT1(atanh)
UCRT_FLOAT1(exp) UCRT_FLOAT1(exp2) UCRT_FLOAT1(expm1) UCRT_FLOAT1(log) UCRT_FLOAT1(log2) UCRT_FLOAT1(log10)
UCRT_FLOAT1(log1p) UCRT_FLOAT1(sqrt) UCRT_FLOAT1(cbrt) UCRT_FLOAT1(fabs) UCRT_FLOAT1(floor) UCRT_FLOAT1(ceil)
UCRT_FLOAT1(trunc) UCRT_FLOAT1(round) UCRT_FLOAT1(rint) UCRT_FLOAT1(erf) UCRT_FLOAT1(erfc) UCRT_FLOAT1(tgamma)
UCRT_FLOAT1(lgamma) UCRT_FLOAT1(logb)
UCRT_FLOAT2(atan2) UCRT_FLOAT2(pow) UCRT_FLOAT2(hypot) UCRT_FLOAT2(fmod) UCRT_FLOAT2(remainder)
UCRT_FLOAT2(copysign) UCRT_FLOAT2(fmin) UCRT_FLOAT2(fmax) UCRT_FLOAT2(fdim)

static inline float ucrt_fmaf(float x, float y, float z)
{
    return (float)ucrt_fma((double)x, (double)y, (double)z);
}

static inline float ucrt_ldexpf(float x, int n)
{
    return (float)ucrt_ldexp((double)x, n);
}

static inline float ucrt_frexpf(float x, int *e)
{
    return (float)ucrt_frexp((double)x, e);
}

static inline float ucrt_modff(float x, float *ip)
{
    double i, f = ucrt_modf((double)x, &i);
    *ip = (float)i;
    return (float)f;
}

#endif /* UCRT_MATH2_H */
