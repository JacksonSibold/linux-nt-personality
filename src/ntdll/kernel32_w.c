/*
 * kernel32_w.c - the wide-character file and path APIs: FindFirstFileW/ExW, FindNextFileW (and the A forms, which
 * convert through them), GetFullPathName, GetLongPathName, GetShortPathName, GetCurrentDirectory and
 * SetCurrentDirectory, GetFileAttributesExW, SetFileAttributesW, SetFileTime, MoveFileEx, CopyFile, the volume and
 * drive functions, and GetFileInformationByHandleEx / SetFileInformationByHandle.
 *
 * Built on the exported CreateFileW and friends of kernel32.c (which serialise their use of shared path buffers),
 * so this file keeps no path buffers of its own: names that need working space take it from the process heap.
 * The rules are in k32_w_pure.h and k32_file_pure.h.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include "k32_file_pure.h"
#include "k32_module_pure.h"
#include "k32_w_pure.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif

typedef PVOID LPVOID;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef UINT
#define UINT unsigned int
#endif

#define KW_INVALID_HANDLE ((HANDLE)(intptr_t)-1)
#define KW_ERROR_FILE_NOT_FOUND 2u
#define KW_ERROR_PATH_NOT_FOUND 3u
#define KW_ERROR_NOT_ENOUGH_MEMORY 8u
#define KW_ERROR_NO_MORE_FILES 18u
#define KW_ERROR_NOT_SUPPORTED 50u
#define KW_ERROR_INVALID_PARAMETER 87u
#define KW_ERROR_INSUFFICIENT_BUFFER 122u
#define KW_ERROR_INVALID_LEVEL 124u
#define KW_ERROR_FILENAME_EXCED_RANGE 206u
#define KW_ERROR_MORE_DATA 234u
#define KW_FILE_LIST_DIRECTORY 0x00000001u
#define KW_FILE_READ_ATTRIBUTES 0x00000080u
#define KW_FILE_WRITE_ATTRIBUTES 0x00000100u
#define KW_DELETE 0x00010000u
#define KW_SYNCHRONIZE 0x00100000u
#define KW_GENERIC_READ 0x80000000u
#define KW_GENERIC_WRITE 0x40000000u
#define KW_FILE_SHARE_ALL 7u
#define KW_CREATE_NEW 1u
#define KW_CREATE_ALWAYS 2u
#define KW_OPEN_EXISTING 3u
#define KW_FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define KW_FILE_ATTRIBUTE_DIRECTORY 0x10u
#define KW_FILE_ATTRIBUTE_NORMAL 0x80u
#define KW_FILE_ATTRIBUTE_REPARSE_POINT 0x400u
#define KW_INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define KW_STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006L)
#define KW_STATUS_NOT_SAME_DEVICE ((NTSTATUS)0xC00000D4L)
#define KW_DIR_BUFFER 16384u

/* information classes */
#define KW_CLASS_FS_VOLUME 1u
#define KW_CLASS_FS_SIZE 3u
#define KW_CLASS_FS_DEVICE 4u
#define KW_CLASS_FS_ATTRIBUTE 5u
#define KW_CLASS_FS_FULLSIZE 7u
#define KW_CLASS_BOTH_DIR 3u
#define KW_CLASS_BASIC 4u
#define KW_CLASS_STANDARD 5u
#define KW_CLASS_NAME 9u
#define KW_CLASS_RENAME 10u
#define KW_CLASS_DISPOSITION 13u
#define KW_CLASS_ALLOCATION 19u
#define KW_CLASS_EOF 20u

__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);
__declspec(dllimport) extern NTSTATUS NtQueryDirectoryFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG, BOOLEAN, PUNICODE_STRING, BOOLEAN);
__declspec(dllimport) extern NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES, PVOID);
__declspec(dllimport) extern NTSTATUS NtClose(HANDLE);
__declspec(dllimport) extern NTSTATUS NtOpenSymbolicLinkObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);

/* Defined in kernel32.c and its neighbours. */
void SetLastError(DWORD e);
DWORD GetLastError(void);
HANDLE WINAPI CreateFileW(const WCHAR *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE templ);
BOOL WINAPI CloseHandle(HANDLE h);
BOOL WINAPI ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov);
BOOL WINAPI WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *wrote, void *ov);
DWORD WINAPI GetFileAttributesW(const WCHAR *name);
BOOL WINAPI DeleteFileW(const WCHAR *name);
HANDLE WINAPI GetProcessHeap(void);
PVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, PVOID p);
BOOL WINAPI GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);

static PTEB kw_teb(void)
{
    PTEB teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    return teb;
}

static void kw_status(NTSTATUS s)
{
    SetLastError(NT_SUCCESS(s) ? 0 : RtlNtStatusToDosError(s));
}

static void *kw_alloc(size_t n)
{
    return HeapAlloc(GetProcessHeap(), 0, n);
}

static void kw_free(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}

static size_t kw_wlen(const WCHAR *s)
{
    size_t n = 0;
    while (n <= K32_PATH_MAX_CHARS && s[n])
        ++n;
    return n;
}

/* The process current directory as stored: "C:\dir\". */
static RTL_USER_PROCESS_PARAMETERS *kw_params(void)
{
    PTEB teb = kw_teb();
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    return peb ? peb->ProcessParameters : NULL;
}

/* Widens an ANSI name into a heap block (caller frees). */
static WCHAR *kw_widen(const char *s)
{
    size_t n = 0;
    WCHAR *w;
    if (!s) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    while (s[n])
        ++n;
    w = (WCHAR *)kw_alloc((n + 1) * sizeof(WCHAR));
    if (!w) {
        SetLastError(KW_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    for (size_t i = 0; i <= n; ++i)
        w[i] = (WCHAR)(unsigned char)s[i];
    return w;
}

/* ---- Full path names --------------------------------------------------------------------------------- */

/* The normalised absolute form of name in a heap block (caller frees), or NULL with the last error set. */
static WCHAR *kw_full_path(const WCHAR *name, size_t *outn)
{
    RTL_USER_PROCESS_PARAMETERS *pp = kw_params();
    const WCHAR *cwd = pp ? pp->CurrentDirectory.DosPath.Buffer : NULL;
    size_t cwdn = pp ? pp->CurrentDirectory.DosPath.Length / sizeof(WCHAR) : 0, n = kw_wlen(name), dn = 0;
    WCHAR *out;
    DWORD err;
    out = (WCHAR *)kw_alloc((n + cwdn + 16) * sizeof(WCHAR));
    if (!out) {
        SetLastError(KW_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    err = k32_path_dos_normalise(name, n, cwd, cwdn, out, n + cwdn + 16, &dn);
    if (err) {
        kw_free(out);
        SetLastError(err);
        return NULL;
    }
    *outn = dn;
    return out;
}

DWORD WINAPI GetFullPathNameW(const WCHAR *name, DWORD cap, WCHAR *buf, WCHAR **file_part)
{
    size_t n;
    WCHAR *full;
    DWORD r;
    if (file_part)
        *file_part = NULL;
    if (!name || !*name) {
        SetLastError(name ? KW_ERROR_PATH_NOT_FOUND : KW_ERROR_INVALID_PARAMETER);
        return 0;
    }
    full = kw_full_path(name, &n);
    if (!full)
        return 0;
    r = k32w_full_path_result(full, n, buf, cap, file_part);
    kw_free(full);
    if (r < cap || (cap == 0 && r > 0))
        SetLastError(0);
    return r;
}

DWORD WINAPI GetFullPathNameA(const char *name, DWORD cap, char *buf, char **file_part)
{
    WCHAR *w = kw_widen(name), *full;
    size_t n;
    DWORD r = 0;
    if (file_part)
        *file_part = NULL;
    if (!w)
        return 0;
    full = kw_full_path(w, &n);
    kw_free(w);
    if (!full)
        return 0;
    if (n + 1 > cap) {
        r = (DWORD)(n + 1);
    } else {
        size_t last = (size_t)-1;
        for (size_t i = 0; i < n; ++i) {
            buf[i] = full[i] <= 0xFF ? (char)full[i] : '?';
            if (full[i] == '\\')
                last = i;
        }
        buf[n] = 0;
        if (file_part && last != (size_t)-1 && last + 1 < n && !(n >= 3 && full[1] == ':' && last + 1 < 3))
            *file_part = buf + last + 1;
        r = (DWORD)n;
    }
    kw_free(full);
    return r;
}

/* The long and short names are the same as the name given: the file systems here keep no separate 8.3 names for the
 * runtime to look up. The file has to exist. */
DWORD WINAPI GetLongPathNameW(const WCHAR *name, WCHAR *buf, DWORD cap)
{
    size_t n;
    WCHAR *full;
    DWORD r;
    if (!name) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return 0;
    }
    full = kw_full_path(name, &n);
    if (!full)
        return 0;
    if (GetFileAttributesW(full) == KW_INVALID_FILE_ATTRIBUTES) {
        kw_free(full);
        return 0;
    }
    r = k32w_full_path_result(full, n, buf, cap, NULL);
    kw_free(full);
    return r;
}

DWORD WINAPI GetShortPathNameW(const WCHAR *name, WCHAR *buf, DWORD cap)
{
    return GetLongPathNameW(name, buf, cap);
}

DWORD WINAPI GetLongPathNameA(const char *name, char *buf, DWORD cap)
{
    WCHAR *w = kw_widen(name), tmp[K32W_MAX_PATH + 8];
    DWORD r;
    if (!w)
        return 0;
    r = GetLongPathNameW(w, tmp, K32W_MAX_PATH + 8);
    kw_free(w);
    if (r == 0 || r >= K32W_MAX_PATH + 8)
        return r;
    if (r + 1 > cap)
        return r + 1;
    for (DWORD i = 0; i <= r; ++i)
        buf[i] = tmp[i] <= 0xFF ? (char)tmp[i] : '?';
    return r;
}

DWORD WINAPI GetShortPathNameA(const char *name, char *buf, DWORD cap)
{
    return GetLongPathNameA(name, buf, cap);
}

/* ---- Current directory -------------------------------------------------------------------------------- */

DWORD WINAPI GetCurrentDirectoryW(DWORD cap, WCHAR *buf)
{
    RTL_USER_PROCESS_PARAMETERS *pp = kw_params();
    DWORD r;
    if (!pp || !pp->CurrentDirectory.DosPath.Buffer) {
        SetLastError(KW_ERROR_PATH_NOT_FOUND);
        return 0;
    }
    r = k32w_curdir_result(pp->CurrentDirectory.DosPath.Buffer, pp->CurrentDirectory.DosPath.Length / sizeof(WCHAR), buf, cap);
    if (r < cap || cap == 0)
        SetLastError(0);
    return r;
}

DWORD WINAPI GetCurrentDirectoryA(DWORD cap, char *buf)
{
    WCHAR tmp[K32W_MAX_PATH + 8];
    DWORD r = GetCurrentDirectoryW(K32W_MAX_PATH + 8, tmp);
    if (r == 0 || r >= K32W_MAX_PATH + 8)
        return r;
    if (r + 1 > cap)
        return r + 1;
    for (DWORD i = 0; i <= r; ++i)
        buf[i] = tmp[i] <= 0xFF ? (char)tmp[i] : '?';
    return r;
}

BOOL WINAPI SetCurrentDirectoryW(const WCHAR *path)
{
    RTL_USER_PROCESS_PARAMETERS *pp = kw_params();
    size_t n, stored;
    WCHAR *full, *neu;
    DWORD attrs;
    if (!path || !pp) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!*path) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    full = kw_full_path(path, &n);
    if (!full)
        return FALSE;
    attrs = GetFileAttributesW(full);
    if (attrs == KW_INVALID_FILE_ATTRIBUTES) {
        kw_free(full);
        return FALSE;
    }
    if (!(attrs & KW_FILE_ATTRIBUTE_DIRECTORY)) {
        kw_free(full);
        SetLastError(267u); /* ERROR_DIRECTORY */
        return FALSE;
    }
    neu = (WCHAR *)kw_alloc((n + 2) * sizeof(WCHAR));
    if (!neu) {
        kw_free(full);
        SetLastError(KW_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    stored = k32w_curdir_stored(full, n, neu, n + 2);
    kw_free(full);
    if (!stored) {
        kw_free(neu);
        SetLastError(KW_ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    /* write in place when the existing buffer is large enough, otherwise swap in the new block */
    if (pp->CurrentDirectory.DosPath.Buffer && (stored + 1) * sizeof(WCHAR) <= pp->CurrentDirectory.DosPath.MaximumLength) {
        memcpy(pp->CurrentDirectory.DosPath.Buffer, neu, (stored + 1) * sizeof(WCHAR));
        pp->CurrentDirectory.DosPath.Length = (USHORT)(stored * sizeof(WCHAR));
        kw_free(neu);
    } else if ((stored + 1) * sizeof(WCHAR) <= 0xFFFFu) {
        pp->CurrentDirectory.DosPath.Buffer = neu; /* the old buffer belongs to whoever built the parameters; leave it */
        pp->CurrentDirectory.DosPath.Length = (USHORT)(stored * sizeof(WCHAR));
        pp->CurrentDirectory.DosPath.MaximumLength = (USHORT)((stored + 1) * sizeof(WCHAR));
    } else {
        kw_free(neu);
        SetLastError(KW_ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetCurrentDirectoryA(const char *path)
{
    WCHAR *w = kw_widen(path);
    BOOL r;
    if (!w)
        return FALSE;
    r = SetCurrentDirectoryW(w);
    kw_free(w);
    return r;
}

/* ---- Directory searches ---------------------------------------------------------------------------- */

typedef struct { DWORD dwLowDateTime; DWORD dwHighDateTime; } KW_FILETIME;

typedef struct {
    DWORD dwFileAttributes;
    KW_FILETIME ftCreationTime;
    KW_FILETIME ftLastAccessTime;
    KW_FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[260];
    WCHAR cAlternateFileName[14];
} KW_FIND_DATAW;

typedef struct {
    DWORD dwFileAttributes;
    KW_FILETIME ftCreationTime;
    KW_FILETIME ftLastAccessTime;
    KW_FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} KW_FIND_DATAA;

/* FILE_BOTH_DIR_INFORMATION (class 3) */
typedef struct {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    CHAR ShortNameLength;
    WCHAR ShortName[12];
    WCHAR FileName[1];
} KW_DIR_ENTRY;

typedef struct {
    HANDLE dir;
    WCHAR mask[K32W_MAX_PATH];
    size_t masklen;
    ULONG pos, valid;
    BOOLEAN restart, eof;
    int basic_info; /* FindExInfoBasic: no alternate name */
    NTSTATUS last;
    BYTE buf[KW_DIR_BUFFER];
} KW_FIND;

static void kw_filetime(const LARGE_INTEGER *t, KW_FILETIME *ft)
{
    k32w_split64((uint64_t)t->QuadPart, &ft->dwLowDateTime, &ft->dwHighDateTime);
}

/* The next matching entry: 1 with data filled, 0 at the end, -1 on an NT error. */
static int kw_find_next(KW_FIND *f, KW_FIND_DATAW *out)
{
    for (;;) {
        KW_DIR_ENTRY *e;
        size_t n;
        if (f->pos >= f->valid) {
            IO_STATUS_BLOCK iosb;
            NTSTATUS s;
            if (f->eof)
                return 0;
            s = NtQueryDirectoryFile(f->dir, NULL, NULL, NULL, &iosb, f->buf, sizeof(f->buf), KW_CLASS_BOTH_DIR, FALSE, NULL, f->restart);
            f->restart = FALSE;
            if (s == KW_STATUS_NO_MORE_FILES) {
                f->eof = TRUE;
                return 0;
            }
            if (!NT_SUCCESS(s)) {
                f->last = s;
                return -1;
            }
            f->valid = (ULONG)iosb.Information;
            f->pos = 0;
            if (f->valid == 0) {
                f->eof = TRUE;
                return 0;
            }
        }
        e = (KW_DIR_ENTRY *)(f->buf + f->pos);
        f->pos = e->NextEntryOffset ? f->pos + e->NextEntryOffset : f->valid;
        n = e->FileNameLength / sizeof(WCHAR);
        if (n >= 260 || !k32w_wild_match(f->mask, f->masklen, e->FileName, n))
            continue;
        memset(out, 0, sizeof(*out));
        out->dwFileAttributes = e->FileAttributes;
        kw_filetime(&e->CreationTime, &out->ftCreationTime);
        kw_filetime(&e->LastAccessTime, &out->ftLastAccessTime);
        kw_filetime(&e->LastWriteTime, &out->ftLastWriteTime);
        k32w_split64((uint64_t)e->EndOfFile.QuadPart, &out->nFileSizeLow, &out->nFileSizeHigh);
        if (e->FileAttributes & KW_FILE_ATTRIBUTE_REPARSE_POINT)
            out->dwReserved0 = e->EaSize; /* the reparse tag */
        for (size_t i = 0; i < n; ++i)
            out->cFileName[i] = e->FileName[i];
        out->cFileName[n] = 0;
        if (!f->basic_info)
            k32w_alt_name(e->ShortName, (size_t)(e->ShortNameLength > 0 ? e->ShortNameLength : 0) / sizeof(WCHAR), out->cAlternateFileName);
        return 1;
    }
}

HANDLE WINAPI FindFirstFileExW(const WCHAR *name, int info_level, void *data, int search_op, void *filter, DWORD flags)
{
    size_t n, dl, ml;
    WCHAR *full, *dir;
    HANDLE d;
    KW_FIND *f;
    int r;
    (void)filter;
    (void)flags;
    if (!name || !data || info_level < 0 || info_level > 1 || search_op < 0 || search_op > 2) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return KW_INVALID_HANDLE;
    }
    if (!*name) {
        SetLastError(KW_ERROR_PATH_NOT_FOUND);
        return KW_INVALID_HANDLE;
    }
    full = kw_full_path(name, &n);
    if (!full)
        return KW_INVALID_HANDLE;
    dl = k32w_dir_len(full, n);
    ml = n - dl;
    /* a pattern of zero length (a path ending in a separator) or one longer than a name can be never matches */
    if (ml == 0) {
        kw_free(full);
        SetLastError(KW_ERROR_FILE_NOT_FOUND);
        return KW_INVALID_HANDLE;
    }
    if (ml >= K32W_MAX_PATH) {
        kw_free(full);
        SetLastError(KW_ERROR_FILENAME_EXCED_RANGE);
        return KW_INVALID_HANDLE;
    }
    dir = (WCHAR *)kw_alloc((dl + 2) * sizeof(WCHAR));
    if (!dir) {
        kw_free(full);
        SetLastError(KW_ERROR_NOT_ENOUGH_MEMORY);
        return KW_INVALID_HANDLE;
    }
    memcpy(dir, full, dl * sizeof(WCHAR));
    dir[dl] = 0;
    d = CreateFileW(dir, KW_FILE_LIST_DIRECTORY | KW_SYNCHRONIZE, KW_FILE_SHARE_ALL, NULL, KW_OPEN_EXISTING, KW_FILE_FLAG_BACKUP_SEMANTICS, NULL);
    kw_free(dir);
    if (d == KW_INVALID_HANDLE) {
        kw_free(full);
        if (GetLastError() == KW_ERROR_FILE_NOT_FOUND)
            SetLastError(KW_ERROR_PATH_NOT_FOUND); /* a missing directory is a missing path here */
        return KW_INVALID_HANDLE;
    }
    f = (KW_FIND *)kw_alloc(sizeof(KW_FIND));
    if (!f) {
        kw_free(full);
        NtClose(d);
        SetLastError(KW_ERROR_NOT_ENOUGH_MEMORY);
        return KW_INVALID_HANDLE;
    }
    f->dir = d;
    f->masklen = k32w_pattern_normalise(full + dl, ml, f->mask, K32W_MAX_PATH);
    kw_free(full);
    f->pos = f->valid = 0;
    f->restart = TRUE;
    f->eof = FALSE;
    f->basic_info = info_level == 1;
    f->last = STATUS_SUCCESS;
    r = kw_find_next(f, (KW_FIND_DATAW *)data);
    if (r == 1) {
        SetLastError(0);
        return (HANDLE)f;
    }
    if (r == 0)
        SetLastError(KW_ERROR_FILE_NOT_FOUND);
    else
        kw_status(f->last);
    NtClose(d);
    kw_free(f);
    return KW_INVALID_HANDLE;
}

HANDLE WINAPI FindFirstFileW(const WCHAR *name, void *data)
{
    return FindFirstFileExW(name, 0, data, 0, NULL, 0);
}

BOOL WINAPI FindNextFileW(HANDLE h, void *data)
{
    KW_FIND *f = (KW_FIND *)h;
    int r;
    if (!f || h == KW_INVALID_HANDLE || !data) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    r = kw_find_next(f, (KW_FIND_DATAW *)data);
    if (r == 1) {
        SetLastError(0);
        return TRUE;
    }
    if (r == 0)
        SetLastError(KW_ERROR_NO_MORE_FILES);
    else
        kw_status(f->last);
    return FALSE;
}

BOOL WINAPI FindClose(HANDLE h)
{
    KW_FIND *f = (KW_FIND *)h;
    if (!f || h == KW_INVALID_HANDLE) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    NtClose(f->dir);
    kw_free(f);
    SetLastError(0);
    return TRUE;
}

static void kw_find_to_ansi(const KW_FIND_DATAW *w, KW_FIND_DATAA *a)
{
    a->dwFileAttributes = w->dwFileAttributes;
    a->ftCreationTime = w->ftCreationTime;
    a->ftLastAccessTime = w->ftLastAccessTime;
    a->ftLastWriteTime = w->ftLastWriteTime;
    a->nFileSizeHigh = w->nFileSizeHigh;
    a->nFileSizeLow = w->nFileSizeLow;
    a->dwReserved0 = w->dwReserved0;
    a->dwReserved1 = w->dwReserved1;
    for (size_t i = 0; i < 260; ++i)
        a->cFileName[i] = w->cFileName[i] <= 0xFF ? (char)w->cFileName[i] : '?';
    for (size_t i = 0; i < 14; ++i)
        a->cAlternateFileName[i] = w->cAlternateFileName[i] <= 0xFF ? (char)w->cAlternateFileName[i] : '?';
    a->cFileName[259] = 0;
    a->cAlternateFileName[13] = 0;
}

HANDLE WINAPI FindFirstFileExA(const char *name, int info_level, void *data, int search_op, void *filter, DWORD flags)
{
    WCHAR *w = kw_widen(name);
    KW_FIND_DATAW wd;
    HANDLE h;
    if (!w)
        return KW_INVALID_HANDLE;
    if (!data) {
        kw_free(w);
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return KW_INVALID_HANDLE;
    }
    h = FindFirstFileExW(w, info_level, &wd, search_op, filter, flags);
    kw_free(w);
    if (h != KW_INVALID_HANDLE)
        kw_find_to_ansi(&wd, (KW_FIND_DATAA *)data);
    return h;
}

HANDLE WINAPI FindFirstFileA(const char *name, void *data)
{
    return FindFirstFileExA(name, 0, data, 0, NULL, 0);
}

BOOL WINAPI FindNextFileA(HANDLE h, void *data)
{
    KW_FIND_DATAW wd;
    if (!data) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!FindNextFileW(h, &wd))
        return FALSE;
    kw_find_to_ansi(&wd, (KW_FIND_DATAA *)data);
    return TRUE;
}

/* ---- Attributes and times -------------------------------------------------------------------------- */

/* FILE_NETWORK_OPEN_INFORMATION, the result of NtQueryFullAttributesFile */
typedef struct {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG FileAttributes;
} KW_NETWORK_OPEN_INFO;

typedef struct {
    DWORD dwFileAttributes;
    KW_FILETIME ftCreationTime;
    KW_FILETIME ftLastAccessTime;
    KW_FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} KW_FILE_ATTRIBUTE_DATA;

typedef struct {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG FileAttributes;
} KW_FILE_BASIC;

/* Opens a file for attribute work; the caller closes it. */
static HANDLE kw_open_attr(const WCHAR *name, DWORD access)
{
    HANDLE h = CreateFileW(name, access | KW_SYNCHRONIZE, KW_FILE_SHARE_ALL, NULL, KW_OPEN_EXISTING, KW_FILE_FLAG_BACKUP_SEMANTICS, NULL);
    return h;
}

BOOL WINAPI GetFileAttributesExW(const WCHAR *name, int level, void *info)
{
    HANDLE h;
    KW_FILE_BASIC bi;
    LARGE_INTEGER size;
    IO_STATUS_BLOCK iosb;
    KW_FILE_ATTRIBUTE_DATA *out = (KW_FILE_ATTRIBUTE_DATA *)info;
    NTSTATUS s;
    if (!name || !info) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (level != 0) { /* only GetFileExInfoStandard exists */
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    h = kw_open_attr(name, KW_FILE_READ_ATTRIBUTES);
    if (h == KW_INVALID_HANDLE)
        return FALSE;
    s = NtQueryInformationFile(h, &iosb, &bi, sizeof(bi), KW_CLASS_BASIC);
    if (!NT_SUCCESS(s)) {
        NtClose(h);
        kw_status(s);
        return FALSE;
    }
    size.QuadPart = 0;
    if ((bi.FileAttributes & KW_FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (!GetFileSizeEx(h, &size))
            size.QuadPart = 0;
    }
    NtClose(h);
    out->dwFileAttributes = bi.FileAttributes;
    kw_filetime(&bi.CreationTime, &out->ftCreationTime);
    kw_filetime(&bi.LastAccessTime, &out->ftLastAccessTime);
    kw_filetime(&bi.LastWriteTime, &out->ftLastWriteTime);
    k32w_split64((uint64_t)size.QuadPart, &out->nFileSizeLow, &out->nFileSizeHigh);
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI GetFileAttributesExA(const char *name, int level, void *info)
{
    WCHAR *w = kw_widen(name);
    BOOL r;
    if (!w)
        return FALSE;
    r = GetFileAttributesExW(w, level, info);
    kw_free(w);
    return r;
}

BOOL WINAPI SetFileAttributesW(const WCHAR *name, DWORD attrs)
{
    HANDLE h;
    KW_FILE_BASIC bi;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    if (!name) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    h = kw_open_attr(name, KW_FILE_WRITE_ATTRIBUTES);
    if (h == KW_INVALID_HANDLE)
        return FALSE;
    memset(&bi, 0, sizeof(bi)); /* zero times: leave them as they are */
    bi.FileAttributes = (attrs & 0x31A7u) ? (attrs & 0x31A7u) : KW_FILE_ATTRIBUTE_NORMAL;
    s = NtSetInformationFile(h, &iosb, &bi, sizeof(bi), KW_CLASS_BASIC);
    NtClose(h);
    if (!NT_SUCCESS(s)) {
        kw_status(s);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetFileAttributesA(const char *name, DWORD attrs)
{
    WCHAR *w = kw_widen(name);
    BOOL r;
    if (!w)
        return FALSE;
    r = SetFileAttributesW(w, attrs);
    kw_free(w);
    return r;
}

BOOL WINAPI SetFileTime(HANDLE h, const KW_FILETIME *created, const KW_FILETIME *accessed, const KW_FILETIME *written)
{
    KW_FILE_BASIC bi;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    memset(&bi, 0, sizeof(bi));
    if (created)
        bi.CreationTime.QuadPart = (LONGLONG)(((ULONGLONG)created->dwHighDateTime << 32) | created->dwLowDateTime);
    if (accessed)
        bi.LastAccessTime.QuadPart = (LONGLONG)(((ULONGLONG)accessed->dwHighDateTime << 32) | accessed->dwLowDateTime);
    if (written)
        bi.LastWriteTime.QuadPart = (LONGLONG)(((ULONGLONG)written->dwHighDateTime << 32) | written->dwLowDateTime);
    s = NtSetInformationFile(h, &iosb, &bi, sizeof(bi), KW_CLASS_BASIC);
    if (!NT_SUCCESS(s)) {
        kw_status(s);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

/* ---- Handle-based file information ------------------------------------------------------------------------- */

typedef struct {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} KW_FILE_STANDARD;

/* FILE_INFO_BY_HANDLE_CLASS values handled: 0 basic, 1 standard, 2 name, 3 rename, 4 disposition, 5 allocation, 6 end of file */
BOOL WINAPI GetFileInformationByHandleEx(HANDLE h, int cls, void *buf, DWORD size)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    switch (cls) {
    case 0: {
        KW_FILE_BASIC *b = (KW_FILE_BASIC *)buf;
        if (size < sizeof(KW_FILE_BASIC)) {
            SetLastError(KW_ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        s = NtQueryInformationFile(h, &iosb, b, sizeof(KW_FILE_BASIC), KW_CLASS_BASIC);
        break;
    }
    case 1: {
        if (size < sizeof(KW_FILE_STANDARD)) {
            SetLastError(KW_ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        s = NtQueryInformationFile(h, &iosb, buf, sizeof(KW_FILE_STANDARD), KW_CLASS_STANDARD);
        break;
    }
    case 2:
        if (size < 8) {
            SetLastError(KW_ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        s = NtQueryInformationFile(h, &iosb, buf, size, KW_CLASS_NAME);
        break;
    default:
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!NT_SUCCESS(s)) {
        kw_status(s);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI SetFileInformationByHandle(HANDLE h, int cls, void *buf, DWORD size)
{
    IO_STATUS_BLOCK iosb;
    ULONG nt_class;
    NTSTATUS s;
    switch (cls) {
    case 0: nt_class = KW_CLASS_BASIC; break;
    case 3: nt_class = KW_CLASS_RENAME; break;
    case 4: nt_class = KW_CLASS_DISPOSITION; break;
    case 5: nt_class = KW_CLASS_ALLOCATION; break;
    case 6: nt_class = KW_CLASS_EOF; break;
    default:
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!buf) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    s = NtSetInformationFile(h, &iosb, buf, size, nt_class);
    if (!NT_SUCCESS(s)) {
        kw_status(s);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

/* ---- Moving and copying -------------------------------------------------------------------------------- */

/* FILE_RENAME_INFORMATION: the destination as a full NT name. */
typedef struct {
    BOOLEAN ReplaceIfExists;
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[1];
} KW_RENAME_INFO;

BOOL WINAPI CopyFileW(const WCHAR *from, const WCHAR *to, BOOL fail_if_exists);

BOOL WINAPI MoveFileExW(const WCHAR *from, const WCHAR *to, DWORD flags)
{
    DWORD err = k32w_move_flags_check(flags);
    HANDLE h;
    WCHAR *full_to = NULL, *nt_to = NULL;
    size_t nto, ntn;
    KW_RENAME_INFO *ri;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    if (err || !from) {
        SetLastError(err ? err : KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (flags & K32W_MOVEFILE_DELAY_UNTIL_REBOOT) {
        SetLastError(KW_ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (!to) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    full_to = kw_full_path(to, &nto);
    if (!full_to)
        return FALSE;
    nt_to = (WCHAR *)kw_alloc((nto + 16) * sizeof(WCHAR));
    if (!nt_to) {
        kw_free(full_to);
        SetLastError(KW_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    ntn = k32_path_to_nt(full_to, nto, nt_to, nto + 16);
    kw_free(full_to);
    if (!ntn) {
        kw_free(nt_to);
        SetLastError(KW_ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    h = CreateFileW(from, KW_DELETE | KW_SYNCHRONIZE, KW_FILE_SHARE_ALL, NULL, KW_OPEN_EXISTING, KW_FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == KW_INVALID_HANDLE) {
        kw_free(nt_to);
        return FALSE;
    }
    ri = (KW_RENAME_INFO *)kw_alloc(sizeof(KW_RENAME_INFO) + ntn * sizeof(WCHAR));
    if (!ri) {
        NtClose(h);
        kw_free(nt_to);
        SetLastError(KW_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    ri->ReplaceIfExists = (flags & K32W_MOVEFILE_REPLACE_EXISTING) ? TRUE : FALSE;
    ri->RootDirectory = NULL;
    ri->FileNameLength = (ULONG)(ntn * sizeof(WCHAR));
    memcpy(ri->FileName, nt_to, ntn * sizeof(WCHAR));
    kw_free(nt_to);
    s = NtSetInformationFile(h, &iosb, ri, (ULONG)(sizeof(KW_RENAME_INFO) + ntn * sizeof(WCHAR)), KW_CLASS_RENAME);
    kw_free(ri);
    NtClose(h);
    if (s == KW_STATUS_NOT_SAME_DEVICE && (flags & K32W_MOVEFILE_COPY_ALLOWED)) {
        if (!CopyFileW(from, to, (flags & K32W_MOVEFILE_REPLACE_EXISTING) ? FALSE : TRUE))
            return FALSE;
        return DeleteFileW(from);
    }
    if (!NT_SUCCESS(s)) {
        kw_status(s);
        return FALSE;
    }
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI MoveFileW(const WCHAR *from, const WCHAR *to)
{
    return MoveFileExW(from, to, 0);
}

BOOL WINAPI MoveFileExA(const char *from, const char *to, DWORD flags)
{
    WCHAR *a = kw_widen(from), *b = kw_widen(to);
    BOOL r = FALSE;
    if (a && b)
        r = MoveFileExW(a, b, flags);
    kw_free(a);
    kw_free(b);
    return r;
}

BOOL WINAPI MoveFileA(const char *from, const char *to)
{
    return MoveFileExA(from, to, 0);
}

BOOL WINAPI CopyFileW(const WCHAR *from, const WCHAR *to, BOOL fail_if_exists)
{
    HANDLE in, out;
    KW_FILE_BASIC bi;
    IO_STATUS_BLOCK iosb;
    BYTE *buf;
    BOOL ok = TRUE;
    if (!from || !to) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    in = CreateFileW(from, KW_GENERIC_READ, 1u /* FILE_SHARE_READ */, NULL, KW_OPEN_EXISTING, 0, NULL);
    if (in == KW_INVALID_HANDLE)
        return FALSE;
    memset(&bi, 0, sizeof(bi));
    NtQueryInformationFile(in, &iosb, &bi, sizeof(bi), KW_CLASS_BASIC);
    out = CreateFileW(to, KW_GENERIC_WRITE, 0, NULL, fail_if_exists ? KW_CREATE_NEW : KW_CREATE_ALWAYS, 0, NULL);
    if (out == KW_INVALID_HANDLE) {
        DWORD e = GetLastError();
        NtClose(in);
        SetLastError(e);
        return FALSE;
    }
    buf = (BYTE *)kw_alloc(65536);
    if (!buf) {
        NtClose(in);
        NtClose(out);
        SetLastError(KW_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    for (;;) {
        DWORD got = 0, wrote = 0;
        if (!ReadFile(in, buf, 65536, &got, NULL)) {
            ok = FALSE;
            break;
        }
        if (got == 0)
            break;
        if (!WriteFile(out, buf, got, &wrote, NULL) || wrote != got) {
            ok = FALSE;
            break;
        }
    }
    kw_free(buf);
    if (ok) {
        /* carry the times and attributes over */
        KW_FILE_BASIC set = bi;
        set.ChangeTime.QuadPart = 0;
        if (set.FileAttributes & KW_FILE_ATTRIBUTE_DIRECTORY)
            set.FileAttributes = KW_FILE_ATTRIBUTE_NORMAL;
        NtSetInformationFile(out, &iosb, &set, sizeof(set), KW_CLASS_BASIC);
    }
    {
        DWORD e = GetLastError();
        NtClose(in);
        NtClose(out);
        if (!ok) {
            DeleteFileW(to);
            SetLastError(e);
        } else {
            SetLastError(0);
        }
    }
    return ok;
}

BOOL WINAPI CopyFileA(const char *from, const char *to, BOOL fail_if_exists)
{
    WCHAR *a = kw_widen(from), *b = kw_widen(to);
    BOOL r = FALSE;
    if (a && b)
        r = CopyFileW(a, b, fail_if_exists);
    kw_free(a);
    kw_free(b);
    return r;
}

BOOL WINAPI CopyFileExW(const WCHAR *from, const WCHAR *to, void *progress, void *data, BOOL *cancel, DWORD flags)
{
    (void)progress;
    (void)data;
    (void)cancel;
    return CopyFileW(from, to, (flags & 1u /* COPY_FILE_FAIL_IF_EXISTS */) ? TRUE : FALSE);
}

/* ---- Drives and volumes ------------------------------------------------------------------------------- */

/* \??\X: exists as a symbolic link when drive X does. */
static int kw_drive_exists(WCHAR letter)
{
    WCHAR name[8] = {'\\', '?', '?', '\\', 0, ':', 0, 0};
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    HANDLE h = NULL;
    NTSTATUS s;
    name[4] = letter;
    us.Buffer = name;
    us.Length = 6 * sizeof(WCHAR);
    us.MaximumLength = 7 * sizeof(WCHAR);
    oa.Length = sizeof(oa);
    oa.RootDirectory = NULL;
    oa.ObjectName = &us;
    oa.Attributes = 0x40;
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;
    s = NtOpenSymbolicLinkObject(&h, 0x1 /* SYMBOLIC_LINK_QUERY */, &oa);
    if (NT_SUCCESS(s)) {
        NtClose(h);
        return 1;
    }
    return 0;
}

DWORD WINAPI GetLogicalDrives(void)
{
    DWORD mask = 0;
    for (WCHAR c = 'A'; c <= 'Z'; ++c)
        if (kw_drive_exists(c))
            mask |= k32w_drive_bit(c);
    return mask;
}

DWORD WINAPI GetLogicalDriveStringsW(DWORD cap, WCHAR *buf)
{
    DWORD mask = GetLogicalDrives(), count = 0, need;
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i))
            ++count;
    need = count * 4 + 1;
    if (cap < need)
        return need;
    {
        size_t o = 0;
        for (int i = 0; i < 26; ++i) {
            if (mask & (1u << i)) {
                buf[o++] = (WCHAR)('A' + i);
                buf[o++] = ':';
                buf[o++] = '\\';
                buf[o++] = 0;
            }
        }
        buf[o] = 0;
    }
    return count * 4;
}

DWORD WINAPI GetLogicalDriveStringsA(DWORD cap, char *buf)
{
    DWORD mask = GetLogicalDrives(), count = 0, need;
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i))
            ++count;
    need = count * 4 + 1;
    if (cap < need)
        return need;
    {
        size_t o = 0;
        for (int i = 0; i < 26; ++i) {
            if (mask & (1u << i)) {
                buf[o++] = (char)('A' + i);
                buf[o++] = ':';
                buf[o++] = '\\';
                buf[o++] = 0;
            }
        }
        buf[o] = 0;
    }
    return count * 4;
}

/* Opens the root of the volume the path is on, for the volume queries. */
static HANDLE kw_open_volume(const WCHAR *root)
{
    WCHAR drive[4];
    const WCHAR *use = root;
    if (!root) {
        use = L"\\";
    } else if (k32w_drive_root(root, kw_wlen(root), drive)) {
        use = drive;
    }
    return CreateFileW(use, KW_FILE_READ_ATTRIBUTES | KW_SYNCHRONIZE, KW_FILE_SHARE_ALL, NULL, KW_OPEN_EXISTING, KW_FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

/* FILE_FS_DEVICE_INFORMATION */
typedef struct {
    ULONG DeviceType;
    ULONG Characteristics;
} KW_FS_DEVICE;

/* GetDriveType values */
#define KW_DRIVE_UNKNOWN 0u
#define KW_DRIVE_NO_ROOT_DIR 1u
#define KW_DRIVE_REMOVABLE 2u
#define KW_DRIVE_FIXED 3u
#define KW_DRIVE_REMOTE 4u
#define KW_DRIVE_CDROM 5u
#define KW_DRIVE_RAMDISK 6u

UINT WINAPI GetDriveTypeW(const WCHAR *root)
{
    HANDLE h;
    KW_FS_DEVICE dev;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    UINT t;
    if (root && !k32w_drive_root(root, kw_wlen(root), (WCHAR[4]){0}) && !(root[0] == '\\'))
        return KW_DRIVE_NO_ROOT_DIR;
    if (root) {
        WCHAR d[4];
        if (k32w_drive_root(root, kw_wlen(root), d) && !kw_drive_exists(d[0]))
            return KW_DRIVE_NO_ROOT_DIR;
    }
    h = kw_open_volume(root);
    if (h == KW_INVALID_HANDLE)
        return KW_DRIVE_NO_ROOT_DIR;
    s = NtQueryVolumeInformationFile(h, &iosb, &dev, sizeof(dev), KW_CLASS_FS_DEVICE);
    NtClose(h);
    if (!NT_SUCCESS(s))
        return KW_DRIVE_UNKNOWN;
    switch (dev.DeviceType) {
    case 2: t = KW_DRIVE_CDROM; break;           /* FILE_DEVICE_CD_ROM */
    case 0x14: t = KW_DRIVE_REMOTE; break;       /* FILE_DEVICE_NETWORK_FILE_SYSTEM */
    case 7:                                      /* FILE_DEVICE_DISK */
    default:
        t = (dev.Characteristics & 1u /* FILE_REMOVABLE_MEDIA */) ? KW_DRIVE_REMOVABLE : KW_DRIVE_FIXED;
        break;
    }
    if (dev.Characteristics & 0x10u /* FILE_REMOTE_DEVICE */)
        t = KW_DRIVE_REMOTE;
    return t;
}

UINT WINAPI GetDriveTypeA(const char *root)
{
    WCHAR *w;
    UINT r;
    if (!root)
        return GetDriveTypeW(NULL);
    w = kw_widen(root);
    if (!w)
        return KW_DRIVE_UNKNOWN;
    r = GetDriveTypeW(w);
    kw_free(w);
    return r;
}

/* FILE_FS_SIZE_INFORMATION and FILE_FS_FULL_SIZE_INFORMATION */
typedef struct {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER AvailableAllocationUnits;
    ULONG SectorsPerAllocationUnit;
    ULONG BytesPerSector;
} KW_FS_SIZE;

typedef struct {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER CallerAvailableAllocationUnits;
    LARGE_INTEGER ActualAvailableAllocationUnits;
    ULONG SectorsPerAllocationUnit;
    ULONG BytesPerSector;
} KW_FS_FULLSIZE;

BOOL WINAPI GetDiskFreeSpaceExW(const WCHAR *dir, ULONGLONG *caller_free, ULONGLONG *total, ULONGLONG *total_free)
{
    HANDLE h = kw_open_volume(dir);
    KW_FS_FULLSIZE fs;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    ULONGLONG unit;
    if (h == KW_INVALID_HANDLE)
        return FALSE;
    s = NtQueryVolumeInformationFile(h, &iosb, &fs, sizeof(fs), KW_CLASS_FS_FULLSIZE);
    NtClose(h);
    if (!NT_SUCCESS(s)) {
        kw_status(s);
        return FALSE;
    }
    unit = (ULONGLONG)fs.SectorsPerAllocationUnit * fs.BytesPerSector;
    if (caller_free)
        *caller_free = (ULONGLONG)fs.CallerAvailableAllocationUnits.QuadPart * unit;
    if (total)
        *total = (ULONGLONG)fs.TotalAllocationUnits.QuadPart * unit;
    if (total_free)
        *total_free = (ULONGLONG)fs.ActualAvailableAllocationUnits.QuadPart * unit;
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExA(const char *dir, ULONGLONG *caller_free, ULONGLONG *total, ULONGLONG *total_free)
{
    WCHAR *w = dir ? kw_widen(dir) : NULL;
    BOOL r;
    if (dir && !w)
        return FALSE;
    r = GetDiskFreeSpaceExW(w, caller_free, total, total_free);
    kw_free(w);
    return r;
}

BOOL WINAPI GetDiskFreeSpaceW(const WCHAR *root, DWORD *sectors_per_cluster, DWORD *bytes_per_sector, DWORD *free_clusters, DWORD *total_clusters)
{
    HANDLE h = kw_open_volume(root);
    KW_FS_SIZE fs;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    if (h == KW_INVALID_HANDLE)
        return FALSE;
    s = NtQueryVolumeInformationFile(h, &iosb, &fs, sizeof(fs), KW_CLASS_FS_SIZE);
    NtClose(h);
    if (!NT_SUCCESS(s)) {
        kw_status(s);
        return FALSE;
    }
    if (sectors_per_cluster)
        *sectors_per_cluster = fs.SectorsPerAllocationUnit;
    if (bytes_per_sector)
        *bytes_per_sector = fs.BytesPerSector;
    if (free_clusters)
        *free_clusters = (DWORD)fs.AvailableAllocationUnits.QuadPart;
    if (total_clusters)
        *total_clusters = (DWORD)fs.TotalAllocationUnits.QuadPart;
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceA(const char *root, DWORD *spc, DWORD *bps, DWORD *fc, DWORD *tc)
{
    WCHAR *w = root ? kw_widen(root) : NULL;
    BOOL r;
    if (root && !w)
        return FALSE;
    r = GetDiskFreeSpaceW(w, spc, bps, fc, tc);
    kw_free(w);
    return r;
}

/* FILE_FS_VOLUME_INFORMATION and FILE_FS_ATTRIBUTE_INFORMATION */
typedef struct {
    LARGE_INTEGER VolumeCreationTime;
    ULONG VolumeSerialNumber;
    ULONG VolumeLabelLength;
    BOOLEAN SupportsObjects;
    WCHAR VolumeLabel[1];
} KW_FS_VOLUME;

typedef struct {
    ULONG FileSystemAttributes;
    LONG MaximumComponentNameLength;
    ULONG FileSystemNameLength;
    WCHAR FileSystemName[1];
} KW_FS_ATTRIBUTE;

BOOL WINAPI GetVolumeInformationW(const WCHAR *root, WCHAR *label, DWORD label_cap, DWORD *serial, DWORD *max_component, DWORD *flags, WCHAR *fsname, DWORD fsname_cap)
{
    HANDLE h = kw_open_volume(root);
    BYTE buf[sizeof(KW_FS_VOLUME) + 256 * sizeof(WCHAR)];
    IO_STATUS_BLOCK iosb;
    NTSTATUS s;
    if (h == KW_INVALID_HANDLE)
        return FALSE;
    if (label || serial) {
        KW_FS_VOLUME *v = (KW_FS_VOLUME *)buf;
        s = NtQueryVolumeInformationFile(h, &iosb, buf, sizeof(buf), KW_CLASS_FS_VOLUME);
        if (!NT_SUCCESS(s)) {
            NtClose(h);
            kw_status(s);
            return FALSE;
        }
        if (serial)
            *serial = v->VolumeSerialNumber;
        if (label && label_cap) {
            size_t n = v->VolumeLabelLength / sizeof(WCHAR);
            if (n + 1 > label_cap) {
                NtClose(h);
                SetLastError(KW_ERROR_MORE_DATA);
                return FALSE;
            }
            for (size_t i = 0; i < n; ++i)
                label[i] = v->VolumeLabel[i];
            label[n] = 0;
        }
    }
    if (max_component || flags || fsname) {
        KW_FS_ATTRIBUTE *a = (KW_FS_ATTRIBUTE *)buf;
        s = NtQueryVolumeInformationFile(h, &iosb, buf, sizeof(buf), KW_CLASS_FS_ATTRIBUTE);
        if (!NT_SUCCESS(s)) {
            NtClose(h);
            kw_status(s);
            return FALSE;
        }
        if (max_component)
            *max_component = (DWORD)a->MaximumComponentNameLength;
        if (flags)
            *flags = a->FileSystemAttributes;
        if (fsname && fsname_cap) {
            size_t n = a->FileSystemNameLength / sizeof(WCHAR);
            if (n + 1 > fsname_cap) {
                NtClose(h);
                SetLastError(KW_ERROR_MORE_DATA);
                return FALSE;
            }
            for (size_t i = 0; i < n; ++i)
                fsname[i] = a->FileSystemName[i];
            fsname[n] = 0;
        }
    }
    NtClose(h);
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI GetVolumePathNameW(const WCHAR *path, WCHAR *out, DWORD cap)
{
    size_t n;
    WCHAR *full;
    WCHAR root[4];
    if (!path || !out) {
        SetLastError(KW_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    full = kw_full_path(path, &n);
    if (!full)
        return FALSE;
    if (k32w_drive_root(full, n, root)) {
        if (cap < 4) {
            kw_free(full);
            SetLastError(KW_ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memcpy(out, root, 4 * sizeof(WCHAR));
    } else {
        /* a UNC path: \\server\share\ */
        size_t seps = 0, end = n;
        for (size_t i = 2; i < n; ++i) {
            if (full[i] == '\\' && ++seps == 2) {
                end = i + 1;
                break;
            }
        }
        if (end + 1 > cap) {
            kw_free(full);
            SetLastError(KW_ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memcpy(out, full, end * sizeof(WCHAR));
        if (end == n) {
            out[end++] = '\\';
        }
        out[end] = 0;
    }
    kw_free(full);
    return TRUE;
}

#endif /* __MINGW32__ */
