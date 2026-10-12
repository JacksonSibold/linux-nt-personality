/* posix_host.c - POSIX path, file and stat entry points for tests/kernel-host.
 *
 * Built with the shim include directory, so <sys/stat.h> resolves to
 * shim/sys/stat.h. Raw Win32/UCRT calls live in posix_raw.c. Paths beginning
 * with "/tmp" are mapped to the Windows temp directory (TEMP, then TMP): the
 * module tests use /tmp as their scratch root.
 *
 * system() is emulated only for the forms the tests use: "mkdir -p \"D\"",
 * "rm -rf \"D\"", and those joined with " && ". Anything else fails with -1.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#define KHOST_PATH_MAX 4096

/* Same values as khost_prelude.h; this unit is built without the prelude. */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0200000
#endif
#ifndef O_SYNC
#define O_SYNC      04010000
#endif

int khost_raw_mkdir(const char *path);

static void translate(const char *in, char *out, size_t outlen)
{
    if (strncmp(in, "/tmp", 4) == 0 && (in[4] == '\0' || in[4] == '/')) {
        const char *t = getenv("TEMP");
        if (!t) t = getenv("TMP");
        if (!t) t = ".";
        snprintf(out, outlen, "%s%s", t, in + 4);
        for (char *p = out; *p; p++)
            if (*p == '\\')
                *p = '/';
        return;
    }
    snprintf(out, outlen, "%s", in);
}

/* Win32 normalises trailing dots and spaces in ordinary paths, which POSIX
 * does not. The \?\ prefix turns that normalisation off. Only absolute
 * drive paths get the prefix; anything else is passed through unchanged. */
static void to_ext_wide(const char *in, wchar_t *out, size_t n)
{
    char tmp[KHOST_PATH_MAX + 8];
    char ext[KHOST_PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s", in);
    for (char *p = tmp; *p; p++)
        if (*p == '/')
            *p = '\\';
    if (((tmp[0] >= 'A' && tmp[0] <= 'Z') || (tmp[0] >= 'a' && tmp[0] <= 'z'))
        && tmp[1] == ':' && tmp[2] == '\\')
        snprintf(ext, sizeof(ext), "\\\\?\\%s", tmp);
    else
        snprintf(ext, sizeof(ext), "%s", tmp);
    MultiByteToWideChar(CP_ACP, 0, ext, -1, out, (int)n);
}

static void filetime_to_timespec(const FILETIME *ft, struct timespec *ts)
{
    unsigned long long t = ((unsigned long long)ft->dwHighDateTime << 32)
                           | ft->dwLowDateTime;
    t -= 116444736000000000ULL;   /* 1601-01-01 to 1970-01-01, 100ns units */
    ts->tv_sec = (time_t)(t / 10000000ULL);
    ts->tv_nsec = (long)((t % 10000000ULL) * 100);
}

/* Fill a POSIX-shaped struct stat from an open handle. Identity fields come
 * from GetFileInformationByHandle so that equal files compare equal. */
static int stat_handle(HANDLE h, struct stat *st)
{
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) {
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_dev = (dev_t)fi.dwVolumeSerialNumber;
    st->st_ino = (ino_t)(((unsigned long long)fi.nFileIndexHigh << 32)
                         | fi.nFileIndexLow);
    st->st_mode = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                      ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_nlink = fi.nNumberOfLinks;
    st->st_size = (off_t)(((unsigned long long)fi.nFileSizeHigh << 32)
                          | fi.nFileSizeLow);
    filetime_to_timespec(&fi.ftLastAccessTime, &st->st_atim);
    filetime_to_timespec(&fi.ftLastWriteTime, &st->st_mtim);
    filetime_to_timespec(&fi.ftCreationTime, &st->st_ctim);
    return 0;
}

int stat(const char *path, struct stat *st)
{
    char buf[KHOST_PATH_MAX];
    wchar_t w[KHOST_PATH_MAX + 8];
    HANDLE h;
    int rc;
    translate(path, buf, sizeof(buf));
    to_ext_wide(buf, w, sizeof(w) / sizeof(w[0]));
    h = CreateFileW(w, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errno = ENOENT;
        return -1;
    }
    rc = stat_handle(h, st);
    CloseHandle(h);
    return rc;
}

int fstat(int fd, struct stat *st)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    return stat_handle(h, st);
}

int khost_mkdir(const char *path, mode_t mode)
{
    char buf[KHOST_PATH_MAX];
    (void)mode;
    translate(path, buf, sizeof(buf));
    return khost_raw_mkdir(buf);
}

/* mkdir -p: create each prefix, ignoring ones that already exist. */
static int mkdir_p(const char *path)
{
    char buf[KHOST_PATH_MAX];
    size_t n;
    translate(path, buf, sizeof(buf));
    n = strlen(buf);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i];
            buf[i] = '\0';
            khost_raw_mkdir(buf);
            buf[i] = c;
        }
    }
    if (khost_raw_mkdir(buf) != 0 && errno != EEXIST)
        return (_access(buf, 0) == 0) ? 0 : -1;
    return 0;
}

/* open() via CreateFile, not _open(): _open() cannot open directories and
 * does not grant FILE_SHARE_DELETE, so POSIX unlink-while-open (used by the
 * delete-on-close paths) would fail on Windows. Mode bits are ignored. */
int khost_open(const char *path, int flags, ...)
{
    char buf[KHOST_PATH_MAX];
    DWORD access, disp, attrs = FILE_ATTRIBUTE_NORMAL;
    HANDLE h;
    int fd;
    wchar_t wbuf[KHOST_PATH_MAX + 8];
    va_list ap;

    /* O_DIRECTORY (0x10000) collides with _O_WTEXT; it is not passed down. */
    flags &= ~(O_DIRECTORY | O_SYNC);
    if (flags & O_CREAT) {
        va_start(ap, flags);
        (void)va_arg(ap, int);
        va_end(ap);
    }
    translate(path, buf, sizeof(buf));
    to_ext_wide(buf, wbuf, sizeof(wbuf) / sizeof(wbuf[0]));

    switch (flags & 3) {
    case _O_RDONLY: access = GENERIC_READ; break;
    case _O_WRONLY: access = GENERIC_WRITE; break;
    default:        access = GENERIC_READ | GENERIC_WRITE; break;
    }
    if (flags & _O_CREAT)
        disp = (flags & _O_EXCL) ? CREATE_NEW
             : (flags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else
        disp = (flags & _O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;

    {
        DWORD a = GetFileAttributesW(wbuf);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            attrs |= FILE_FLAG_BACKUP_SEMANTICS;
            access = GENERIC_READ;
        }
    }

    h = CreateFileW(wbuf, access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, disp, attrs, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: errno = EEXIST; break;
        default:                   errno = EACCES; break;
        }
        return -1;
    }
    fd = _open_osfhandle((intptr_t)h, _O_BINARY | (flags & _O_APPEND));
    if (fd < 0)
        CloseHandle(h);
    return fd;
}

int khost_unlink(const char *path)
{
    char buf[KHOST_PATH_MAX];
    translate(path, buf, sizeof(buf));
    return _unlink(buf);
}

int khost_rmdir(const char *path)
{
    char buf[KHOST_PATH_MAX];
    translate(path, buf, sizeof(buf));
    return _rmdir(buf);
}

int khost_rename(const char *from, const char *to)
{
    char a[KHOST_PATH_MAX], b[KHOST_PATH_MAX];
    translate(from, a, sizeof(a));
    translate(to, b, sizeof(b));
    return rename(a, b);
}

static int run_segment(char *seg)
{
    char *q1, *q2, path[KHOST_PATH_MAX], cmd[KHOST_PATH_MAX + 64];
    while (*seg == ' ')
        seg++;
    q1 = strchr(seg, '"');
    if (!q1)
        return -1;
    q2 = strchr(q1 + 1, '"');
    if (!q2)
        return -1;
    *q2 = '\0';
    snprintf(path, sizeof(path), "%s", q1 + 1);
    if (strncmp(seg, "mkdir -p", 8) == 0)
        return mkdir_p(path);
    if (strncmp(seg, "rm -rf", 6) == 0) {
        char win[KHOST_PATH_MAX];
        translate(path, win, sizeof(win));
        for (char *p = win; *p; p++)
            if (*p == '/')
                *p = '\\';
        snprintf(cmd, sizeof(cmd), "rd /s /q \"%s\" 2>nul", win);
        system(cmd);
        return 0;
    }
    return -1;
}

int khost_system(const char *command)
{
    char work[KHOST_PATH_MAX * 2];
    char *seg, *amp;
    if (!command)
        return 1;
    snprintf(work, sizeof(work), "%s", command);
    seg = work;
    for (;;) {
        amp = strstr(seg, "&&");
        if (amp)
            *amp = '\0';
        if (run_segment(seg) != 0) {
            fprintf(stderr, "khost_system: unsupported command: %s\n", command);
            return -1;
        }
        if (!amp)
            break;
        seg = amp + 2;
    }
    return 0;
}

/* Unbuffer stdout: some tests end with _exit() paths that would otherwise
 * drop buffered output when the log is a file. */
__attribute__((constructor)) static void khost_unbuffer_stdout(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
}
