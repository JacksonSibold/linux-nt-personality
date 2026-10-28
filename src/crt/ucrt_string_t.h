/*
 * ucrt_string_t.h - the string functions, written once for char and for 16-bit wide characters.
 *
 * Included twice by ucrt_string.h with UCRT_T, UCRT_UT (the unsigned form used for comparisons), UCRT_P (the
 * name prefix) and UCRT_FN(name) set. Case mapping is the "C" locale's: ASCII only. The functions follow the C
 * standard and the Microsoft extensions (_stricmp and friends); the secure _s functions are in ucrt_string.h.
 */
#define UCRT_CAT_(a, b) a##b
#define UCRT_CAT(a, b) UCRT_CAT_(a, b)
#define UCRT_FN(name) UCRT_CAT(UCRT_P, name)

static inline size_t UCRT_FN(len)(const UCRT_T *s)
{
    const UCRT_T *p = s;
    while (*p)
        ++p;
    return (size_t)(p - s);
}

static inline size_t UCRT_FN(nlen)(const UCRT_T *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        ++n;
    return n;
}

static inline UCRT_T *UCRT_FN(cpy)(UCRT_T *dst, const UCRT_T *src)
{
    UCRT_T *d = dst;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

/* strncpy pads the rest of dst with zeros up to n. */
static inline UCRT_T *UCRT_FN(ncpy)(UCRT_T *dst, const UCRT_T *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; ++i)
        dst[i] = src[i];
    for (; i < n; ++i)
        dst[i] = 0;
    return dst;
}

static inline UCRT_T *UCRT_FN(cat)(UCRT_T *dst, const UCRT_T *src)
{
    UCRT_FN(cpy)(dst + UCRT_FN(len)(dst), src);
    return dst;
}

/* strncat appends at most n characters and always terminates. */
static inline UCRT_T *UCRT_FN(ncat)(UCRT_T *dst, const UCRT_T *src, size_t n)
{
    UCRT_T *d = dst + UCRT_FN(len)(dst);
    size_t i = 0;
    for (; i < n && src[i]; ++i)
        d[i] = src[i];
    d[i] = 0;
    return dst;
}

static inline int UCRT_FN(cmp)(const UCRT_T *a, const UCRT_T *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (UCRT_UT)*a < (UCRT_UT)*b ? -1 : (UCRT_UT)*a > (UCRT_UT)*b ? 1 : 0;
}

static inline int UCRT_FN(ncmp)(const UCRT_T *a, const UCRT_T *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (a[i] != b[i] || !a[i])
            return (UCRT_UT)a[i] < (UCRT_UT)b[i] ? -1 : (UCRT_UT)a[i] > (UCRT_UT)b[i] ? 1 : 0;
    }
    return 0;
}

/* ASCII case folding for the "C" locale. */
static inline UCRT_T UCRT_FN(lc)(UCRT_T c)
{
    return (c >= 'A' && c <= 'Z') ? (UCRT_T)(c + ('a' - 'A')) : c;
}

static inline UCRT_T UCRT_FN(uc)(UCRT_T c)
{
    return (c >= 'a' && c <= 'z') ? (UCRT_T)(c - ('a' - 'A')) : c;
}

/* _stricmp compares after mapping to lower case, as the Microsoft runtime does. */
static inline int UCRT_FN(icmp)(const UCRT_T *a, const UCRT_T *b)
{
    for (;; ++a, ++b) {
        UCRT_UT x = (UCRT_UT)UCRT_FN(lc)(*a), y = (UCRT_UT)UCRT_FN(lc)(*b);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
    }
}

static inline int UCRT_FN(nicmp)(const UCRT_T *a, const UCRT_T *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        UCRT_UT x = (UCRT_UT)UCRT_FN(lc)(a[i]), y = (UCRT_UT)UCRT_FN(lc)(b[i]);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
    }
    return 0;
}

/* strchr finds the terminator when c is 0. */
static inline UCRT_T *UCRT_FN(chr)(const UCRT_T *s, int c)
{
    for (;; ++s) {
        if (*s == (UCRT_T)c)
            return (UCRT_T *)(uintptr_t)s;
        if (!*s)
            return NULL;
    }
}

static inline UCRT_T *UCRT_FN(rchr)(const UCRT_T *s, int c)
{
    const UCRT_T *last = NULL;
    for (;; ++s) {
        if (*s == (UCRT_T)c)
            last = s;
        if (!*s)
            return (UCRT_T *)(uintptr_t)last;
    }
}

static inline UCRT_T *UCRT_FN(str)(const UCRT_T *hay, const UCRT_T *needle)
{
    size_t nl = UCRT_FN(len)(needle);
    if (!nl)
        return (UCRT_T *)(uintptr_t)hay;
    for (; *hay; ++hay) {
        if (*hay == *needle && UCRT_FN(ncmp)(hay, needle, nl) == 0)
            return (UCRT_T *)(uintptr_t)hay;
    }
    return NULL;
}

static inline int UCRT_FN(in_set)(UCRT_T c, const UCRT_T *set)
{
    for (; *set; ++set)
        if (*set == c)
            return 1;
    return 0;
}

static inline size_t UCRT_FN(spn)(const UCRT_T *s, const UCRT_T *accept)
{
    size_t n = 0;
    while (s[n] && UCRT_FN(in_set)(s[n], accept))
        ++n;
    return n;
}

static inline size_t UCRT_FN(cspn)(const UCRT_T *s, const UCRT_T *reject)
{
    size_t n = 0;
    while (s[n] && !UCRT_FN(in_set)(s[n], reject))
        ++n;
    return n;
}

static inline UCRT_T *UCRT_FN(pbrk)(const UCRT_T *s, const UCRT_T *set)
{
    for (; *s; ++s)
        if (UCRT_FN(in_set)(*s, set))
            return (UCRT_T *)(uintptr_t)s;
    return NULL;
}

/* strtok_s with the caller's context: skips leading delimiters, ends the token at the next one. */
static inline UCRT_T *UCRT_FN(tok_r)(UCRT_T *s, const UCRT_T *delim, UCRT_T **ctx)
{
    UCRT_T *tok;
    if (!s)
        s = *ctx;
    if (!s)
        return NULL;
    s += UCRT_FN(spn)(s, delim);
    if (!*s) {
        *ctx = s;
        return NULL;
    }
    tok = s;
    s += UCRT_FN(cspn)(s, delim);
    if (*s) {
        *s = 0;
        *ctx = s + 1;
    } else {
        *ctx = s;
    }
    return tok;
}

static inline UCRT_T *UCRT_FN(lwr)(UCRT_T *s)
{
    for (UCRT_T *p = s; *p; ++p)
        *p = UCRT_FN(lc)(*p);
    return s;
}

static inline UCRT_T *UCRT_FN(upr)(UCRT_T *s)
{
    for (UCRT_T *p = s; *p; ++p)
        *p = UCRT_FN(uc)(*p);
    return s;
}

static inline UCRT_T *UCRT_FN(rev)(UCRT_T *s)
{
    size_t n = UCRT_FN(len)(s), i;
    for (i = 0; i < n / 2; ++i) {
        UCRT_T t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
    return s;
}

static inline UCRT_T *UCRT_FN(set)(UCRT_T *s, int c)
{
    for (UCRT_T *p = s; *p; ++p)
        *p = (UCRT_T)c;
    return s;
}

static inline UCRT_T *UCRT_FN(nset)(UCRT_T *s, int c, size_t n)
{
    for (size_t i = 0; i < n && s[i]; ++i)
        s[i] = (UCRT_T)c;
    return s;
}

/* ---- Secure functions (the _s family) -------------------------------------------------------------- */

/* Errors report through the invalid parameter path and the returned errno_t. A destination the function
 * could not fill is set to the empty string, as the Microsoft runtime does. */
static inline ucrt_errno_t UCRT_FN(cpy_s)(UCRT_T *dst, size_t size, const UCRT_T *src)
{
    size_t i;
    if (!dst || size == 0) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (!src) {
        dst[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    for (i = 0; i < size; ++i) {
        if ((dst[i] = src[i]) == 0)
            return 0;
    }
    dst[0] = 0;
    return ucrt_invalid(UCRT_ERANGE);
}

static inline ucrt_errno_t UCRT_FN(cat_s)(UCRT_T *dst, size_t size, const UCRT_T *src)
{
    size_t used, i;
    if (!dst || size == 0) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    used = UCRT_FN(nlen)(dst, size);
    if (used == size) { /* dst is not terminated within its size */
        dst[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (!src) {
        dst[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    for (i = 0; used + i < size; ++i) {
        if ((dst[used + i] = src[i]) == 0)
            return 0;
    }
    dst[0] = 0;
    return ucrt_invalid(UCRT_ERANGE);
}

#define UCRT_TRUNCATE ((size_t)-1)

/* strncpy_s copies at most n characters. With n == _TRUNCATE it copies as much as fits and returns
 * STRUNCATE instead of failing. */
static inline ucrt_errno_t UCRT_FN(ncpy_s)(UCRT_T *dst, size_t size, const UCRT_T *src, size_t n)
{
    size_t i;
    if (n == 0 && dst && size == 0)
        return 0;
    if (!dst || size == 0) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (n == 0) {
        dst[0] = 0;
        return 0;
    }
    if (!src) {
        dst[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (n == UCRT_TRUNCATE) {
        for (i = 0; i < size - 1 && src[i]; ++i)
            dst[i] = src[i];
        dst[i] = 0;
        return src[i] ? 80 /* STRUNCATE */ : 0;
    }
    for (i = 0; i < n && src[i]; ++i) {
        if (i + 1 >= size) {
            dst[0] = 0;
            return ucrt_invalid(UCRT_ERANGE);
        }
        dst[i] = src[i];
    }
    if (i >= size) {
        dst[0] = 0;
        return ucrt_invalid(UCRT_ERANGE);
    }
    dst[i] = 0;
    return 0;
}

static inline ucrt_errno_t UCRT_FN(ncat_s)(UCRT_T *dst, size_t size, const UCRT_T *src, size_t n)
{
    size_t used, i;
    if (!dst || size == 0) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    used = UCRT_FN(nlen)(dst, size);
    if (used == size) {
        dst[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (n == 0)
        return 0;
    if (!src) {
        dst[0] = 0;
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (n == UCRT_TRUNCATE) {
        for (i = 0; used + i < size - 1 && src[i]; ++i)
            dst[used + i] = src[i];
        dst[used + i] = 0;
        return src[i] ? 80 : 0;
    }
    for (i = 0; i < n && src[i]; ++i) {
        if (used + i + 1 >= size) {
            dst[0] = 0;
            return ucrt_invalid(UCRT_ERANGE);
        }
        dst[used + i] = src[i];
    }
    dst[used + i] = 0;
    return 0;
}

#undef UCRT_TRUNCATE

#undef UCRT_FN
#undef UCRT_CAT
#undef UCRT_CAT_
