/* posix_raw.c - UCRT-side POSIX file primitives for tests/kernel-host.
 *
 * Built WITHOUT the shim include directory, so it sees the toolchain's own
 * <sys/stat.h>. posix_host.c (built with the shim directory) wraps these
 * into the POSIX-shaped API that src/kernel/nt expects.
 */
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

int khost_raw_mkdir(const char *path)
{
    return _mkdir(path);
}

ssize_t khost_pread(int fd, void *buf, size_t n, off_t off)
{
    long long cur = _lseeki64(fd, 0, SEEK_CUR);
    int r;
    if (cur < 0 || _lseeki64(fd, off, SEEK_SET) < 0)
        return -1;
    r = _read(fd, buf, (unsigned int)n);
    _lseeki64(fd, cur, SEEK_SET);
    return r;
}

ssize_t khost_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    long long cur = _lseeki64(fd, 0, SEEK_CUR);
    int r;
    if (cur < 0 || _lseeki64(fd, off, SEEK_SET) < 0)
        return -1;
    r = _write(fd, buf, (unsigned int)n);
    _lseeki64(fd, cur, SEEK_SET);
    return r;
}

int khost_fsync(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    return FlushFileBuffers(h) ? 0 : -1;
}
