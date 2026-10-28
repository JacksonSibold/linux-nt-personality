/*
 * ucrt_bignum.h - fixed-capacity unsigned big integers for the exact decimal conversions (printf of floating
 * point values, strtod). Limbs are 32 bits, least significant first; n is the number of limbs in use with no
 * leading zero limb, so zero has n == 0. The capacity covers 10^1100 times a 53-bit mantissa with room to spare.
 */
#ifndef UCRT_BIGNUM_H
#define UCRT_BIGNUM_H

#include <stddef.h>
#include <stdint.h>

#define UCRT_BIG_LIMBS 176

typedef struct ucrt_big {
    uint32_t n;
    uint32_t d[UCRT_BIG_LIMBS];
} ucrt_big;

static inline void ucrt_big_zero(ucrt_big *a)
{
    a->n = 0;
}

static inline void ucrt_big_trim(ucrt_big *a)
{
    while (a->n && a->d[a->n - 1] == 0)
        --a->n;
}

static inline void ucrt_big_set_u64(ucrt_big *a, uint64_t v)
{
    a->n = 0;
    while (v) {
        a->d[a->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static inline int ucrt_big_is_zero(const ucrt_big *a)
{
    return a->n == 0;
}

/* Number of significant bits; 0 for zero. */
static inline unsigned ucrt_big_bits(const ucrt_big *a)
{
    uint32_t top;
    unsigned b = 0;
    if (!a->n)
        return 0;
    top = a->d[a->n - 1];
    while (top) {
        ++b;
        top >>= 1;
    }
    return (a->n - 1) * 32 + b;
}

/* a = a * m + add. Returns 0 on overflow of the capacity (a is then unspecified). */
static inline int ucrt_big_muladd_u32(ucrt_big *a, uint32_t m, uint32_t add)
{
    uint64_t carry = add;
    uint32_t i;
    for (i = 0; i < a->n; ++i) {
        uint64_t t = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry) {
        if (a->n >= UCRT_BIG_LIMBS)
            return 0;
        a->d[a->n++] = (uint32_t)carry;
    }
    return 1;
}

/* a = a / div, returning the remainder. */
static inline uint32_t ucrt_big_divmod_u32(ucrt_big *a, uint32_t div)
{
    uint64_t rem = 0;
    for (uint32_t i = a->n; i-- > 0;) {
        uint64_t cur = (rem << 32) | a->d[i];
        a->d[i] = (uint32_t)(cur / div);
        rem = cur % div;
    }
    ucrt_big_trim(a);
    return (uint32_t)rem;
}

/* a = a << bits. Returns 0 on overflow of the capacity. */
static inline int ucrt_big_shl(ucrt_big *a, unsigned bits)
{
    unsigned limbs = bits / 32, rem = bits % 32;
    if (!a->n)
        return 1;
    if (a->n + limbs + 1 > UCRT_BIG_LIMBS)
        return 0;
    if (rem) {
        uint32_t carry = 0;
        for (uint32_t i = 0; i < a->n; ++i) {
            uint32_t v = a->d[i];
            a->d[i] = (v << rem) | carry;
            carry = v >> (32 - rem);
        }
        if (carry)
            a->d[a->n++] = carry;
    }
    if (limbs) {
        for (uint32_t i = a->n; i-- > 0;)
            a->d[i + limbs] = a->d[i];
        for (uint32_t i = 0; i < limbs; ++i)
            a->d[i] = 0;
        a->n += limbs;
    }
    return 1;
}

/* a = a >> bits. */
static inline void ucrt_big_shr(ucrt_big *a, unsigned bits)
{
    unsigned limbs = bits / 32, rem = bits % 32;
    if (limbs >= a->n) {
        a->n = 0;
        return;
    }
    if (limbs) {
        for (uint32_t i = 0; i + limbs < a->n; ++i)
            a->d[i] = a->d[i + limbs];
        a->n -= limbs;
    }
    if (rem) {
        for (uint32_t i = 0; i < a->n; ++i) {
            uint32_t hi = (i + 1 < a->n) ? a->d[i + 1] : 0;
            a->d[i] = (a->d[i] >> rem) | (hi << (32 - rem));
        }
    }
    ucrt_big_trim(a);
}

/* -1, 0, 1 */
static inline int ucrt_big_cmp(const ucrt_big *a, const ucrt_big *b)
{
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (uint32_t i = a->n; i-- > 0;) {
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

/* a = a - b; requires a >= b. */
static inline void ucrt_big_sub(ucrt_big *a, const ucrt_big *b)
{
    int64_t borrow = 0;
    for (uint32_t i = 0; i < a->n; ++i) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        if (t < 0) {
            t += (int64_t)1 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        a->d[i] = (uint32_t)t;
    }
    ucrt_big_trim(a);
}

/* Bit i of a (0 is the least significant). */
static inline unsigned ucrt_big_bit(const ucrt_big *a, unsigned i)
{
    return i / 32 < a->n ? (a->d[i / 32] >> (i % 32)) & 1u : 0u;
}

/* 1 when any of the low `bits` bits of a is set. */
static inline int ucrt_big_low_bits_nonzero(const ucrt_big *a, unsigned bits)
{
    unsigned full = bits / 32, rem = bits % 32;
    for (unsigned i = 0; i < full && i < a->n; ++i)
        if (a->d[i])
            return 1;
    if (rem && full < a->n && (a->d[full] & (((uint32_t)1 << rem) - 1)))
        return 1;
    return 0;
}

/* a = a * 10^k. Returns 0 on overflow of the capacity. */
static inline int ucrt_big_mul_pow10(ucrt_big *a, unsigned k)
{
    while (k >= 9) {
        if (!ucrt_big_muladd_u32(a, 1000000000u, 0))
            return 0;
        k -= 9;
    }
    if (k) {
        static const uint32_t p[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
        if (!ucrt_big_muladd_u32(a, p[k], 0))
            return 0;
    }
    return 1;
}

/* a = a mod 2^bits. */
static inline void ucrt_big_truncate_bits(ucrt_big *a, unsigned bits)
{
    unsigned full = bits / 32, rem = bits % 32;
    if (full >= a->n)
        return;
    if (rem) {
        a->d[full] &= ((uint32_t)1 << rem) - 1;
        a->n = full + 1;
    } else {
        a->n = full;
    }
    ucrt_big_trim(a);
}

/* The top 64 bits of a (the whole value when it has at most 64 bits) and whether any lower bit is set. */
static inline uint64_t ucrt_big_top64(const ucrt_big *a, int *sticky)
{
    unsigned bits = ucrt_big_bits(a);
    ucrt_big t = *a;
    uint64_t v = 0;
    *sticky = 0;
    if (bits > 64) {
        *sticky = ucrt_big_low_bits_nonzero(a, bits - 64);
        ucrt_big_shr(&t, bits - 64);
    }
    for (uint32_t i = t.n; i-- > 0;)
        v = (v << 32) | t.d[i];
    return v;
}

#endif /* UCRT_BIGNUM_H */
