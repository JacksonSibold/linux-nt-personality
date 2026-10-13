# 0062: UNWIND_INFO and chain reads are not bounds-checked (ntdll UNW-5)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` UNW-5 (Low, needs owner in the medium and low pass).
Date: 2026-10-12

## Context

`src/ntdll/unwind.c:187` (`build_chain`) and `:153` (`get_unwind_info`) read `UnwindInfoAddress`, `Flags` and `CountOfCodes` without a check against the module size. Only trusted images are unwound today.

## Options

1. Check each RVA and count against `sizeOfImage`, with a NULL path through `build_chain` and `get_unwind_info` for when the size is not known. Tradeoff: more code on every unwind.
2. Keep the trusted-image rule and document it. Tradeoff: an untrusted image can read out of bounds.

## Recommendation (proposal)

Option 1, using the bounds helper already added in `src/ntdll/pe_bounds.h`.

## Patch sketch

- `unwind.c:153`, `:187`: check the RVA and the code count with `pe_range_fits` before reading.

## Test plan

- Host test: a unwind table whose RVA is past the image is rejected, and a valid one still unwinds.

## Not verified

Not built or run.

## Owner question

Should unwind reads be bounds-checked now, given that only trusted images are unwound today?
