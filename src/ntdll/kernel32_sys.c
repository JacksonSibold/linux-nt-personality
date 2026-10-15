/*
 * kernel32_sys.c - system and process information shims (K32-Y1):
 * GetPriorityClass, GetThreadPriority, SetThreadPriority,
 * GetHandleInformation, GetProcessHandleCount, GetBinaryTypeA and
 * SetUnhandledExceptionFilter. The rules are in k32_sys_pure.h.
 *
 * The priority calls use NtQueryInformationProcess (ProcessPriorityClass),
 * NtQueryInformationThread (ThreadBasicInformation) and NtSetInformationThread
 * (ThreadBasePriority). GetHandleInformation uses NtQueryObject
 * (ObjectHandleFlagInformation). GetProcessHandleCount uses
 * NtQueryInformationProcess (ProcessHandleCount, class 20), which reports the
 * live count of the target's handle table.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include "k32_sys_pure.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef UINT
#define UINT unsigned int
#endif

typedef PVOID LPVOID;

#define K32Y_INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define K32Y_ERROR_INVALID_PARAMETER 87u
#define K32Y_ERROR_CALL_NOT_IMPLEMENTED 120u
#define K32Y_ERROR_BAD_EXE_FORMAT 193u
#define K32Y_GENERIC_READ 0x80000000u
#define K32Y_FILE_SHARE_READ 0x00000001u
#define K32Y_OPEN_EXISTING 3u
#define K32Y_CLASS_PROCESS_PRIORITY 18u
#define K32Y_CLASS_THREAD_BASIC 0u
#define K32Y_THREAD_BASE_PRIORITY 3u
#define K32Y_CLASS_HANDLE_FLAG 4u
#define K32Y_BINARY_HEADER_BYTES 4096u

__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);
__declspec(dllimport) extern NTSTATUS NtQueryInformationProcess(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtQueryInformationThread(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtSetInformationThread(HANDLE, ULONG, PVOID, ULONG);
__declspec(dllimport) extern NTSTATUS NtQueryObject(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtReadFile(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
__declspec(dllimport) extern NTSTATUS NtClose(HANDLE);

/* Defined in kernel32.c and kernel32_fs.c. */
void SetLastError(DWORD e);
HANDLE WINAPI CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE templ);

/* PROCESS_PRIORITY_CLASS (ProcessPriorityClass). */
typedef struct {
    BOOLEAN Foreground;
    UCHAR PriorityClass;
} K32Y_PROCESS_PRIORITY_CLASS;

/* THREAD_BASIC_INFORMATION (x64): BasePriority is the thread's base. */
typedef struct {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
    ULONG_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
} K32Y_THREAD_BASIC_INFORMATION;

/* OBJECT_HANDLE_FLAG_INFORMATION. */
typedef struct {
    BOOLEAN Inherit;
    BOOLEAN ProtectFromClose;
} K32Y_OBJECT_HANDLE_FLAG_INFORMATION;

static void k32y_status(NTSTATUS s)
{
    SetLastError(NT_SUCCESS(s) ? 0 : RtlNtStatusToDosError(s));
}

static PVOID g_k32y_unhandled_filter;

DWORD WINAPI GetPriorityClass(HANDLE h)
{
    K32Y_PROCESS_PRIORITY_CLASS pc;
    NTSTATUS s = NtQueryInformationProcess(h, K32Y_CLASS_PROCESS_PRIORITY, &pc, sizeof(pc), NULL);
    if (!NT_SUCCESS(s)) {
        k32y_status(s);
        return 0;
    }
    SetLastError(0);
    return k32y_priority_class_from_nt(pc.PriorityClass);
}

int WINAPI GetThreadPriority(HANDLE h)
{
    K32Y_THREAD_BASIC_INFORMATION tbi;
    NTSTATUS s = NtQueryInformationThread(h, K32Y_CLASS_THREAD_BASIC, &tbi, sizeof(tbi), NULL);
    if (!NT_SUCCESS(s)) {
        k32y_status(s);
        return K32Y_THREAD_PRIORITY_ERROR_RETURN;
    }
    SetLastError(0);
    return k32y_priority_from_thread_base(tbi.BasePriority);
}

BOOL WINAPI SetThreadPriority(HANDLE h, int prio)
{
    if (!k32y_thread_priority_valid(prio)) {
        SetLastError(K32Y_ERROR_INVALID_PARAMETER);
        return 0;
    }
    LONG increment = prio;
    NTSTATUS s = NtSetInformationThread(h, K32Y_THREAD_BASE_PRIORITY, &increment, sizeof(increment));
    k32y_status(s);
    return NT_SUCCESS(s) ? TRUE : FALSE;
}

BOOL WINAPI GetHandleInformation(HANDLE h, DWORD *flags)
{
    if (!flags) {
        SetLastError(K32Y_ERROR_INVALID_PARAMETER);
        return 0;
    }
    K32Y_OBJECT_HANDLE_FLAG_INFORMATION fi;
    NTSTATUS s = NtQueryObject(h, K32Y_CLASS_HANDLE_FLAG, &fi, sizeof(fi), NULL);
    if (!NT_SUCCESS(s)) {
        k32y_status(s);
        return 0;
    }
    *flags = k32y_handle_flags(fi.Inherit, fi.ProtectFromClose);
    SetLastError(0);
    return 1;
}

/* ProcessHandleCount (class 20): the live handle count of the process's own
 * table. Windows does not clear the last error on success, so none is set. */
#define K32Y_PROCESS_HANDLE_COUNT 20
BOOL WINAPI GetProcessHandleCount(HANDLE h, DWORD *count)
{
    ULONG n = 0;
    NTSTATUS s;
    if (!count) {
        SetLastError(K32Y_ERROR_INVALID_PARAMETER);
        return 0;
    }
    s = NtQueryInformationProcess(h, K32Y_PROCESS_HANDLE_COUNT, &n, sizeof(n), NULL);
    if (!NT_SUCCESS(s)) {
        k32y_status(s);
        return 0;
    }
    *count = (DWORD)n;
    return 1;
}

BOOL WINAPI GetBinaryTypeA(const char *app, DWORD *type)
{
    if (!app || !type) {
        SetLastError(K32Y_ERROR_INVALID_PARAMETER);
        return 0;
    }
    HANDLE h = CreateFileA(app, K32Y_GENERIC_READ, K32Y_FILE_SHARE_READ, NULL,
                           K32Y_OPEN_EXISTING, 0, NULL);
    if (h == K32Y_INVALID_HANDLE_VALUE)
        return 0;
    BYTE hdr[K32Y_BINARY_HEADER_BYTES];
    IO_STATUS_BLOCK iosb;
    NTSTATUS s = NtReadFile(h, NULL, NULL, NULL, &iosb, hdr, sizeof(hdr), NULL, NULL);
    NtClose(h);
    if (!NT_SUCCESS(s) && s != STATUS_END_OF_FILE) {
        k32y_status(s);
        return 0;
    }
    DWORD kind = 0;
    if (!k32y_binary_type(hdr, (size_t)iosb.Information, &kind)) {
        SetLastError(K32Y_ERROR_BAD_EXE_FORMAT);
        return 0;
    }
    *type = kind;
    SetLastError(0);
    return 1;
}

/* SetUnhandledExceptionFilter keeps the filter and returns the previous one.
 * The filter is not called yet: UnhandledExceptionFilter reports 1 without
 * running it. */
PVOID WINAPI SetUnhandledExceptionFilter(PVOID filter)
{
    PVOID old = g_k32y_unhandled_filter;
    g_k32y_unhandled_filter = filter;
    return old;
}

#endif /* __MINGW32__ */
