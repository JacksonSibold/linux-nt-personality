# Module: proc_thread (Process and thread services)

Sources: `src/kernel/nt/proc_thread.c`, `src/kernel/nt/proc_thread.h`
Related: `object.md` (process and thread objects), `handle.md` (creation returns handles), `mm.md` (address space and TEB or stack setup), `sync.md` (thread alert and wait state), `iocp.md` (APC queueing), `dispatch.md` (the service table).

## Purpose

Implements NT process and thread creation, suspend and resume counts, termination, open-by-id, information-class queries, and thread context access. The file has a userspace shim (`#ifndef __KERNEL__`, pthreads) and kernel code that starts at the `#else /* __KERNEL__ */` on `proc_thread.c:950` and runs to the end of the file (line 1954). The kernel side is meant to use `kernel_clone` and the x86 GS base. The shim's service bodies are at `proc_thread.c:667-949` and the kernel's at `:1656-1954`. The header comment states that the shim and kernel branches differ, and the design notes describe the intended kernel behaviour.

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

- `void nt_thread_check_suspend(void)` - call on return to user mode. It blocks while the thread's suspend count is non-zero (`proc_thread.c:280` for the shim and a second definition at `proc_thread.c:1000` for the kernel branch).

Token slots (see `security.md`): a process object gets its primary token in `ObCreateProcessObject` (`object.c:1119`, token at `object.c:1175`). A thread object starts with a NULL impersonation token (`object.c:1214`). Both are released when the object is destroyed.

## Key data structures

The thread and process objects themselves are declared in `object.h` (`struct nt_eprocess`, `struct nt_ethread`). Relevant fields: `suspend_count`, `terminated`, `exit_status`, `teb`, `stack_base`, `stack_limit`, `process`, `tid`, `pid`, and locks and wait queues (`pthread_cond_t` or `wait_queue_head_t`).

## Behaviour (as designed and as implemented)

Suspend count:

- Each thread has an integer suspend count. `NtSuspendThread` returns the previous count and increments it. `NtResumeThread` decrements it if positive and returns the previous count. The comment cites golden values: suspend reports 0 then 1, resume reports 2 then 1.
- Kernel design (comment at the top of the file): `atomic_inc_return` on `suspend_count`, set `TIF_NTSUSPEND` on the target, and block on return to user through `nt_thread_check_suspend`. This keeps the check off the context-switch path.
- Shim design: `suspend_count` is an `atomic_int` under `thr->lock`, with a `suspend_cond` gate. There is no TIF flag. The check runs explicitly in the trampoline and after a self-suspend.
- Termination checks: `NtSuspendThread` and `NtResumeThread` check `terminated` under the lock and return `STATUS_THREAD_IS_TERMINATING` if set.

Process creation:

- The design note (top of file) says NT creates an empty process and then populates it, unlike Linux `execve`. The shim emulates this with an `eprocess` that has an empty handle table and no committed VADs (`nt_create_process_internal`, `proc_thread.c:363`, called from `proc_thread.c:435` and `:458`). It does not `fork`, so there is no separate OS pid and all threads share one address space.
- The kernel design plans `kernel_clone(CLONE_NEWPID|VM)` with an emptied mm (`proc_thread.c:49-50`). See the gaps below: this is not implemented in the code.

Thread start:

- A 48-byte synthetic user stub is copied into a page. It performs `NtTestAlert` (0x1c8), calls the start routine, then calls `NtTerminateThread` (trampoline comment at `proc_thread.c:1077-1090`, stub bytes from `:1105`). The design note compares this with using `RtlUserThreadStart` from ntdll and keeps the stub. A `BUILD_BUG_ON` at `proc_thread.c:1118` keeps the stub length at 48 bytes. `nt_get_ntdll_RtlUserThreadStart` (`:1132`) probes for an ntdll export, and the minimal export lookup is not written (`:1152`).
- TEB: populated with `NtTib.Self`, stack bounds, the PEB pointer, client id, last error and TLS slots. The design note says static TLS from a PE `.tls` section is copied by `ldr_copy_static_tls()`. In the shim, the static TLS copy at `proc_thread.c:248` is a stub comment ("zero slots already"). GS base is set with `WRMSR` in the kernel and with `arch_prctl(ARCH_SET_GS, ...)` in the shim (`proc_thread.c:244`).

## Invariants and locking (as implemented)

- Shim: thread state is protected by `thr->lock` (`pthread_mutex_t`), with `suspend_cond` as the gate. `pthread_cond_broadcast` is used at about 9 sites.
- Kernel: per-thread `spinlock_t lock` plus `wait_queue_head_t` objects (`suspend_wq`, `exit_wq`), with `wake_up` at about 8 sites. The design notes say the blocking wait is done outside any lock.
- Lock ordering: the design note says the suspend path must not hold a lock the blocked path needs. A self-suspend sets the flag and returns, and the block happens on the return path.

## Known gaps (evident in code)

- Kernel build, process creation: `nt_svc_NtCreateProcessEx`, `nt_svc_NtCreateProcess` and `nt_svc_NtCreateUserProcess` are one-line stubs in the kernel branch that return `STATUS_NOT_IMPLEMENTED` (`proc_thread.c:1397-1399`). The `kernel_clone` plan in the design note is therefore not in the code I read. Only the shim has a process-creation implementation.
- Kernel build, context access: `nt_svc_NtGetContextThread` and `nt_svc_NtSetContextThread` return `STATUS_NOT_IMPLEMENTED` in both branches (`proc_thread.c:939-947` shim, `1951-1952` kernel).
- Information classes: `nt_svc_NtSetInformationProcess` and `nt_svc_NtSetInformationThread` return `STATUS_INVALID_INFO_CLASS` for every class in the shim branch (`proc_thread.c:927-938`), so set operations always fail there. The kernel branch stubs at `proc_thread.c:1949-1950` also return `STATUS_INVALID_INFO_CLASS`. `nt_svc_NtQueryInformationProcess` has real bodies in both branches (`proc_thread.c:667` and `1656`). I did not enumerate which classes it answers.
- The inventory in `docs/status/SERVICES.md` marks `NtCreateProcessEx`, `NtCreateProcess`, `NtCreateUserProcess`, `NtSetInformationProcess` and `NtSetInformationThread` as IMPLEMENTED. The tool classifies each service by its first strong definition (the shim here), and it counts any body that is not a bare `STATUS_NOT_IMPLEMENTED` return as implemented. The kernel stubs above and the shim's always-failing set calls are therefore not visible in the counts.
- The shim documents its limits in the design note: no copy-on-write isolation, no separate VAD, no fork pid, exit codes propagate through a condition variable rather than `waitpid`, and the image name is synthesised rather than taken from `execve`.
- The shim's note at `proc_thread.c:1042` says it documents this as a gap, and the kernel calls the helper "if available". I did not trace that helper.
- The trampoline design is a synthetic RWX-style page, which the design note itself flags as a W^X concern (`proc_thread.c:1080-1090`). The alternative (using ntdll's `RtlUserThreadStart`) is noted as not yet exported by ntdll.
