# 0038: GetTempFileNameA uses a fixed path and an unbounded copy (ntdll K32-9)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` K32-9 (Medium, needs owner).
Date: 2026-10-12

## Context

`src/ntdll/kernel32.c:422-427` always writes `/tmp/tmpfile.tmp` with `strcpy`. The name is predictable (CWE-377), ignores the prefix and `uniq`, and the copy has no size limit. The API takes no buffer size, so the bound needs a MAX_PATH contract. Unique names need file creation, which is missing (K32-8).

## Options

1. Generate a unique name in `dir`, honour `uniq`, and bound the copy by MAX_PATH. Tradeoff: needs working file creation first.
2. Return `ERROR_CALL_NOT_IMPLEMENTED` until file creation exists. Tradeoff: callers that use temp files fail.

## Recommendation (proposal)

Option 2 now, option 1 after K32-8 and K32-15 are done.

## Patch sketch

- `kernel32.c:422-427`: return failure before any copy (option 2).
- Later: create the file with `NtCreateFile` and copy with a bound.

## Test plan

- Host test: no write past a MAX_PATH buffer.
- Host test: the function does not return a fixed name.

## Not verified

MinGW-only file; checked by syntax only.

## Owner question

Should the temp-file call assume a MAX_PATH buffer and create real files, or fail until file creation works?
