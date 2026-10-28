/*
 * host_prelude.h - the C library names that the string sources (string.c, wcstring.c, printf.c) and their unit tests
 * take from the Microsoft headers, and, in the host build, the renames of the functions those files define.
 * Included by ntdll_misc.h, and forced into the host unit tests.
 * Part of the Rtl port (see docs/policy/THIRD-PARTY.md).
 */
#ifndef RTL_COMPAT_HOST_PRELUDE_H
#define RTL_COMPAT_HOST_PRELUDE_H

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winnt.h"
#include "winternl.h"

/* A Windows long has 32 bits; the unit tests compare the results of wcstol and its relatives with these limits. */
#undef LONG_MAX
#undef LONG_MIN
#undef ULONG_MAX
#define LONG_MAX 2147483647
#define LONG_MIN (-2147483647 - 1)
#define ULONG_MAX 0xffffffffu

/* Names the string sources take from the Microsoft C library headers. A Windows long has 32 bits. */
typedef int errno_t;
typedef LONG __msvcrt_long;
typedef ULONG __msvcrt_ulong;
#ifndef I64_MAX
#define I64_MAX INT64_MAX
#define I64_MIN INT64_MIN
#define UI64_MAX UINT64_MAX
#endif
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
#ifndef _MAX_DRIVE
#define _MAX_DRIVE 3
#define _MAX_DIR 256
#define _MAX_FNAME 256
#define _MAX_EXT 256
#endif
#ifndef STRUNCATE
#define STRUNCATE 80
#endif

#ifdef __MINGW32__
/* NTDLL_sprintf is defined in printf.c. */
int WINAPIV NTDLL_sprintf(char *str, const char *format, ...);
#endif

/* MSVC spells these without the underscore; swprintf is the counted form (Wine's headers pick the old two-argument one). */
#define wcsicmp _wcsicmp
#define wcsnicmp _wcsnicmp
#define swprintf _snwprintf

/* printf.c names this one NTDLL_ to stay clear of the compiler's own declaration; ntdll.def exports it as sprintf. */
#define sprintf NTDLL_sprintf

/* wcstring.c and string.c define the C library names that ntdll.dll exports, with 16-bit wide characters. A native
 * wchar_t has 32 bits, so the host tests rename them to keep clear of the C library. */
#ifndef __MINGW32__
#include "host_rename.h"
#endif

#ifdef WINE_UNIX_LIB
/* winternl.h leaves these out where there is no Microsoft va_list. */
NTSTATUS WINAPI RtlFormatMessage(LPCWSTR, ULONG, BOOLEAN, BOOLEAN, BOOLEAN, va_list *, LPWSTR, ULONG, ULONG *);
NTSTATUS WINAPI vDbgPrintEx(ULONG, ULONG, LPCSTR, va_list);
NTSTATUS WINAPI vDbgPrintExWithPrefix(LPCSTR, ULONG, ULONG, LPCSTR, va_list);
NTSTATUS WINAPI RtlFormatMessageEx(LPCWSTR, ULONG, BOOLEAN, BOOLEAN, BOOLEAN, va_list *, LPWSTR, ULONG, ULONG *, ULONG);
#endif

#endif
