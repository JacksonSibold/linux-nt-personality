# 0065: printf-family stubs and float formats fail silently (ntdll RT-3, CRT-5)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` RT-3 (Low) and CRT-5 (Medium), both needs owner.
Date: 2026-10-12

## Context

- RT-3: `src/ntdll/ntdll_runtime.c:126` (`snprintf`) writes an empty string and returns 0. `:193` (`printf`) prints nothing. Callers get no sign of failure.
- CRT-5: `src/ntdll/msvcrt.c:195` prints `0.0` for `%f`, `%e`, `%g` and their upper-case forms (the comment says "simplified").

Implementing the formats is a product choice, as the review notes.

## Options

1. Implement the printf family and float formatting. Tradeoff: the largest option, and the formats must match the CRT.
2. Fail at link time for the stubs, and refuse float conversions visibly (for example an error return). Tradeoff: programs that call these fail to link or fail at run time.
3. Keep the stubs and document them. Tradeoff: output is lost silently.

## Recommendation (proposal)

Option 2 now, so a caller cannot lose output without a signal. Option 1 when the owner wants these features.

## Patch sketch

- `ntdll_runtime.c:126`, `:193`: remove the stub definitions so the link fails, or return a failure value and set `errno`.
- `msvcrt.c:195`: return an error for float conversions instead of `0.0`.

## Test plan

- Link check: a test program that uses `printf` fails with the chosen behaviour.
- Host test: a float conversion returns the chosen error, not `0.0`.

## Not verified

Not built or run. `msvcrt.c` is MinGW-only.

## Owner question

Should the printf family and float formatting be implemented, or should the missing features fail visibly or at link time?
