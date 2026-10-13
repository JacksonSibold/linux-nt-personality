# Module: proc_thread (Process and thread services)

Sources: `src/kernel/nt/proc_thread.c`, `src/kernel/nt/proc_thread.h`. Mirror: `arch/x86/nt/proc_thread.c` (an older copy that differs; see [tree-divergence](../analysis/tree-divergence.md)).
Related: `object.md` (process and thread objects), `handle.md` (creation returns handles, inheritance), `mm.md` (address space and stack setup), `sync.md` (alert and wait state), `iocp.md` (`NtTestAlert` and APC queueing), `security.md` (process primary token), `dispatch.md` (service table), `process-creation.md` (plan for process creation).

## Purpose

Implements NT process and thread creation, suspend and resume counts, termination, open by id, information-class set and query, alerts, and thread context access. The file has three regions:

- the userspace shim, `#ifndef __KERNEL__` (pthreads), which runs to `proc_thread.c:993`;
- the kernel branch, `#else /* __KERNEL__ */` at `proc_thread.c:994` to the `#endif` at `:2034`;
- a common tail from `:2035` to the end, which holds the information-class services, the alert services and `pt_thread_post_alert`.

## Services and their definitions

Each service has a typed definition in the shim branch and a raw-argument definition (`unsigned long a0, ...`) in the kernel branch, which dispatch calls. Line numbers in the table are the shim (typed) and kernel (raw) definitions.

| Service | Shim (typed) | Kernel (raw) | Kernel build |
|---|---|---|---|
| `NtCreateProcessEx` | `:473` | `:1441` | Stub, `STATUS_NOT_IMPLEMENTED` |
| `NtCreateProcess` | `:482` (forwards to `NtCreateProcessEx`) | `:1442` | Stub, `STATUS_NOT_IMPLEMENTED` |
| `NtCreateUserProcess` | `:491` | `:1454` | Validates the arguments (`proc_create_args.h`), then `STATUS_NOT_IMPLEMENTED` |
| `NtCreateThreadEx` | `:447` | `:1255` | Implemented over `kernel_clone` (`:1265`); not built on this host |
| `NtCreateThread` | `:462` | `:1427` | Forwards to `NtCreateThreadEx` |
| `NtSuspendThread`, `NtResumeThread` | `:518`, `:547` | `:1470`, `:1512` | Both builds |
| `NtSuspendProcess`, `NtResumeProcess` | `:572`, `:589` | `:1556`, `:1575` | Both builds |
| `NtTerminateThread`, `NtTerminateProcess` | `:603`, `:645` | `:1593`, `:1640` | Both builds |
| `NtOpenProcess`, `NtOpenThread` | `:678`, `:695` | `:1673`, `:1698` | Both builds |
| `NtQueryInformationProcess` | `:716` | `:1726` | Both builds |
| `NtQueryInformationThread` | `:868` | `:1904` | Both builds |
| `NtQuerySystemInformation(Ex)`, `NtSetSystemInformation` | `:949`, `:972`, `:977` | `:1991`, `:2020`, `:2025` | Both builds |
| `NtGetContextThread`, `NtSetContextThread` | `:983`, `:988` | `:2031`, `:2032` | `STATUS_NOT_IMPLEMENTED` in both |
| `NtSetInformationProcess` | `:2173` (common tail, one definition) | | Both builds |
| `NtSetInformationThread` | `:2260` (common tail, one definition) | | Both builds |
| `NtAlertThread`, `NtAlertThreadByThreadId`, `NtAlertResumeThread` | `:2310`, `:2320`, `:2329` (common tail) | | Both builds |

The `NtTestAlert` service is in `iocp.c:767`. Token open services are in `security.c`.

`docs/status/SERVICES.md` lists `NtCreateProcessEx` and `NtCreateProcess` as IMPLEMENTED (userspace) and STUB-NOT-IMPLEMENTED (kernel), and `NtCreateUserProcess` as IMPLEMENTED in both builds. The last label is the inventory's reading of a body that does copy-in and then refuses; it does not create a process in the kernel build.

## Public API (proc_thread.h)

- `typedef LONG KPRIORITY` (defined here if `nt_types.h` lacks it), and the `PROCESS_BASIC_INFORMATION`, `THREAD_BASIC_INFORMATION` and `KERNEL_USER_TIMES` structures.
- The `nt_svc_*` prototypes listed above.
- `void nt_thread_check_suspend(void)` - blocks while the thread's suspend count is non-zero. Shim definition at `proc_thread.c:324`, kernel definition at `:1044`.

## Behaviour

### Suspend count

- Each thread has an integer suspend count. `NtSuspendThread` returns the previous count and increments it. `NtResumeThread` decrements it if positive and returns the previous count. The code cites golden values: suspend reports 0 then 1, and resume reports 2 then 1.
- The kernel design sets `TIF_NTSUSPEND` on the target and blocks on return to user through `nt_thread_check_suspend`. This keeps the check off the context-switch path.
- The shim keeps `suspend_count` as an atomic under `thr->lock`, with a condition-variable gate. It has no TIF flag. The check runs explicitly in the trampoline and after a self-suspend.
- Both suspend and resume check `terminated` under the lock and return `STATUS_THREAD_IS_TERMINATING` if it is set.

### Process creation

- The shim (`nt_create_process_internal`, `proc_thread.c:407`) creates an `eprocess` with an empty handle table and no committed VADs. When `OBJ_INHERIT` is requested it calls `ObInheritHandles` (`proc_thread.c:426`), and it ignores the status. The shim does not `fork`, so there is no separate OS pid and all threads share one address space.
- The shim's `NtCreateUserProcess` (`:491`) creates a process, then a thread at the dummy address `0x140001000`. If an attribute list is given, its pointer is used as the start address. This is a test hack, marked as such in the code.
- The kernel build does not create processes. `NtCreateUserProcess` copies in and checks `PS_CREATE_INFO` and `PS_ATTRIBUTE_LIST` ([proc_create_args](proc_create_args.md)) and then refuses. `ProcessParameters` is not read ([process_params](process_params.md)).
- The kernel `NtCreateThreadEx` (`:1255`) uses `kernel_clone` (`:1265`) and points the child at the trampoline. Its behaviour has not been run.

### Thread start

- A 48-byte synthetic user stub is copied into a page (`BUILD_BUG_ON` at `proc_thread.c:1162` keeps the length). It calls `NtTestAlert` (syscall 0x1c8), calls the start routine, and then calls `NtTerminateThread`. The stub is built at `:1121` to `:1165`.
- `nt_get_ntdll_RtlUserThreadStart` (`:1176`) is a probe for an ntdll export. The export lookup it would need is not written (`:1196`), so the synthetic stub is the path used.
- TEB: `NtTib.Self`, stack bounds, PEB pointer, client id, last error and TLS slots are filled in. Static TLS from a PE `.tls` section is not copied; the shim's static-TLS copy is a stub comment.
- The GS base is written with `WRMSR_GS_BASE` in the kernel build (the write happens on context switch) and with `arch_prctl(ARCH_SET_GS, ...)` in the shim (`proc_thread.c:288`).

### Set and query information

`NtSetInformationProcess` (`proc_thread.c:2173`) implements these classes, all needing `PROCESS_SET_INFORMATION`. Length and buffer are checked before the handle is opened.

| Class | Rule |
|---|---|
| `ProcessBasePriority` (5) | `LONG`, clamped to 0..31. Set-only. |
| `ProcessPriorityClass` (18) | 2 bytes (Foreground, PriorityClass). Classes 1 to 6 valid; others `STATUS_INVALID_PARAMETER`. REALTIME needs `SeIncreaseBasePriorityPrivilege`. |
| `ProcessDefaultHardErrorMode` (12) | `ULONG`, stored as given. |
| `ProcessAffinityMask` (21) | `ULONG_PTR`, non-zero and a subset of the system mask. |
| `ProcessBreakOnTermination` (29) | `ULONG` as a flag. Needs `SeDebugPrivilege`. |
| `ProcessHandleTracing` (32) | `STATUS_NOT_SUPPORTED`. |

Every other class, including `ProcessForegroundInformation` (25) and `ProcessWx86Information` (19), returns `STATUS_INVALID_INFO_CLASS`.

`NtSetInformationThread` (`:2260`) implements `ThreadPriority` (2) and `ThreadBasePriority` (3), a `LONG` in -15..15, and `ThreadAffinityMask` (4), a non-empty subset of the process mask. User buffers are read with `copy_from_user` after `access_ok`. Terminated threads return `STATUS_THREAD_IS_TERMINATING`.

### Alerts

- One alertable flag. `pt_thread_post_alert` (`proc_thread.c:2073`) sets the alert on the thread's sync record (`nt_thread_alert(thr->sync_thread)`). The sync record is the flag that alertable waits test (`sync.c`, `nt_obj.c:2109`) and that `NtTestAlert` (`iocp.c:767`) consumes. A thread with no sync record gets `STATUS_NO_MEMORY`. A terminated thread gets `STATUS_THREAD_IS_TERMINATING`.
- `NtAlertThread` posts the alertable flag (unknown handle: `STATUS_INVALID_HANDLE`). `NtAlertResumeThread` resumes as `NtResumeThread` does, then posts it.
- `NtAlertThreadByThreadId` (unknown id: `STATUS_INVALID_CID`) sets the by-thread-id token instead (`nt_thread_tid_alert`). It does not set the alertable flag. A terminated thread gets `STATUS_THREAD_IS_TERMINATING`, and a missing sync record `STATUS_NO_MEMORY`, as for the flag.
- `NtWaitForAlertByThreadId(Address, Timeout)` waits for that token. It returns `STATUS_ALERTED` when it consumes the token and `STATUS_TIMEOUT` when the timeout expires. `Address` is unused. The wait is not alertable: `NtAlertThread` does not end it.

### Continue and raise

- `NtContinue(CONTEXT*, TestAlert)` and `NtContinueEx(CONTEXT*, KCONTINUE_ARGUMENT*)` validate the CONTEXT: NULL or unreadable is `STATUS_ACCESS_VIOLATION`, a pointer not 16-byte aligned is `STATUS_DATATYPE_MISALIGNMENT`, and ContextFlags must carry `CONTEXT_AMD64` with no bit outside 0xFF. The shim copies the context into `nt_ethread.pending_continue` (`object.h`) and returns `STATUS_SUCCESS`. TestAlert, or bit 0 of `ContinueFlags` for `NtContinueEx`, first consumes a pending alert: then the call returns `STATUS_ALERTED` and nothing is recorded. Nothing consumes the recorded context, because the shim has no CPU state to restore. The kernel build validates and returns `STATUS_NOT_IMPLEMENTED`.
- `NtContinueEx` reads only `ContinueFlags`, the first `USHORT` of the argument. The rest of that layout is not confirmed here.
- `NtRaiseException(EXCEPTION_RECORD*, CONTEXT*, FirstChance)` validates both arguments and returns `STATUS_NOT_IMPLEMENTED`. The only dispatch in the tree runs in user mode: `_NtRaiseException` in `src/ntdll/except.c` calls `RtlDispatchException` in-process and `UnhandledExceptionFilter` when nothing handles the exception. The kernel has no path into it.
- The kernel delay path honours an alert only when the wait is alertable. The kernel wait paths are not run on the host.

## Invariants and locking (as implemented)

- Shim: thread state is protected by `thr->lock` (`pthread_mutex_t`), with a condition variable as the suspend gate.
- Kernel: per-thread `spinlock_t` plus wait queues. The blocking wait is done outside any lock.
- Lock ordering (design note): a suspend path must not hold a lock the blocked path needs. A self-suspend sets the state and returns; the block happens on the return path.
- The copy-out of `PreviousSuspendCount` in `arch/x86/nt/proc_thread.c` happens after the count changes. A fault there returns `STATUS_ACCESS_VIOLATION` with the suspend or resume already applied. A probe before the change removes the fault in the normal case; the residual race is accepted with reason (audit pass 1 item 38b1b0a).

## Known gaps (evident in code)

- **Kernel process creation is not implemented.** `NtCreateProcessEx`, `NtCreateProcess` and the process part of `NtCreateUserProcess` return `STATUS_NOT_IMPLEMENTED` in the kernel build. Only the shim creates processes ([process-creation](../architecture/process-creation.md), milestones M2 and M7).
- `NtGetContextThread` and `NtSetContextThread` return `STATUS_NOT_IMPLEMENTED` in both builds.
- `arch/x86/nt/proc_thread.c` (`:910`-`:921`) still returns `STATUS_INVALID_INFO_CLASS` from `NtSetInformationProcess` and `NtSetInformationThread`. The class implementation above is in `src/kernel/nt/proc_thread.c` only, so the two copies differ.
- `NtCreateUserProcess` in the shim uses a dummy start address (`0x140001000`) and an attribute-list pointer as the entry point. Milestone M7 removes it.
- Any caller can open any process or thread by id, with a zero `DesiredAccess` granting the full set. There is no access check (decision record 0002, open; arch P-1).
- The kernel build of `src/kernel/nt/proc_thread.c` copies `PreviousSuspendCount` to user memory while `thr->lock` is held, in `NtSuspendThread` and `NtResumeThread` (`copy_to_user` at `:1500` and `:1541`). The arch copy no longer does this (audit pass 1, P-9 pattern).
- The trampoline page is a synthetic RWX-style page, which the code comments flag as a W^X concern (`proc_thread.c:1125`-`:1134`). The alternative, ntdll's `RtlUserThreadStart`, is not yet exported.
- Shim limits, from the design note: no copy-on-write isolation, no separate VAD, no fork pid, exit codes passed through a condition variable rather than `waitpid`, and the image name is synthesised rather than taken from `execve`.
- Thread scheduling values in `NtQueryInformationThread` are the process base priority plus the relative priority, clamped to 0..31. No dynamic boost is modelled.
