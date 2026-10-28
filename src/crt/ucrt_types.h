/*
 * ucrt_types.h - the types and limits shared by the C runtime sources in src/crt.
 *
 * The runtime is freestanding: it includes only <stddef.h>, <stdint.h>, <stdarg.h> and <limits.h> style
 * constants of its own. Functions are static inline in headers with the prefix ucrt_, so the host tests can
 * compare each one with the system C library under its usual name, and the PE wrappers (src/ntdll/ucrtbase.c,
 * msvcrt.c) export them under the Windows names.
 *
 * wchar_t is 16 bits, as on Windows. A wide character above U+FFFF is a surrogate pair.
 */
#ifndef UCRT_TYPES_H
#define UCRT_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint16_t ucrt_wchar;
typedef int ucrt_errno_t;

#define UCRT_EOF (-1)
#define UCRT_WEOF ((ucrt_wchar)0xFFFFu)

/* errno values the runtime sets, with the numbers MSVC uses. */
#define UCRT_EPERM 1
#define UCRT_ENOENT 2
#define UCRT_ESRCH 3
#define UCRT_EINTR 4
#define UCRT_EIO 5
#define UCRT_ENXIO 6
#define UCRT_E2BIG 7
#define UCRT_ENOEXEC 8
#define UCRT_EBADF 9
#define UCRT_ECHILD 10
#define UCRT_EAGAIN 11
#define UCRT_ENOMEM 12
#define UCRT_EACCES 13
#define UCRT_EFAULT 14
#define UCRT_EBUSY 16
#define UCRT_EEXIST 17
#define UCRT_EXDEV 18
#define UCRT_ENODEV 19
#define UCRT_ENOTDIR 20
#define UCRT_EISDIR 21
#define UCRT_EINVAL 22
#define UCRT_ENFILE 23
#define UCRT_EMFILE 24
#define UCRT_ENOTTY 25
#define UCRT_EFBIG 27
#define UCRT_ENOSPC 28
#define UCRT_ESPIPE 29
#define UCRT_EROFS 30
#define UCRT_EMLINK 31
#define UCRT_EPIPE 32
#define UCRT_EDOM 33
#define UCRT_ERANGE 34
#define UCRT_EDEADLK 36
#define UCRT_ENAMETOOLONG 38
#define UCRT_ENOLCK 39
#define UCRT_ENOSYS 40
#define UCRT_ENOTEMPTY 41
#define UCRT_EILSEQ 42
#define UCRT_STRUNCATE 80

#endif /* UCRT_TYPES_H */
