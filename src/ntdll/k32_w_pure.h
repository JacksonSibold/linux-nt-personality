/*
 * k32_w_pure.h - pure rules for the wide-character file and path APIs (kernel32_w.c): the FindFirstFileW wildcard
 * match (with the DOS wildcards the NT file systems understand), the GetFullPathNameW result and file part, the
 * current directory in its stored and reported forms, MoveFileEx flag checks, and the DOS attribute and time
 * conversions of a directory entry.
 *
 * Nothing here touches the TEB or the NT services, so tests/ntdll-host compiles it on the host
 * (test_k32_w_pure.c).
 */
#ifndef NTDLL_K32_W_PURE_H
#define NTDLL_K32_W_PURE_H

#include "nt_types.h"
#include <stddef.h>
#include <stdint.h>

#include "../crt/ucrt_ctype.h"

#define K32W_MAX_PATH 260u

#define K32W_MOVEFILE_REPLACE_EXISTING 0x1u
#define K32W_MOVEFILE_COPY_ALLOWED 0x2u
#define K32W_MOVEFILE_DELAY_UNTIL_REBOOT 0x4u
#define K32W_MOVEFILE_WRITE_THROUGH 0x8u
#define K32W_MOVEFILE_CREATE_HARDLINK 0x10u
#define K32W_MOVEFILE_FAIL_IF_NOT_TRACKABLE 0x20u
#define K32W_MOVEFILE_VALID_MASK 0x3Fu

#define K32W_ERROR_INVALID_PARAMETER 87u

/* Case folding for names: simple Unicode upper case. */
static inline WCHAR k32w_upcase(WCHAR c)
{
    return (WCHAR)ucrt_towupper((ucrt_wchar)c);
}

/* The wildcard expression of the NT file systems (FsRtlIsNameInExpression): '*' any run, '?' one character, and the DOS
 * forms '<' (a star that stops at the last dot), '>' (one character, or none at the end of the name or before a dot) and
 * '"' (a dot, or the end of the name). Compared without regard to case. */
static inline int k32w_wild_match(const WCHAR *pat, size_t pn, const WCHAR *name, size_t nn)
{
    /* the classic backtracking matcher, extended for the DOS wildcards by treating them as star or optional character */
    size_t p = 0, i = 0, star_p = (size_t)-1, star_i = 0;
    /* "<" matches any run that does not contain the last dot of the name; find the last dot once */
    size_t last_dot = (size_t)-1;
    for (size_t k = 0; k < nn; ++k)
        if (name[k] == '.')
            last_dot = k;
    for (;;) {
        if (p < pn) {
            WCHAR pc = pat[p];
            if (pc == '*' || pc == '<') {
                star_p = p++;
                star_i = i;
                continue;
            }
            if (pc == '>') { /* one character other than a dot, or nothing at the end of the name or before a dot */
                if (i < nn && name[i] != '.') {
                    ++p;
                    ++i;
                    continue;
                }
                ++p;
                continue;
            }
            if (pc == '"') { /* a dot, or the end of the name */
                if (i < nn && name[i] == '.') {
                    ++p;
                    ++i;
                    continue;
                }
                if (i == nn) {
                    ++p;
                    continue;
                }
            } else if (i < nn && (pc == '?' || k32w_upcase(pc) == k32w_upcase(name[i]))) {
                ++p;
                ++i;
                continue;
            }
        } else if (i == nn) {
            return 1;
        }
        /* mismatch: let the last star take one more character (a '<' star may not swallow the last dot) */
        if (star_p != (size_t)-1 && star_i < nn) {
            if (pat[star_p] == '<' && last_dot != (size_t)-1 && star_i == last_dot) {
                /* this star stops before the last dot: no more to give */
            } else {
                i = ++star_i;
                p = star_p + 1;
                continue;
            }
        }
        return 0;
    }
}

/* Win32 turns "*.*" into "*" (every name) before the file system sees it. Returns the pattern length to use. */
static inline size_t k32w_pattern_normalise(const WCHAR *pat, size_t n, WCHAR *out, size_t cap)
{
    size_t i = 0;
    if (n == 3 && pat[0] == '*' && pat[1] == '.' && pat[2] == '*') {
        if (cap < 1)
            return 0;
        out[0] = '*';
        return 1;
    }
    for (; i < n && i < cap; ++i)
        out[i] = pat[i];
    return i;
}

/* Splits a search path into the directory (everything before the last separator) and the pattern. The directory keeps
 * its separator, so the length is also the offset of the pattern. */
static inline size_t k32w_dir_len(const WCHAR *path, size_t n)
{
    size_t k = n;
    while (k > 0 && path[k - 1] != '\\' && path[k - 1] != '/' && !(k == 2 && path[1] == ':'))
        --k;
    return k;
}

/* Does the pattern contain a wildcard? A search for a name without one is a single lookup. */
static inline int k32w_has_wildcard(const WCHAR *pat, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (pat[i] == '*' || pat[i] == '?' || pat[i] == '<' || pat[i] == '>' || pat[i] == '"')
            return 1;
    return 0;
}

/* GetFullPathName's result convention. full is the normalised path (n characters). If it fits in cap characters (with the
 * NUL) it is copied and the length without the NUL returned, and *file_part is set to the last component (NULL when the
 * path ends with a separator or names only a root); otherwise the size needed including the NUL is returned and nothing
 * is written. */
static inline DWORD k32w_full_path_result(const WCHAR *full, size_t n, WCHAR *buf, DWORD cap, WCHAR **file_part)
{
    size_t i, last_sep = (size_t)-1, root_end = 0;
    if (file_part)
        *file_part = NULL;
    if (n + 1 > cap)
        return (DWORD)(n + 1);
    for (i = 0; i < n; ++i) {
        buf[i] = full[i];
        if (full[i] == '\\')
            last_sep = i;
    }
    buf[n] = 0;
    /* where the root ends: "X:\", "\\server\share\", "\\?\X:\" */
    if (n >= 3 && full[1] == ':' && full[2] == '\\') {
        root_end = 3;
    } else if (n >= 2 && full[0] == '\\' && full[1] == '\\') {
        size_t seps = 0;
        for (i = 2; i < n; ++i) {
            if (full[i] == '\\' && ++seps == 2) {
                root_end = i + 1;
                break;
            }
        }
        if (!root_end)
            root_end = n;
    }
    if (file_part && last_sep != (size_t)-1 && last_sep + 1 < n && last_sep + 1 >= root_end && last_sep + 1 > 0)
        *file_part = buf + last_sep + 1;
    return (DWORD)n;
}

/* The process current directory is stored with a trailing backslash ("C:\dir\"); GetCurrentDirectory reports it without,
 * except for a root ("C:\"). Returns the reported length (without the NUL). */
static inline size_t k32w_curdir_reported_len(const WCHAR *stored, size_t n)
{
    if (n > 3 && stored[n - 1] == '\\')
        return n - 1;
    return n;
}

/* The stored form of an absolute path: the path with a trailing backslash added when missing. Returns the length or 0 if it
 * does not fit. */
static inline size_t k32w_curdir_stored(const WCHAR *path, size_t n, WCHAR *out, size_t cap)
{
    size_t need = n + ((n > 0 && path[n - 1] != '\\') ? 1 : 0);
    if (need + 1 > cap)
        return 0;
    for (size_t i = 0; i < n; ++i)
        out[i] = path[i];
    if (need > n)
        out[n] = '\\';
    out[need] = 0;
    return need;
}

/* GetCurrentDirectory's result convention (the same as GetFullPathName's, without a file part). */
static inline DWORD k32w_curdir_result(const WCHAR *stored, size_t n, WCHAR *buf, DWORD cap)
{
    size_t len = k32w_curdir_reported_len(stored, n);
    if (len + 1 > cap)
        return (DWORD)(len + 1);
    for (size_t i = 0; i < len; ++i)
        buf[i] = stored[i];
    buf[len] = 0;
    return (DWORD)len;
}

/* The 8.3 name for WIN32_FIND_DATA.cAlternateFileName: only when the NT entry carries one and the name is not already
 * short. n is the short name length in characters; copies at most 13 (12 + NUL). */
static inline void k32w_alt_name(const WCHAR *short_name, size_t n, WCHAR *out)
{
    size_t i = 0;
    for (; i < n && i < 12; ++i)
        out[i] = short_name[i];
    out[i] = 0;
}

/* Is the name one of the dot entries that FindFirstFile returns for a directory listing? */
static inline int k32w_is_dot_entry(const WCHAR *name, size_t n)
{
    return (n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.');
}

/* MoveFileEx flags: unknown bits are an invalid parameter; DELAY_UNTIL_REBOOT cannot be combined with COPY_ALLOWED. */
static inline DWORD k32w_move_flags_check(DWORD flags)
{
    if (flags & ~K32W_MOVEFILE_VALID_MASK)
        return K32W_ERROR_INVALID_PARAMETER;
    if ((flags & K32W_MOVEFILE_DELAY_UNTIL_REBOOT) && (flags & K32W_MOVEFILE_COPY_ALLOWED))
        return K32W_ERROR_INVALID_PARAMETER;
    return 0;
}

/* A 64-bit FILETIME or file size split in two DWORDs. */
static inline void k32w_split64(uint64_t v, DWORD *lo, DWORD *hi)
{
    *lo = (DWORD)(v & 0xFFFFFFFFu);
    *hi = (DWORD)(v >> 32);
}

/* The drive letter bit of GetLogicalDrives: bit 0 for A. */
static inline DWORD k32w_drive_bit(WCHAR letter)
{
    WCHAR u = k32w_upcase(letter);
    return (u >= 'A' && u <= 'Z') ? (1u << (u - 'A')) : 0;
}

/* The root path of a drive for GetVolumePathName and the drive functions: "C:\" from "C:\x\y" or "c:x". */
static inline int k32w_drive_root(const WCHAR *path, size_t n, WCHAR out[4])
{
    if (n < 2 || path[1] != ':' || !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
        return 0;
    out[0] = k32w_upcase(path[0]);
    out[1] = ':';
    out[2] = '\\';
    out[3] = 0;
    return 1;
}

#endif /* NTDLL_K32_W_PURE_H */
