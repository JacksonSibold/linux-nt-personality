# 0059: Dispatch state is global and unsynchronised; lock primitive is not chosen (ntdll EXC-4, EXC-5, K32-14)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` EXC-4 and EXC-5 (Low), and K32-14 (Medium), all needs owner.
Date: 2026-10-12

## Context

- EXC-4: `src/ntdll/except.c:456` (`g_nested_depth`), `:41-42` (`g_veh_head`, `g_vch_head`) and the registration functions have no lock. `t_in_dispatch` is `__thread` only for non-MinGW builds, so the MinGW build shares one flag across threads.
- EXC-5: `except.c:68-80` frees a node in `RemoveVectoredExceptionHandler`, and the dispatch loop at `:471-472` reads `n->next` after the handler returns. A handler that removes itself is a use-after-free.
- K32-14: `kernel32.c:306-311` makes `EnterCriticalSection` and `LeaveCriticalSection` no-ops, and `TryEnterCriticalSection` always returns TRUE.

All three need one lock primitive (keyed event or futex), and EXC-4 also needs per-thread state.

## Options

1. Choose the primitive first (keyed event or futex) and use it for the handler lists and critical sections. Then move the dispatch state into per-thread storage (TEB), which depends on records 0008 and 0026.
2. For EXC-5 only: defer the free, or copy the list under the lock. Tradeoff: the copy costs time on every exception.

## Recommendation (proposal)

Option 1 with the primitive chosen first, because K32-14 is the most visible defect and needs it anyway. EXC-5 is fixed with the same lock.

## Patch sketch

- Add one lock wrapper in `src/ntdll`; use it in `except.c` and in the critical-section functions in `kernel32.c`.
- Move `g_nested_depth` and `t_in_dispatch` into per-thread storage once the TEB policy is set.

## Test plan

- Host test: two threads register and remove handlers under load; no crash.
- Host test: a handler removes itself during dispatch; no use-after-free under the sanitiser.
- Critical section: two threads increment a counter under the lock; the total is correct.

## Not verified

Not built or run. MinGW-only code is checked by syntax only.

## Owner question

Which lock primitive (keyed event or futex) should the shim use, and should dispatch state wait for the TEB decision in records 0008 and 0026?
