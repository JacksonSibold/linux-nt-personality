# Module: job (Job objects)

Sources: `src/kernel/nt/job.c`, `src/kernel/nt/job.h`
Hooks outside the module: `object.h` (the `job` field of `struct nt_eprocess`, the `nt_job_remove_process` prototype), `object.c` (`ObDestroyProcessObject` calls `nt_job_remove_process`, with a weak no-op fallback).
Related: `object.md` (object types and references), `handle.md` (`ObReferenceObjectByHandle`, handle close callbacks), `proc_thread.md` (process termination, `ObTerminateProcessThreads`).
Host test: `tests/kernel-host/job_test.c` (11 tests, run by `make check-quick`).

## Purpose

A job is an object that groups processes. Its services are `NtCreateJobObject`, `NtOpenJobObject`, `NtAssignProcessToJobObject`, `NtTerminateJobObject`, `NtQueryInformationJobObject` and `NtSetInformationJobObject` (SSNs 0x00b0, 0x0125, 0x008f, 0x01c7, 0x0150, 0x019f). Before this module they were `MISSING` in [SERVICES.md](../status/SERVICES.md).

The module implements the membership and termination model and the information classes it can back with real state. Most limits are stored, not enforced. The list is under Known gaps.

## Public API (job.h)

- `NtCreateJobObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES)`: an unnamed or named job. A NULL handle pointer is `STATUS_INVALID_PARAMETER`. The object is inserted with `ObInsertObject`, so the namespace rules of the object manager apply.
- `NtOpenJobObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES)`: opens a named job. A missing name is `STATUS_OBJECT_NAME_NOT_FOUND`.
- `NtAssignProcessToJobObject(HANDLE job, HANDLE process)`: needs `JOB_OBJECT_ASSIGN_PROCESS` on the job handle and `PROCESS_SET_QUOTA | PROCESS_TERMINATE` on the process handle.
- `NtTerminateJobObject(HANDLE job, NTSTATUS exit_status)`: needs `JOB_OBJECT_TERMINATE`. Terminates every live member with `exit_status`.
- `NtQueryInformationJobObject` and `NtSetInformationJobObject`: need `JOB_OBJECT_QUERY` and `JOB_OBJECT_SET_ATTRIBUTES` respectively.
- `void nt_job_remove_process(struct nt_eprocess *proc)`: removes a process from its job. Called first by `ObDestroyProcessObject`.
- `extern struct nt_object_type nt_job_type`: the `Job` object type.

Access masks: `JOB_OBJECT_ASSIGN_PROCESS` 0x1, `SET_ATTRIBUTES` 0x2, `QUERY` 0x4, `TERMINATE` 0x8, `SET_SECURITY_ATTRIBUTES` 0x10, `IMPERSONATE` 0x20, `ALL_ACCESS` 0x001F003F.

Information classes:

| Class | Query | Set | Notes |
|---|---|---|---|
| 1 `BasicAccountingInformation` | yes | no | `TotalProcesses`, `ActiveProcesses`, `TotalTerminatedProcesses`. Times and page faults are zero. |
| 2 `BasicLimitInformation` | yes | yes | Exact length required. Round-trips `LimitFlags`, `ActiveProcessLimit` and the rest. |
| 3 `BasicProcessIdList` | yes | no | Live members only. A buffer that cannot hold all of them returns `STATUS_MORE_ENTRIES` with the entries that fit. |
| 4 `BasicUIRestrictions` | no | no | Not supported: `STATUS_INVALID_INFO_CLASS`. |
| 9 `ExtendedLimitInformation` | yes | yes | Exact length required. Round-trips the basic limits and the memory limit fields. IO counters are zero. |

Unknown limit flag bits (outside `JOB_OBJECT_LIMIT_VALID_FLAGS`) are `STATUS_INVALID_PARAMETER`. A wrong length is `STATUS_INFO_LENGTH_MISMATCH`.

## Key data structures

- `struct nt_job` (job.c, the object body): `lock` (spinlock), `hdr` (the job's object header), `members` (list of `struct job_member`), `total_assigned`, `removed_terminated`, and `limits` (a `JOBOBJECT_EXTENDED_LIMIT_INFORMATION`).
- `struct job_member`: a list node and a `proc` pointer. Each member node holds one reference on the job object.
- `struct nt_eprocess::job`: the job a process belongs to, or NULL. Written only under the job lock.

## Membership and lifetime

- A process belongs to at most one job. Assigning a process that is already in a job (the same job or another) is `STATUS_ACCESS_DENIED`. Nested jobs are not supported.
- Assigning a process that has already exited is `STATUS_PROCESS_IS_TERMINATING`.
- Assigning through a job whose last handle has closed is `STATUS_INVALID_HANDLE`. The assign checks the handle count under the job lock, so it cannot add a member after the KILL_ON_JOB_CLOSE pass.
- Each member holds a reference on the job, so the job outlives its members. The job is freed when the last handle and the last member reference are gone.
- Membership ends in one of two ways. `nt_job_remove_process` runs at the top of `ObDestroyProcessObject`, before any teardown, so the job drops the process before its handles close. An exited member is also pruned (see below).

### Exit path

Ordinary process exit in this tree does not call `ObDestroyProcessObject`. Its only callers are the creation-failure path in `proc_thread.c` and a temporary process in `registry.c`. A process that exits normally therefore keeps its membership until the job is next queried, terminated, or its last handle closes. At each of those points `job_prune_exited` removes every exited member, clears its `proc->job`, and releases its reference. Between the exit and the next such call, the exited member is still on the list, but it is not counted in `ActiveProcesses` and not listed in the process-id list.

## Termination

`NtTerminateJobObject` and the kill-on-close path call `job_proc_terminate` for each member. It sets `exit_status` and `terminated` under the process lock, wakes waiters, and calls `ObTerminateProcessThreads`. This matches the target branch of `NtTerminateProcess`. It does not call that service, because a handle to the process is not needed here. A process already terminated is left alone.

`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` is enforced in the close callback (`job_close`). It runs after the handle count drops to zero, with the process table lock not held. Members are terminated with exit status 0.

## Locking

Lock order: job lock, then process lock, then the thread registry lock taken inside `ObTerminateProcessThreads`. Every path follows that order.

- Reference and dereference of the job object happen after the job lock is dropped, except where the caller holds its own reference (query, terminate, close). The object manager is never called under the job lock with the last reference in hand.
- Kernel memory for a membership node is allocated before the lock, because the allocation may sleep.

## Known gaps

- Limits stored but not enforced: working set, process and job time, active process count, process and job memory, affinity, priority class, scheduling class, die on unhandled exception, breakaway flags. `ActiveProcessLimit` is stored and not checked at assign time.
- Accounting: `TotalUserTime`, `TotalKernelTime`, the per-period times, `TotalPageFaultCount` and the IO counters are always zero.
- `BasicUIRestrictions` is not implemented.
- Nested jobs and `NtIsProcessInJob` are not implemented.
- `JOB_OBJECT_SET_SECURITY_ATTRIBUTES` and `JOB_OBJECT_IMPERSONATE` are defined but not checked.
- The kernel ABI wrappers (`__KERNEL__` branch at the end of `job.c`) are not compiled by the host build or by `tests/syntax/check.sh`, because both define `USERSPACE_KSHIM`. The kernel build has not been run. The information-class wrappers stage through a kernel buffer of at most 4096 bytes.
- A membership reference keeps the job alive until the process is destroyed or pruned. A process that is never destroyed and never pruned keeps its job.

## Tests

`tests/kernel-host/job_test.c` (run from `make check-quick`):

1. Create (NULL pointer refused, unnamed), 2. named create and open, 3. access masks on each service, 4. exclusive assignment, 5. accounting and the process-id list (including `STATUS_MORE_ENTRIES`), 6. basic and extended limit round trip and the refusal cases, 7. terminate, 8. kill on last handle close, 9. no kill without the flag, 10. process destroy removes membership, 11. an exited member is pruned on query. The test ends with a leak check (`kshim_alloc_count`).
