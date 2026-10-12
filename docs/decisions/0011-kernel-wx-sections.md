# 0011: Sections are mapped writable and executable (K-3)

Status: Proposed. Blocked/needs owner (see the note at the end). Not implemented.
Finding: `docs/review/kernel-fs.md` K-3 (High).
Date: 2026-10-12

## Context

`pe_map_sections` (`src/kernel/fs/binfmt_pe.c:437-438`) and `pe_map_ntdll` (`712-713`) map every section with `prot | PROT_WRITE`. Nothing calls `mprotect` afterward. Code sections stay writable and executable, and read-only data stays writable. The NT VAD gets the correct protection (`pe_nt_prot_from_chars`, `446-447` and `718-719`), so the NT view and the page tables disagree.

## Options

1. Keep the RW mapping for the copy, then set each section to its final protection under `mmap_write_lock(current->mm)` (`vm_mprotect` or `mprotect_fixup`). Tradeoff: one extra pass per section. Small change.
2. Map with the final protection and fill pages from the kernel (`access_process_vm` with `FOLL_FORCE`). Tradeoff: more code in the copy path.
3. Reject sections that request WRITE and EXECUTE together (policy; see `docs/review/loader.md` L-12). Tradeoff: rejects images that rely on RWX. A policy call, separate from option 1.

## Recommendation

Option 1 now, so the page tables match the VAD. Decide option 3 separately.

## Patch sketch

- `pe_map_sections`: after the copy loop, change each section to `prot` under `mmap_write_lock`. Check the result (failure path depends on 0012).
- `pe_map_ntdll`: the same.

## Test plan

- Kernel test (not host): after exec, read the VMAs of the new mm and check `.text` is r-x and `.rdata` is r--.
- Regression: an image with a WRITE|EXECUTE section is either rejected or flagged, per the policy chosen under option 3.
- `binfmt_pe_test.c` does not compile today (kernel-fs T-1), so the kernel test needs that fixed first.

## Not verified

Not built or run. The kernel tree and its test harness are not part of this worktree.

## Blocked/needs owner

Not implemented. Option 1 calls an mm protection API (`mmap_write_lock` plus `vm_mprotect` or `mprotect_fixup`), and the signatures of those functions differ across kernel versions. The target kernel version is not settled in this tree, so the call cannot be checked against it. The failure path it depends on (0012) is now in place.
