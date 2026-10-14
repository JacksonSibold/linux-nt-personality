/*
 * k32_find_pure.h - pure rules for the kernel32 file search and temp file
 * shims (kernel32_fs.c): the wildcard match for FindFirstFile, the split of a
 * search pattern into directory and mask, the temp path and temp file names,
 * and the environment variable match used to find TMP and TEMP.
 *
 * Names are ASCII here. The wide characters of a directory entry are compared
 * as-is; anything above 0x7F is never equal to an ASCII pattern character.
 */
#ifndef NTDLL_K32_FIND_PURE_H
#define NTDLL_K32_FIND_PURE_H

#include "nt_types.h"

/* MAX_PATH: the cFileName buffer holds 259 characters and a NUL. */
#define K32F_MAX_NAME 259u

/* Win32 path separators. */
#define K32F_IS_SEP(c) ((c) == '\\' || (c) == '/')

/* ASCII case fold for the match and the environment name compare. */
static inline unsigned int k32f_fold(unsigned int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    return c;
}

/* Wildcard match: '*' is any run (including empty), '?' is one character.
 * Case-insensitive, as NTFS and Win32 search are for ASCII. name is n wide
 * characters, pat is a NUL-terminated narrow pattern. Iterative, so a long
 * pattern cannot exhaust the stack. */
static inline int k32f_wild_match(const char *pat, const WCHAR *name, size_t n)
{
    const size_t none = (size_t)-1;
    size_t p = 0, i = 0;
    size_t star_p = none, star_i = 0;
    for (;;) {
        if (pat[p] == '*') {
            star_p = p++;
            star_i = i;
            continue;
        }
        if (i == n) {
            if (pat[p] == '\0')
                return 1;
        } else if (pat[p] != '\0' &&
                   (pat[p] == '?' ||
                    (name[i] <= 0x7Fu && k32f_fold((unsigned char)pat[p]) == k32f_fold((unsigned int)name[i])))) {
            ++p;
            ++i;
            continue;
        }
        /* Mismatch: the last '*' takes one more character and the match
         * resumes after it. */
        if (star_p != none && star_i < n) {
            i = ++star_i;
            p = star_p + 1;
            continue;
        }
        return 0;
    }
}

/* FindFirstFile splits the pattern at the last separator. The directory is
 * the text before it, and the mask is the rest. Returns the directory length
 * in characters (0 for a pattern with no separator, meaning the current
 * directory). */
static inline size_t k32f_dir_len(const char *path)
{
    size_t dir = 0;
    for (size_t i = 0; path[i] != '\0'; ++i) {
        if (K32F_IS_SEP(path[i]))
            dir = i;
    }
    return dir;
}

/* The entry name fits in cFileName. */
static inline int k32f_name_fits(size_t n)
{
    return n <= K32F_MAX_NAME;
}

/* The temp path from an environment value (TMP or TEMP), with the trailing
 * separator Win32 returns. value may be NULL or empty, meaning the fallback
 * C:\tmp\ (the personality maps C:\tmp to /tmp). Returns the length, or 0 when
 * cap is too small to hold it with the NUL. */
static inline size_t k32f_temp_path(const char *value, char *out, size_t cap)
{
    const char *fallback = "C:\\tmp\\";
    const char *src = (value && value[0] != '\0') ? value : fallback;
    size_t n = 0;
    while (src[n] != '\0')
        ++n;
    int needs_sep = !(n > 0 && K32F_IS_SEP(src[n - 1]));
    size_t total = n + (needs_sep ? 1u : 0u);
    if (total + 1 > cap)
        return 0;
    for (size_t i = 0; i < n; ++i)
        out[i] = src[i];
    if (needs_sep)
        out[n] = '\\';
    out[total] = '\0';
    return total;
}

/* GetTempFileName name: the directory (with a separator), the first three
 * characters of the prefix, the unique number as four hex digits, and ".tmp".
 * Returns the length, or 0 when cap is too small. */
static inline size_t k32f_temp_file_name(const char *dir, const char *prefix, unsigned int unique,
                                         char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t d = 0;
    while (dir[d] != '\0')
        ++d;
    size_t p = 0;
    while (p < 3 && prefix[p] != '\0')
        ++p;
    int needs_sep = !(d > 0 && K32F_IS_SEP(dir[d - 1]));
    /* dir, separator, prefix, 4 hex digits, ".tmp", NUL. */
    size_t total = d + (needs_sep ? 1u : 0u) + p + 4u + 4u;
    if (total + 1 > cap)
        return 0;
    size_t pos = 0;
    for (size_t i = 0; i < d; ++i)
        out[pos++] = dir[i];
    if (needs_sep)
        out[pos++] = '\\';
    for (size_t i = 0; i < p; ++i)
        out[pos++] = prefix[i];
    for (int shift = 12; shift >= 0; shift -= 4)
        out[pos++] = hex[(unique >> shift) & 0xF];
    const char *ext = ".tmp";
    for (size_t i = 0; ext[i] != '\0'; ++i)
        out[pos++] = ext[i];
    out[pos] = '\0';
    return pos;
}

/* Does the environment entry "NAME=value" (n wide characters, not NUL
 * terminated) name the variable name? The compare is case-insensitive. On a
 * match, *value_off is the index of the first character of value. */
static inline int k32f_env_name_match(const WCHAR *entry, size_t n, const char *name, size_t *value_off)
{
    size_t i = 0;
    for (; name[i] != '\0'; ++i) {
        if (i >= n)
            return 0;
        WCHAR c = entry[i];
        if (c > 0x7Fu || k32f_fold((unsigned int)c) != k32f_fold((unsigned char)name[i]))
            return 0;
    }
    if (i >= n || entry[i] != '=')
        return 0;
    *value_off = i + 1;
    return 1;
}

#endif /* NTDLL_K32_FIND_PURE_H */
