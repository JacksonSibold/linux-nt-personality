# 0066: errno definition clashes with MinGW's _errno (ntdll RT-4)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/ntdll.md` RT-4 (Low, needs owner).
Date: 2026-10-12

## Context

`src/ntdll/ntdll_runtime.c:184` defines `int errno;` and `int *_errno(void)`. Both conflict with the dllimport declaration in MinGW's headers. Under `-Werror` this is a hard error on a MinGW host. `tests/ntdll-host/Makefile` relaxes `-Wattributes` for this object only.

## Options

1. Use the CRT's `_errno` and remove the local definition. Tradeoff: changes the CRT ABI that the runtime exports.
2. Drop the `errno` definition and keep the rest. Tradeoff: the same ABI change, with the symbol removed.

## Recommendation (proposal)

Option 1, so the symbol matches what MinGW expects, and remove the `-Wattributes` relaxation.

## Patch sketch

- `ntdll_runtime.c:184`: remove `errno` and `_errno` definitions; use the CRT's.
- `tests/ntdll-host/Makefile`: remove the `-Wattributes` relaxation once the build is clean.

## Test plan

- MinGW host build with `-Werror` for `ntdll_runtime.c` passes without the relaxation.
- Host tests still pass.

## Not verified

Not built or run on a MinGW host.

## Owner question

Should the runtime use the CRT's `_errno` and drop its own `errno`, which changes the CRT ABI?
