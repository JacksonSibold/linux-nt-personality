# 0016: clear_user() return values are ignored in the exec path (K-13)

Status: Accepted (option 1). Implemented in 419f675. Behaviour change: -EFAULT from pe_map_ntdll is now fatal, which also covers the existing copy_to_user failures in that function.
Finding: `docs/review/kernel-fs.md` K-13 (Low).
Date: 2026-10-12

## Context

`src/kernel/fs/binfmt_pe.c` calls `clear_user()` at lines 421, 464, 698 and 734 without checking the result. A failed call leaves the new mapping partly non-zero, so bytes from the file's unused data can end up in BSS.

## Options

1. Check the return and fail with `-EFAULT`. Tradeoff: at these sites the mapping is already committed, so the failure must go through the post-commit path from 0012. Small change per site.
2. Zero the range through a kernel buffer and `copy_to_user`, checking the result. Tradeoff: more copying; same dependency on 0012.

## Recommendation

Option 1, after 0012 lands.

## Patch sketch

`if (clear_user(dst, len)) { err = -EFAULT; goto fail; }` at each site, with `fail` mapped to the post-commit helper where the mapping is committed.

## Test plan

- Kernel test with fault injection on the target range: exec fails (or SIGSEGV after commit) and no non-zero bytes from the file are visible in BSS.
- Host: not applicable.

## Not verified

Not built or run. The exact line-to-phase mapping (before or after commit) was taken from the review and not re-checked.
