# 0027: kernel_stubs.c is not C and breaks the build (arch B-1)

Status: Proposed. Build file in `arch/x86/nt`. Recorded only in this batch, not implemented.
Finding: `docs/review/arch-x86-nt.md` B-1 (High).
Date: 2026-10-12

## Context

`arch/x86/nt/kernel_stubs.c` contains only the token `test` followed by CRLF. `arch/x86/nt/Makefile:36` builds `kernel_stubs.o` whenever `CONFIG_NT_PERSONALITY` is set, so the build fails. No `kernel_stubs.c` exists in `src/kernel/nt/`, so nothing overrides it.

## Options

1. Delete the file and its Makefile line. Tradeoff: nothing is lost; the file has no code. Small change.
2. Restore the intended content. Tradeoff: the content is not in the tree. Check the git history of the file first; if a previous version has real code, this option becomes the choice.

## Recommendation

Option 1, after checking `git log --follow` on the file for any earlier content.

## Patch sketch

- Remove `arch/x86/nt/kernel_stubs.c`.
- Remove the `kernel_stubs.o` line at `arch/x86/nt/Makefile:36`.

## Test plan

- Kernel build with `CONFIG_NT_PERSONALITY=y` completes for the arch tree.
- `grep -r kernel_stubs` returns no reference after the change.
- Host: not applicable.

## Not verified

Not built. The git history of the file was not checked for this record.
