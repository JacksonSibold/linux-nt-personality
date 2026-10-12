# 0047: VAD is published before its mapping exists (arch M-11)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` M-11 (Medium).
Date: 2026-10-12

## Context

`arch/x86/nt/mm.c:919-922` (reserve) and `:961-969` (commit only) insert the VAD under `ntmm_lock`, release the lock, and only then create the mapping. In between, another thread can run protect, query or free on a range that is not yet mapped.

## Options

1. Hold the lock across the mapping. Tradeoff: record 0005 notes that `kshim_mmap_commit` may sleep and that holding `ntmm_lock` through it is not acceptable.
2. Mark the VAD as pending and reject protect, query and free on pending VADs. Tradeoff: a new VAD state and new error paths.

## Recommendation (proposal)

Option 2, because option 1 conflicts with the sleep in `kshim_mmap_commit` recorded in 0005.

## Patch sketch

- Add a `pending` state to the VAD; set it under the lock before release.
- After the mapping succeeds, clear it under the lock; on failure, remove the VAD.
- Protect, query, and free return a pending-state status for those VADs.

## Test plan

- Race test: a second thread queries a range during the window; it sees the pending state, not a mapped region. Needs a kernel test (VM).

## Not verified

Not built or run. Race behaviour was not reproduced.

## Owner question

May the VAD lock be released during the mapping with a pending VAD state, or must the lock be held across the mapping?
