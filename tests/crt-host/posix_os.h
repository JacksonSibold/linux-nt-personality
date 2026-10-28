/*
 * posix_os.h - a POSIX implementation of the runtime's OS interface (ucrt_os.h) for the host tests.
 * Handles are descriptors plus one, so no handle is NULL. Win32 error codes are produced from errno.
 */
#ifndef POSIX_OS_H
#define POSIX_OS_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ucrt_os.h"

static unsigned posix_win32_error(int e)
{
    switch (e) {
    case ENOENT: return 2;
    case ENOTDIR: return 3;
    case EACCES: case EPERM: case EISDIR: case EROFS: return 5;
    case EBADF: return 6;
    case EEXIST: return 80;
    case EINVAL: return 87;
    case ENOSPC: return 112;
    case ENOTEMPTY: return 145;
    case EPIPE: return 232;
    case EMFILE: return 4;
    default: return 87;
    }
}

static inline int posix_fd(void *h)
{
    return (int)(intptr_t)h - 1;
}

static void *posix_handle(int fd)
{
    return (void *)(intptr_t)(fd + 1);
}

static void posix_narrow(const ucrt_wchar *w, char *out, size_t cap)
{
    size_t i = 0;
    for (; w[i] && i + 1 < cap; ++i)
        out[i] = (char)w[i];
    out[i] = 0;
}

static unsigned posix_open(const ucrt_wchar *wpath, unsigned access, unsigned share, unsigned disp, unsigned attrs, void **h)
{
    char path[1024];
    int flags = 0, fd;
    (void)share;
    posix_narrow(wpath, path, sizeof path);
    if ((access & UCRT_GENERIC_READ) && (access & UCRT_GENERIC_WRITE))
        flags = O_RDWR;
    else if (access & UCRT_GENERIC_WRITE)
        flags = O_WRONLY;
    else
        flags = O_RDONLY;
    switch (disp) {
    case UCRT_CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
    case UCRT_CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case UCRT_OPEN_ALWAYS: flags |= O_CREAT; break;
    case UCRT_TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default: break;
    }
    fd = open(path, flags, (attrs & UCRT_ATTR_READONLY) ? 0444 : 0644);
    if (fd < 0)
        return posix_win32_error(errno);
    *h = posix_handle(fd);
    return 0;
}

static unsigned posix_read(void *h, void *buf, unsigned n, unsigned *got)
{
    ssize_t r = read(posix_fd(h), buf, n);
    if (r < 0)
        return posix_win32_error(errno);
    *got = (unsigned)r;
    return 0;
}

static unsigned posix_write(void *h, const void *buf, unsigned n, unsigned *wrote)
{
    ssize_t r = write(posix_fd(h), buf, n);
    if (r < 0)
        return posix_win32_error(errno);
    *wrote = (unsigned)r;
    return 0;
}

static unsigned posix_seek(void *h, int64_t off, int whence, int64_t *pos)
{
    off_t r = lseek(posix_fd(h), (off_t)off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0)
        return errno == EINVAL ? 131u : posix_win32_error(errno);
    *pos = r;
    return 0;
}

static unsigned posix_close(void *h)
{
    return close(posix_fd(h)) ? posix_win32_error(errno) : 0;
}

static unsigned posix_filetype(void *h)
{
    struct stat st;
    if (fstat(posix_fd(h), &st))
        return UCRT_FILE_TYPE_UNKNOWN;
    if (S_ISREG(st.st_mode))
        return UCRT_FILE_TYPE_DISK;
    if (S_ISCHR(st.st_mode))
        return UCRT_FILE_TYPE_CHAR;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
        return UCRT_FILE_TYPE_PIPE;
    return UCRT_FILE_TYPE_DISK;
}

static unsigned posix_set_eof(void *h, int64_t size)
{
    return ftruncate(posix_fd(h), (off_t)size) ? posix_win32_error(errno) : 0;
}

static unsigned posix_flush(void *h)
{
    return fsync(posix_fd(h)) && errno != EINVAL ? posix_win32_error(errno) : 0;
}

static unsigned posix_dup(void *h, void **out)
{
    int fd = dup(posix_fd(h));
    if (fd < 0)
        return posix_win32_error(errno);
    *out = posix_handle(fd);
    return 0;
}

static unsigned posix_unlink(const ucrt_wchar *w)
{
    char path[1024];
    posix_narrow(w, path, sizeof path);
    return unlink(path) ? posix_win32_error(errno) : 0;
}

static unsigned posix_rename(const ucrt_wchar *a, const ucrt_wchar *b)
{
    char pa[1024], pb[1024];
    posix_narrow(a, pa, sizeof pa);
    posix_narrow(b, pb, sizeof pb);
    return rename(pa, pb) ? posix_win32_error(errno) : 0;
}

static unsigned posix_get_std(unsigned n, void **h)
{
    *h = posix_handle((int)n);
    return 0;
}

static const ucrt_os posix_os = {
    posix_open, posix_read, posix_write, posix_seek, posix_close, posix_filetype, posix_set_eof, posix_flush,
    posix_dup, posix_unlink, posix_rename, posix_get_std, NULL, NULL,
};

#endif /* POSIX_OS_H */
