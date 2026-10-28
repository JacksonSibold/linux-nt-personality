/*
 * ucrt_string.h - <string.h> and <wchar.h> string functions, the memory functions, and strerror.
 *
 * Narrow functions are ucrt_str* (ucrt_strlen, ucrt_stricmp, ...); wide ones are ucrt_wcs*. The wide
 * character is 16 bits. Host tests: tests/crt-host/test_string.c compares each with the system library.
 */
#ifndef UCRT_STRING_H
#define UCRT_STRING_H

#include "ucrt_state.h"

#define UCRT_T char
#define UCRT_UT unsigned char
#define UCRT_P ucrt_str
#include "ucrt_string_t.h"
#undef UCRT_T
#undef UCRT_UT
#undef UCRT_P

#define UCRT_T ucrt_wchar
#define UCRT_UT ucrt_wchar
#define UCRT_P ucrt_wcs
#include "ucrt_string_t.h"
#undef UCRT_T
#undef UCRT_UT
#undef UCRT_P

/* ---- Memory ---------------------------------------------------------------------------------------- */

/* memcpy handles overlap (it is memmove): the Microsoft runtime's does too. With UCRT_MEM_ASM defined, the
 * DLL build uses rep movsb and rep stosb, which are fast on current processors; the portable loops are
 * what the host tests run, so the sanitizers see every access. */
static inline void *ucrt_memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n) {
#if defined(UCRT_MEM_ASM) && defined(__x86_64__)
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t v;
            __builtin_memcpy(&v, s + i, 8);
            __builtin_memcpy(d + i, &v, 8);
        }
        for (; i < n; ++i)
            d[i] = s[i];
#endif
    } else { /* the destination overlaps the end of the source: copy backwards */
        size_t i = n;
        while (i >= 8) {
            uint64_t v;
            i -= 8;
            __builtin_memcpy(&v, s + i, 8);
            __builtin_memcpy(d + i, &v, 8);
        }
        while (i--)
            d[i] = s[i];
    }
    return dst;
}

static inline void *ucrt_memcpy(void *dst, const void *src, size_t n)
{
    return ucrt_memmove(dst, src, n);
}

static inline void *ucrt_memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
#if defined(UCRT_MEM_ASM) && defined(__x86_64__)
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
#else
    uint64_t pat = 0x0101010101010101ull * (uint8_t)c;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        __builtin_memcpy(d + i, &pat, 8);
    for (; i < n; ++i)
        d[i] = (uint8_t)c;
#endif
    return dst;
}

/* memcmp returns the difference of the first bytes that differ, or 0. */
static inline int ucrt_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t u, v;
        __builtin_memcpy(&u, x + i, 8);
        __builtin_memcpy(&v, y + i, 8);
        if (u != v)
            break;
    }
    for (; i < n; ++i)
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    return 0;
}

static inline void *ucrt_memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < n; ++i)
        if (p[i] == (uint8_t)c)
            return (void *)(uintptr_t)(p + i);
    return NULL;
}

/* _memccpy copies until c has been copied or n bytes are done; returns the byte after c, or NULL. */
static inline void *ucrt_memccpy(void *dst, const void *src, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
        if (s[i] == (uint8_t)c)
            return d + i + 1;
    }
    return NULL;
}

/* _memicmp: like memcmp after ASCII lower-casing. */
static inline int ucrt_memicmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; ++i) {
        int p = ucrt_strlc((char)x[i]) & 0xFF, q = ucrt_strlc((char)y[i]) & 0xFF;
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}

/* ---- Secure memory functions --------------------------------------------------------------------- */

static inline ucrt_errno_t ucrt_memcpy_s(void *dst, size_t dst_size, const void *src, size_t count)
{
    if (count == 0)
        return 0;
    if (!dst) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (!src) {
        ucrt_memset(dst, 0, dst_size);
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (dst_size < count) {
        ucrt_memset(dst, 0, dst_size);
        return ucrt_invalid(UCRT_ERANGE);
    }
    ucrt_memmove(dst, src, count);
    return 0;
}

static inline ucrt_errno_t ucrt_memmove_s(void *dst, size_t dst_size, const void *src, size_t count)
{
    if (count == 0)
        return 0;
    if (!dst || !src) {
        return ucrt_invalid(UCRT_EINVAL);
    }
    if (dst_size < count) {
        return ucrt_invalid(UCRT_ERANGE);
    }
    ucrt_memmove(dst, src, count);
    return 0;
}

/* ---- strerror ------------------------------------------------------------------------------------- */

/* The messages of the Microsoft runtime for errno 0 to 42; anything else is "Unknown error". */
static const char *const ucrt_errlist[43] = {
    "No error", "Operation not permitted", "No such file or directory", "No such process",
    "Interrupted function call", "Input/output error", "No such device or address", "Arg list too long",
    "Exec format error", "Bad file descriptor", "No child processes", "Resource temporarily unavailable",
    "Not enough space", "Permission denied", "Bad address", "Unknown error",
    "Resource device", "File exists", "Improper link", "No such device",
    "Not a directory", "Is a directory", "Invalid argument", "Too many open files in system",
    "Too many open files", "Inappropriate I/O control operation", "Unknown error", "File too large",
    "No space left on device", "Invalid seek", "Read-only file system", "Too many links",
    "Broken pipe", "Domain error", "Result too large", "Unknown error",
    "Resource deadlock avoided", "Unknown error", "Filename too long", "No locks available",
    "Function not implemented", "Directory not empty", "Illegal byte sequence",
};

static inline const char *ucrt_strerror(int e)
{
    if (e >= 0 && e < 43)
        return ucrt_errlist[e];
    return "Unknown error";
}

#endif /* UCRT_STRING_H */
