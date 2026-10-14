/*
 * k32_sys_pure.h - pure rules for the kernel32 system information shims
 * (kernel32_sys.c): thread and process priority mapping, handle flags, and
 * the binary type from a PE header.
 */
#ifndef NTDLL_K32_SYS_PURE_H
#define NTDLL_K32_SYS_PURE_H

#include "nt_types.h"

/* THREAD_PRIORITY_* (Win32) and the NT base priority of a thread. The NT base
 * for THREAD_PRIORITY_NORMAL is 8, so the base is 8 + offset for -2..2. */
#define K32Y_THREAD_PRIORITY_IDLE          (-15)
#define K32Y_THREAD_PRIORITY_LOWEST        (-2)
#define K32Y_THREAD_PRIORITY_TIME_CRITICAL 15
#define K32Y_THREAD_PRIORITY_ERROR_RETURN  0x7FFFFFFF
#define K32Y_NT_BASE_NORMAL                8

/* Win32 priority class values (dwPriorityClass). */
#define K32Y_IDLE_PRIORITY_CLASS         0x00000040u
#define K32Y_BELOW_NORMAL_PRIORITY_CLASS 0x00004000u
#define K32Y_NORMAL_PRIORITY_CLASS       0x00000020u
#define K32Y_ABOVE_NORMAL_PRIORITY_CLASS 0x00008000u
#define K32Y_HIGH_PRIORITY_CLASS         0x00000080u
#define K32Y_REALTIME_PRIORITY_CLASS     0x00000100u
/* PROCESS_PRIORITY_CLASS values the NT service reports. */
#define K32Y_NT_CLASS_IDLE               1u
#define K32Y_NT_CLASS_NORMAL             2u
#define K32Y_NT_CLASS_HIGH               3u
#define K32Y_NT_CLASS_REALTIME           4u
#define K32Y_NT_CLASS_BELOW_NORMAL       5u
#define K32Y_NT_CLASS_ABOVE_NORMAL       6u

/* SetThreadPriority values: the seven Win32 levels. The kernel stores the
 * value as the thread's increment over its process priority (proc_thread.c,
 * ThreadBasePriority), so it is passed through unchanged. */
static inline int k32y_thread_priority_valid(int prio)
{
    return prio == K32Y_THREAD_PRIORITY_IDLE || prio == K32Y_THREAD_PRIORITY_TIME_CRITICAL ||
           (prio >= K32Y_THREAD_PRIORITY_LOWEST && prio <= 2);
}

/* GetThreadPriority from an NT base priority. Bases that are not one of the
 * seven map to THREAD_PRIORITY_ERROR_RETURN. */
static inline int k32y_priority_from_thread_base(LONG base)
{
    if (base == 1) return K32Y_THREAD_PRIORITY_IDLE;
    if (base == 15) return K32Y_THREAD_PRIORITY_TIME_CRITICAL;
    if (base < 6 || base > 10) return K32Y_THREAD_PRIORITY_ERROR_RETURN;
    return (int)base - K32Y_NT_BASE_NORMAL;
}

/* GetPriorityClass from the NT PROCESS_PRIORITY_CLASS value. Unknown values
 * map to NORMAL. */
static inline DWORD k32y_priority_class_from_nt(unsigned int nt)
{
    switch (nt) {
    case K32Y_NT_CLASS_IDLE:         return K32Y_IDLE_PRIORITY_CLASS;
    case K32Y_NT_CLASS_BELOW_NORMAL: return K32Y_BELOW_NORMAL_PRIORITY_CLASS;
    case K32Y_NT_CLASS_ABOVE_NORMAL: return K32Y_ABOVE_NORMAL_PRIORITY_CLASS;
    case K32Y_NT_CLASS_HIGH:         return K32Y_HIGH_PRIORITY_CLASS;
    case K32Y_NT_CLASS_REALTIME:     return K32Y_REALTIME_PRIORITY_CLASS;
    default:                         return K32Y_NORMAL_PRIORITY_CLASS;
    }
}

/* The NT PROCESS_PRIORITY_CLASS for a Win32 class. Returns 0 for a value that
 * is not one of the six. */
static inline unsigned int k32y_nt_class_from_priority(DWORD win)
{
    switch (win) {
    case K32Y_IDLE_PRIORITY_CLASS:         return K32Y_NT_CLASS_IDLE;
    case K32Y_BELOW_NORMAL_PRIORITY_CLASS: return K32Y_NT_CLASS_BELOW_NORMAL;
    case K32Y_NORMAL_PRIORITY_CLASS:       return K32Y_NT_CLASS_NORMAL;
    case K32Y_ABOVE_NORMAL_PRIORITY_CLASS: return K32Y_NT_CLASS_ABOVE_NORMAL;
    case K32Y_HIGH_PRIORITY_CLASS:         return K32Y_NT_CLASS_HIGH;
    case K32Y_REALTIME_PRIORITY_CLASS:     return K32Y_NT_CLASS_REALTIME;
    default:                               return 0;
    }
}

/* GetHandleInformation flags from NtQueryObject(ObjectHandleFlagInformation):
 * Inherit is HANDLE_FLAG_INHERIT (1), ProtectFromClose is
 * HANDLE_FLAG_PROTECT_FROM_CLOSE (2). */
static inline DWORD k32y_handle_flags(BOOLEAN inherit, BOOLEAN protect)
{
    return (inherit ? 1u : 0u) | (protect ? 2u : 0u);
}

/* GetBinaryType from the first bytes of a file. Returns 1 and stores the
 * SCS_* value for a PE32+ x64 image (SCS_64BIT_BINARY 6) or a PE32 i386 image
 * (SCS_32BIT_BINARY 0). Returns 0 when the bytes are not a PE image of a
 * supported machine (the caller fails with ERROR_BAD_EXE_FORMAT). */
#define K32Y_SCS_32BIT_BINARY 0u
#define K32Y_SCS_64BIT_BINARY 6u
static inline int k32y_binary_type(const unsigned char *hdr, size_t n, DWORD *type)
{
    if (n < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z')
        return 0;
    unsigned long lfanew = (unsigned long)hdr[0x3c] | ((unsigned long)hdr[0x3d] << 8) |
                           ((unsigned long)hdr[0x3e] << 16) | ((unsigned long)hdr[0x3f] << 24);
    if (lfanew > n || n - lfanew < 6)
        return 0;
    const unsigned char *pe = hdr + lfanew;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0)
        return 0;
    unsigned int machine = (unsigned int)pe[4] | ((unsigned int)pe[5] << 8);
    if (machine == 0x8664u) { *type = K32Y_SCS_64BIT_BINARY; return 1; }
    if (machine == 0x014Cu) { *type = K32Y_SCS_32BIT_BINARY; return 1; }
    return 0;
}

#endif /* NTDLL_K32_SYS_PURE_H */
