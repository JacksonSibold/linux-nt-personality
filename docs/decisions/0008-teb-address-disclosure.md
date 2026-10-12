# 0008: TEB address returned by NtQueryInformationThread (PT-1)

Status: Proposed. The correct value depends on how the TEB is mapped, which is not settled in the tree. Not implemented.
Finding: `docs/review/kernel-nt.md` PT-1 (Medium).
Date: 2026-10-12

## Context

In the kernel build the TEB is allocated with `kzalloc(2 * PAGE_SIZE)` (`src/kernel/nt/proc_thread.c:1005-1010`), and `thr->teb_base` is set to that kernel address. `NtQueryInformationThread` returns `target->teb_base` as `TebBaseAddress` (`proc_thread.c:823`, and the second copy at `:1837`). Any thread can be queried (see P-1), so this discloses a kernel heap address.

The same value is also used as the GS base for the new thread (`proc_thread.c:1323`, `nt_write_gsbase(child, thr->teb_base)`). That matters for the fix: if user code reads TEB fields through GS, then the kernel allocation is reachable from user mode through that base, which means the TEB is either mapped into user space by some path not visible here, or the GS-based access is not working. The review did not establish which.

## Options

1. **Return the user-space TEB address (recommended).** Keep the user mapping address in `thr` (for example `thr->teb_user`) at the point where the TEB is mapped for the thread, and return that. Tradeoff: needs the mapping path to be read first, which the review did not do (`proc_thread.c:932-1200` was not read). Also depends on the GS question above.
2. **Restrict the query.** Return `teb_base` only when the caller is the target thread, otherwise `STATUS_ACCESS_DENIED`. Tradeoff: stops cross-thread disclosure but still returns a kernel address to the thread itself, so it is not a full fix.
3. **Return 0 for the kernel build.** Tradeoff: breaks any caller that reads the TEB through this query.

## Recommendation

Option 1, after the mapping path is read. Option 2 is the fallback if the user TEB address cannot be found in time; it closes the cross-thread leak and should be reported as partial.

## Patch sketch

- Read the TEB mapping in `proc_thread.c` (`:932-1200`, creation path) to find where a user address is established. Record it as `thr->teb_user`.
- `proc_thread.c:823` and `:1837`: `tbi->TebBaseAddress = (PVOID)(ULONG_PTR)target->teb_user;` (Option 1), or add the `target == cur` check before the assignment (Option 2).
- `struct nt_ethread` in `object.h` (around `:267`): add `ULONG_PTR teb_user;`, which is internal and not ABI.

## Test plan

- `src/kernel/nt/proc_thread_test.c`: `NtQueryInformationThread(ThreadBasicInformation)` on a second thread does not return a value in the kernel heap range (compare with the `kzalloc` range or a sentinel).
- Self query returns the same user address that `NtCurrentTeb` (or GS) reports.
- `tests/kernel-host` is not used.

## Open questions

- Where the TEB user mapping is established, and whether GS-based TEB access is live in the kernel build. This must be answered before the patch is written.
