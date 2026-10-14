/*
 * kernel32_fs.c - file search, file information and temp files (K32-F1):
 * FindFirstFileA, FindNextFileA, FindClose, GetFileTime,
 * GetFileInformationByHandle, GetTempPathA and GetTempFileNameA.
 *
 * FindFirstFile opens the directory with CreateFileA and reads it with
 * NtQueryDirectoryFile (FileBothDirectoryInformation). The wildcard is applied
 * here, by k32f_wild_match, so the kernel needs no pattern support. The search
 * context is a page-aligned block from NtAllocateVirtualMemory, freed by
 * FindClose. Names are ASCII; a wide character above 0x7F becomes '?' in the
 * ANSI result.
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
#include "k32_find_pure.h"
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif

#define K32FS_INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define K32FS_ERROR_FILE_NOT_FOUND     2u
#define K32FS_ERROR_NOT_ENOUGH_MEMORY  8u
#define K32FS_ERROR_INVALID_PARAMETER  87u
#define K32FS_ERROR_NO_MORE_FILES      18u
#define K32FS_ERROR_FILENAME_EXCED     206u
#define K32FS_FILE_LIST_DIRECTORY      0x00000001u
#define K32FS_SYNCHRONIZE              0x00100000u
#define K32FS_FILE_SHARE_ALL           0x00000007u
#define K32FS_OPEN_EXISTING            3u
#define K32FS_CREATE_NEW               1u
#define K32FS_FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define K32FS_FILE_ATTRIBUTE_NORMAL    0x00000080u
#define K32FS_GENERIC_WRITE            0x40000000u
#define K32FS_MEM_COMMIT_RESERVE       0x00003000u
#define K32FS_MEM_RELEASE              0x00008000u
#define K32FS_PAGE_READWRITE           0x04u
#define K32FS_STATUS_NO_MORE_FILES     ((NTSTATUS)0x80000006L)
/* Information classes. */
#define K32FS_CLASS_VOLUME                 1u
#define K32FS_CLASS_BOTH_DIR               3u
#define K32FS_CLASS_BASIC                  4u
#define K32FS_CLASS_STANDARD               5u
#define K32FS_CLASS_INTERNAL               6u
#define K32FS_DIR_BUFFER                   8192u
#define K32FS_ENV_MAX_CHARS                32768u

typedef PVOID LPVOID;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef UINT
#define UINT unsigned int
#endif

__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);
__declspec(dllimport) extern NTSTATUS NtQueryDirectoryFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG, BOOLEAN, PUNICODE_STRING, BOOLEAN);
__declspec(dllimport) extern NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtAllocateVirtualMemory(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
__declspec(dllimport) extern NTSTATUS NtFreeVirtualMemory(HANDLE, PVOID *, PSIZE_T, ULONG);
__declspec(dllimport) extern NTSTATUS NtClose(HANDLE);
__declspec(dllimport) extern NTSTATUS NtQueryPerformanceCounter(PLARGE_INTEGER, PLARGE_INTEGER);

/* Defined in kernel32.c. */
void SetLastError(DWORD e);
HANDLE WINAPI CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE templ);
BOOL WINAPI CloseHandle(HANDLE h);
DWORD WINAPI GetTickCount(void);

/* FILETIME, WIN32_FIND_DATAA and BY_HANDLE_FILE_INFORMATION (Win32 x64 layout). */
typedef struct { DWORD dwLowDateTime; DWORD dwHighDateTime; } K32FS_FILETIME;

typedef struct {
    DWORD dwFileAttributes;
    K32FS_FILETIME ftCreationTime;
    K32FS_FILETIME ftLastAccessTime;
    K32FS_FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} K32FS_WIN32_FIND_DATAA;

typedef struct {
    DWORD dwFileAttributes;
    K32FS_FILETIME ftCreationTime;
    K32FS_FILETIME ftLastAccessTime;
    K32FS_FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
} K32FS_BY_HANDLE_FILE_INFORMATION;

/* FILE_BOTH_DIR_INFORMATION (class 3). The name is FileName[FileNameLength / 2]. */
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
} K32FS_DIR_ENTRY;

/* FILE_BASIC_INFORMATION (class 4), FILE_STANDARD_INFORMATION (class 5),
 * FILE_INTERNAL_INFORMATION (class 6). */
typedef struct {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG FileAttributes;
} K32FS_FBASIC;

typedef struct {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} K32FS_FSTANDARD;

typedef struct {
    ULONGLONG IndexNumber;
} K32FS_FINTERNAL;

/* The search context. dir is the directory handle, buf holds the last
 * NtQueryDirectoryFile result, pos is the next entry, valid the bytes in buf. */
typedef struct {
    HANDLE dir;
    char mask[K32F_MAX_NAME + 1];
    ULONG pos;
    ULONG valid;
    BOOLEAN restart;
    BOOLEAN eof;
    NTSTATUS last;
    BYTE buf[K32FS_DIR_BUFFER];
} K32FS_FIND;

static void k32fs_status(NTSTATUS s)
{
    SetLastError(NT_SUCCESS(s) ? 0 : RtlNtStatusToDosError(s));
}

static void k32fs_filetime(const LARGE_INTEGER *t, K32FS_FILETIME *ft)
{
    ft->dwLowDateTime = (DWORD)(t->QuadPart & 0xFFFFFFFFu);
    ft->dwHighDateTime = (DWORD)((ULONGLONG)t->QuadPart >> 32);
}

/* A wide name as ANSI: ASCII as-is, anything else '?'. */
static void k32fs_narrow(const WCHAR *in, size_t n, char *out, size_t cap)
{
    size_t i = 0;
    for (; i < n && i + 1 < cap; ++i)
        out[i] = (in[i] <= 0x7Fu) ? (char)in[i] : '?';
    out[i] = '\0';
}

/* Next entry that matches the mask. Returns 1 with the entry in out, 0 at the
 * end, -1 on an NT error (last holds the status). */
static int k32fs_next(K32FS_FIND *f, K32FS_WIN32_FIND_DATAA *out)
{
    for (;;) {
        if (f->pos >= f->valid) {
            if (f->eof)
                return 0;
            IO_STATUS_BLOCK iosb;
            NTSTATUS s = NtQueryDirectoryFile(f->dir, NULL, NULL, NULL, &iosb, f->buf,
                                              sizeof(f->buf), K32FS_CLASS_BOTH_DIR,
                                              FALSE, NULL, f->restart);
            f->restart = FALSE;
            if (s == K32FS_STATUS_NO_MORE_FILES) {
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
        K32FS_DIR_ENTRY *e = (K32FS_DIR_ENTRY *)(f->buf + f->pos);
        ULONG next = e->NextEntryOffset;
        f->pos = next ? f->pos + next : f->valid;
        size_t n = e->FileNameLength / sizeof(WCHAR);
        if (!k32f_wild_match(f->mask, e->FileName, n) || !k32f_name_fits(n))
            continue;
        memset(out, 0, sizeof(*out));
        out->dwFileAttributes = e->FileAttributes;
        k32fs_filetime(&e->CreationTime, &out->ftCreationTime);
        k32fs_filetime(&e->LastAccessTime, &out->ftLastAccessTime);
        k32fs_filetime(&e->LastWriteTime, &out->ftLastWriteTime);
        out->nFileSizeHigh = (DWORD)((ULONGLONG)e->EndOfFile.QuadPart >> 32);
        out->nFileSizeLow = (DWORD)(e->EndOfFile.QuadPart & 0xFFFFFFFFu);
        k32fs_narrow(e->FileName, n, out->cFileName, sizeof(out->cFileName));
        k32fs_narrow(e->ShortName, e->ShortNameLength / sizeof(WCHAR),
                     out->cAlternateFileName, sizeof(out->cAlternateFileName));
        return 1;
    }
}

static K32FS_FIND *k32fs_alloc(void)
{
    PVOID base = NULL;
    SIZE_T size = sizeof(K32FS_FIND);
    NTSTATUS s = NtAllocateVirtualMemory((HANDLE)(intptr_t)-1, &base, 0, &size,
                                         K32FS_MEM_COMMIT_RESERVE, K32FS_PAGE_READWRITE);
    if (!NT_SUCCESS(s)) {
        SetLastError(K32FS_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return (K32FS_FIND *)base;
}

static void k32fs_free(K32FS_FIND *f)
{
    PVOID base = f;
    SIZE_T size = 0;
    NtFreeVirtualMemory((HANDLE)(intptr_t)-1, &base, &size, K32FS_MEM_RELEASE);
}

HANDLE WINAPI FindFirstFileA(const char *name, K32FS_WIN32_FIND_DATAA *data)
{
    if (!name || !data) {
        SetLastError(K32FS_ERROR_INVALID_PARAMETER);
        return K32FS_INVALID_HANDLE_VALUE;
    }
    size_t dl = k32f_dir_len(name);
    if (!k32f_name_fits(dl)) {
        SetLastError(K32FS_ERROR_FILENAME_EXCED);
        return K32FS_INVALID_HANDLE_VALUE;
    }
    /* The directory: the text before the last separator, "." when there is
     * none, and "X:\" for a drive root. */
    char dir[K32F_MAX_NAME + 4];
    if (dl == 0) {
        dir[0] = '.';
        dir[1] = '\0';
    } else if (dl == 2 && name[1] == ':') {
        dir[0] = name[0];
        dir[1] = ':';
        dir[2] = '\\';
        dir[3] = '\0';
    } else {
        for (size_t i = 0; i < dl; ++i)
            dir[i] = name[i];
        dir[dl] = '\0';
    }
    const char *mask = (dl == 0) ? name : name + dl + 1;
    size_t mlen = 0;
    while (mask[mlen] != '\0')
        ++mlen;
    if (!k32f_name_fits(mlen)) {
        SetLastError(K32FS_ERROR_FILENAME_EXCED);
        return K32FS_INVALID_HANDLE_VALUE;
    }
    HANDLE d = CreateFileA(dir, K32FS_FILE_LIST_DIRECTORY | K32FS_SYNCHRONIZE, K32FS_FILE_SHARE_ALL,
                           NULL, K32FS_OPEN_EXISTING, K32FS_FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (d == K32FS_INVALID_HANDLE_VALUE)
        return K32FS_INVALID_HANDLE_VALUE;
    K32FS_FIND *f = k32fs_alloc();
    if (!f) {
        NtClose(d);
        return K32FS_INVALID_HANDLE_VALUE;
    }
    f->dir = d;
    f->pos = 0;
    f->valid = 0;
    f->restart = TRUE;
    f->eof = FALSE;
    f->last = STATUS_SUCCESS;
    for (size_t i = 0; i <= mlen; ++i)
        f->mask[i] = mask[i];
    int r = k32fs_next(f, data);
    if (r == 1) {
        SetLastError(0);
        return (HANDLE)f;
    }
    if (r == 0)
        SetLastError(K32FS_ERROR_FILE_NOT_FOUND);
    else
        k32fs_status(f->last);
    NtClose(d);
    k32fs_free(f);
    return K32FS_INVALID_HANDLE_VALUE;
}

BOOL WINAPI FindNextFileA(HANDLE h, K32FS_WIN32_FIND_DATAA *data)
{
    K32FS_FIND *f = (K32FS_FIND *)h;
    if (!f || h == K32FS_INVALID_HANDLE_VALUE || !data) {
        SetLastError(K32FS_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int r = k32fs_next(f, data);
    if (r == 1) {
        SetLastError(0);
        return 1;
    }
    if (r == 0)
        SetLastError(K32FS_ERROR_NO_MORE_FILES);
    else
        k32fs_status(f->last);
    return 0;
}

BOOL WINAPI FindClose(HANDLE h)
{
    K32FS_FIND *f = (K32FS_FIND *)h;
    if (!f || h == K32FS_INVALID_HANDLE_VALUE) {
        SetLastError(K32FS_ERROR_INVALID_PARAMETER);
        return 0;
    }
    NtClose(f->dir);
    k32fs_free(f);
    SetLastError(0);
    return 1;
}

BOOL WINAPI GetFileTime(HANDLE h, K32FS_FILETIME *created, K32FS_FILETIME *accessed, K32FS_FILETIME *written)
{
    K32FS_FBASIC bi;
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = NtQueryInformationFile(h, &iosb, &bi, sizeof(bi), K32FS_CLASS_BASIC);
    if (!NT_SUCCESS(s)) {
        k32fs_status(s);
        return 0;
    }
    if (created) k32fs_filetime(&bi.CreationTime, created);
    if (accessed) k32fs_filetime(&bi.LastAccessTime, accessed);
    if (written) k32fs_filetime(&bi.LastWriteTime, written);
    SetLastError(0);
    return 1;
}

BOOL WINAPI GetFileInformationByHandle(HANDLE h, K32FS_BY_HANDLE_FILE_INFORMATION *info)
{
    if (!info) {
        SetLastError(K32FS_ERROR_INVALID_PARAMETER);
        return 0;
    }
    IO_STATUS_BLOCK iosb;
    K32FS_FBASIC bi;
    K32FS_FSTANDARD si;
    K32FS_FINTERNAL ii;
    NTSTATUS s = NtQueryInformationFile(h, &iosb, &bi, sizeof(bi), K32FS_CLASS_BASIC);
    if (NT_SUCCESS(s))
        s = NtQueryInformationFile(h, &iosb, &si, sizeof(si), K32FS_CLASS_STANDARD);
    if (NT_SUCCESS(s))
        s = NtQueryInformationFile(h, &iosb, &ii, sizeof(ii), K32FS_CLASS_INTERNAL);
    if (!NT_SUCCESS(s)) {
        k32fs_status(s);
        return 0;
    }
    /* FILE_FS_VOLUME_INFORMATION: VolumeCreationTime (8 bytes), then the serial. */
    struct { LARGE_INTEGER create; ULONG serial; ULONG label_len; BOOLEAN objects; WCHAR label[1]; } vol;
    NTSTATUS vs = NtQueryVolumeInformationFile(h, &iosb, &vol, sizeof(vol), K32FS_CLASS_VOLUME);
    if (!NT_SUCCESS(vs) && vs != STATUS_BUFFER_OVERFLOW) {
        k32fs_status(vs);
        return 0;
    }
    memset(info, 0, sizeof(*info));
    info->dwFileAttributes = bi.FileAttributes;
    k32fs_filetime(&bi.CreationTime, &info->ftCreationTime);
    k32fs_filetime(&bi.LastAccessTime, &info->ftLastAccessTime);
    k32fs_filetime(&bi.LastWriteTime, &info->ftLastWriteTime);
    info->dwVolumeSerialNumber = vol.serial;
    info->nFileSizeHigh = (DWORD)((ULONGLONG)si.EndOfFile.QuadPart >> 32);
    info->nFileSizeLow = (DWORD)(si.EndOfFile.QuadPart & 0xFFFFFFFFu);
    info->nNumberOfLinks = si.NumberOfLinks;
    info->nFileIndexHigh = (DWORD)(ii.IndexNumber >> 32);
    info->nFileIndexLow = (DWORD)(ii.IndexNumber & 0xFFFFFFFFu);
    SetLastError(0);
    return 1;
}

/* The value of a variable in the process environment (ANSI). Returns 1 with
 * the value in out, or 0 when the variable is not set. */
static int k32fs_env_value(const char *name, char *out, size_t cap)
{
    PTEB teb = NULL;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb) :: "memory");
    PEB *peb = teb ? teb->ProcessEnvironmentBlock : NULL;
    RTL_USER_PROCESS_PARAMETERS *pp = peb ? peb->ProcessParameters : NULL;
    const WCHAR *env = pp ? (const WCHAR *)pp->Environment : NULL;
    if (!env)
        return 0;
    size_t limit = pp->EnvironmentSize ? pp->EnvironmentSize / sizeof(WCHAR) : K32FS_ENV_MAX_CHARS;
    size_t pos = 0;
    while (pos < limit && env[pos] != 0) {
        size_t len = 0;
        while (pos + len < limit && env[pos + len] != 0)
            ++len;
        size_t off = 0;
        if (k32f_env_name_match(env + pos, len, name, &off)) {
            k32fs_narrow(env + pos + off, len - off, out, cap);
            return 1;
        }
        pos += len + 1;
    }
    return 0;
}

DWORD WINAPI GetTempPathA(DWORD len, char *buf)
{
    char value[K32F_MAX_NAME + 1];
    char path[K32F_MAX_NAME + 4];
    int have = k32fs_env_value("TMP", value, sizeof(value)) || k32fs_env_value("TEMP", value, sizeof(value));
    size_t n = k32f_temp_path(have ? value : NULL, path, sizeof(path));
    if (n == 0) {
        SetLastError(K32FS_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!buf || len < n + 1) {
        SetLastError(0);
        return (DWORD)(n + 1);
    }
    for (size_t i = 0; i <= n; ++i)
        buf[i] = path[i];
    SetLastError(0);
    return (DWORD)n;
}

UINT WINAPI GetTempFileNameA(const char *dir, const char *prefix, UINT uniq, char *out)
{
    if (!dir || !prefix || !out) {
        SetLastError(K32FS_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (uniq != 0) {
        /* A non-zero uUnique is used as given and no file is created. */
        char name[K32F_MAX_NAME + 4];
        size_t n = k32f_temp_file_name(dir, prefix, uniq, name, sizeof(name));
        if (n == 0) {
            SetLastError(K32FS_ERROR_FILENAME_EXCED);
            return 0;
        }
        for (size_t i = 0; i <= n; ++i)
            out[i] = name[i];
        SetLastError(0);
        return uniq;
    }
    /* uUnique 0: pick a number from the counter and create the file, moving
     * on to the next number while the name is taken. */
    LARGE_INTEGER c, f;
    unsigned int unique = 1;
    if (NT_SUCCESS(NtQueryPerformanceCounter(&c, &f)))
        unique = (unsigned int)(c.QuadPart & 0xFFFF);
    if (unique == 0)
        unique = 1;
    for (unsigned int tries = 0; tries < 0xFFFF; ++tries) {
        char name[K32F_MAX_NAME + 4];
        size_t n = k32f_temp_file_name(dir, prefix, unique, name, sizeof(name));
        if (n == 0) {
            SetLastError(K32FS_ERROR_FILENAME_EXCED);
            return 0;
        }
        HANDLE h = CreateFileA(name, K32FS_GENERIC_WRITE, 0, NULL, K32FS_CREATE_NEW,
                               K32FS_FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != K32FS_INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            for (size_t i = 0; i <= n; ++i)
                out[i] = name[i];
            SetLastError(0);
            return unique;
        }
        unique = (unique + 1) & 0xFFFF;
        if (unique == 0)
            unique = 1;
    }
    SetLastError(80); /* ERROR_FILE_EXISTS */
    return 0;
}

#endif /* __MINGW32__ */
