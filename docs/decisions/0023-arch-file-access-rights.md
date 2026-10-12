# 0023: No access rights enforced on file operations (arch IO-4)

Status: Proposed. Arch source (`arch/x86/nt`). Recorded only in this batch, not implemented.
Finding: `docs/review/arch-x86-nt.md` IO-4 (High).
Date: 2026-10-12

## Context

`arch/x86/nt/nt_io.c` calls `ntio_handle_lookup(FileHandle, 0)` with `required = 0` at `1890` (NtReadFile), `1962` (NtWriteFile), `2150` (NtQueryInformationFile), `2327` (NtSetInformationFile) and `2416` (NtFlushBuffersFile). The granted access is never checked, so a handle opened with only `SYNCHRONIZE` can be written. `ntio_set_disposition` (`2247-2257`) sets `delete_pending` without a `DELETE` check. Related to 0006 (the write half of kernel-nt IO-1).

## Options

1. Pass the right each operation needs to `ntio_handle_lookup`. Tradeoff: one line per call site. Small change.
2. Check rights only at open time and trust the handle afterward. Tradeoff: the current behavior.
3. Generic-rights mapping table for every class. Tradeoff: more code for the same result.

## Recommendation

Option 1. Use the minimal right per information class (query and set classes differ, so check the class, not one constant).

## Patch sketch

- `NtReadFile`: `FILE_READ_DATA`.
- `NtWriteFile`: `FILE_WRITE_DATA`, or `FILE_APPEND_DATA` for an append write.
- `NtQueryInformationFile`: `FILE_READ_ATTRIBUTES`.
- `NtSetInformationFile`: `FILE_WRITE_ATTRIBUTES`, and `DELETE` for the disposition class.
- `NtFlushBuffersFile`: `FILE_WRITE_DATA`.

## Test plan

- Kernel test: open with `FILE_READ_DATA` only, then `NtWriteFile` returns `STATUS_ACCESS_DENIED`.
- Kernel test: open with `SYNCHRONIZE` only, then each operation is denied with the matching status.
- Kernel test: a delete disposition without `DELETE` is denied, and the file survives.
- Host: not applicable.

## Not verified

Not built or run. The exact rights per class of `NtQueryInformationFile` and `NtSetInformationFile` should be checked against the NT documentation when implementing.
