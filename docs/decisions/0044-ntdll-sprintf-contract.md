# 0044: sprintf and vsprintf write without a size (ntdll CRT-1)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` CRT-1 (High, needs owner).
Date: 2026-10-12

## Context

`src/ntdll/msvcrt.c:273-279` formats `sprintf` and `vsprintf` into the caller's buffer with a size of 8192 (`my_vsprintf(s, 8192, ...)`), whatever the buffer size really is. A caller with a smaller buffer overflows it. The 8192 limit truncates output; it does not guard the buffer. The defect is in the export, so the fix is an API choice.

## Options

1. Remove `sprintf` and `vsprintf` from `msvcrt.def` and `msvcrt.c`. Tradeoff: a program that calls them fails to link.
2. Keep them and require callers to use `snprintf`. Tradeoff: an unsafe export stays available.

## Recommendation (proposal)

Option 1. The export cannot be made safe without a buffer size it does not receive.

## Patch sketch

- Remove the two entries from `msvcrt.def` and the two functions from `msvcrt.c`.
- Confirm `snprintf` truncates and returns the required length.

## Test plan

- Link check: `msvcrt.def` does not export `sprintf` or `vsprintf`.
- Host test of `snprintf` is not possible for `msvcrt.c` (MinGW-only); check by build.

## Not verified

Not built or run.

## Owner question

Should `sprintf` and `vsprintf` be removed from the CRT export, so that only size-bounded formatting remains?
