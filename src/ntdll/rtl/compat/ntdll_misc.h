/*
 * ntdll_misc.h - stand-in for Wine's private ntdll header. It carries the few helpers the ported sources use.
 * Part of the Rtl port (see docs/policy/THIRD-PARTY.md).
 */
#ifndef RTL_COMPAT_NTDLL_MISC_H
#define RTL_COMPAT_NTDLL_MISC_H

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "winbase.h"
#include "wine/debug.h"

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* The shared user data page. Programs read it at this fixed address. */
#define user_shared_data ((KUSER_SHARED_DATA *)0x7ffe0000)

#include "host_prelude.h"

/* A NULL heap handle means the process heap in ntdll_heap.c, and in the host test support code. */
#define GetProcessHeap() ((HANDLE)NULL)

#endif
