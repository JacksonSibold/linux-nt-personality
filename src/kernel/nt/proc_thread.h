/* proc_thread.h - NT process/thread objects, suspend count, query info
 * Dual compilation: __KERNEL__ and USERSPACE_KSHIM
 */
#ifndef NT_PROC_THREAD_H
#define NT_PROC_THREAD_H

#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "object.h"
#include "handle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* KPRIORITY missing from nt_types.h - define */
#ifndef KPRIORITY_DEFINED
#define KPRIORITY_DEFINED
typedef LONG KPRIORITY;
#endif

/* ------------------------------------------------------------------ */
/* Structures for query (ABI exact)                                    */
/* ------------------------------------------------------------------ */
typedef struct _PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    PPEB PebBaseAddress;
    ULONG_PTR AffinityMask;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION, *PPROCESS_BASIC_INFORMATION;

typedef struct _THREAD_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    CLIENT_ID ClientId;
    ULONG_PTR AffinityMask;
    KPRIORITY Priority;
    KPRIORITY BasePriority;
} THREAD_BASIC_INFORMATION, *PTHREAD_BASIC_INFORMATION;

typedef struct _KERNEL_USER_TIMES {
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER ExitTime;
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
} KERNEL_USER_TIMES, *PKERNEL_USER_TIMES;

/* ------------------------------------------------------------------ */
/* Process / Thread creation flags                                      */
/* ------------------------------------------------------------------ */
#define CREATE_SUSPENDED_FLAG 0x00000004UL
#define THREAD_CREATE_SUSPENDED 0x00000001UL
#define STILL_ACTIVE 259

#ifdef __KERNEL__
struct pt_regs;
void nt_exc_set_dispatcher(unsigned long va);
bool nt_exc_user_fault(struct pt_regs *regs, unsigned long address, unsigned long error_code);
bool nt_exc_user_trap(struct pt_regs *regs, int trapnr);
#endif

/* ------------------------------------------------------------------ */
/* Suspend count design (documented in proc_thread.c)                   */
/* ------------------------------------------------------------------ */
/*
 * Design: per-thread suspend_count (atomic) + gate + TIF_NTSUSPEND flag.
 * See proc_thread.c for full design and race analysis.
 */

/* ------------------------------------------------------------------ */
/* Kernel/User API                                                      */
/* ------------------------------------------------------------------ */

NTSTATUS nt_svc_NtCreateProcessEx(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                                  POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ParentProcess,
                                  ULONG Flags, HANDLE SectionHandle, HANDLE DebugPort,
                                  HANDLE ExceptionPort, PVOID JobMemberLevel);
NTSTATUS nt_svc_NtCreateProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                                POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ParentProcess,
                                BOOLEAN InheritObjectTable, HANDLE SectionHandle,
                                HANDLE DebugPort, HANDLE ExceptionPort);
NTSTATUS nt_svc_NtCreateUserProcess(PHANDLE ProcessHandle, PHANDLE ThreadHandle,
                                    ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess,
                                    POBJECT_ATTRIBUTES ProcessObjectAttributes,
                                    POBJECT_ATTRIBUTES ThreadObjectAttributes,
                                    ULONG ProcessFlags, ULONG ThreadFlags,
                                    PRTL_USER_PROCESS_PARAMETERS ProcessParameters,
                                    PVOID CreateInfo, PVOID AttributeList);

NTSTATUS nt_svc_NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                                 PVOID StartRoutine, PVOID Argument, ULONG CreateFlags,
                                 SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize,
                                 PVOID AttributeList);
NTSTATUS nt_svc_NtCreateThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                               POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                               PVOID ClientId, PVOID ThreadContext, PVOID InitialTeb,
                               BOOLEAN CreateSuspended);

NTSTATUS nt_svc_NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
NTSTATUS nt_svc_NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
NTSTATUS nt_svc_NtSuspendProcess(HANDLE ProcessHandle);
NTSTATUS nt_svc_NtResumeProcess(HANDLE ProcessHandle);

NTSTATUS nt_svc_NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus);
NTSTATUS nt_svc_NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);

NTSTATUS nt_svc_NtOpenProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes, CLIENT_ID *ClientId);
NTSTATUS nt_svc_NtOpenThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                             POBJECT_ATTRIBUTES ObjectAttributes, CLIENT_ID *ClientId);

NTSTATUS nt_svc_NtQueryInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass,
                                          PVOID ProcessInformation, ULONG ProcessInformationLength,
                                          PULONG ReturnLength);
NTSTATUS nt_svc_NtQueryInformationThread(HANDLE ThreadHandle, ULONG ThreadInformationClass,
                                         PVOID ThreadInformation, ULONG ThreadInformationLength,
                                         PULONG ReturnLength);
NTSTATUS nt_svc_NtSetInformationProcess(HANDLE ProcessHandle, ULONG Class, PVOID Info, ULONG Length);
NTSTATUS nt_svc_NtSetInformationThread(HANDLE ThreadHandle, ULONG Class, PVOID Info, ULONG Length);

NTSTATUS nt_svc_NtAlertResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
NTSTATUS nt_svc_NtAlertThread(HANDLE ThreadHandle);
NTSTATUS nt_svc_NtAlertThreadByThreadId(HANDLE ThreadId);

NTSTATUS nt_svc_NtGetContextThread(HANDLE ThreadHandle, PCONTEXT Context);
NTSTATUS nt_svc_NtSetContextThread(HANDLE ThreadHandle, PCONTEXT Context);

/* Helpers */
void nt_thread_check_suspend(void); /* call on return to user */

#ifdef __cplusplus
}
#endif
#endif
