# 0070: File mappings return a fake handle (ntdll K32-12)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` K32-12 (Medium, needs owner).
Date: 2026-10-12

## Context

`src/ntdll/kernel32.c:440-448`: `CreateFileMapping` returns the handle 1 and reports success. `MapViewOfFile` fails with `ERROR_OUTOFMEMORY`. A real mapping needs a section lifetime design (handles and view unmapping), which is records 0001 and 0004 in the kernel tree.

## Options

1. Implement with `NtCreateSection` and `NtMapViewOfSection` after the section lifetime design. Tradeoff: depends on records 0001 and 0004.
2. Fail `CreateFileMapping` with `ERROR_CALL_NOT_IMPLEMENTED` until then. Tradeoff: programs that map files fail visibly.

## Recommendation (proposal)

Option 2 now, and option 1 after records 0001 and 0004 are decided.

## Patch sketch

- `kernel32.c:440-448`: return `NULL` with `ERROR_CALL_NOT_IMPLEMENTED`.

## Test plan

- `CreateFileMapping` returns NULL with the chosen error; no handle 1 is returned.

## Not verified

Not built or run. MinGW-only file.

## Owner question

Should `CreateFileMapping` wait for the section lifetime design in records 0001 and 0004, or fail now?
