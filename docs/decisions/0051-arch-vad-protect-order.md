# 0051: VAD protection changed before the page protection (arch M-5)

Status: Proposed. Needs an owner decision (see the questions at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` M-5 (Medium).
Date: 2026-10-12

## Context

`arch/x86/nt/mm.c:1018-1019` (commit of an existing range) and `mm.c:1325-1326` (`NtProtectVirtualMemory`) update the VAD (`mid->protect = Protect` at `:1010`, `mid->protect = NewProtect` at `:1306`) before `kshim_mprotect`. On failure the function returns `STATUS_INVALID_PAGE_PROTECTION` and the VAD is not restored. `mm.c:1257` calls `kshim_mmap_commit(..., PROT_NONE)` in `MEM_DECOMMIT` and ignores the result, after the VAD has already become `MEM_RESERVE`.

## Options

1. Change the page protection first and update the VAD only on success. Tradeoff: the page change must be reversible if a later step fails.
2. Keep the VAD change and restore it on failure. Tradeoff: a restore can itself fail.

Both options need the decommit result at `:1257` checked.

## Recommendation (proposal)

Option 1, with the decommit result checked and failure returned. The rollback design of record 0005 applies to the multi-piece case.

## Patch sketch

- `mm.c:1018-1019`, `:1325-1326`: call `kshim_mprotect` before the VAD assignment; assign on success.
- `mm.c:1257`: check the return and fail the decommit.

## Test plan

- Force `kshim_mprotect` to fail: the VAD protection is unchanged.
- Force the decommit mapping call to fail: the service returns a failure and the VAD is unchanged. Needs a kernel test build (VM).

## Not verified

Not built or run.

## Owner questions

Should a failed page-protection change leave the VAD unchanged, and should a failed decommit fail the service?
