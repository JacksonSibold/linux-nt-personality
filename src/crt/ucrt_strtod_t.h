/*
 * ucrt_strtod_t.h - strtod, strtof and the wide forms: correctly rounded decimal and hexadecimal text to binary.
 *
 * Included by ucrt_strtod.h with UCRT_T, UCRT_P and UCRT_ISSPACE set. The conversion is exact: the decimal
 * digits (up to UCRT_STRTOD_MAXDIG of them, with a sticky digit for the rest) become a big integer, scaled by a
 * power of ten, and rounded half to even into the target format, denormals included. A fast path handles the
 * common case of at most 15 digits and a small exponent with one double multiplication or division.
 */
#define UCRT_CAT_(a, b) a##b
#define UCRT_CAT(a, b) UCRT_CAT_(a, b)
#define UCRT_FN(name) UCRT_CAT(UCRT_P, name)

static inline int UCRT_FN(lower_ascii)(UCRT_T c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Matches the lower-case ASCII word at *p, ignoring case; advances *p over it on success. */
static inline int UCRT_FN(match_word)(const UCRT_T **p, const char *word)
{
    const UCRT_T *q = *p;
    for (; *word; ++word, ++q)
        if (UCRT_FN(lower_ascii)(*q) != *word)
            return 0;
    *p = q;
    return 1;
}

/* Parses a floating point number at s. Returns the encoded bits of the target format (sign included), sets
 * *end to just after the number (or to s when there is none), and *flags: UCRT_FP_OVERFLOW when the magnitude
 * is too large (the result is infinity), UCRT_FP_UNDERFLOW when a nonzero value became zero. */
static inline uint64_t UCRT_FN(parse_fp)(const UCRT_T *s, const UCRT_T **end, const ucrt_fp_fmt *f, int *flags)
{
    const UCRT_T *p = s;
    int neg = 0, seen = 0;
    uint64_t sign_bit = (uint64_t)1 << (f->exp_bits + f->prec - 1);
    uint64_t inf_bits = (((uint64_t)1 << f->exp_bits) - 1) << (f->prec - 1);
    *flags = 0;
    if (end)
        *end = s;
    while (UCRT_ISSPACE(*p))
        ++p;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        ++p;
    }
    /* infinity and nan */
    {
        const UCRT_T *q = p;
        if (UCRT_FN(match_word)(&q, "inf")) {
            const UCRT_T *r = q;
            if (UCRT_FN(match_word)(&r, "inity"))
                q = r;
            if (end)
                *end = q;
            return inf_bits | (neg ? sign_bit : 0);
        }
        q = p;
        if (UCRT_FN(match_word)(&q, "nan")) {
            uint64_t quiet = (uint64_t)1 << (f->prec - 2);
            uint64_t bits = inf_bits | quiet;
            int indefinite = 0, signalling = 0;
            if (*q == '(') {
                const UCRT_T *r = q + 1;
                const UCRT_T *w = r;
                while ((*r >= '0' && *r <= '9') || (*r >= 'a' && *r <= 'z') || (*r >= 'A' && *r <= 'Z') || *r == '_')
                    ++r;
                if (*r == ')') {
                    const UCRT_T *a = w;
                    if (UCRT_FN(match_word)(&a, "ind") && a == r)
                        indefinite = 1;
                    a = w;
                    if (UCRT_FN(match_word)(&a, "snan") && a == r)
                        signalling = 1;
                    q = r + 1;
                }
            }
            if (signalling)
                bits = inf_bits | ((uint64_t)1 << (f->prec - 3));
            if (end)
                *end = q;
            return bits | ((neg || indefinite) ? sign_bit : 0);
        }
    }
    /* hexadecimal: 0x[hex][.hex][p[+-]dec] */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        const UCRT_T *q = p + 2;
        unsigned __int128 mant = 0;
        int stored = 0, sticky = 0, bin_exp = 0, any = 0, in_frac = 0;
        for (;; ++q) {
            int d;
            if (*q == '.' && !in_frac) {
                in_frac = 1;
                continue;
            }
            d = UCRT_FN(digit_value)(*q);
            if (d < 0 || d >= 16)
                break;
            any = 1;
            if (stored < 30 && !(stored == 0 && d == 0)) {
                mant = (mant << 4) | (unsigned)d;
                ++stored;
                if (in_frac)
                    bin_exp -= 4;
            } else if (stored == 0) { /* a leading zero: only the exponent moves when it is in the fraction */
                if (in_frac)
                    bin_exp -= 4;
            } else {
                if (d)
                    sticky = 1;
                if (!in_frac)
                    bin_exp += 4;
            }
        }
        if (any) {
            int pexp = 0;
            if (*q == 'p' || *q == 'P') {
                const UCRT_T *r = q + 1;
                int eneg = 0;
                if (*r == '+' || *r == '-') {
                    eneg = *r == '-';
                    ++r;
                }
                if (*r >= '0' && *r <= '9') {
                    while (*r >= '0' && *r <= '9') {
                        if (pexp < 100000)
                            pexp = pexp * 10 + (*r - '0');
                        ++r;
                    }
                    if (eneg)
                        pexp = -pexp;
                    q = r;
                }
            }
            if (end)
                *end = q;
            if (mant == 0)
                return neg ? sign_bit : 0;
            {
                uint64_t bits = ucrt_fp_round(mant, bin_exp + pexp, sticky, f, flags);
                return bits | (neg ? sign_bit : 0);
            }
        }
        /* "0x" with no digits: the number is the zero before the x. */
        if (end)
            *end = p + 1;
        return neg ? sign_bit : 0;
    }
    /* decimal */
    {
        char dig[UCRT_STRTOD_MAXDIG + 2];
        int nd = 0, dec_exp = 0, trunc_nonzero = 0, exp10 = 0;
        const UCRT_T *q = p;
        for (; *q >= '0' && *q <= '9'; ++q) {
            seen = 1;
            if (nd == 0 && *q == '0')
                continue;
            if (nd < UCRT_STRTOD_MAXDIG) {
                dig[nd++] = (char)*q;
            } else {
                if (*q != '0')
                    trunc_nonzero = 1;
                ++dec_exp;
            }
        }
        if (*q == '.') {
            const UCRT_T *r = q + 1;
            if (seen || (*r >= '0' && *r <= '9')) {
                q = r;
                for (; *q >= '0' && *q <= '9'; ++q) {
                    seen = 1;
                    if (nd == 0 && *q == '0') {
                        --dec_exp;
                        continue;
                    }
                    if (nd < UCRT_STRTOD_MAXDIG) {
                        dig[nd++] = (char)*q;
                        --dec_exp;
                    } else if (*q != '0') {
                        trunc_nonzero = 1;
                    }
                }
            }
        }
        if (!seen)
            return 0; /* no number: *end is s */
        if (*q == 'e' || *q == 'E') {
            const UCRT_T *r = q + 1;
            int eneg = 0;
            if (*r == '+' || *r == '-') {
                eneg = *r == '-';
                ++r;
            }
            if (*r >= '0' && *r <= '9') {
                for (; *r >= '0' && *r <= '9'; ++r)
                    if (exp10 < 100000)
                        exp10 = exp10 * 10 + (*r - '0');
                if (eneg)
                    exp10 = -exp10;
                q = r;
            }
        }
        if (end)
            *end = q;
        if (nd == 0)
            return neg ? sign_bit : 0;
        if (trunc_nonzero) { /* dropped digits were nonzero: a sticky digit keeps the rounding honest */
            dig[nd++] = '1';
            --dec_exp;
        }
        exp10 += dec_exp;
        {
            int mag = nd + exp10;
            ucrt_big d, den;
            unsigned __int128 qv;
            int e2 = 0, sticky = 0;
            uint64_t bits;
            if (mag > f->max_dec) {
                *flags |= UCRT_FP_OVERFLOW;
                return inf_bits | (neg ? sign_bit : 0);
            }
            if (mag < f->min_dec) {
                *flags |= UCRT_FP_UNDERFLOW;
                return neg ? sign_bit : 0;
            }
            /* Fast path for double: at most 15 digits and an exact power of ten. */
            if (f->prec == 53 && nd <= 15 && exp10 >= -22 && exp10 <= 22) {
                static const double p10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                               1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
                uint64_t m = 0;
                double v;
                for (int i = 0; i < nd; ++i)
                    m = m * 10 + (uint64_t)(dig[i] - '0');
                v = (double)m;
                v = exp10 >= 0 ? v * p10[exp10] : v / p10[-exp10];
                __builtin_memcpy(&bits, &v, sizeof(bits));
                return bits | (neg ? sign_bit : 0);
            }
            ucrt_big_zero(&d);
            for (int i = 0; i < nd; ++i)
                ucrt_big_muladd_u32(&d, 10, (uint32_t)(dig[i] - '0'));
            if (exp10 >= 0) {
                uint64_t top;
                unsigned nb;
                ucrt_big_mul_pow10(&d, (unsigned)exp10);
                nb = ucrt_big_bits(&d);
                top = ucrt_big_top64(&d, &sticky);
                qv = top;
                e2 = nb > 64 ? (int)(nb - 64) : 0;
            } else {
                unsigned nb = ucrt_big_bits(&d), db, s_shift = 0, t_shift = 0;
                ucrt_big rem;
                ucrt_big_set_u64(&den, 1);
                ucrt_big_mul_pow10(&den, (unsigned)(-exp10));
                db = ucrt_big_bits(&den);
                /* Scale so that the quotient has 66 or 67 bits: shift the numerator up when it is too small,
                 * or the denominator up when it is too big. The quotient then fits in 128 bits and the
                 * remainder says whether it was exact. */
                if (66 + db > nb)
                    s_shift = 66 + db - nb;
                else
                    t_shift = nb - db - 66;
                ucrt_big_shl(&d, s_shift);
                ucrt_big_shl(&den, t_shift);
                db = ucrt_big_bits(&den);
                /* qv = floor(d / den) by shift and subtract. */
                rem = d;
                qv = 0;
                {
                    unsigned rb = ucrt_big_bits(&rem);
                    int shift = (int)rb - (int)db;
                    ucrt_big dens = den;
                    if (shift >= 0) {
                        ucrt_big_shl(&dens, (unsigned)shift);
                        for (int i = shift; i >= 0; --i) {
                            qv <<= 1;
                            if (ucrt_big_cmp(&rem, &dens) >= 0) {
                                ucrt_big_sub(&rem, &dens);
                                qv |= 1;
                            }
                            ucrt_big_shr(&dens, 1);
                        }
                    }
                }
                sticky = !ucrt_big_is_zero(&rem);
                e2 = (int)t_shift - (int)s_shift;
            }
            bits = ucrt_fp_round(qv, e2, sticky, f, flags);
            return bits | (neg ? sign_bit : 0);
        }
    }
}

static inline double UCRT_FN(tod)(const UCRT_T *s, const UCRT_T **end)
{
    int flags;
    uint64_t bits = UCRT_FN(parse_fp)(s, end, &ucrt_fmt_double, &flags);
    double v;
    if (flags & (UCRT_FP_OVERFLOW | UCRT_FP_UNDERFLOW))
        ucrt_set_errno(UCRT_ERANGE);
    __builtin_memcpy(&v, &bits, sizeof(v));
    return v;
}

static inline float UCRT_FN(tof)(const UCRT_T *s, const UCRT_T **end)
{
    int flags;
    uint32_t bits = (uint32_t)UCRT_FN(parse_fp)(s, end, &ucrt_fmt_float, &flags);
    float v;
    if (flags & (UCRT_FP_OVERFLOW | UCRT_FP_UNDERFLOW))
        ucrt_set_errno(UCRT_ERANGE);
    __builtin_memcpy(&v, &bits, sizeof(v));
    return v;
}

#undef UCRT_FN
#undef UCRT_CAT
#undef UCRT_CAT_
