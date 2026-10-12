# 0043: Thread object is leaked when there is no ethread (arch D-5)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` D-5 (Low).
Date: 2026-10-12

## Context

`arch/x86/nt/dispatch.c:275-293` (`nt_enter_personality`): if `nt_current_ethread()` returns NULL, the new `nt_thread` `t` is made current at `:289` but never stored, so it leaks on exit. A failed `nt_thread_create` is ignored.

## Options

1. Store `t` in a per-thread slot that the exit path frees, and log a failed create. Tradeoff: a field in the thread state.
2. Free `t` as soon as it is not needed, and fail the enter call when creation fails. Tradeoff: the thread then runs without a personality object.

## Recommendation (proposal)

Option 1, so the exit path has one owner for the object.

## Patch sketch

- `dispatch.c:275-293`: store `t` where the exit path can reach it, and check the return of `nt_thread_create`.

## Test plan

- Enter and exit the personality with no ethread present; the object count returns to its starting value.

## Not verified

Not built or run. The free path is not in this directory (the review's note).

## Owner question

Who owns an `nt_thread` created when no ethread exists, and which object frees it on exit?
