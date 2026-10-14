/*
 * kernel32_thread.c - thread and process shims (K32-T1): CreateThread,
 * TerminateThread, SuspendThread, ResumeThread, GetExitCodeThread and
 * GetExitCodeProcess on the NT thread and process services. The rules are in
 * k32_thread_pure.h.
 *
 * CreateProcessA and CreateProcessW are not implemented. NtCreateUserProcess
 * needs a process-parameters block and a section for the image; that is not
 * built yet, so they fail with ERROR_CALL_NOT_IMPLEMENTED.
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
#include "k32_thread_pure.h"
#include <stddef.h>
#include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef NTAPI
#define NTAPI __stdcall
#endif

typedef PVOID LPVOID;

#define K32T_ERROR_CALL_NOT_IMPLEMENTED 120u

__declspec(dllimport) ULONG RtlNtStatusToDosError(NTSTATUS Status);
__declspec(dllimport) extern NTSTATUS NtCreateThreadEx(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, HANDLE, PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
__declspec(dllimport) extern NTSTATUS NtTerminateThread(HANDLE, NTSTATUS);
__declspec(dllimport) extern NTSTATUS NtSuspendThread(HANDLE, PULONG);
__declspec(dllimport) extern NTSTATUS NtResumeThread(HANDLE, PULONG);
__declspec(dllimport) extern NTSTATUS NtQueryInformationThread(HANDLE, ULONG, PVOID, ULONG, PULONG);
__declspec(dllimport) extern NTSTATUS NtQueryInformationProcess(HANDLE, ULONG, PVOID, ULONG, PULONG);

/* Defined in kernel32.c. */
void SetLastError(DWORD e);

/* THREAD_BASIC_INFORMATION (ThreadBasicInformation, class 0) and
 * PROCESS_BASIC_INFORMATION (ProcessBasicInformation, class 0), x64 layout.
 * Only the leading fields are read: ExitStatus and the thread id. */
typedef struct {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} K32_CLIENT_ID;

typedef struct {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    K32_CLIENT_ID ClientId;
    ULONG_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
} K32_THREAD_BASIC_INFORMATION;

typedef struct {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} K32_PROCESS_BASIC_INFORMATION;

#define K32T_THREAD_BASIC_INFORMATION_CLASS 0
#define K32T_PROCESS_BASIC_INFORMATION_CLASS 0

static void k32t_status(NTSTATUS s)
{
    SetLastError(NT_SUCCESS(s) ? 0 : RtlNtStatusToDosError(s));
}

HANDLE WINAPI CreateThread(void *sa, SIZE_T stack, void *start, void *param, DWORD flags, DWORD *id)
{
    (void)sa;
    ULONG ntflags = 0;
    if (!start || !k32t_create_flags(flags, &ntflags)) {
        SetLastError(K32T_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    HANDLE h = NULL;
    /* The current process is the pseudo handle -1. ZeroBits 0, and the stack
     * commit and reserve are both the caller's dwStackSize (0 takes the
     * default). */
    NTSTATUS s = NtCreateThreadEx(&h, K32T_THREAD_ALL_ACCESS, NULL, (HANDLE)(intptr_t)-1,
                                  start, param, ntflags, 0, stack, stack, NULL);
    if (!NT_SUCCESS(s)) {
        k32t_status(s);
        return NULL;
    }
    if (id) {
        K32_THREAD_BASIC_INFORMATION tbi;
        NTSTATUS q = NtQueryInformationThread(h, K32T_THREAD_BASIC_INFORMATION_CLASS,
                                              &tbi, sizeof(tbi), NULL);
        if (!NT_SUCCESS(q)) {
            k32t_status(q);
            return h;
        }
        *id = (DWORD)(ULONG_PTR)tbi.ClientId.UniqueThread;
    }
    SetLastError(0);
    return h;
}

BOOL WINAPI TerminateThread(HANDLE h, DWORD code)
{
    NTSTATUS s = NtTerminateThread(h, (NTSTATUS)code);
    k32t_status(s);
    return NT_SUCCESS(s) ? 1 : 0;
}

DWORD WINAPI SuspendThread(HANDLE h)
{
    ULONG prev = 0;
    NTSTATUS s = NtSuspendThread(h, &prev);
    if (!NT_SUCCESS(s)) k32t_status(s);
    else SetLastError(0);
    return k32t_suspend_result(s, prev);
}

DWORD WINAPI ResumeThread(HANDLE h)
{
    ULONG prev = 0;
    NTSTATUS s = NtResumeThread(h, &prev);
    if (!NT_SUCCESS(s)) k32t_status(s);
    else SetLastError(0);
    return k32t_suspend_result(s, prev);
}

BOOL WINAPI GetExitCodeThread(HANDLE h, DWORD *code)
{
    if (!code) {
        SetLastError(K32T_ERROR_INVALID_PARAMETER);
        return 0;
    }
    K32_THREAD_BASIC_INFORMATION tbi;
    NTSTATUS s = NtQueryInformationThread(h, K32T_THREAD_BASIC_INFORMATION_CLASS,
                                          &tbi, sizeof(tbi), NULL);
    if (!NT_SUCCESS(s)) {
        k32t_status(s);
        return 0;
    }
    *code = k32t_exit_code(tbi.ExitStatus);
    SetLastError(0);
    return 1;
}

BOOL WINAPI GetExitCodeProcess(HANDLE h, DWORD *code)
{
    if (!code) {
        SetLastError(K32T_ERROR_INVALID_PARAMETER);
        return 0;
    }
    K32_PROCESS_BASIC_INFORMATION pbi;
    NTSTATUS s = NtQueryInformationProcess(h, K32T_PROCESS_BASIC_INFORMATION_CLASS,
                                           &pbi, sizeof(pbi), NULL);
    if (!NT_SUCCESS(s)) {
        k32t_status(s);
        return 0;
    }
    *code = k32t_exit_code(pbi.ExitStatus);
    SetLastError(0);
    return 1;
}

/* K32-T2: not implemented, see the file comment. */
HANDLE WINAPI CreateProcessA(const char *app, char *cmd, void *pa, void *ta, BOOL inh, DWORD flags, void *env, const char *dir, void *si, void *pi)
{
    (void)app;(void)cmd;(void)pa;(void)ta;(void)inh;(void)flags;(void)env;(void)dir;(void)si;(void)pi;
    SetLastError(K32T_ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
}

HANDLE WINAPI CreateProcessW(const WCHAR *app, WCHAR *cmd, void *pa, void *ta, BOOL inh, DWORD flags, void *env, const WCHAR *dir, void *si, void *pi)
{
    (void)app;(void)cmd;(void)pa;(void)ta;(void)inh;(void)flags;(void)env;(void)dir;(void)si;(void)pi;
    SetLastError(K32T_ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
}

#endif /* __MINGW32__ */
