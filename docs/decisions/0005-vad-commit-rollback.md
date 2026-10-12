# 0005: VAD state rollback on failed commit or decommit (M-4)

Status: Proposed. Changes VAD update ordering under the MM lock. Not implemented.
Finding: `docs/review/kernel-nt.md` M-4 (Medium).
Date: 2026-10-12

## Context

Two paths mutate VAD state before the kernel mapping call, and neither can undo the mutation.

1. `NtAllocateVirtualMemory` commit (`src/kernel/nt/mm.c`, the commit branch that ends at `:1061-1075`). The loop at `:1040-1060` splits reserved VADs and sets `state = MEM_COMMIT` under `ntmm_lock`, then releases the lock, then calls `kshim_mmap_commit`. On failure the rollback block (`:1061-1075`) walks the VADs and does nothing: its body is an empty `if` with the comment "approximate; tests don't hit rollback path". The VADs stay `MEM_COMMIT` while no pages are mapped, so `NtQueryVirtualMemory` reports committed memory that faults on access.
2. `NtFreeVirtualMemory` with `MEM_DECOMMIT` (`mm.c:1205-1237`). The loop splits VADs and sets `MEM_RESERVE` while it walks. An error in the middle (`nt_vad_split` returning NULL at `:1219`, or the `STATUS_INVALID_PARAMETER` return at `:1227`) returns with earlier pieces already changed. There is no undo, and `kshim_mmap_commit(..., PROT_NONE)` at `:1237` is skipped on those paths, so the VAD and the real mapping disagree.

## Options

1. **Validate, then mutate (recommended).** Walk the range once under the lock with no mutation. Check every piece is in the expected state (`MEM_RESERVE` for commit, `MEM_COMMIT` for decommit), and count the splits needed. Allocate all split nodes up front (at most one per VAD touched plus one for each end). Only then mutate, which cannot fail. The kernel call happens after the mutation, and on its failure the undo is to restore the states from the preallocated record. Tradeoff: an extra walk and a preallocation array sized by the number of VADs in range.
2. **Record and restore.** Before mutating, record `(start, end, state, protect, guard)` for each piece in a small array. On error, restore them in reverse. Tradeoff: the array can fail to allocate; the rollback needs its own allocation, so the failure path can still fail.
3. **Perform the kernel call first, then the VAD update.** Tradeoff: the kernel map can be live while the VAD still says reserved, which is the reverse of the current bug and just as wrong for a concurrent query.
4. **Keep the lock across the kernel call.** Tradeoff: `kshim_mmap_commit` may sleep; holding `ntmm_lock` through it is not acceptable.

## Recommendation

Option 1. It makes the mutation infallible, so no rollback is needed for the VAD side, and the kernel call failure restores the states from the preallocated record.

## Patch sketch

- `src/kernel/nt/mm.c` commit path (`:1023-1075`): before the existing split loop, add a `count_pieces(mm, commit_start, commit_end)` helper that returns the number of VADs to split and checks their states. Allocate `struct nt_vad *spare[n]` (or a single array of nodes) under the lock; on allocation failure return `STATUS_NO_MEMORY` with nothing changed.
- The split loop then uses the preallocated nodes instead of `nt_vad_split` (add a `nt_vad_split_into(mm, cur, start, end, spare)` variant in the same file).
- Replace the empty rollback at `:1061-1075` with a restore from the saved `(start, end, state, protect, guard)` array taken during validation, then free the spares that were not used.
- `NtFreeVirtualMemory` (`:1205-1237`): same pattern. Validate the whole range first (this also removes the early return at `:1227` from the mutation phase).
- Fold in M-6 (`end = base + size` overflow, `mm.c:1205` and `:1268`) since both touch the same range computation.

## Test plan

- `src/kernel/nt/mm_test.c`: force `kshim_mmap_commit` to fail (test hook, or a stub that returns -1 for one call) and check that `NtQueryVirtualMemory` still reports `MEM_RESERVE` for the range afterwards.
- Decommit over a range that contains a non-committed VAD in the middle: expect `STATUS_INVALID_PARAMETER` and no VAD change (compare the VAD list before and after).
- Forced `nt_vad_split` failure via a fault-injection hook: the VAD list is unchanged.
- `tests/kernel-host` is not used.

## Open questions

- Whether a test-only fault-injection hook for `kshim_mmap_commit` and `nt_vad_split` is acceptable in the kernel build, or whether the test should be built only for `__KERNEL__ == 0`.
