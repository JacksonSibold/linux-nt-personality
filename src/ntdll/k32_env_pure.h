/*
 * k32_env_pure.h - pure rules for the kernel32 environment functions
 * (kernel32_env.c): GetEnvironmentVariable, SetEnvironmentVariable,
 * ExpandEnvironmentStrings and the environment block helpers.
 *
 * An environment block is UTF-16: "NAME=value" entries, each NUL terminated,
 * then one more NUL that ends the list. The process block is
 * PEB->ProcessParameters->Environment (written by pe_build_process_parameters).
 * In this header a block "length" (units) counts every WCHAR up to and
 * including that final NUL. An empty block is a single NUL (one unit).
 *
 * Rules:
 *   - Names compare case-insensitively (ASCII fold, as k32f_env_name_match
 *     in k32_find_pure.h). The name of an entry is the text before its first
 *     '='; an entry that starts with '=' (a hidden entry such as "=C:=C:\")
 *     keeps that first '=' as part of the name.
 *   - An entry with no '=' has an empty value.
 *   - Set replaces a matching entry in place; a new variable is appended.
 *     Delete removes it. Order of the other entries is kept.
 *   - Expansion: %NAME% is replaced by the value of NAME, matched as above.
 *     A name that is not set is copied unchanged, and so is a '%' without a
 *     closing '%'. "%%" is not an escape: the two '%' are copied unchanged
 *     and the scan restarts after them.
 *   - Size limit: a block, and every expanded string, is at most
 *     K32E_MAX_UNITS WCHARs including the terminator.
 *
 * Error codes returned by the functions here (the kernel32 layer sets them as
 * the last error):
 *   K32E_ERROR_INVALID_PARAMETER     87   bad name (empty, or contains '=' on set)
 *   K32E_ERROR_NOT_ENOUGH_MEMORY      8   result over K32E_MAX_UNITS (shim choice)
 *   K32E_ERROR_ENVVAR_NOT_FOUND     203   name not set (get, or delete of an unset name)
 */
#ifndef NTDLL_K32_ENV_PURE_H
#define NTDLL_K32_ENV_PURE_H

#include "nt_types.h"

#define K32E_ERROR_INVALID_PARAMETER    87u
#define K32E_ERROR_NOT_ENOUGH_MEMORY     8u
#define K32E_ERROR_ENVVAR_NOT_FOUND    203u

/* Largest block or expanded string, in WCHARs, including the final NUL. */
#define K32E_MAX_UNITS 32767u

/* Length of s (at most max WCHARs), not counting the NUL. Returns max if no NUL was found. */
static inline size_t k32e_wlen(const WCHAR *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != 0)
        ++n;
    return n;
}

/* ASCII case fold for the name compare. */
static inline WCHAR k32e_fold(WCHAR c)
{
    return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + ('a' - 'A')) : c;
}

/* Length of the name part of an entry of n WCHARs (text before the first '=',
 * searched from index 1 so a leading '=' stays in the name). n if there is no '='. */
static inline size_t k32e_name_len(const WCHAR *e, size_t n)
{
    size_t i = (n > 0 && e[0] == '=') ? 1 : 0;
    for (; i < n; ++i) {
        if (e[i] == '=')
            return i;
    }
    return n;
}

/* Does the entry e (n WCHARs, no NUL) have the name name (nlen WCHARs)? */
static inline int k32e_entry_matches(const WCHAR *e, size_t n, const WCHAR *name, size_t nlen)
{
    if (k32e_name_len(e, n) != nlen)
        return 0;
    for (size_t i = 0; i < nlen; ++i) {
        if (k32e_fold(e[i]) != k32e_fold(name[i]))
            return 0;
    }
    return 1;
}

/* Length of the block in WCHARs, up to and including the terminating NUL (the
 * empty entry). 0 if there is no terminator within max WCHARs. */
static inline size_t k32e_block_units(const WCHAR *blk, size_t max)
{
    size_t i = 0;
    while (i < max) {
        size_t n = k32e_wlen(blk + i, max - i);
        if (i + n >= max)
            return 0;          /* no NUL before the bound */
        if (n == 0)
            return i + 1;      /* the empty entry ends the list */
        i += n + 1;
    }
    return 0;
}

/* Finds the entry for name in a block of units WCHARs. On success, *value_off is
 * the offset of the value's first WCHAR in blk and *value_len its length. */
static inline int k32e_find(const WCHAR *blk, size_t units, const WCHAR *name, size_t nlen,
                            size_t *value_off, size_t *value_len)
{
    size_t i = 0;
    while (i < units && blk[i] != 0) {
        size_t n = k32e_wlen(blk + i, units - i);
        if (k32e_entry_matches(blk + i, n, name, nlen)) {
            size_t np = k32e_name_len(blk + i, n);
            size_t v = (np < n) ? np + 1 : n;
            *value_off = i + v;
            *value_len = n - v;
            return 1;
        }
        i += n + 1;
    }
    return 0;
}

/* Writes one WCHAR to dst when dst is not NULL and there is room; always counts it. */
static inline void k32e_emit(WCHAR *dst, size_t cap, size_t *out, WCHAR c)
{
    if (dst && *out < cap)
        dst[*out] = c;
    ++*out;
}

/* Builds the block that results from setting name to value (has_value nonzero)
 * or deleting it (has_value zero), from the block src of units WCHARs.
 * Pass dst NULL to get *need (units, including the final NUL). Otherwise dst
 * must hold *need WCHARs; call once with dst NULL, allocate, then call again
 * with the same arguments. Returns 0, or K32E_ERROR_NOT_ENOUGH_MEMORY when the
 * result is over K32E_MAX_UNITS. src may be NULL (an empty block). */
static inline void k32e_emit_entry(WCHAR *dst, size_t cap, size_t *out,
                                   const WCHAR *name, size_t nlen, const WCHAR *value, size_t vlen)
{
    for (size_t k = 0; k < nlen; ++k)
        k32e_emit(dst, cap, out, name[k]);
    k32e_emit(dst, cap, out, '=');
    for (size_t k = 0; k < vlen; ++k)
        k32e_emit(dst, cap, out, value[k]);
    k32e_emit(dst, cap, out, 0);
}

static inline int k32e_set_build(WCHAR *dst, size_t cap, const WCHAR *src, size_t units,
                                 const WCHAR *name, size_t nlen,
                                 const WCHAR *value, size_t vlen, int has_value, size_t *need)
{
    size_t out = 0;
    size_t i = 0;
    int placed = 0;
    while (src && i < units && src[i] != 0) {
        size_t n = k32e_wlen(src + i, units - i);
        if (!k32e_entry_matches(src + i, n, name, nlen)) {
            for (size_t k = 0; k <= n; ++k)     /* the entry and its NUL */
                k32e_emit(dst, cap, &out, src[i + k]);
        } else if (has_value && !placed) {
            /* The new value takes the old entry's place. Later duplicates are dropped. */
            k32e_emit_entry(dst, cap, &out, name, nlen, value, vlen);
            placed = 1;
        }
        i += n + 1;
    }
    if (has_value && !placed)
        k32e_emit_entry(dst, cap, &out, name, nlen, value, vlen);
    k32e_emit(dst, cap, &out, 0);      /* the list terminator */
    *need = out;
    return (out > K32E_MAX_UNITS) ? K32E_ERROR_NOT_ENOUGH_MEMORY : 0;
}

/* Expands %NAME% in src (slen WCHARs, no NUL needed) with the block blk
 * (units WCHARs, may be 0 for no variables). Same two-call pattern as
 * k32e_set_build: dst NULL gives *need, the total including the final NUL.
 * Returns 0, or K32E_ERROR_NOT_ENOUGH_MEMORY when the result is over
 * K32E_MAX_UNITS. */
static inline int k32e_expand(WCHAR *dst, size_t cap, const WCHAR *src, size_t slen,
                              const WCHAR *blk, size_t units, size_t *need)
{
    size_t out = 0;
    size_t i = 0;
    while (i < slen) {
        if (src[i] == '%') {
            size_t j = i + 1;
            while (j < slen && src[j] != '%')
                ++j;
            if (j < slen && j > i + 1) {
                const WCHAR *name = src + i + 1;
                size_t nlen = j - i - 1;
                size_t voff = 0, vlen = 0;
                if (blk && k32e_find(blk, units, name, nlen, &voff, &vlen)) {
                    for (size_t k = 0; k < vlen; ++k)
                        k32e_emit(dst, cap, &out, blk[voff + k]);
                } else {
                    for (size_t k = i; k <= j; ++k)
                        k32e_emit(dst, cap, &out, src[k]);
                }
                i = j + 1;
                continue;
            }
        }
        k32e_emit(dst, cap, &out, src[i]);
        ++i;
    }
    k32e_emit(dst, cap, &out, 0);
    *need = out;
    return (out > K32E_MAX_UNITS) ? K32E_ERROR_NOT_ENOUGH_MEMORY : 0;
}

#endif /* NTDLL_K32_ENV_PURE_H */
