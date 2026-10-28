/*
 * ucrt_dtoa.h - exact decimal digits of a double, correctly rounded (half to even on the exact value), for
 * printf's %e, %f and %g and for _fcvt/_ecvt.
 *
 * ucrt_dtoa(v, mode, ndigits, out, &decpt) writes the digits of the finite value |v| without leading zeros and
 * sets decpt so that |v| ~ 0.DIGITS * 10^decpt:
 *   mode 2: exactly ndigits significant digits (1 <= ndigits <= UCRT_DTOA_MAX_DIGITS);
 *   mode 3: ndigits digits after the decimal point (0 <= ndigits <= UCRT_DTOA_MAX_DIGITS); the digit string
 *           ends at the last of them, so it can be empty when the value rounds to zero.
 * It returns the number of digits written. The digits are not terminated. The value 0 gives "0" with decpt 1
 * in mode 2 and no digits in mode 3.
 *
 * The expansion of the double is exact (a mantissa times a power of two, turned into decimal with big
 * integers), so rounding is exactly that of the infinitely precise value. Host tests compare it with glibc's
 * printf, which is also exact: tests/crt-host/test_dtoa.c.
 */
#ifndef UCRT_DTOA_H
#define UCRT_DTOA_H

#include "ucrt_bignum.h"

#define UCRT_DTOA_MAX_DIGITS 1100
/* The most digits of the exact expansion of any double: 309 integer digits and 1074 fraction digits. */
#define UCRT_DTOA_EXACT_MAX 1400

static inline int ucrt_dtoa(double v, int mode, int ndigits, char *out, int *decpt)
{
    char all[UCRT_DTOA_EXACT_MAX + 64];
    uint64_t bits, mant;
    int e2, ilen = 0, alen, z, keep_end, i;
    ucrt_big f;
    unsigned k = 0;
    int rest_nonzero = 0;
    char *p;

    __builtin_memcpy(&bits, &v, sizeof(bits));
    bits &= 0x7FFFFFFFFFFFFFFFull;
    {
        int be = (int)(bits >> 52);
        mant = bits & 0xFFFFFFFFFFFFFull;
        if (be == 0) {
            e2 = -1074;
        } else {
            mant |= 1ull << 52;
            e2 = be - 1075;
        }
    }
    if (mant == 0) {
        if (mode == 2) {
            out[0] = '0';
            *decpt = 1;
            return 1;
        }
        *decpt = 1;
        return 0;
    }
    if (ndigits > UCRT_DTOA_MAX_DIGITS)
        ndigits = UCRT_DTOA_MAX_DIGITS;
    if (mode == 2 && ndigits < 1)
        ndigits = 1;
    if (ndigits < 0)
        ndigits = 0;

    /* Integer part into `all`, most significant digit first. */
    ucrt_big_zero(&f);
    if (e2 >= 0) {
        ucrt_big ip;
        char tmp[UCRT_DTOA_EXACT_MAX];
        int t = 0;
        ucrt_big_set_u64(&ip, mant);
        ucrt_big_shl(&ip, (unsigned)e2);
        while (!ucrt_big_is_zero(&ip)) {
            uint32_t chunk = ucrt_big_divmod_u32(&ip, 1000000000u);
            for (int d = 0; d < 9; ++d) {
                tmp[t++] = (char)('0' + chunk % 10);
                chunk /= 10;
            }
        }
        while (t > 0 && tmp[t - 1] == '0')
            --t; /* leading zeros of the top chunk */
        for (i = 0; i < t; ++i)
            all[i] = tmp[t - 1 - i];
        ilen = t;
        alen = t;
        k = 0;
    } else {
        uint64_t ip;
        char tmp[24];
        int t = 0;
        k = (unsigned)(-e2);
        ip = k < 64 ? mant >> k : 0;
        while (ip) {
            tmp[t++] = (char)('0' + ip % 10);
            ip /= 10;
        }
        for (i = 0; i < t; ++i)
            all[i] = tmp[t - 1 - i];
        ilen = t;
        alen = t;
        ucrt_big_set_u64(&f, mant);
        ucrt_big_truncate_bits(&f, k);
    }

    /* Index of the first nonzero digit so far (leading zeros of the fraction are generated on demand). */
    z = -1;
    for (i = 0; i < alen; ++i) {
        if (all[i] != '0') {
            z = i;
            break;
        }
    }
    /* Generate fraction digits until the rounding decision is settled: keep_end plus one digit. The target
     * depends on z, which may itself be found while generating. */
    for (;;) {
        int need;
        if (mode == 3)
            need = ilen + ndigits + 1;
        else if (z >= 0)
            need = z + ndigits + 1;
        else
            need = 0x7FFFFFFF;
        if (alen >= need || ucrt_big_is_zero(&f))
            break;
        /* Nine more digits of the fraction: f * 10^9 / 2^k. */
        ucrt_big_muladd_u32(&f, 1000000000u, 0);
        {
            ucrt_big top = f;
            uint32_t chunk = 0;
            ucrt_big_shr(&top, k);
            if (top.n)
                chunk = top.d[0];
            ucrt_big_truncate_bits(&f, k);
            for (int d = 8; d >= 0; --d) {
                all[alen + d] = (char)('0' + chunk % 10);
                chunk /= 10;
            }
            alen += 9;
        }
        if (z < 0) {
            for (i = alen - 9; i < alen; ++i) {
                if (all[i] != '0') {
                    z = i;
                    break;
                }
            }
        }
    }
    rest_nonzero = !ucrt_big_is_zero(&f);
    if (z < 0) { /* only zeros were generated: cannot happen for a nonzero double with enough digits, but be safe */
        z = alen;
    }

    keep_end = (mode == 3) ? ilen + ndigits : z + ndigits;
    /* The rounding digit and what follows it. */
    {
        int rd = keep_end < alen ? all[keep_end] - '0' : 0;
        int tail = rest_nonzero;
        int up;
        for (i = keep_end + 1; i < alen && !tail; ++i)
            if (all[i] != '0')
                tail = 1;
        if (keep_end < z) { /* the rounding digit is a leading zero */
            rd = 0;
            tail = 0;
        }
        /* The last kept digit decides a tie. When nothing is kept it counts as even. */
        {
            int last_odd = (keep_end > z && keep_end - 1 < alen) ? ((all[keep_end - 1] - '0') & 1) : 0;
            up = rd > 5 || (rd == 5 && (tail || last_odd));
        }
        if (keep_end < z)
            up = 0;
        p = out;
        if (keep_end <= z) {
            /* Nothing kept. A rounded-up value is one unit in the last place. */
            if (up) {
                out[0] = '1';
                *decpt = ilen - keep_end + 1;
                return 1;
            }
            *decpt = (mode == 3) ? 1 - ndigits : 1;
            if (mode == 2) { /* cannot occur: mode 2 keeps at least one digit */
                out[0] = '0';
                return 1;
            }
            return 0;
        }
        {
            int n = keep_end - z, carry = up;
            for (i = 0; i < n; ++i) {
                int idx = z + i;
                p[i] = idx < alen ? all[idx] : '0';
            }
            for (i = n - 1; i >= 0 && carry; --i) {
                if (p[i] == '9') {
                    p[i] = '0';
                } else {
                    ++p[i];
                    carry = 0;
                }
            }
            *decpt = ilen - z;
            if (carry) { /* 99.9 -> 100.0: a new leading digit */
                for (i = n; i > 0; --i)
                    p[i] = p[i - 1];
                p[0] = '1';
                *decpt += 1;
                if (mode == 2) /* keep exactly ndigits significant digits */
                    return n;
                return n + 1;
            }
            return n;
        }
    }
}

#endif /* UCRT_DTOA_H */
