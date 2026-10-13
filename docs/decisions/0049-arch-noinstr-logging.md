# 0049: noinstr function calls instrumented code (arch D-7)

Status: Proposed. Needs an owner decision and a kernel build to verify (see the question at the end). Not implemented.
Finding: `docs/review/arch-x86-nt.md` D-7 (Low).
Date: 2026-10-12

## Context

`arch/x86/nt/dispatch.c:107` marks `do_nt_syscall` as `noinstr`, but it calls `pr_info` (lines 114 and 127) and `nt_thread_check_suspend` (line 145). objtool will flag these calls.

## Options

1. Move the logging and the suspend check out of the `noinstr` region. Tradeoff: the code path changes shape.
2. Wrap the calls with `instrumentation_begin()` and `instrumentation_end()`. Tradeoff: correct only if the calls are safe at that point in the entry path.

## Recommendation (proposal)

Option 2 if the calls are safe in that window, otherwise option 1. The review cannot decide this without a build.

## Patch sketch

- `dispatch.c:114`, `:127`, `:145`: either move them out, or bracket them.

## Test plan

- Run objtool on the kernel build; no `noinstr` warnings for `do_nt_syscall`.
- Needs a kernel build and objtool (VM).

## Not verified

Not built or run. Objtool output was not seen.

## Owner question

Should the logging and suspend check be moved out of the `noinstr` region, or bracketed with instrumentation markers?
