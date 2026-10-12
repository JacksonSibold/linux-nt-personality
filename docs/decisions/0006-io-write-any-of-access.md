# 0006: NtWriteFile requires FILE_WRITE_DATA or FILE_APPEND_DATA (IO-1, write half)

Status: Accepted. Implemented in 80b6fea.
Finding: `docs/review/kernel-nt.md` IO-1 (Medium), write half. The read half is fixed in `1d160e8`.
Date: 2026-10-12

## Context

`nt_svc_NtWriteFile` (`src/kernel/nt/io.c`, around `:1961`) looks the handle up with `required == 0`, which skips the granted-access test in `ntio_handle_lookup` (`io.c:402-428`). A handle opened with only `SYNCHRONIZE` or `FILE_READ_ATTRIBUTES` can therefore be written.

The lookup helper only supports all-of masks: `(granted & required) == required`. Windows allows a write through a handle that has `FILE_APPEND_DATA` but not `FILE_WRITE_DATA`, so requiring `FILE_WRITE_DATA` alone would break append handles. The read half could use a single bit; the write half needs "any of".

## Options

1. **Add an any-of lookup (chosen).** Refactor the body of `ntio_handle_lookup` into one static helper with a mode flag, and expose `ntio_handle_lookup_any_of(h, mask)`. `NtWriteFile` uses `FILE_WRITE_DATA | FILE_APPEND_DATA`. Tradeoff: one more exported function in `io.h`. No ABI change.
2. **Require FILE_WRITE_DATA only.** Tradeoff: breaks append-only handles; rejected.
3. **Check the bits in `NtWriteFile` after lookup.** Tradeoff: needs the granted mask exposed, which is `ntio_handle_granted` (`io.c:2532`) and a second lock round-trip. Works, but a second lookup is racy against close.

## Recommendation

Option 1.

## Patch

- `src/kernel/nt/io.c:402-428`: move the body into `static void *ntio_handle_lookup_mode(HANDLE h, ACCESS_MASK mask, bool any_of)`. The all-of branch keeps the `(granted & mask) != mask` test; the any-of branch uses `!(granted & mask)`. `ntio_handle_lookup(h, required)` becomes a wrapper with `any_of = false`. A `required == 0` call still skips the test.
- `src/kernel/nt/io.c` (new, next to the wrapper): `void *ntio_handle_lookup_any_of(HANDLE h, ACCESS_MASK mask) { return ntio_handle_lookup_mode(h, mask, true); }`.
- `src/kernel/nt/io.h:394`: declare `void *ntio_handle_lookup_any_of(HANDLE h, ACCESS_MASK mask);`.
- `src/kernel/nt/io.c:1961`: `file = ntio_handle_lookup_any_of(FileHandle, FILE_WRITE_DATA | FILE_APPEND_DATA);`.
- The existing unused static `ntio_handle_lookup_any` (`io.c` near `:431`) is left alone; the new name avoids the clash.

## Test plan

- `src/kernel/nt/io_test.c`, `byte_round_trip` (already writes with `FILE_GENERIC_WRITE`): must still pass.
- New case: open with `SYNCHRONIZE | FILE_READ_ATTRIBUTES` only, `NtWriteFile` returns `STATUS_INVALID_HANDLE`.
- New case: open with `FILE_APPEND_DATA` only, write succeeds.
- Read half unchanged: `NtReadFile` with the same handle returns `STATUS_INVALID_HANDLE`.
- `tests/kernel-host` is not used.

## Not verified

No C compiler is installed on the machine used for this batch (no `gcc` or `cc` on `PATH`), so the change was not built or run. The three test cases above must be run before merge.
