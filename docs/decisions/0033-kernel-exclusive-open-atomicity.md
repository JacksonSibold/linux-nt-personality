# 0033: Exclusive-open check is not atomic with handle creation (O-2)

Status: Blocked. Needs an owner decision on lock order. Not implemented.
Finding: `docs/review/kernel-nt.md` O-2 (Low).
Date: 2026-10-12

## Context

`src/kernel/nt/object.c` `ObOpenObjectByName` (around `:638-669`) tests `obj->is_exclusive && atomic64_read(&obj->handle_count) > 0`, then sets `is_exclusive` for `OBJ_EXCLUSIVE` opens, then calls `ObCreateHandle`. The test, the flag write and the handle-count increment are not under one lock. Two opens can both see zero handles and both succeed.

## Options

1. Hold the object's own lock across the check and `ObCreateHandle`. Tradeoff: `ObCreateHandle` takes the process handle-table lock, so the object lock would nest above it. That lock order has not been decided.
2. Hold the parent directory lock across the check. Tradeoff: the same nesting problem, plus a directory lock taken on every open. The review already rejects this.
3. Claim the exclusive state with an atomic compare-and-swap before `ObCreateHandle`, and roll the claim back if handle creation fails. Tradeoff: needs a separate claim word, cleared on the last handle close. No new lock order.

## Recommendation

Option 3. It fixes the race without adding a lock order, so it can be done without waiting for the lock-order decision. If the owner prefers a lock, record the order first.

## Patch sketch

- Add `atomic_t excl_claim` to `struct nt_object_header` (zero = unclaimed).
- In `ObOpenObjectByName`, when `obj->is_exclusive` is set or `OBJ_EXCLUSIVE` is requested: `if (atomic_cmpxchg(&obj->excl_claim, 0, 1) != 0) return STATUS_SHARING_VIOLATION;`, then `ObCreateHandle`. On failure, `atomic_set(&obj->excl_claim, 0)`.
- Clear `excl_claim` in the last-handle close path, next to the existing `is_exclusive` handling.
- Keep the existing `handle_count` test as a fast path, so behaviour for a single open is unchanged.

## Test plan

- Host (`object_test`, pthreads): two threads race `ObOpenObjectByName` on one `OBJ_EXCLUSIVE` object, repeated 1000 times with a fresh object each time. Exactly one must succeed per round. Fails today on a race window; may need a barrier to make it reliable.
- Host: one open, close, reopen still succeeds (no leaked claim).
- Host: an open that fails inside `ObCreateHandle` leaves the claim clear.

## Not verified

Not built or run. The race test is timing-dependent and has not been written.
