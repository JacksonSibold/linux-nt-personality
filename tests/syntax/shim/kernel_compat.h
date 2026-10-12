/*
 * kernel_compat.h - force-included (-include) by tests/syntax/check.sh.
 *
 * Supplies the few Linux/POSIX names that the userspace (USERSPACE_KSHIM)
 * branch of the kernel-facing sources uses but that MinGW does not provide.
 * Used for syntax checks only; nothing here is linked or run.
 */
#ifndef SYNTAX_SHIM_KERNEL_COMPAT_H
#define SYNTAX_SHIM_KERNEL_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <direct.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0200000
#endif
#ifndef O_SYNC
#define O_SYNC 04010000
#endif

/* Linux kernel loff_t, used in kshim.h prototypes. */
typedef long long loff_t;

/* POSIX positional I/O (MinGW lacks them). */
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);

/* POSIX mkdir(path, mode) on MinGW is _mkdir(path). */
#define mkdir(path, mode) _mkdir(path)

/* POSIX fsync(fd); MinGW has _commit(fd) in <io.h>. Syntax-check stub. */
static inline int fsync(int fd) { (void)fd; return 0; }

#endif
