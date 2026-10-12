/* Minimal POSIX <sys/mman.h> for syntax-checking on MinGW (no real mmap). */
#ifndef SYNTAX_SHIM_SYS_MMAN_H
#define SYNTAX_SHIM_SYS_MMAN_H
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED           0x01
#define MAP_PRIVATE          0x02
#define MAP_FIXED            0x10
#define MAP_ANONYMOUS        0x20
#define MAP_ANON             MAP_ANONYMOUS
#define MAP_FIXED_NOREPLACE  0x100000
#define MAP_FAILED           ((void *)-1)

#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

#define MADV_DONTNEED 4
#define MADV_FREE     8

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);
int   msync(void *addr, size_t len, int flags);
int   madvise(void *addr, size_t len, int advice);

#endif
