/*
 * ucrt_strtod.h - strtod, strtof, atof and the wide forms, with exact rounding. See ucrt_strtod_t.h.
 * The rounding of a scaled integer into a binary format is ucrt_fp_round, shared by the decimal and
 * hexadecimal paths. Host tests: tests/crt-host/test_strtod.c against glibc.
 */
#ifndef UCRT_STRTOD_H
#define UCRT_STRTOD_H

#include "ucrt_bignum.h"
#include "ucrt_conv.h"

#define UCRT_STRTOD_MAXDIG 780
#define UCRT_FP_OVERFLOW 1
#define UCRT_FP_UNDERFLOW 2
#define UCRT_FP_INEXACT 4

/* A binary floating point format: precision in bits (with the hidden bit), the number of exponent bits, the
 * exponent of the smallest denormal's last place, the largest exponent of a top bit, the bias, and the decimal
 * magnitudes beyond which the result is certainly infinity or zero. */
typedef struct ucrt_fp_fmt {
    int prec;
    int exp_bits;
    int min_ulp_exp;
    int max_top_exp;
    int bias;
    int max_dec;
    int min_dec;
} ucrt_fp_fmt;

static const ucrt_fp_fmt ucrt_fmt_double = {53, 11, -1074, 1023, 1023, 310, -330};
static const ucrt_fp_fmt ucrt_fmt_float = {24, 8, -149, 127, 127, 40, -50};

static inline int ucrt_u128_bits(unsigned __int128 v)
{
    int b = 0;
    if (v >> 64) {
        b = 64;
        v >>= 64;
    }
    uint64_t t = (uint64_t)v;
    while (t) {
        ++b;
        t >>= 1;
    }
    return b;
}

/* Rounds q * 2^e2 (q > 0) to the format, half to even; sticky says the true value is slightly above that.
 * Returns the encoding without the sign bit and sets overflow and underflow flags. */
static inline uint64_t ucrt_fp_round(unsigned __int128 q, int e2, int sticky, const ucrt_fp_fmt *f, int *flags)
{
    int bits = ucrt_u128_bits(q);
    int t = bits - 1 + e2;
    int step = t - (f->prec - 1);
    int sh;
    unsigned __int128 m;
    int exact = !sticky;
    if (step < f->min_ulp_exp)
        step = f->min_ulp_exp;
    sh = step - e2;
    if (sh <= 0) {
        m = q << (unsigned)(-sh);
    } else if (sh > 128) {
        m = 0; /* far below half of the smallest step */
        exact = 0;
    } else {
        unsigned __int128 half = (unsigned __int128)1 << (sh - 1);
        unsigned __int128 rem, up;
        if (sh == 128) {
            m = 0;
            rem = q;
        } else {
            m = q >> sh;
            rem = q & (((unsigned __int128)1 << sh) - 1);
        }
        if (rem)
            exact = 0;
        up = rem > half || (rem == half && (sticky || (m & 1)));
        m += up;
    }
    (void)exact;
    if (m == 0) {
        *flags |= UCRT_FP_UNDERFLOW;
        return 0;
    }
    if (m >> f->prec) { /* rounding carried into a new bit */
        m >>= 1;
        ++step;
    }
    {
        int top = ucrt_u128_bits(m) - 1 + step;
        uint64_t mm = (uint64_t)m;
        uint64_t frac_bits = (uint64_t)1 << (f->prec - 1);
        if (top > f->max_top_exp) {
            *flags |= UCRT_FP_OVERFLOW;
            return (((uint64_t)1 << f->exp_bits) - 1) << (f->prec - 1);
        }
        if (mm < frac_bits)
            return mm; /* denormal */
        return ((uint64_t)(top + f->bias) << (f->prec - 1)) | (mm - frac_bits);
    }
}

#define UCRT_T char
#define UCRT_P ucrt_str
#define UCRT_ISSPACE(c) ucrt_isspace((unsigned char)(c))
#include "ucrt_strtod_t.h"
#undef UCRT_T
#undef UCRT_P
#undef UCRT_ISSPACE

#define UCRT_T ucrt_wchar
#define UCRT_P ucrt_wcs
#define UCRT_ISSPACE(c) ucrt_iswspace((ucrt_wchar)(c))
#include "ucrt_strtod_t.h"
#undef UCRT_T
#undef UCRT_P
#undef UCRT_ISSPACE

static inline double ucrt_atof(const char *s)
{
    return ucrt_strtod(s, NULL);
}

#endif /* UCRT_STRTOD_H */
