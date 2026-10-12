# 0048: Kernel stub for file-backed sections drops the file (arch M-14)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` M-14 (Low).
Date: 2026-10-12

## Context

`arch/x86/nt/mm.c:144` (`kshim_section_create_file`) is an unconditional stub in the kernel build. It ignores `path`, so a file-backed section is anonymous. `mm.c:1517` and `:1527` cast the handle to a path (`(const char *)FileHandle`), which the shim dereferences and the kernel ignores. The kernel-nt tree already returns `STATUS_NOT_IMPLEMENTED` for a file handle (`docs/review/kernel-nt.md` M-7).

## Options

1. Resolve `FileHandle` through the NT handle table to a file object and implement kernel-side backing. Tradeoff: a large change that depends on records 0001 and 0004.
2. Return `STATUS_NOT_SUPPORTED` for a non-NULL file handle until backing exists. Tradeoff: callers that expected a file mapping fail.

## Recommendation (proposal)

Option 2 now, which matches the kernel-nt M-7 fix, then option 1 later.

## Patch sketch

- `mm.c:1517`, `:1527`: remove the handle-as-path cast; return the status for a file handle.

## Test plan

- Create a section with a non-NULL file handle: the status is the chosen failure, and no anonymous section is created.

## Not verified

Not built or run.

## Owner question

Should a file-backed section fail with `STATUS_NOT_SUPPORTED` until kernel-side backing exists?
