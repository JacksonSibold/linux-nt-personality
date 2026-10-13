# Module: proc_thread (Process and thread services)

Sources: `src/kernel/nt/proc_thread.c`, `src/kernel/nt/proc_thread.h`
Related: `object.md` (process and thread objects), `handle.md` (creation returns handles), `mm.md` (address space and TEB or stack setup), `sync.md` (thread alert and wait state), `iocp.md` (APC queueing), `dispatch.md` (the service table).

## Purpose

Implements NT process and thread creation, suspend and resume counts, termination, open-by-id, information-class queries, and thread context access. The file has two complete compilation branches: a userspace shim (`#ifndef __KERNEL__`, pthreads, approximately lines 83-931) and a kernel branch (`#else`, approximately lines 932-1916) that is meant to use `kernel_clone` and the x86 GS base. The header comment states that the shim and kernel branches differ, and the design notes describe the intended kernel behaviour.

## Public API (proc_thread.h)

Types:

- `typedef LONG KPRIORITY` (defined here if `nt_types.h` lacks it).
- `PROCESS_BASIC_INFORMATION`, `THREAD_BASIC_INFORMATION`, `KERNEL_USER_TIMES` structures.

Service entry points (native-style signatures):

- `nt_svc_NtCreateProcessEx(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ParentProcess, ULONG Flags, HANDLE SectionHandle, HANDLE DebugPort, HANDLE ExceptionPort, PVOID JobMemberLevel)` - create a process object.
- `nt_svc_NtCreateProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ParentProcess, BOOLEAN InheritObjectTable, HANDLE SectionHandle, HANDLE DebugPort, HANDLE ExceptionPort)` - older form; in the shim it forwards to `NtCreateProcessEx`.
- `nt_svc_NtCreateUserProcess(PHANDLE ProcessHandle, PHANDLE ThreadHandle, ...)` - create a process and its initial thread together.
- `nt_svc_NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, ...)` and `nt_svc_NtCreateThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, ...)`.
- `nt_svc_NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)` and `nt_svc_NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)`.
- `nt_svc_NtSuspendProcess(HANDLE ProcessHandle)` and `nt_svc_NtResumeProcess(HANDLE ProcessHandle)`.
- `nt_svc_NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus)` and `nt_svc_NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus)`.
- `nt_svc_NtOpenProcess(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, ...)` and `nt_svc_NtOpenThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, ...)`.
- `nt_svc_NtQueryInformationProcess(HANDLE ProcessHandle, ULONG ProcessInformationClass, ...)` and `nt_svc_NtQueryInformationThread(HANDLE ThreadHandle, ULONG ThreadInformationClass, ...)`.
- `nt_svc_NtSetInformationProcess(HANDLE ProcessHandle, ULONG Class, PVOID Info, ULONG Length)` and `nt_svc_NtSetInformationThread(HANDLE ThreadHandle, ULONG Class, PVOID Info, ULONG Length)`.
- `nt_svc_NtGetContextThread(HANDLE ThreadHandle, PCONTEXT Context)` and `nt_svc_NtSetContextThread(HANDLE ThreadHandle, PCONTEXT Context)`.
- `nt_svc_NtQuerySystemInformation` and `nt_svc_NtQuerySystemInformationEx`, `nt_svc_NtSetSystemInformation` are defined in this file.

Kernel helper:

- `void nt_thread_check_suspend(void)` - call on return to user mode. It blocks while the thread's suspend count is non-zero (`proc_thread.c:263` for the shim and a second definition at `proc_thread.c:982` for the kernel branch).

## Key data structures

The thread and process objects themselves are declared in `object.h` (`struct nt_eprocess`, `struct nt_ethread`). Relevant fields: `suspend_count`, `terminated`, `exit_status`, `teb`, `stack_base`, `stack_limit`, `process`, `tid`, `pid`, and locks and wait queues (`pthread_cond_t` or `wait_queue_head_t`).

## Behaviour (as designed and as implemented)

Suspend count:

- Each thread has an integer suspend count. `NtSuspendThread` returns the previous count and increments it. `NtResumeThread` decrements it if positive and returns the previous count. The comment cites golden values: suspend reports 0 then 1, resume reports 2 then 1.
- Kernel design (comment at the top of the file): `atomic_inc_return` on `suspend_count`, set `TIF_NTSUSPEND` on the target, and block on return to user through `nt_thread_check_suspend`. This keeps the check off the context-switch path.
- Shim design: `suspend_count` is an `atomic_int` under `thr->lock`, with a `suspend_cond` gate. There is no TIF flag. The check runs explicitly in the trampoline and after a self-suspend.
- Termination checks: `NtSuspendThread` and `NtResumeThread` check `terminated` under the lock and return `STATUS_THREAD_IS_TERMINATING` if set.

Process creation:

- The design note (top of file) says NT creates an empty process and then populates it, unlike Linux `execve`. The shim emulates this with an `eprocess` that has an empty handle table and no committed VADs. It does not `fork`, so there is no separate OS pid and all threads share one address space.
- The kernel design plans `kernel_clone(CLONE_NEWPID|VM)` with an emptied mm (`proc_thread.c:49-50`). See the gaps below: this is not implemented in the code.

Thread start:

- A 48-byte synthetic user stub is copied into a page. It performs `NtTestAlert` (0x1c8), calls the start routine, then calls `NtTerminateThread` (`proc_thread.c:1063-1101`). The design note compares this with using `RtlUserThreadStart` from ntdll and keeps the stub. The `BUILD_BUG_ON` the comment mentions ensures the stub length is 48 bytes.
- TEB: populated with `NtTib.Self`, stack bounds, the PEB pointer, client id, last error and TLS slots. The design note says static TLS from a PE `.tls` section is copied by `ldr_copy_static_tls()`. In the shim, the static TLS copy at `proc_thread.c:228` is a stub comment ("zero slots already"). GS base is set with `WRMSR` in the kernel and with `arch_prctl(ARCH_SET_GS, ...)` in the shim.

## Invariants and locking (as implemented)

- Shim: thread state is protected by `thr->lock` (`pthread_mutex_t`), with `suspend_cond` as the gate. `pthread_cond_broadcast` is used at about 9 sites.
- Kernel: per-thread `spinlock_t lock` plus `wait_queue_head_t` objects (`suspend_wq`, `exit_wq`), with `wake_up` at about 8 sites. The design notes say the blocking wait is done outside any lock.
- Lock ordering: the design note says the suspend path must not hold a lock the blocked path needs. A self-suspend sets the flag and returns, and the block happens on the return path.

## Known gaps (evident in code)

- Kernel build, process creation: `nt_svc_NtCreateProcessEx`, `nt_svc_NtCreateProcess` and `nt_svc_NtCreateUserProcess` are one-line stubs in the kernel branch that return `STATUS_NOT_IMPLEMENTED` (`proc_thread.c:1379-1381`). The `kernel_clone` plan in the design note is therefore not in the code I read. Only the shim has a process-creation implementation (`nt_create_process_internal`, `proc_thread.c:346`, used at lines 418 and 441).
- Kernel build, context access: `nt_svc_NtGetContextThread` and `nt_svc_NtSetContextThread` return `STATUS_NOT_IMPLEMENTED` in both branches (`proc_thread.c:923-929` shim, `1933-1934` kernel).
- Set information: `nt_svc_NtSetInformationThread` implements `ThreadPriority` (2), `ThreadBasePriority` (3) and `ThreadAffinityMask` (4) with the rules below. `nt_svc_NtSetInformationProcess` still returns `STATUS_INVALID_INFO_CLASS` for every class in the shim branch, so process-side set operations always fail. A process affinity set needs a system affinity mask, which the object layer does not record.
- Thread scheduling values: `ThreadBasicInformation` reports `Priority` and `BasePriority` as the process base priority plus the relative priority, clamped to 0..31 (no dynamic boost is modelled). `AffinityMask` is the thread mask, or the process mask when the thread has none. `ThreadPriority`, `ThreadBasePriority` and `ThreadAffinityMask` are set-only: query returns `STATUS_INVALID_INFO_CLASS`.
- Alerts: `nt_svc_NtAlertThread`, `nt_svc_NtAlertThreadByThreadId` and `nt_svc_NtAlertResumeThread` set `thread->alerted`. `nt_svc_NtTestAlert` (iocp.c) consumes it and returns `STATUS_ALERTED` once. The alert state is the per-thread field in `object.h`; `sync.c` keeps a separate alert flag on its own `struct nt_thread`, which is not connected to this one.
- Process state: `NtTerminateProcess` returns `STATUS_PROCESS_IS_TERMINATING` for a second terminate, and `NtSuspendProcess` returns the same status once the process has exited. The first exit status is kept.
- Open by id: an unknown process or thread id returns `STATUS_INVALID_CID`. The access-policy gap (a zero `DesiredAccess` grants `0x1FFFFF`, and any caller can open any process) is still open; see `docs/decisions/0002-process-open-access-check.md`.
- `nt_svc_NtQueryInformationProcess` has real bodies in both branches (`proc_thread.c` shim and kernel). Several classes still return fixed zero values or are not implemented: `ProcessHandleCount` (20) returns 0, and `ProcessTimes` (4) returns zeros.
- The shim documents its limits in the design note: no copy-on-write isolation, no separate VAD, no fork pid, exit codes propagate through a condition variable rather than `waitpid`, and the image name is synthesised rather than taken from `execve`.
- The shim's note at `proc_thread.c:1024` says a helper is a documented gap in the shim, and the kernel calls the helper "if available". I did not trace that helper.
- The trampoline design is a synthetic RWX-style page, which the design note itself flags as a W^X concern. The alternative (using ntdll's `RtlUserThreadStart`) is noted as not yet exported by ntdll.
