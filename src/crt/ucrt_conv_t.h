/*
 * ucrt_conv_t.h - integer parsing and printing, written once for char and 16-bit wide characters.
 * Included by ucrt_conv.h with UCRT_T, UCRT_P (name prefix), UCRT_ISSPACE(c) set.
 */
#define UCRT_CAT_(a, b) a##b
#define UCRT_CAT(a, b) UCRT_CAT_(a, b)
#define UCRT_FN(name) UCRT_CAT(UCRT_P, name)

/* Value of c as a digit in some base up to 36, or -1. */
static inline int UCRT_FN(digit_value)(UCRT_T c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}

/* The core of strtol and its relatives: skips white space, an optional sign and the base prefix, then reads
 * digits. Returns the magnitude saturated at UINT64_MAX; *neg and *overflow report the sign and whether the
 * magnitude did not fit in 64 bits. *end is s when no number was read (and for an invalid base). */
static inline uint64_t UCRT_FN(tou64)(const UCRT_T *s, const UCRT_T **end, int base, int *neg, int *overflow)
{
    const UCRT_T *p = s;
    uint64_t v = 0;
    int digits = 0;
    *neg = 0;
    *overflow = 0;
    if (end)
        *end = s;
    if (base < 0 || base == 1 || base > 36) {
        ucrt_invalid(UCRT_EINVAL);
        return 0;
    }
    while (UCRT_ISSPACE(*p))
        ++p;
    if (*p == '+' || *p == '-') {
        *neg = *p == '-';
        ++p;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && UCRT_FN(digit_value)(p[2]) >= 0 &&
        UCRT_FN(digit_value)(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    for (;; ++p) {
        int d = UCRT_FN(digit_value)(*p);
        if (d < 0 || d >= base)
            break;
        ++digits;
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) {
            *overflow = 1;
            v = UINT64_MAX;
        } else if (!*overflow) {
            v = v * (uint64_t)base + (uint64_t)d;
        }
    }
    if (digits && end)
        *end = p;
    return digits ? v : 0;
}

/* strtoll / _strtoi64. */
static inline int64_t UCRT_FN(toi64)(const UCRT_T *s, const UCRT_T **end, int base)
{
    int neg, ovf;
    uint64_t m = UCRT_FN(tou64)(s, end, base, &neg, &ovf);
    if (neg) {
        if (ovf || m > (uint64_t)1 << 63) {
            ucrt_set_errno(UCRT_ERANGE);
            return INT64_MIN;
        }
        return (int64_t)(0 - m);
    }
    if (ovf || m > (uint64_t)INT64_MAX) {
        ucrt_set_errno(UCRT_ERANGE);
        return INT64_MAX;
    }
    return (int64_t)m;
}

/* strtoull / _strtoui64: a minus sign negates modulo 2^64. */
static inline uint64_t UCRT_FN(toui64)(const UCRT_T *s, const UCRT_T **end, int base)
{
    int neg, ovf;
    uint64_t m = UCRT_FN(tou64)(s, end, base, &neg, &ovf);
    if (ovf) {
        ucrt_set_errno(UCRT_ERANGE);
        return UINT64_MAX;
    }
    return neg ? 0 - m : m;
}

/* strtol on Windows: long is 32 bits. */
static inline int32_t UCRT_FN(tol)(const UCRT_T *s, const UCRT_T **end, int base)
{
    int neg, ovf;
    uint64_t m = UCRT_FN(tou64)(s, end, base, &neg, &ovf);
    if (neg) {
        if (ovf || m > (uint64_t)1 << 31) {
            ucrt_set_errno(UCRT_ERANGE);
            return INT32_MIN;
        }
        return (int32_t)(0 - (uint32_t)m);
    }
    if (ovf || m > (uint64_t)INT32_MAX) {
        ucrt_set_errno(UCRT_ERANGE);
        return INT32_MAX;
    }
    return (int32_t)m;
}

static inline uint32_t UCRT_FN(toul)(const UCRT_T *s, const UCRT_T **end, int base)
{
    int neg, ovf;
    uint64_t m = UCRT_FN(tou64)(s, end, base, &neg, &ovf);
    if (ovf || m > UINT32_MAX) {
        ucrt_set_errno(UCRT_ERANGE);
        return UINT32_MAX;
    }
    return neg ? (uint32_t)(0 - (uint32_t)m) : (uint32_t)m;
}

/* atoi, atol, atoll: strtol with base 10 and no end pointer. atoi and atol give the 32-bit long. */
static inline int UCRT_FN(atoi)(const UCRT_T *s)
{
    return UCRT_FN(tol)(s, NULL, 10);
}

static inline int64_t UCRT_FN(atoi64)(const UCRT_T *s)
{
    return UCRT_FN(toi64)(s, NULL, 10);
}

/* _itoa and friends: writes the digits of v in the given radix into buf. A signed value in radix 10 gets a
 * minus sign; other radixes print the unsigned bit pattern. Returns the length, 0 if radix is out of range. */
static inline size_t UCRT_FN(utoa_raw)(uint64_t v, UCRT_T *buf, int radix, int neg)
{
    UCRT_T tmp[72];
    size_t n = 0, len = 0, i;
    if (radix < 2 || radix > 36)
        return 0;
    do {
        unsigned d = (unsigned)(v % (unsigned)radix);
        tmp[n++] = (UCRT_T)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (unsigned)radix;
    } while (v);
    if (neg)
        buf[len++] = '-';
    for (i = n; i-- > 0;)
        buf[len++] = tmp[i];
    buf[len] = 0;
    return len;
}

/* The _s forms take the buffer size and return errno_t; they fail with ERANGE and an empty string when the
 * digits (with the terminator) do not fit, and EINVAL for a bad radix or buffer. */
static inline ucrt_errno_t UCRT_FN(i64toa_s)(int64_t v, UCRT_T *buf, size_t size, int radix, int is_unsigned_value)
{
    UCRT_T tmp[72];
    size_t n;
    int neg = 0;
    uint64_t u;
    if (!buf || size == 0)
        return ucrt_invalid(UCRT_EINVAL);
    if (radix < 2 || radix > 36) {
        buf[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    u = (uint64_t)v;
    if (!is_unsigned_value && radix == 10 && v < 0) {
        neg = 1;
        u = 0 - (uint64_t)v;
    }
    n = UCRT_FN(utoa_raw)(u, tmp, radix, neg);
    if (n + 1 > size) {
        buf[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    for (size_t i = 0; i <= n; ++i)
        buf[i] = tmp[i];
    return 0;
}

#undef UCRT_FN
#undef UCRT_CAT
#undef UCRT_CAT_
