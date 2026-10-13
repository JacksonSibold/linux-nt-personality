/*
 * k32_file_pure.h - pure helpers for the kernel32 file shims.
 *
 * CreateFileW, DeleteFileW, CreateDirectoryW, RemoveDirectoryW,
 * GetFileAttributesW, GetFileSize, SetFilePointer and related calls in
 * kernel32.c use the values computed here: the access, share, disposition,
 * flag and attribute mapping to NtCreateFile, the create result rules, the
 * Win32 to DOS path normalisation and the NT object-name form (\??\ prefix),
 * and the seek and size arithmetic.
 *
 * Nothing here touches the TEB, the PEB or the NT syscalls, so
 * tests/ntdll-host can compile it on the host (test_k32_file_pure.c).
 *
 * Error values are Win32 error codes. Unless a function says otherwise, a
 * return of 0 means "no error".
 *
 * Decisions recorded here (see docs/modules/ntdll-win32.md):
 *   - The normaliser resolves a relative name against the process current
 *     directory, which the caller reads from the PEB and passes in. A drive
 *     with no current directory of its own (the "X:foo" form on another drive)
 *     resolves to the root of that drive.
 *   - Paths with the \\?\ or \\.\ prefix are not normalised; the NT name is
 *     the same path under \??\.
 *   - The Win32 long-path limit is 32,767 characters (K32_PATH_MAX_CHARS).
 */
#ifndef NTDLL_K32_FILE_PURE_H
#define NTDLL_K32_FILE_PURE_H

#include "nt_types.h"
#include "ntstatus.h"
#include <stddef.h>
#include <stdint.h>

#define K32_ERROR_FILE_NOT_FOUND     2
#define K32_ERROR_PATH_NOT_FOUND     3
#define K32_ERROR_ACCESS_DENIED      5
#define K32_ERROR_INVALID_PARAMETER  87
#define K32_ERROR_FILE_EXISTS        80
#define K32_ERROR_INVALID_NAME       123
#define K32_ERROR_NEGATIVE_SEEK      131
#define K32_ERROR_ALREADY_EXISTS     183
#define K32_ERROR_FILENAME_EXCED_RANGE 206

/* Win32 CreateFile dwCreationDisposition values. */
#define K32_CREATE_NEW         1
#define K32_CREATE_ALWAYS      2
#define K32_OPEN_EXISTING      3
#define K32_OPEN_ALWAYS        4
#define K32_TRUNCATE_EXISTING  5

/* NtCreateFile IoStatus.Information values. */
#define K32_FILE_SUPERSEDED   0
#define K32_FILE_OPENED       1
#define K32_FILE_CREATED      2
#define K32_FILE_OVERWRITTEN  3

/* Win32 access bits and the NT rights they expand to. */
#define K32_GENERIC_READ     0x80000000u
#define K32_GENERIC_WRITE    0x40000000u
#define K32_GENERIC_EXECUTE  0x20000000u
#define K32_GENERIC_ALL      0x10000000u
#define K32_SYNCHRONIZE      0x00100000u
#define K32_FILE_GENERIC_READ    0x00120089u
#define K32_FILE_GENERIC_WRITE   0x00120116u
#define K32_FILE_GENERIC_EXECUTE 0x001200A0u
#define K32_FILE_ALL_ACCESS      0x001F01FFu

/* Win32 CreateFile dwFlagsAndAttributes flag bits (the high word). */
#define K32_FILE_FLAG_WRITE_THROUGH      0x80000000u
#define K32_FILE_FLAG_OVERLAPPED         0x40000000u
#define K32_FILE_FLAG_NO_BUFFERING       0x20000000u
#define K32_FILE_FLAG_RANDOM_ACCESS      0x10000000u
#define K32_FILE_FLAG_SEQUENTIAL_SCAN    0x08000000u
#define K32_FILE_FLAG_DELETE_ON_CLOSE    0x04000000u
#define K32_FILE_FLAG_BACKUP_SEMANTICS   0x02000000u
#define K32_FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u
#define K32_FILE_FLAG_OPEN_NO_RECALL     0x00100000u

/* NtCreateFile CreateOptions values used here. */
#define K32_FILE_DIRECTORY_FILE             0x00000001u
#define K32_FILE_WRITE_THROUGH              0x00000002u
#define K32_FILE_SEQUENTIAL_ONLY            0x00000004u
#define K32_FILE_NO_INTERMEDIATE_BUFFERING  0x00000008u
#define K32_FILE_SYNCHRONOUS_IO_NONALERT    0x00000020u
#define K32_FILE_NON_DIRECTORY_FILE         0x00000040u
#define K32_FILE_RANDOM_ACCESS              0x00000800u
#define K32_FILE_DELETE_ON_CLOSE            0x00001000u
#define K32_FILE_OPEN_FOR_BACKUP_INTENT     0x00004000u
#define K32_FILE_OPEN_REPARSE_POINT         0x00200000u
#define K32_FILE_OPEN_NO_RECALL             0x00400000u

/* FILE_SHARE_* (the same values in Win32 and NT). */
#define K32_FILE_SHARE_ALL 0x7u

/* NtCreateFile CreateDisposition values (NT, not Win32). */
#define K32_NT_FILE_OPEN    1
#define K32_NT_FILE_CREATE  2

/* Rights used by the delete, create and attribute calls. */
#define K32_DELETE               0x00010000u
#define K32_FILE_LIST_DIRECTORY  0x00000001u
#define K32_FILE_READ_ATTRIBUTES 0x00000080u

/* Longest path in characters (Win32 long-path limit). */
#define K32_PATH_MAX_CHARS 32767u

/* Access for NtCreateFile DesiredAccess. GENERIC_* expand to FILE_GENERIC_*;
 * file-specific and standard rights pass through. SYNCHRONIZE is always set,
 * because synchronous I/O needs it. */
static inline ULONG k32_file_access_to_nt(DWORD a)
{
    ULONG r = (ULONG)(a & 0x0000FFFFu) | (ULONG)(a & 0x001F0000u);
    if (a & K32_GENERIC_ALL)     r |= K32_FILE_ALL_ACCESS;
    if (a & K32_GENERIC_READ)    r |= K32_FILE_GENERIC_READ;
    if (a & K32_GENERIC_WRITE)   r |= K32_FILE_GENERIC_WRITE;
    if (a & K32_GENERIC_EXECUTE) r |= K32_FILE_GENERIC_EXECUTE;
    return r | K32_SYNCHRONIZE;
}

/* ShareAccess: the three FILE_SHARE bits; other bits are dropped. */
static inline ULONG k32_file_share_to_nt(DWORD share)
{
    return (ULONG)(share & K32_FILE_SHARE_ALL);
}

/* A dwCreationDisposition is valid when it is one of the five Win32 values. */
static inline int k32_disposition_valid(DWORD d)
{
    return d >= K32_CREATE_NEW && d <= K32_TRUNCATE_EXISTING;
}

/* CreateOptions for dwFlagsAndAttributes. Without BACKUP_SEMANTICS the open is
 * for a non-directory. Without OVERLAPPED the open is synchronous (the NT
 * synchronous flag, which needs SYNCHRONIZE). Random access wins over
 * sequential scan when both are set (NT rejects having both). */
static inline ULONG k32_file_options(DWORD flags)
{
    ULONG o = 0;
    if (flags & K32_FILE_FLAG_BACKUP_SEMANTICS) o |= K32_FILE_OPEN_FOR_BACKUP_INTENT;
    else                                        o |= K32_FILE_NON_DIRECTORY_FILE;
    if (!(flags & K32_FILE_FLAG_OVERLAPPED)) o |= K32_FILE_SYNCHRONOUS_IO_NONALERT;
    if (flags & K32_FILE_FLAG_WRITE_THROUGH) o |= K32_FILE_WRITE_THROUGH;
    if (flags & K32_FILE_FLAG_NO_BUFFERING) o |= K32_FILE_NO_INTERMEDIATE_BUFFERING;
    if (flags & K32_FILE_FLAG_RANDOM_ACCESS) o |= K32_FILE_RANDOM_ACCESS;
    else if (flags & K32_FILE_FLAG_SEQUENTIAL_SCAN) o |= K32_FILE_SEQUENTIAL_ONLY;
    if (flags & K32_FILE_FLAG_DELETE_ON_CLOSE) o |= K32_FILE_DELETE_ON_CLOSE;
    if (flags & K32_FILE_FLAG_OPEN_REPARSE_POINT) o |= K32_FILE_OPEN_REPARSE_POINT;
    if (flags & K32_FILE_FLAG_OPEN_NO_RECALL) o |= K32_FILE_OPEN_NO_RECALL;
    return o;
}

/* FileAttributes for a create: the Win32 attribute bits (READONLY 0x1, HIDDEN
 * 0x2, SYSTEM 0x4, ARCHIVE 0x20, NORMAL 0x80, TEMPORARY 0x100). NORMAL is
 * dropped when another attribute is set, as Win32 does. */
static inline ULONG k32_file_attributes_to_nt(DWORD fa)
{
    ULONG a = (ULONG)(fa & 0x000001A7u);
    if (a & ~0x80u) a &= ~0x80u;
    return a;
}

/* Error for a successful create: CREATE_ALWAYS and OPEN_ALWAYS that found the
 * file already there report ERROR_ALREADY_EXISTS (the call still succeeds).
 * Otherwise 0. */
static inline DWORD k32_create_success_error(DWORD disp, ULONG_PTR info)
{
    int existed = info == K32_FILE_OPENED || info == K32_FILE_OVERWRITTEN;
    if ((disp == K32_CREATE_ALWAYS || disp == K32_OPEN_ALWAYS) && existed)
        return K32_ERROR_ALREADY_EXISTS;
    return 0;
}

/* Error override for a failed create. CREATE_NEW on an existing file is
 * ERROR_FILE_EXISTS (80), not the ERROR_ALREADY_EXISTS (183) that the NT status
 * maps to by default. Returns 0 when the default mapping applies. */
static inline DWORD k32_create_error_override(DWORD disp, NTSTATUS s)
{
    if (disp == K32_CREATE_NEW && s == STATUS_OBJECT_NAME_COLLISION)
        return K32_ERROR_FILE_EXISTS;
    return 0;
}

/* ---- Path normalisation ------------------------------------------------- */

/* A WCHAR output buffer with room for a terminating NUL. */
typedef struct {
    WCHAR *buf;
    size_t cap;
    size_t len;
} k32_wbuf_t;

static inline int k32_wput(k32_wbuf_t *b, WCHAR c)
{
    if (b->len + 1 >= b->cap) return 0;
    b->buf[b->len++] = c;
    return 1;
}

static inline int k32_path_sep(WCHAR c)     { return c == '\\' || c == '/'; }
static inline int k32_path_alpha(WCHAR c)   { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static inline WCHAR k32_path_upper(WCHAR c)    { return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c; }

/* Characters that are not allowed in a file name segment. */
static inline int k32_bad_name_char(WCHAR c)
{
    return c < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*';
}

/* Root kinds from k32_root_of. */
#define K32_ROOT_UNC       1   /* \\server\share */
#define K32_ROOT_DRIVE     2   /* X:\ (absolute drive) */
#define K32_ROOT_DRIVE_REL 3   /* X: (drive-relative) */
#define K32_ROOT_RELATIVE  4   /* \ (rooted, no drive) */
#define K32_ROOT_NONE      5   /* relative, no root */

/* Writes the root of s into ob (for UNC and drive roots; a rooted path writes
 * nothing) and reports its kind, the length of the root in ob, and how many
 * characters of s the root used. */
static inline DWORD k32_root_of(const WCHAR *s, size_t n, k32_wbuf_t *ob,
                                size_t *rootlen, size_t *used, int *kind)
{
    *rootlen = 0;
    *used = 0;
    *kind = K32_ROOT_NONE;
    if (n >= 2 && k32_path_sep(s[0]) && k32_path_sep(s[1])) {
        size_t i = 2, ss = 2;
        while (i < n && !k32_path_sep(s[i])) ++i;
        size_t sl = i - ss;
        if (sl == 0 || i >= n) return K32_ERROR_INVALID_NAME;
        for (size_t j = ss; j < i; ++j) if (k32_bad_name_char(s[j])) return K32_ERROR_INVALID_NAME;
        ++i;
        size_t hs = i;
        while (i < n && !k32_path_sep(s[i])) ++i;
        size_t hl = i - hs;
        if (hl == 0) return K32_ERROR_INVALID_NAME;
        for (size_t j = hs; j < i; ++j) if (k32_bad_name_char(s[j])) return K32_ERROR_INVALID_NAME;
        if (!k32_wput(ob, '\\') || !k32_wput(ob, '\\')) return K32_ERROR_FILENAME_EXCED_RANGE;
        for (size_t j = ss; j < ss + sl; ++j) if (!k32_wput(ob, s[j])) return K32_ERROR_FILENAME_EXCED_RANGE;
        if (!k32_wput(ob, '\\')) return K32_ERROR_FILENAME_EXCED_RANGE;
        for (size_t j = hs; j < hs + hl; ++j) if (!k32_wput(ob, s[j])) return K32_ERROR_FILENAME_EXCED_RANGE;
        *rootlen = ob->len;
        *used = i;
        *kind = K32_ROOT_UNC;
        return 0;
    }
    if (n >= 2 && k32_path_alpha(s[0]) && s[1] == ':') {
        if (!k32_wput(ob, s[0]) || !k32_wput(ob, ':')) return K32_ERROR_FILENAME_EXCED_RANGE;
        *rootlen = 2;
        *used = 2;
        *kind = (n >= 3 && k32_path_sep(s[2])) ? K32_ROOT_DRIVE : K32_ROOT_DRIVE_REL;
        return 0;
    }
    if (n >= 1 && k32_path_sep(s[0])) {
        *used = 1;
        *kind = K32_ROOT_RELATIVE;
        return 0;
    }
    return 0;
}

/* Appends the segments of s to ob, applying '.' and '..'. '..' never removes
 * the root (rootlen). Trailing spaces and dots are dropped from each segment,
 * as Win32 does. */
static inline DWORD k32_push_segments(k32_wbuf_t *ob, size_t rootlen, const WCHAR *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        while (i < n && k32_path_sep(s[i])) ++i;
        if (i >= n) break;
        size_t st = i;
        while (i < n && !k32_path_sep(s[i])) ++i;
        size_t len = i - st;
        for (size_t j = st; j < i; ++j)
            if (k32_bad_name_char(s[j])) return K32_ERROR_INVALID_NAME;
        if (len == 1 && s[st] == '.') continue;
        if (len == 2 && s[st] == '.' && s[st + 1] == '.') {
            if (ob->len > rootlen) {
                while (ob->len > rootlen && ob->buf[ob->len - 1] != '\\') --ob->len;
                if (ob->len > rootlen) --ob->len;
            }
            continue;
        }
        size_t keep = len;
        while (keep > 0 && (s[st + keep - 1] == ' ' || s[st + keep - 1] == '.')) --keep;
        if (keep == 0) continue;
        if (!k32_wput(ob, '\\')) return K32_ERROR_FILENAME_EXCED_RANGE;
        for (size_t j = st; j < st + keep; ++j)
            if (!k32_wput(ob, s[j])) return K32_ERROR_FILENAME_EXCED_RANGE;
    }
    return 0;
}

/* 1 when cwd names the same drive letter as letter. */
static inline int k32_cwd_is_drive(const WCHAR *cwd, size_t cwdn, WCHAR letter)
{
    return cwdn >= 2 && k32_path_alpha(cwd[0]) && cwd[1] == ':' && k32_path_upper(cwd[0]) == k32_path_upper(letter);
}

/* Converts a Win32 path to an absolute DOS path in out (NUL-terminated;
 * *outn gets the length without the NUL).
 *
 *   in, n       the name as given (not necessarily terminated)
 *   cwd, cwdn   the process current directory (absolute DOS path)
 *   out, cap    output buffer (cap counts the NUL)
 *
 * Returns 0 or a Win32 error: ERROR_PATH_NOT_FOUND for an empty name or a
 * relative name with no current directory, ERROR_INVALID_NAME for a bad
 * server, share or segment, ERROR_FILENAME_EXCED_RANGE when the result does not
 * fit cap or the 32,767-character limit. */
static inline DWORD k32_path_dos_normalise(const WCHAR *in, size_t n,
                                           const WCHAR *cwd, size_t cwdn,
                                           WCHAR *out, size_t cap, size_t *outn)
{
    if (outn) *outn = 0;
    if (!in || !out || cap < 2) return K32_ERROR_INVALID_PARAMETER;
    if (n == 0) return K32_ERROR_PATH_NOT_FOUND;
    if (n > K32_PATH_MAX_CHARS) return K32_ERROR_FILENAME_EXCED_RANGE;
    k32_wbuf_t ob = { out, cap, 0 };
    DWORD err;

    /* Extended (\\?\) and device (\\.\) names are kept as given. */
    if (n >= 4 && k32_path_sep(in[0]) && k32_path_sep(in[1]) &&
        (in[2] == '?' || in[2] == '.') && k32_path_sep(in[3])) {
        for (size_t i = 0; i < n; ++i)
            if (!k32_wput(&ob, in[i])) return K32_ERROR_FILENAME_EXCED_RANGE;
        out[ob.len] = 0;
        if (outn) *outn = ob.len;
        return 0;
    }

    size_t rootlen = 0, used = 0;
    int kind = K32_ROOT_NONE;
    err = k32_root_of(in, n, &ob, &rootlen, &used, &kind);
    if (err) return err;

    int relative = kind == K32_ROOT_NONE ||
                   (kind == K32_ROOT_DRIVE_REL && k32_cwd_is_drive(cwd, cwdn, in[0]));
    int rooted = kind == K32_ROOT_RELATIVE;
    if (relative || rooted) {
        /* Start from the current directory's root. A relative name also takes
         * the current directory's segments; a rooted name keeps only the root. */
        ob.len = 0;
        if (cwdn == 0) return K32_ERROR_PATH_NOT_FOUND;
        size_t crl = 0, cused = 0;
        int ck = K32_ROOT_NONE;
        err = k32_root_of(cwd, cwdn, &ob, &crl, &cused, &ck);
        if (err) return err;
        if (ck != K32_ROOT_UNC && ck != K32_ROOT_DRIVE) return K32_ERROR_PATH_NOT_FOUND;
        rootlen = crl;
        if (relative) {
            err = k32_push_segments(&ob, rootlen, cwd + cused, cwdn - cused);
            if (err) return err;
        }
    }
    /* A drive-relative name on another drive resolves against that drive's
     * root (its current directory is not tracked here). */

    err = k32_push_segments(&ob, rootlen, in + used, n - used);
    if (err) return err;
    if (ob.len == rootlen && rootlen > 0) {
        if (!k32_wput(&ob, '\\')) return K32_ERROR_FILENAME_EXCED_RANGE;
    }
    if (ob.len == 0) return K32_ERROR_PATH_NOT_FOUND;
    out[ob.len] = 0;
    if (outn) *outn = ob.len;
    return 0;
}

/* Converts an absolute DOS path to the NT object name, written to out (not
 * NUL-terminated by the function; the caller adds it when needed). Returns the
 * number of characters, or 0 when the name does not fit cap.
 *   X:\rest            -> \??\X:\rest
 *   \\server\share\x   -> \??\UNC\server\share\x
 *   \\?\rest, \\.\rest -> \??\rest */
static inline size_t k32_path_to_nt(const WCHAR *dos, size_t n, WCHAR *out, size_t cap)
{
    if (!dos || n == 0) return 0;
    size_t skip = 0;
    int unc = 0;
    if (n >= 4 && k32_path_sep(dos[0]) && k32_path_sep(dos[1]) &&
        (dos[2] == '?' || dos[2] == '.') && k32_path_sep(dos[3])) {
        skip = 4;
    } else if (n >= 2 && k32_path_sep(dos[0]) && k32_path_sep(dos[1])) {
        unc = 1;
        skip = 2;
    }
    size_t need = 4 + (unc ? 4 : 0) + (n - skip);
    if (need > K32_PATH_MAX_CHARS + 4 || need + 1 > cap) return 0;
    size_t o = 0;
    out[o++] = '\\'; out[o++] = '?'; out[o++] = '?'; out[o++] = '\\';
    if (unc) {
        out[o++] = 'U'; out[o++] = 'N'; out[o++] = 'C'; out[o++] = '\\';
    }
    for (size_t i = skip; i < n; ++i) out[o++] = dos[i];
    return o;
}

/* ---- Sizes, seeks and attributes --------------------------------------- */

/* GetFileSize: the low DWORD is returned and the high DWORD goes to hi. */
static inline void k32_file_size_split(LONGLONG size, DWORD *lo, DWORD *hi)
{
    uint64_t u = (uint64_t)size;
    if (lo) *lo = (DWORD)(u & 0xFFFFFFFFu);
    if (hi) *hi = (DWORD)(u >> 32);
}

/* SetFilePointer(Ex) target. method 0 FILE_BEGIN (base 0), 1 FILE_CURRENT
 * (base cur), 2 FILE_END (base end). Returns 0, or ERROR_INVALID_PARAMETER for
 * an unknown method or an overflow, or ERROR_NEGATIVE_SEEK when the target is
 * before the start. */
static inline DWORD k32_seek_target(DWORD method, LONGLONG cur, LONGLONG end,
                                    LONGLONG dist, LONGLONG *target)
{
    LONGLONG base;
    switch (method) {
    case 0:  base = 0;   break;
    case 1:  base = cur; break;
    case 2:  base = end; break;
    default: return K32_ERROR_INVALID_PARAMETER;
    }
    if ((dist > 0 && base > INT64_MAX - dist) || (dist < 0 && base < INT64_MIN - dist))
        return K32_ERROR_INVALID_PARAMETER;
    LONGLONG t = base + dist;
    if (t < 0) return K32_ERROR_NEGATIVE_SEEK;
    *target = t;
    return 0;
}

/* GetFileAttributes result from the NT FileAttributes: the low 15 bits, with
 * NORMAL dropped when other bits are set, and NORMAL when none are set. */
static inline DWORD k32_attributes_from_nt(ULONG nt)
{
    DWORD a = (DWORD)(nt & 0x7FFFu);
    if (a & ~0x80u) a &= ~0x80u;
    if (a == 0) a = 0x80u;
    return a;
}

#endif /* NTDLL_K32_FILE_PURE_H */
