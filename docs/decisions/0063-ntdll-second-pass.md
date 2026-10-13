# 0063: No second pass for exception unwinding (ntdll EXC-3)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` EXC-3 (Medium, needs owner). Shares the design with UNW-2 (High).
Date: 2026-10-12

## Context

`src/ntdll/except.c:543-550` documents that the unwind pass is not implemented, so `__finally` and cleanup code never runs for a dispatched exception. `RtlUnwindExWrapper` (`except.c:594`) is a partial implementation with no caller (EXC-6, fixed).

## Options

1. Implement the unwind pass once, shared with UNW-2. Tradeoff: the largest change in the exception code.
2. Document that termination handlers are not run, and keep the first pass only. Tradeoff: cleanup code is skipped.

## Recommendation (proposal)

Option 1, as one design with UNW-2, since both need the same pass. Option 2 as a stop-gap, stated in the user documentation.

## Patch sketch

- `except.c`: after a handler accepts, walk the frames and call each `__finally` in order; reuse `RtlUnwindExWrapper` as the entry.
- The unwind data reads depend on the bounds work in record 0057.

## Test plan

- Host test: a handler that is a cleanup block runs once when an exception is handled.
- Host test: nested cleanup blocks run in the right order.

## Not verified

Not built or run.

## Owner question

Should the second pass be implemented now (shared with UNW-2), or should termination handlers be documented as not run?
