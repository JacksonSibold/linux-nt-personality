/* khost_prelude.h - force-included (-include) before every host-built
 * src/kernel/nt translation unit. Supplies the Linux/POSIX names that
 * kshim.h and the module sources assume but the UCRT toolchain lacks.
 * Implementations are in posix_host.c and mman_win.c.
 */
#ifndef KHOST_PRELUDE_H
#define KHOST_PRELUDE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>   /* shim version: must precede the mkdir macro below */
#include <dirent.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0200000
#endif
#ifndef O_SYNC
#define O_SYNC      04010000
#endif

typedef int64_t loff_t;

ssize_t khost_pread(int fd, void *buf, size_t n, off_t off);
ssize_t khost_pwrite(int fd, const void *buf, size_t n, off_t off);
int khost_fsync(int fd);
int khost_open(const char *path, int flags, ...);
int khost_unlink(const char *path);
int khost_rmdir(const char *path);
int khost_rename(const char *from, const char *to);
int khost_system(const char *command);
DIR *khost_opendir(const char *path);

#define pread(fd, buf, n, off)  khost_pread((fd), (buf), (n), (off))
#define pwrite(fd, buf, n, off) khost_pwrite((fd), (buf), (n), (off))
#define fsync(fd)               khost_fsync(fd)
#define mkdir(path, mode)       khost_mkdir((path), (mode))
#define open                    khost_open
#define unlink                  khost_unlink
#define rmdir                   khost_rmdir
#define rename                  khost_rename
#define system                  khost_system
#define opendir                 khost_opendir

#endif
