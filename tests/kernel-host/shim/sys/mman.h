/* sys/mman.h - host (Windows gcc / UCRT) shim for the POSIX memory API.
 *
 * The UCRT toolchain has no <sys/mman.h>. This header declares the subset
 * of the POSIX API used by src/kernel/nt under USERSPACE_KSHIM; the
 * implementation lives in ../mman_win.c and is built with the tests.
 * Anonymous private mappings only; file-backed mappings are unsupported.
 */
#ifndef KHOST_SYS_MMAN_H
#define KHOST_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4
#define PROT_GROWSDOWN  0x01000000
#define PROT_GROWSUP    0x02000000

#define MAP_SHARED           0x01
#define MAP_PRIVATE          0x02
#define MAP_FIXED            0x10
#define MAP_ANONYMOUS        0x20
#define MAP_ANON             MAP_ANONYMOUS
#define MAP_FIXED_NOREPLACE  0x100000
#define MAP_FAILED           ((void *)-1)

#define MADV_NORMAL    0
#define MADV_DONTNEED  4
#define MADV_FREE      8

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t len, int advice);

#endif
