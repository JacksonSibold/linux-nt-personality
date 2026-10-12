# arch/x86/nt vs src/kernel/nt: tree divergence

Date: 2026-10-12. Branch: `haiku/diverge`. Scope: static read of the two trees, `git diff --no-index`, and the build scripts in `vm/` and `build/`. No source file was modified and no build was run. Follows up on arch review finding B-1 and B-3 in `docs/review/arch-x86-nt.md`.

Diff direction used below: `git diff --no-index arch/x86/nt/F src/kernel/nt/F`. Lines marked `-` are in `arch/x86/nt`, lines marked `+` are in `src/kernel/nt`.

## 1. Inventory

| File | In arch | In src | Status |
|---|---|---|---|
| `mm.c` | yes | yes | differs: 41 ins, 84 del, 11 hunks |
| `dispatch.c` | yes | yes | differs: 1 ins, 2 del, 2 hunks |
| `proc_thread.c` | yes | yes | differs: 2 ins, 22 del, 8 hunks |
| `nt_services.c` | yes | yes | identical |
| `weak.c` | yes | yes | identical |
| `nt_internal.h` | yes | yes | identical |
| `nt_service_list.h` | yes | yes | identical |
| `mm.h` | yes | yes | identical |
| `Makefile` | yes | no | arch only |
| `kernel_stubs.c` | yes | no | arch only, not valid C (see section 4) |
| `nt_io.c` | yes | no | arch only (src has `io.c`, a different file) |

Src-only files that the arch Makefile needs but arch does not contain: `handle.c`, `kshim.c`, `iocp.c`, `object.c`, `sync.c`, `security.c`, `nt_obj.c`, `ntstatus_kernel.c`, `debugfs_tests.c`. `stubs.c` is generated (empty) by the scripts. The arch tree on its own cannot build; it is an overlay that depends on the copy step.

## 2. Real differences per file

### mm.c (125 changed lines, 11 hunks)

The largest divergence. Three groups:

1. **VAD coalescing is removed in src.** `nt_vad_coalesce` and `nt_coalesce_all` are no-ops in src. `NtAllocateVirtualMemory` no longer coalesces after commit. `NtQueryVirtualMemory` no longer extends the region across neighbouring VADs (it reports a single VAD extent). The src comments say this matches a Windows oracle (`probe_virtualquery_direct`, "4084 S1 divergences"). Arch keeps the merge logic. This changes user-visible `NtQueryVirtualMemory` output, so it is a behaviour decision, not a copy error. `ntmm_check_invariants` is also relaxed in src to allow adjacent identical VADs.
2. **`nt_vad_attrs_match` is stricter in src.** It adds checks for `section_offset`, `write_watch`, `alloc_protect` (COMMIT) and `protect` (RESERVE). In arch the function is still live; in src it is marked `__attribute__((unused))`.
3. **Security and correctness fixes present only in arch.**
   - `NtMapViewOfSection`: arch rejects `offset > sec->size` and uses the non-wrapping `actual_size > sec->size - offset`. Src has `offset + actual_size > sec->size`, which can wrap. This is the content of arch commit `d921888` ("reject section view offset past end and wrapping size").
   - `nt_vad_split`: src sets `section_offset = 0` for non-section VADs; arch computes it unconditionally.
   - Src removes a `printk(KERN_EMERG ...)` debug line in `NtProtectVirtualMemory`. This one is src-only cleanup.

Net: neither copy is a superset. Arch is the one with the security fix, src has the coalescing change and the attrs checks.

### dispatch.c (3 changed lines, 2 hunks)

- Line 258 (`NtQueryVirtualMemory` wrapper): arch `kzalloc`, src `kmalloc`. Arch is the fix from commit `16af7b7`. Src still copies uninitialised kernel heap to user space (review D-1).
- `nt_exit_personality` (line 304 in arch): arch calls `nt_set_current_thread(NULL)` after abandoning mutants. Src does not. Arch is the one that clears the thread pointer.

### proc_thread.c (24 changed lines, 8 hunks)

Src lacks every hardening commit that arch has on this branch:

- `NtCreateThreadEx`: arch probes `ThreadHandle` before creating the thread (commit `54baad9`). Src does not.
- `NtSuspendProcess` / `NtResumeProcess`: arch checks `nt_current_eprocess()` (commit `34b3fa8`). Src does not.
- `NtOpenProcess` / `NtOpenThread`: arch closes the new handle if `copy_to_user` fails (commit `3899c83`). Src leaks the handle.
- `NtQueryInformationProcess` / `NtQueryInformationThread`: arch zeroes `PROCESS_BASIC_INFORMATION` / `THREAD_BASIC_INFORMATION` before fill (padding leak). Src does not.
- `NtQueryInformationProcess` image-path class: arch rejects names too long for `USHORT` (`6f4e944`). Src does not.

### nt_services.c, weak.c, nt_internal.h, nt_service_list.h, mm.h

Byte-identical in both trees. No action beyond deleting one copy.

## 3. Which tree wins in the build

There are two build paths. They produce different KDIR contents.

**`vm/kbuild.sh` (restore path, lines 68-135).** The copy of `arch/x86/nt/.` then `src/kernel/nt/.` (lines 73-79) runs only when `KDIR` is missing `dispatch.c`, `proc_thread.c` or `fs/binfmt_pe.c`. In that case **src wins for every overlapping file**: `mm.c`, `dispatch.c`, `proc_thread.c`. Result: the pre-fix src versions are built. Caveat: because the arch copy runs first and `kernel_stubs.c` is then not overwritten (the `if [ ! -f ... ]` guard at line 87), the broken arch `kernel_stubs.c` is compiled in this path.

**`vm/rebuild_kdir.sh` (from-scratch path, lines 130-160).** Copies `arch/x86/nt/.` first, then copies `src/kernel/nt/*` except a skip list: `dispatch.c nt_io.c nt_services.c proc_thread.c nt_internal.h weak.c Makefile`. Result, per file:

| File | Winner in rebuild_kdir | Why |
|---|---|---|
| `mm.c` | **src** | not on the skip list, so src overwrites arch |
| `dispatch.c` | arch | skip list |
| `proc_thread.c` | arch | skip list |
| `nt_services.c`, `weak.c`, `nt_internal.h` | arch (identical anyway) | skip list |
| `nt_io.c` | arch | no src counterpart |
| `Makefile` | arch | copied last |
| `kernel_stubs.c` | generated banner | heredoc overwrites unconditionally (line 164) |

So the default from-scratch build takes `mm.c` from src (which lacks the `d921888` section-offset fix) and takes `dispatch.c` and `proc_thread.c` from arch (which has the fixes). The two scripts disagree on `mm.c`, `dispatch.c` and `proc_thread.c`, and the review's comment in `rebuild_kdir.sh:133` ("arch takes precedence") is only true for three of the four overlapping files.

Other notes:
- `vm/freshness.sh` hashes both trees, so editing either one marks the build stale. It does not record which copy the build used.
- `vm/preflight.sh` and `vm/kbuild.sh` completeness checks look only at the 17 object names in the arch Makefile, not at which copy was used.
- Review B-1/B-3 line references are slightly off: `arch/x86/nt/Makefile` has 18 lines, `kernel_stubs.o` is at line 18 (not 36). The `mm.c` difference is 125 changed lines, not about 155. `dispatch.c` differs in two hunks (258 and 304), not one. `proc_thread.c` differs by 24 lines and is not mentioned in B-3.

## 4. kernel_stubs.c

- `arch/x86/nt/kernel_stubs.c` is 6 bytes: the token `test` followed by CRLF. It is not valid C. It is tracked (added in `5eb8026`) and listed in `arch/x86/nt/Makefile:18`.
- There is no `kernel_stubs.c` in `src/kernel/nt`.
- The scripts write a different `kernel_stubs.c` into KDIR: a banner function `nt_banner_init` (`rebuild_kdir.sh:164`, `kbuild.sh:88`). That generated file is valid C. Its only observable effect is a `pr_info` banner.
- The symbol `nt_banner_init` is required by `build/verify_nt_symbols.sh:16` and `tests/kernel/check_kernel_complete.sh:44`, so the banner is not optional for those checks. The tracked file is never compiled from the repo's own build, but kbuild's restore path does compile it (see section 3).

Recommendation: replace the broken file with a real `kernel_stubs.c` in the canonical tree that defines `nt_banner_init`, and remove the heredoc from both scripts. Do not simply delete the file and the Makefile line without also removing `nt_banner_init` from the two checks, or those checks will fail. This is an owner decision, as the review noted.

## 5. Recommendation

1. **One canonical source tree: `src/kernel/nt`.** It holds 39 tracked files, and the sources for every Makefile object except `nt_io.o` (arch only), `kernel_stubs.o` (arch only) and the generated `stubs.o`. Arch is a partial overlay.
2. **Port the arch-only fixes into src before deleting anything:** `dispatch.c` (`kzalloc`, `nt_set_current_thread(NULL)`), `proc_thread.c` (commits `54baad9`, `34b3fa8`, `3899c83`, `6f4e944` and the `memset` padding fixes), and `mm.c` (`d921888`, offset/size check and `section_offset` guard). The `nt_io.c` fixes (`39de0fb`) belong in `nt_io.c`, which lives only in arch and needs a home decision (rename into `src/kernel/nt/io.c` or keep it as a second source).
3. **Decide the `mm.c` coalescing question explicitly.** Src disables coalescing and tightens `nt_vad_attrs_match` for an oracle match. Arch keeps coalescing. Pick one behaviour, test `NtQueryVirtualMemory` region sizes against the probe, and record the decision in the commit message. Do not merge by hand without that decision.
4. **Delete the duplicates once (2) and (3) are settled:** `arch/x86/nt/{dispatch.c,mm.c,proc_thread.c,nt_services.c,weak.c,nt_internal.h,nt_service_list.h,mm.h,nt_io.c}` and `arch/x86/nt/kernel_stubs.c`. Keep `arch/x86/nt/Makefile` only if the tree keeps its overlay role; otherwise move the Makefile into `src/kernel/nt` and update the scripts.
5. **Make both scripts use the same rule.** Replace the copy blocks in `vm/kbuild.sh` (lines 68-135) and `vm/rebuild_kdir.sh` (lines 130-160) with one copy of `src/kernel/nt/*` into KDIR. Remove the skip list and the "arch takes precedence" comment. Add a preflight check that fails if any file exists in both trees.
6. **Re-run the arch review (findings M-1 to M-13 and P-1 to P-2) against the merged `src/kernel/nt` text** before acting on them, as B-3 already says.

Nothing in this document changes source. The order of work above is a suggestion for the owner; steps 2 and 3 need a design decision and should not be done by the analysis pass.

## 6. Status

Updated 2026-10-12 on branch `haiku/port` (step 1 of section 5). Arch files were not deleted.

### Ported into `src/kernel/nt` (one commit per arch hunk group)

| Commit | Port of arch commit | Content |
|---|---|---|
| `545b374` | `16af7b7` | `dispatch.c`: `kmalloc` to `kzalloc` in NtQueryVirtualMemory bounce buffer |
| `ab11b7c` | `5eb8026` (no hardening commit) | `dispatch.c`: `nt_set_current_thread(NULL)` in `nt_exit_personality` |
| `ecf6a9e` | `54baad9` | `proc_thread.c`: probe ThreadHandle before creating the thread |
| `c6f98dd` | `34b3fa8` | `proc_thread.c`: NULL eprocess check in Suspend/ResumeProcess |
| `0d931bd` | `3899c83` | `proc_thread.c`: close handle when copy_to_user fails (NtOpenProcess/NtOpenThread) |
| `c57c6ff` | `182ee9f` | `proc_thread.c`: zero PROCESS/THREAD_BASIC_INFORMATION before fill |
| `ba78f96` | `6f4e944` | `proc_thread.c`: reject image path too long for USHORT |
| `40f6db0` | `d921888` | `mm.c`: NtMapViewOfSection offset and wrapping-size check |

Each commit message names its arch origin. `git log --oneline -- src/kernel/nt` lists them.

### Result

- `dispatch.c`: identical to arch now (`git diff --no-index` empty).
- `proc_thread.c`: identical to arch now (empty diff). Arch was the only copy with these fixes, so src is now the superset.
- `mm.c`: 10 hunks remain (was 11), 122 changed lines (was 125). Remaining hunks, by category:
  - **VAD coalescing (owner decision, left as src):** `nt_vad_attrs_match` (two copies, stricter in src, `__attribute__((unused))`), `nt_vad_coalesce` (no-op in src, two copies), `nt_coalesce_all`, both `ntmm_check_invariants` loops, `NtAllocateVirtualMemory` (no coalesce after commit), `NtQueryVirtualMemory` (reports single-VAD extent, no merge). Src follows the `probe_virtualquery_direct` oracle; arch keeps merging. Pick one behaviour, test `NtQueryVirtualMemory` region sizes against the probe, then record it. Not changed in this step.
  - **Src-only, kept:** `nt_vad_split` `section_offset` guard (src sets 0 for non-section VADs, arch computes it unconditionally). This is the opposite of what section 2 says. Src is ahead here, so nothing was ported. `NtProtectVirtualMemory` `printk(KERN_EMERG ...)` removal, a debug line only in arch. Not re-added.
- Build paths: the `vm/kbuild.sh` and `vm/rebuild_kdir.sh` disagreement from section 3 now only affects `mm.c`. For `dispatch.c` and `proc_thread.c` both copy paths produce the same text after this step.

### Not done in this step

- `nt_io.c` fixes `39de0fb`, `79d1de2`, `6da43d2`. `nt_io.c` exists only in arch, so they need the home decision from step 2. Not ported.
- `arch/x86/nt/kernel_stubs.c` (broken, section 4) is unchanged.
- Arch duplicates are still present. Deletion waits for step 4 and for the mm.c coalescing decision.
- Syntax check not run. No C compiler on PATH (`gcc` not found), and `C:/Users/Jackson/repos/TOOLCHAIN.md` does not exist. The changes are small and mirror arch text that was already in the tree, but none of it has been compiled.
