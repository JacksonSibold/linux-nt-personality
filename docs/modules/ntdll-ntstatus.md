# Module: ntdll-ntstatus (NTSTATUS constants and RtlNtStatusToDosError)

Sources: `src/ntdll/ntstatus.h`, `src/ntdll/ntstatus_to_dos.c`
Tests: `tests/ntdll-host/test_ntstatus_to_dos.c` (table invariants, golden values, loader symbols)
Related: `ntdll-loader.md` and `ntdll-startup.md` (return NTSTATUS values that this module maps), `ntdll-win32.md` (`GetLastError` through `SetLastErrorFromStatus`), `docs/modules/ntstatus_kernel.md` (the kernel-side copy of the same mapping), `docs/review/ntdll.md`.

## Purpose

`ntstatus.h` defines the `STATUS_*` constants used by the ntdll code. `ntstatus_to_dos.c` implements `RtlNtStatusToDosError`, which converts an NTSTATUS into the Win32 error code that `GetLastError` returns. The conversion is what makes a failed system call look like a Win32 failure to a program.

## Public API

- `ULONG RtlNtStatusToDosError(NTSTATUS Status)`: the only exported function in this module.
- `ntstatus.h`: 532 `#define STATUS_*` constants, each `((NTSTATUS)0xXXXXXXXXL)`.
- Inside `ntstatus_to_dos.c`: `ERROR_*` macros for every Win32 code the table uses, and the `NTSTATUS_TO_DOS` struct (`Status`, `DosError`).

## Table

- `NtStatusToDosErrorTable` has 520 entries. It is `static const`, so only the file can see it.
- The table is strictly ascending by NTSTATUS, with no duplicates. This is checked by `table_is_strictly_ascending` in the host test. The binary search depends on this.
- Rows use symbol names where a symbol has the right value, and literal values where the name is not established (for example `(NTSTATUS)0xC0000228L`, kept at 599 from the golden column).
- Two entries in the file are called out in comments because they are easy to get wrong: `STATUS_BUFFER_OVERFLOW` (0x80000005) maps to `ERROR_MORE_DATA` (234), and `STATUS_BUFFER_TOO_SMALL` (0xC0000023) maps to `ERROR_INSUFFICIENT_BUFFER` (122). The two must not be merged.

## Lookup

`RtlNtStatusToDosError` runs a binary search over the table. An exact match returns the Win32 code. A miss goes to `RtlNtStatusToDosErrorFallback`:

- `0` maps to `0`.
- A status with bit 29 set (`0x20000000`) is returned unchanged.
- A status in the customer-alias range `0xD0000000..0xDFFFFFFF` has bit 28 cleared first.
- A status whose high word is `0xC001`, `0x8007` or `0xC007` returns its low word.
- Anything else returns `ERROR_MR_MID_NOT_FOUND` (317).

The fallback rules follow the comment in the source, which says they match the Windows-compatible generic algorithm. This review did not check them against Windows.

## Verification status

- The golden column in `tests/differential/DISCREPANCIES.md` section 3 matches the current table for every row. The "ours" column in that report is stale (see NTS-2 in `docs/review/ntdll.md`).
- `STATUS_DLL_NOT_FOUND` (0xC0000135) maps to 126, and `STATUS_ENTRYPOINT_NOT_FOUND` (0xC0000139) maps to 127. These values were corrected in `08920c8` (header) and `d855c17` (table rows), with the test in `308c5b6`.
- `ERROR_NO_SUCH_MEMBER` (1387) is marked UNVERIFIED in the source.

## Known gaps

- `STATUS_INVALID_IMAGE_WIN_16` and `STATUS_REGISTRY_CORRUPT` in `ntstatus.h` carry the same values as the DLL and entry-point statuses. The names are doubtful; the values are not changed here (NTS-2).
- `STATUS_NOT_IMPLEMENTED` (0xC0000002) maps to `ERROR_INVALID_FUNCTION` (1), as the golden column records. The source also defines `ERROR_CALL_NOT_IMPLEMENTED` (120), which the table does not use.
- The table is maintained by hand. The kernel copy is generated from it (`src/kernel/nt/gen_ntstatus_table.py`), and `tools/ntstatus_lookup.py` reads it. This review found no check that compares the table with `ntstatus.h`, which is how the DLL and entry-point values drifted (NTS-1).
