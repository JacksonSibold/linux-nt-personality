# 0050: Output parameters are written back when the service fails (arch D-8)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` D-8 (Low).
Date: 2026-10-12

## Context

`arch/x86/nt/dispatch.c:178-181`, `205-208` and `234-239` copy `BaseAddress` and `RegionSize` back to user space after the NT call returns an error. NT defines these only on success, so a caller may act on values that were not produced by the service.

## Options

1. Write the output parameters back only when the service succeeds. Tradeoff: a caller that relied on the current write-back sees unchanged values on failure.
2. Keep the write-back and document it. Tradeoff: the behaviour differs from NT.

## Recommendation (proposal)

Option 1, because it matches the NT contract.

## Patch sketch

- `dispatch.c:178-181`, `205-208`, `234-239`: move the copy-back under a status check.

## Test plan

- A failing `NtAllocateVirtualMemory` call leaves the caller's `BaseAddress` and `RegionSize` as they were.
- Host test if the dispatch shim can run on the host; otherwise syntax and review.

## Not verified

Not built or run.

## Owner question

Should output parameters be written back only when the service succeeds, as NT defines?
