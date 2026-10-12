# 0042: kshim.h has no guard against kernel inclusion (kernel-fs S-1)

Status: Proposed. Needs an owner decision (see the question at the end). Not implemented.
Finding: `docs/review/kernel-fs.md` S-1 (Low).
Date: 2026-10-12

## Context

`src/kernel/fs/kshim.h:69` defines `current` as `kshim_current_task()`. Lines 136-148 define `printk`, `pr_info`, `kmalloc` and `kfree` as userspace functions with no `__KERNEL__` guard. A kernel translation unit that includes the header gets its own `current` and allocation calls replaced. `src/kernel/nt` headers include `kshim.h`, so the guard changes includes outside this review's scope.

## Options

1. Wrap the whole header in `#ifndef __KERNEL__`. Tradeoff: the `src/kernel/nt` includes stop seeing the shim, which may break those files.
2. Rename the shim macros with a `KSHIM_` prefix. Tradeoff: every user of the shim changes, including the test.

## Recommendation (proposal)

Option 1, after checking which `src/kernel/nt` files rely on the shim names.

## Patch sketch

- `kshim.h`: `#ifndef __KERNEL__` around the shim definitions.
- Check each `src/kernel/nt` include of `kshim.h` and add the shim explicitly where it is needed.

## Test plan

- Syntax check of each translation unit that includes the header, in both modes.
- Kernel build (VM) to confirm `current` and `kmalloc` are the kernel ones.

## Not verified

Not built or run.

## Owner question

Should `kshim.h` be guarded for kernel builds, given that `src/kernel/nt` includes it?
