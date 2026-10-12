# 0064: Win32 file operations report false success or always fail (ntdll K32-8)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` K32-8 (Medium, needs owner).
Date: 2026-10-12

## Context

`src/ntdll/kernel32.c:392-398` makes `DeleteFileA` and `DeleteFileW` always set `ERROR_FILE_NOT_FOUND` and return FALSE. `:399-402` make `CreateDirectoryA/W` and `RemoveDirectoryA/W` return TRUE without doing anything. The real implementation needs the Win32-to-NT path conversion first (K32-15 is only partly fixed).

## Options

1. Implement with `NtCreateFile`, `NtSetInformationFile` and `NtDeleteFile`. Tradeoff: needs the path conversion.
2. Return `ERROR_CALL_NOT_IMPLEMENTED` until the conversion exists. Tradeoff: callers that expect the operation fail visibly.

## Recommendation (proposal)

Option 2 now. It replaces false success with a visible failure, and option 1 follows the path-conversion work.

## Patch sketch

- `kernel32.c:392-402`: set `ERROR_CALL_NOT_IMPLEMENTED` and return FALSE in each of the six functions.

## Test plan

- Each of the six calls returns FALSE with `ERROR_CALL_NOT_IMPLEMENTED`.
- Later: create, delete and remove a directory under a test root.

## Not verified

Not built or run. MinGW-only file.

## Owner question

Should the Win32 file create, delete and directory calls be implemented now, or fail with `ERROR_CALL_NOT_IMPLEMENTED` until path conversion exists?
