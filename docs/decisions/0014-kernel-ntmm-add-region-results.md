# 0014: ntmm_add_region() results are ignored in the exec path (K-10)

Status: Accepted (option 1). Implemented in 7729288. The ntdll site is fatal on a region conflict (and, see 0016, on -EFAULT).
Finding: `docs/review/kernel-fs.md` K-10 (Medium).
Date: 2026-10-12

## Context

`src/kernel/fs/binfmt_pe.c` calls `ntmm_add_region` at lines 407, 447, 552, 570, 594, 686, 719 and 876, under `CONFIG_NT_PERSONALITY`, and discards each `NTSTATUS`. `ntmm_add_region` can fail with `STATUS_CONFLICTING_ADDRESSES` or `STATUS_NO_MEMORY` (`arch/x86/nt/mm.c:1660-1686`). The NT VAD table then disagrees with the real mappings. The table is also global (arch M-1, 0020), which makes conflicts more likely.

## Options

1. Check each result and fail the exec. Tradeoff: after `begin_new_exec` this needs the post-commit path from 0012. Small change per site.
2. Log the failure and continue. Tradeoff: the mismatch stays silent to the user; this is the current behavior plus a log line.
3. Make the VAD per-process (arch M-1, 0020). Tradeoff: removes the cross-process conflicts that cause the failure; large change.

## Recommendation

Option 1, after 0012 lands, so a failure can be reported without tearing the mm. Option 3 is the end state.

## Patch sketch

At each of the eight sites, replace the bare call with `status = ntmm_add_region(...); if (!NT_SUCCESS(status)) goto fail;` (or the post-commit helper from 0012 after `begin_new_exec`).

## Test plan

- Kernel test: map a region with `NtMapViewOfSection` in the same process before exec, so the image range conflicts. Expect the exec to fail (before commit) or SIGSEGV (after commit), never a silent success.
- Host: not applicable.

## Not verified

Not built or run.
