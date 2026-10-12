# 0015: Weak NtTerminateProcess fallback kills the caller (K-12)

Status: Accepted (option 1). Implemented in 83e3349.
Finding: `docs/review/kernel-fs.md` K-12 (Low).
Date: 2026-10-12

## Context

`src/kernel/fs/binfmt_pe.c:1043-1060` defines `NtTerminateProcess` as a weak symbol. The body calls `do_group_exit` on the caller for any handle value, including handles to other processes. The comment at 1044-1046 calls it a "strong override", which is wrong for a weak definition. If the strong definition (`arch/x86/nt/proc_thread.c:1545`) is not linked, the fallback kills the caller.

## Options

1. Make the fallback return `STATUS_NOT_IMPLEMENTED`, and correct the comment. Tradeoff: a caller gets an error instead of a kill when the strong symbol is absent. Smallest change.
2. Make the fallback honor the handle (look it up and terminate the target). Tradeoff: needs the object table, which is not visible from this file.
3. Delete the fallback, so the link fails if the strong symbol is absent. Tradeoff: a build break instead of a runtime error.

## Recommendation

Option 1 now. Option 3 once the build is known to always link the strong symbol.

## Patch sketch

- Replace the body of the weak `NtTerminateProcess` with `return STATUS_NOT_IMPLEMENTED;`.
- Correct the comment at 1044-1046.

## Test plan

- Kernel build without `proc_thread.c` linked: calling `NtTerminateProcess` on a foreign handle returns `STATUS_NOT_IMPLEMENTED` and the caller keeps running.
- Build with `proc_thread.c` linked: the strong symbol is used.
- Host: not applicable.

## Not verified

Not built or run. The strong symbol's behavior is from the arch review (P-1, 0025).
