/*
 * debugobj.h - NT DEBUG objects (kernel/nt/debugobj.c).
 *
 * A debug object is a queue of debug state changes for the processes attached
 * to it. A debugger attaches a process with NtDebugActiveProcess, waits for
 * events with NtWaitForDebugEvent and resumes each one with NtDebugContinue.
 * A process has at most one debug object (proc->debug_obj).
 *
 * The state-change structure keeps the NT layout for the fields this module
 * fills (NewState, AppClientId, the exit statuses and the thread handle slot).
 * The exception and load-DLL payloads are not defined here, because no event
 * of those kinds is generated in this tree.
 */

#ifndef NTDEBUGOBJ_H
#define NTDEBUGOBJ_H

#include "object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Flags and access rights (winnt.h)                                          */
/* -------------------------------------------------------------------------- */

/* NtCreateDebugObject flags. Any other bit is STATUS_INVALID_PARAMETER. */
#define DEBUG_KILL_ON_CLOSE                 0x00000001UL

#define DEBUG_OBJECT_WAIT_STATE_CHANGE      0x0001UL
#define DEBUG_OBJECT_ADD_REMOVE_PROCESS     0x0002UL
#define DEBUG_OBJECT_SET_INFORMATION        0x0004UL
/* STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x7 */
#define DEBUG_OBJECT_ALL_ACCESS             0x001F0007UL

#ifndef PROCESS_SUSPEND_RESUME
#define PROCESS_SUSPEND_RESUME              0x0800UL
#endif

#ifndef STATUS_PORT_NOT_SET
#define STATUS_PORT_NOT_SET                 ((NTSTATUS)0xC0000353L)
#endif

/* Continue statuses accepted by NtDebugContinue (ntdbg.h). */
#define DBG_CONTINUE                        ((NTSTATUS)0x00010002L)
#define DBG_EXCEPTION_NOT_HANDLED           ((NTSTATUS)0x80010001L)

/* -------------------------------------------------------------------------- */
/* Information classes and state changes                                      */
/* -------------------------------------------------------------------------- */

typedef enum _DEBUGOBJECTINFOCLASS {
    DebugObjectKillProcessOnExitInformation = 1,   /* BOOLEAN payload */
} DEBUGOBJECTINFOCLASS;

typedef enum _DBG_STATE {
    DbgIdle                     = 0,
    DbgReplyPending             = 1,
    DbgCreateThreadStateChange  = 2,
    DbgCreateProcessStateChange = 3,
    DbgExitThreadStateChange    = 4,
    DbgExitProcessStateChange   = 5,
    DbgExceptionStateChange     = 6,
    DbgBreakpointStateChange    = 7,
    DbgSingleStepStateChange    = 8,
    DbgLoadDllStateChange       = 9,
    DbgUnloadDllStateChange     = 10,
} DBG_STATE, *PDBG_STATE;

typedef struct _DBGUI_WAIT_STATE_CHANGE {
    DBG_STATE NewState;
    CLIENT_ID AppClientId;     /* UniqueProcess = pid, UniqueThread = tid (0 for process events) */
    union {
        struct { HANDLE HandleToThread; } CreateThread;
        struct { HANDLE HandleToProcess; HANDLE HandleToThread; } CreateProcessInfo;
        struct { NTSTATUS ExitStatus; } ExitThread;
        struct { NTSTATUS ExitStatus; } ExitProcess;
    } StateInfo;
} DBGUI_WAIT_STATE_CHANGE, *PDBGUI_WAIT_STATE_CHANGE;

/* -------------------------------------------------------------------------- */
/* Object body and hooks                                                      */
/* -------------------------------------------------------------------------- */

struct nt_debug_obj;

/* Object type. Registered by the create service. */
extern struct nt_object_type nt_debug_object_type;

/* Queues a state change for proc, if a debugger is attached. Called by the
 * process and thread exit and create paths (proc_thread.c). Lock-free for a
 * process that is not attached; drops the event if memory is short. Must not
 * be called with proc->lock or a thread lock held. */
void nt_debug_post_event(struct nt_eprocess *proc, DBG_STATE state, ULONG tid, NTSTATUS status);

#ifndef __KERNEL__
/* Userspace service entry points (typed). The kernel build takes the ULONG
 * ABI wrappers in debugobj.c instead. NtSetInformationDebugObject has no SSN
 * in nt_service_list.h, so it is reachable only from the host tests. */
NTSTATUS nt_svc_NtCreateDebugObject(PHANDLE DebugObjectHandle, ACCESS_MASK DesiredAccess,
                                    POBJECT_ATTRIBUTES ObjectAttributes, ULONG Flags);
NTSTATUS nt_svc_NtDebugActiveProcess(HANDLE ProcessHandle, HANDLE DebugObjectHandle);
NTSTATUS nt_svc_NtRemoveProcessDebug(HANDLE ProcessHandle, HANDLE DebugObjectHandle);
NTSTATUS nt_svc_NtWaitForDebugEvent(HANDLE DebugObjectHandle, BOOLEAN Alertable,
                                    PLARGE_INTEGER Timeout,
                                    PDBGUI_WAIT_STATE_CHANGE WaitStateChange);
NTSTATUS nt_svc_NtDebugContinue(HANDLE DebugObjectHandle, PCLIENT_ID ClientId,
                                NTSTATUS ContinueStatus);
NTSTATUS nt_svc_NtSetInformationDebugObject(HANDLE DebugObjectHandle,
                                            DEBUGOBJECTINFOCLASS DebugObjectInformationClass,
                                            PVOID DebugInformation,
                                            ULONG DebugInformationLength,
                                            PULONG ReturnLength);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTDEBUGOBJ_H */
