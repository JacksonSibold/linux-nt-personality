/*
 * win32_policy.h - pure argument mapping for the kernel32 shims.
 *
 * The shims in kernel32.c call into ntdll with values computed here. Keeping
 * the mapping in a header lets tests/ntdll-host check it without the MinGW
 * TEB access or the NT syscall stubs that kernel32.c needs.
 */
#ifndef NTDLL_WIN32_POLICY_H
#define NTDLL_WIN32_POLICY_H

#include "nt_types.h"

/* Win32 PAGE_* protection value to NT protection value. The two sets of
 * numbers are the same for the base values. Returns 0 for a value that is
 * not exactly one base protection; the caller reports ERROR_INVALID_PARAMETER.
 * Modifier bits (PAGE_GUARD, PAGE_NOCACHE, PAGE_WRITECOMBINE) are ignored, as
 * in VirtualAlloc. */
static inline BOOL win32_protect_to_nt(DWORD prot, ULONG *nt)
{
    switch (prot & 0xFF) {
    case 0x01: /* PAGE_NOACCESS */
    case 0x02: /* PAGE_READONLY */
    case 0x04: /* PAGE_READWRITE */
    case 0x08: /* PAGE_WRITECOPY */
    case 0x10: /* PAGE_EXECUTE */
    case 0x20: /* PAGE_EXECUTE_READ */
    case 0x40: /* PAGE_EXECUTE_READWRITE */
    case 0x80: /* PAGE_EXECUTE_WRITECOPY */
        *nt = prot & 0xFF;
        return 1;
    default:
        return 0;
    }
}

#endif /* NTDLL_WIN32_POLICY_H */
