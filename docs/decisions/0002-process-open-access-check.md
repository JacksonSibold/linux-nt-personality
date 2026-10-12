# 0002: Access check and reference counting for NtOpenProcess / NtOpenThread (P-1)

Status: Proposed. The reference fix is mechanical; the access policy needs an owner decision. Not implemented.
Finding: `docs/review/kernel-nt.md` P-1 (High).
Date: 2026-10-12

## Context

`nt_svc_NtOpenProcess` (`src/kernel/nt/proc_thread.c:1579-1600`) discards `ObjectAttributes` (`:1579`), calls `ObFindProcessByPid(pid)` (`:1589`), and then calls `ObCreateHandle(proc, p->obj, DesiredAccess ? DesiredAccess : 0x1FFFFF, 0, &h)` (`:1592`). Three problems:

1. No caller check. Any process can open any other process by PID, and then terminate it (`NtTerminateProcess` on that handle reaches `ObTerminateProcessThreads`, `proc_thread.c:1790-1815`).
2. `DesiredAccess == 0` becomes full access `0x1FFFFF`.
3. `ObFindProcessByPid` (`object.c:897-906`) releases the registry lock before it returns. `p` is not referenced, so the process can be destroyed before `p->obj` is read at `:1592`. `NtOpenThread` (`:1610-1616`, via `ObFindThreadByTid`) has the same shape.

## Options

1. **Reference under the registry lock, plus an access policy (recommended).** `ObFindProcessByPid` and `ObFindThreadByTid` take a reference before they drop the lock. Policy: a process may always open itself and its own threads; opening another process or thread needs a privilege flag on the caller's `nt_eprocess` (default deny). Tradeoff: the privilege flag and its setters are new state, which is a policy decision.
2. **Owner-only policy.** Only the creating process may open a process. Tradeoff: breaks parent and debugger patterns that the tests may use; still needs the reference fix.
3. **Mask access only.** Grant only the requested bits and reject `0` with `STATUS_ACCESS_DENIED`, but allow cross-process open. Tradeoff: does not stop one process from terminating another, so it does not close P-1.
4. **Token-based check through `security.c`.** Not available: `security.c` has no call sites outside its tests (`docs/modules/security.md`).

## Recommendation

Option 1, in two steps. Step (a), the reference fix, has no policy content and can land first. Step (b), the privilege flag and its default, needs the owner's answer. Step (a) changes `object.c` reference counting, which is part of the object model, so it is not implemented in this batch.

## Patch sketch

- `src/kernel/nt/object.c:897-906` (`ObFindProcessByPid`) and `:938-947` (`ObFindThreadByTid`): before the unlock on the match path, take a reference using the same helper `ObReferenceObjectByHandle` uses. Callers release it with `ObDereferenceObject`.
- `src/kernel/nt/proc_thread.c:1589-1594`: after the lookup, `if (p != proc && !nt_eprocess_may_open(proc, p)) { ObDereferenceObject(p->obj); return STATUS_ACCESS_DENIED; }`. `nt_eprocess_may_open` is the new hook, initially `return proc == p || proc->priv_open_any;`, with `priv_open_any` a new field in `struct nt_eprocess` (`object.h`) that defaults to false.
- `:1592`: pass `DesiredAccess` through. Replace the `0x1FFFFF` default with the Windows default for `0` (`PROCESS_QUERY_LIMITED_INFORMATION`), or reject `0`. Pick one and document it.
- Same change for `NtOpenThread` at `:1610-1616`, with `nt_ethread_may_open`.

## Test plan

- `src/kernel/nt/proc_thread_test.c`: open self by PID (succeeds); open a second process by PID without the flag (`STATUS_ACCESS_DENIED`); open with the flag (succeeds); open with `DesiredAccess == 0` and check the granted access matches the chosen default.
- Reference test: look up a process, destroy it on another thread, and confirm the object is still valid until `ObDereferenceObject`.
- `tests/kernel-host` is not used.

## Open questions

- Which principal holds `priv_open_any`: a parent process, a debugger, or only the system? Needs the owner's decision before step (b).
