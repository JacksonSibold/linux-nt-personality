/* sys/stat.h - host (UCRT) replacement for the POSIX stat API.
 *
 * Declares a Linux-layout struct stat (st_atim/st_mtim/st_ctim are
 * struct timespec) and the stat/fstat/mkdir entry points used by
 * src/kernel/nt. Implemented in ../posix_host.c on top of the UCRT
 * _stat64/_fstat64 calls. Replaces the toolchain's <sys/stat.h>, which
 * has no timespec members.
 */
#ifndef KHOST_SYS_STAT_H
#define KHOST_SYS_STAT_H

#include <sys/types.h>
#include <time.h>

struct stat {
    dev_t         st_dev;
    ino_t         st_ino;
    mode_t        st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    dev_t         st_rdev;
    off_t         st_size;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

#ifndef S_IFMT
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#endif
#ifndef S_IFLNK
#define S_IFLNK  0120000
#endif
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int khost_mkdir(const char *path, mode_t mode);

#endif
