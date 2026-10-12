# Module: ntstatus_kernel (Kernel NTSTATUS to DOS error mapping)

Sources: `src/kernel/nt/ntstatus_kernel.c`, `src/kernel/nt/ntstatus_kernel.h`, generator `src/kernel/nt/gen_ntstatus_table.py`
Source of truth: `src/ntdll/ntstatus_to_dos.c` (userspace table)
Related: `dispatch.md` (the SSN table that would route the wrapper), `i2_test.c` (tests the wrapper).

## Purpose

Maps an NTSTATUS code to a Win32 DOS error code (`RtlNtStatusToDosError`) in the kernel personality. The kernel table is meant to be generated from the userspace table so that the two cannot drift. The file also provides a syscall-style wrapper, `nt_svc_NtRtlNtStatusToDosError`, which writes the DOS error to a caller-supplied pointer.

## Public API

Declared in `ntstatus_kernel.h`:

- `ULONG RtlNtStatusToDosError(NTSTATUS Status)` - look up the DOS error. Returns the mapped value, or the fallback described below.
- `ULONG nt_kernel_RtlNtStatusToDosError(NTSTATUS Status)` - kernel-only alias, declared and defined under `#ifdef __KERNEL__`.

Defined in `ntstatus_kernel.c` but not declared in `ntstatus_kernel.h`:

- `NTSTATUS nt_svc_NtRtlNtStatusToDosError(NTSTATUS Status, ULONG *DosError)` - wrapper. The kernel variant checks the pointer with `access_ok`, then writes with `copy_to_user`. It returns `STATUS_ACCESS_VIOLATION` on a null pointer or a failed user copy. The userspace variant writes directly through the pointer.

## Key data structures

- `struct _NTSTATUS_TO_DOS_K` (typedef `NTSTATUS_TO_DOS_K`) - `NTSTATUS Status` and `ULONG DosError`.
- `static const NTSTATUS_TO_DOS_K NtStatusToDosTableKernel[]` - filled by `#include "ntstatus_table.inc"`.
- `ERROR_MR_MID_NOT_FOUND` = 317, the fallback value.

## Lookup behaviour

1. Binary search over `NtStatusToDosTableKernel`, comparing the `NTSTATUS` values as signed `NTSTATUS` (`LONG`) quantities.
2. On a miss, `RtlNtStatusToDosErrorFallbackK` applies these rules in order:
   - Status 0 maps to 0.
   - If bit 29 (0x20000000, the customer bit) is set, the status is returned unchanged.
   - If the top nibble is 0xD, bit 28 is cleared (`0xD...` is treated as `0xC...`).
   - If the high word is 0xC001, 0x8007 or 0xC007, the low 16 bits are returned.
   - Otherwise, 317 (`ERROR_MR_MID_NOT_FOUND`) is returned.

## Table generation

- `gen_ntstatus_table.py --input src/ntdll/ntstatus_to_dos.c --output src/kernel/nt/ntstatus_table.inc` reads the userspace table body with a regex (`{ NAME, 123L }` entries) and writes an `.inc` file with an "AUTO-GENERATED - DO NOT EDIT" header and the entries.
- The generator warns if the count is not 527. It does not check sort order (its comment at `gen_ntstatus_table.py:44-45` says verifying order would need evaluating the `STATUS_` defines).
- Build note: the generator's own usage example writes `ntstatus_table.inc` into this directory.

## Invariants (as implemented)

- The table must be sorted by signed NTSTATUS value for the binary search to be correct. I checked the entries in `ntstatus_to_dos.c` of the form `{ STATUS_...` (511 entries, resolved to values through `ntstatus.h`). Those are in non-decreasing signed order. This is a one-time check, not an enforced invariant. The generator does not enforce it.
- The kernel and userspace paths share the same search and fallback logic, as the file header states.

## Known gaps (evident in code)

- `ntstatus_table.inc` is not in the repository. `ntstatus_kernel.c:22` includes it, and the generator is the only thing that creates it. The file therefore cannot compile as checked in until the generator runs. The generator needs `src/ntdll/ntstatus_to_dos.c`, which is present.
- The generator's expected count (527) does not match the 511 `{ STATUS_...`-form entries I could resolve in `ntstatus_to_dos.c`. The generator's regex might capture entries that do not start with `STATUS_`. I did not reconcile the difference.
- The table is sorted only by the convention of the source file. Nothing in the generator or in `ntstatus_kernel.c` enforces it, and a misordered table would give silent wrong answers rather than an error.
- The wrapper is listed in `nt_service_list.h:171` as `NT_SERVICE(0x01db, NtRtlNtStatusToDosError, 2)`, so it is reached through the dispatch table at SSN 0x1db when `ntstatus_kernel.c` is linked. `i2_test.c` also calls it directly.
- The fallback's bit-28 handling only clears the bit when the top nibble is 0xD. Other statuses with bit 28 set are not normalised. This matches the code as written; I did not compare it with the Windows behaviour.
