# 0036: Import walk has no total bound across descriptors (L-16)

Status: Implemented in 7f7e5d1 (L-16). `pe_imports_walk` in `src/loader/pe_parse.c` counts descriptors and thunks over the whole walk and fails with `PE_ERR_CORRUPT` at `PE_MAX_IMPORT_DESCRIPTORS` (1024) and `PE_MAX_IMPORT_THUNKS` (262144). The limits are in `src/loader/pe_parse.h`. Host tests: `test_import_thunk_budget_exact`, `test_import_thunk_budget_exceeded`, `test_import_descriptor_count_bounded` in `tests/host/test_pe_parse.c`. Not covered: `tests/fuzz` seed (the fuzzer directory is on branch `haiku/fuzz`, not on this branch, so no seed was added here).
Finding: `docs/review/loader.md` L-16 (Medium).
Date: 2026-10-12

## Context

`pe_imports_walk` visits each import descriptor, then walks its ILT to the zero terminator. The per-descriptor loop has no total. Several descriptors can name the same ILT, so the callbacks made are the sum over descriptors of the ILT length.

The import directory size is capped at 0x10000 bytes (`pe_directory_get` and the directory check in `pe_validate`), which allows at most 3275 descriptors. The probe in L-16 used 3276 descriptors sharing one ILT of about 61000 thunks in a 512 KiB file. It made 187,858,944 callbacks in 2.6 s. The work is descriptors times ILT length, so a file of 16 MiB could reach roughly 7 x 10^9 callbacks. Callbacks run in the kernel loader and in ntdll, so this is a denial of service on load.

## Options

1. **Total thunk budget across the walk.** Count thunks over all descriptors and fail above a documented constant. Tradeoff: one counter and one constant. A legitimate image can never reach the bound, so it has no effect on real files.
2. **Reject overlapping ILTs.** Refuse a descriptor whose ILT range overlaps one already walked. Tradeoff: rejects shared-ILT layouts that some tools may produce, and needs an interval check. Whether real loaders accept shared ILTs was not verified here.
3. **Bound work by file size.** Fail above `size / 8` thunks. Tradeoff: ties the limit to file size, so a large legitimate file gets a large limit, and the bound is still high (2M thunks for 16 MiB).

## Decision

Option 1, with a second bound on descriptor count.

- `PE_MAX_IMPORT_THUNKS` = 2^18 = 262144. The largest real import tables are on the order of ten thousand entries, so this leaves more than 20x headroom. At the rate measured in L-16 (about 72 million callbacks per second) a full walk costs a few milliseconds. The walk stops before the callback for the excess thunk, so no more than the budget is delivered.
- `PE_MAX_IMPORT_DESCRIPTORS` = 1024. Real images import a few hundred DLLs at most. The bound stops the walk before the DLL callback for the excess descriptor. The directory size cap alone allowed 3275.
- Both count only real entries. Each ILT's zero terminator and the null descriptor are not counted, so an image at exactly the limit still parses.

Option 2 is not taken because it changes the accepted layout set, which needs an owner check against real loaders. Option 3 gives a bound that grows with the file, which is the shape the attack needs.

## Patch sketch (as implemented)

In `pe_imports_walk`, before the descriptor loop, `n_desc` and `n_thunks` start at zero. Each non-null descriptor increments `n_desc` before its DLL callback, and each non-zero thunk increments `n_thunks` before its symbol callback. Either counter above its limit returns `PE_ERR_CORRUPT`.

## Test plan

- Host: 256 descriptors sharing a 1024-entry ILT (262144 thunks) parses with `PE_OK` and delivers exactly the budget.
- Host: 257 descriptors on the same ILT fails with `PE_ERR_CORRUPT`. Callbacks stop at the budget (262144 symbols, 257 DLL callbacks).
- Host: 1024 descriptors each with one thunk parses; 1025 fails with `PE_ERR_CORRUPT`.
- Before the fix, the two budget tests and the descriptor test failed (263168 and 1025 callbacks reached). After the fix all 45 pe_parse tests and 10 real-PE tests pass.
- Not run: the fuzzer smoke (`tests/fuzz`, not on this branch). The MSYS2 toolchain has no sanitizer runtime, so the host build ran without ASan/UBSan.

## Not verified

- Whether any real loader accepts ILTs shared across descriptors. The limits assume legitimate images stay far below the budget, which the size argument supports but does not prove for every packer.
- The kernel build (`binfmt_pe.c`) and the ntdll consumer were not compiled. They see only the new error code, which they already handle as a failed walk.
