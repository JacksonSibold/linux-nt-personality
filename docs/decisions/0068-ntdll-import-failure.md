# 0068: Import failures and thunk overflow do not stop process start (ntdll MAIN-3, MAIN-6)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` MAIN-3 (Medium) and MAIN-6 (Low), both needs owner.
Date: 2026-10-12

## Context

- MAIN-3: `src/ntdll/ntdll_main.c:661` (nested dependency imports) stops counting at 257 entries; `:700` (executable imports) stops at 513. The patch loops then use the capped count, so later IAT slots keep their ILT values and the first call through one jumps to a bogus address. `LdrInitializeThunk` is void, so "fail the load" needs a termination behaviour first.
- MAIN-6: `ntdll_main.c:740` only logs `Ldr: proc not found`. The slot keeps its ILT value and the load continues.

## Options

1. Count to the terminating zero thunk, and fail process start with a status when the table is larger than the buffer or an import is missing.
2. Keep logging and continuing. Tradeoff: calls through unpatched slots jump to an arbitrary address.

## Recommendation (proposal)

Option 1. The termination behaviour is needed by both findings, so decide it once.

## Patch sketch

- `ntdll_main.c:661`, `:700`: loop to the zero thunk, bounded by the buffer; fail on overflow.
- `ntdll_main.c:740`: fail the start with a status and stop the load.
- `LdrInitializeThunk`: the chosen termination path (owner's choice).

## Test plan

- Host test: an import table with 514 entries fails the load.
- Host test: a missing import fails the start with the chosen status.

## Not verified

Not built or run. The loader code that does the patching is excluded from the MinGW build (LDR-1, record 0029).

## Owner question

When an import cannot be resolved or the thunk table is too large, should process start terminate with a status, or continue?
