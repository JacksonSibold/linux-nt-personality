# 0059: Exclusive-open check is not atomic with handle creation (kernel-nt O-2)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/kernel-nt.md` O-2 (Low, needs owner in the lows pass).
Date: 2026-10-12

## Context

`src/kernel/nt/object.c:638-653` (`ObOpenObjectByName`) tests `is_exclusive && handle_count > 0` and calls `ObCreateHandle` later, with no common lock. Two opens can both pass the test.

## Options

1. Do the check and the handle-count increment under one lock. The obvious lock, the parent directory lock, nests under the handle table lock, and the lock order is not decided.
2. Use an atomic compare-and-exchange on the handle count. Tradeoff: no new lock order, but the increment and the check must be one operation.

## Recommendation (proposal)

Option 2, because it avoids the undecided lock order.

## Patch sketch

- `object.c:638-653`: replace the test and increment with a compare-and-exchange that succeeds only when the count is zero and the exclusive flag is clear.

## Test plan

- Host test in `object_test`: two threads open the same exclusive object; exactly one succeeds.

## Not verified

Not built or run.

## Owner question

Which primitive should serialise the exclusive-open check, given that the lock order between the directory and handle-table locks is not decided?
