# 0025: Process and thread open without access checks (arch P-1)

Status: Proposed. Arch source (`arch/x86/nt`). Recorded only in this batch, not implemented. Decide together with 0002.
Finding: `docs/review/arch-x86-nt.md` P-1 (High).
Date: 2026-10-12

## Context

`arch/x86/nt/proc_thread.c:1264` creates thread handles with `ObCreateHandle(cur_proc, thr->obj, 0x1FFFFF, 0, &h)`. The caller's `DesiredAccess` is ignored (`(void)DesiredAccess;` at `1208`). `NtOpenProcess` (`1589-1592`) and `NtOpenThread` (`1611-1614`) find any process or thread by ID and grant the requested rights verbatim. A zero mask gives full access. `NtTerminateProcess` (`1545-1572`) then ends any process. This is the arch counterpart of kernel-nt P-1 (0002).

## Options

1. Check the caller against the target's owner or a security descriptor. Map generic rights. Grant only what was requested. Tradeoff: needs an ownership or ACL model. End state.
2. Interim: allow only the caller's own process and threads; other targets get `STATUS_ACCESS_DENIED`. Tradeoff: no cross-process open at all. Small change.
3. Give every object a security descriptor. Tradeoff: object-model change.

## Recommendation

Option 2 now, option 1 as the end state. Use the same policy as 0002.

## Patch sketch

- `NtOpenProcess` and `NtOpenThread`: when the target is not the caller, return `STATUS_ACCESS_DENIED`.
- Remove the `DesiredAccess ? DesiredAccess : 0x1FFFFF` default. Grant `DesiredAccess`, or fail with `STATUS_INVALID_PARAMETER` when it is zero and no default is defined.
- Thread creation at `1264`: grant the access the caller asked for.

## Test plan

- Kernel test: process A opens process B by ID with `PROCESS_TERMINATE` and is refused (option 2).
- Kernel test: opening the caller's own process with `PROCESS_QUERY_INFORMATION` returns a handle without `PROCESS_TERMINATE`.
- Kernel test: `NtTerminateProcess` on a handle without the terminate right fails.
- Host: not applicable.

## Not verified

Not built or run.
