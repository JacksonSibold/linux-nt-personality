# 0027: kernel_stubs.c is not C and breaks the build (arch B-1)

Status: Fixed in 974db57. `arch/x86/nt/kernel_stubs.c` is now a valid C translation unit that defines `nt_banner_init()`. The Makefile line and the vm scripts are unchanged. Not built in the kernel tree; the syntax check passes.
Finding: `docs/review/arch-x86-nt.md` B-1 (High).
Date: 2026-10-12

## Context

`arch/x86/nt/kernel_stubs.c` contained only the token `test` followed by CRLF. `arch/x86/nt/Makefile:18` builds `kernel_stubs.o` whenever `CONFIG_NT_PERSONALITY` is set, so the build failed. No `kernel_stubs.c` exists in `src/kernel/nt/`, so nothing overrides it. The file has no earlier content in git history (only `5eb8026` touches it).

`vm/rebuild_kdir.sh` (heredoc at line 164) overwrites `KDIR/arch/x86/nt/kernel_stubs.c` unconditionally. `vm/kbuild.sh` (line 88) writes it only when missing. Both write a valid file that defines `nt_banner_init()` with a `pr_info` banner. `tests/kernel/check_kernel_complete.sh` and `build/verify_nt_symbols.sh` require that symbol in `vmlinux`. Deleting the file and its Makefile line would have broken those checks.

## Options

1. Delete the file and its Makefile line. Tradeoff: the two vmlinux checks would fail because `nt_banner_init` disappears, so they would also need edits.
2. Restore the intended content: a real `kernel_stubs.c` that defines `nt_banner_init()`, with the same banner text the scripts write. Tradeoff: the banner exists in two places (the tracked file and the script heredocs) until the heredocs are removed.

## Decision

Option 2. The tracked file now matches the scripts' banner, so both build paths produce the same object. The scripts are not changed in this record. Removing their heredocs (the tree-divergence recommendation) is a separate change.

## Patch

- `arch/x86/nt/kernel_stubs.c`: header comment, `#include <linux/kernel.h>`, and `nt_banner_init()` with the banner text (commit 974db57).

## Test plan

- `bash tests/syntax/check.sh`: `arch/x86/nt/kernel_stubs.c` moves from FAIL to PASS (0 warnings).
- Kernel build with `CONFIG_NT_PERSONALITY=y` completes for the arch tree, and `nm vmlinux` shows `nt_banner_init`. Not run yet.

## Not verified

The kernel build and the vmlinux symbol check were not run. The syntax check uses the userspace shims, so `pr_info` and `linux/kernel.h` are modelled there, not checked against the 6.18 headers.
