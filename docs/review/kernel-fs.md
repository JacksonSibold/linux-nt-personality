# Review: src/kernel/fs (PE exec loader, test shim)

Scope: `src/kernel/fs/binfmt_pe.c`, `binfmt_pe_test.c`, `kshim.c`, `kshim.h`.
Method: read-only static review. Every finding was checked against the cited lines. Nothing was compiled or run.

Context: `binfmt_pe.c` is the kernel exec handler for PE files. It runs as the new image is built, so a crafted PE that a user executes reaches all of this code with full kernel privileges.

Severity: High / Medium / Low / Info.

## Summary

| Severity | Count |
|---|---|
| High | 5 |
| Medium | 7 |
| Low | 9 |
| Info | 3 |

Most of the High items are memory-safety or exec-contract problems in the loader. They are reachable by executing an attacker-supplied file, and in one case by any local user who can write `/tmp`.

## Findings

### K-1 [High] e_lfanew bounds check wraps in u32, giving an out-of-bounds kernel read
- **Status:** fixed in b8930c9 (exe path) and 4ffc5e5 (ntdll path).
- `src/kernel/fs/binfmt_pe.c:181`: `if (nt_off + 6 > (u32)n)`. `nt_off` is a u32 read from the file. For `nt_off` in `0xFFFFFFFA..0xFFFFFFFF` the sum wraps to `0..5`, the test passes, and line 186 reads `headers_buf + nt_off` (about 4 GiB past a 4 KiB buffer).
- The same pattern appears in `pe_map_ntdll` at line 639.
- Fix: compare without wrapping, for example `if (nt_off > (u32)n || (u32)n - nt_off < 6)`, or do the addition in `size_t`/`u64`. Apply to both sites.

### K-2 [Medium] Header fields read past the 4 KiB buffer and past the optional header
- **Status:** partially fixed in b205768: exe path only. The ntdll reads (`pe_map_ntdll`, near the old 741-742 and 799-800) are still unchecked.
- `src/kernel/fs/binfmt_pe.c:202`: `size_opt` is read at `nt_off + 20` before any check that `nt_off + 22 <= n`. Line 232 reads `Characteristics` at `nt_off + 22` with no check at all.
- Lines 213-230 read optional-header fields up to `oh_off + 88` with no check against `SizeOfOptionalHeader` (only `oh_off + size_opt <= n` is tested at 209, and `size_opt` may be 2).
- `headers_buf` is `kzalloc(PAGE_SIZE)` (line 166). If `nt_off` is close to 4 KiB (up to 4090 passes line 181), reads at `nt_off + 22` and `oh_off + 88` run up to about 16-90 bytes past the allocation.
- Same unchecked reads in `pe_apply_relocs` (478-481) and in `pe_map_ntdll` (741-742, 799-800).
- Fix: require `nt_off + 24 + 88 <= n` (or `headers_size`), and require `size_opt >= 88` (preferably 240 for PE32+) before reading the optional header fields.

### K-3 [High] Sections are left writable and executable
- `src/kernel/fs/binfmt_pe.c:437-438` (`pe_map_sections`) and `712-713` (`pe_map_ntdll`): every section is mapped with `prot | PROT_WRITE`. Nothing in the file calls `mprotect`, `vm_mprotect`, or `mprotect_fixup` afterward, so code sections end up RWX and read-only data ends up writable.
- The NT VAD is given the correct protection (`pe_nt_prot_from_chars` at 446-447 and 718-719), so the NT view and the real page tables disagree.
- Fix: after copying section contents, change protection to `prot` under `mmap_write_lock(current->mm)` (for example via `vm_mprotect` or `mprotect_fixup`). Consider mapping with the final protection and copying through a kernel buffer where possible.

### K-4 [High] Error returns after begin_new_exec (point of no return)
- `src/kernel/fs/binfmt_pe.c:906` calls `begin_new_exec()`. After that point the old mm is gone. The following failures still do `goto out` and return an error from `load_pe_binary`: `setup_arg_pages` (915), `pe_pick_base` (924), `pe_map_sections` (927), `pe_apply_relocs` (930), `pe_setup_stack` (933), `pe_build_process_parameters` (936), `pe_init_peb` (939), `pe_init_teb` (942), and the TEB range check (1028-1029).
- The exec core cannot roll back a committed exec. Returning an error here leaves the task with a partially built or empty address space and a stale `bprm`.
- Fix: move every fallible step before `begin_new_exec`, or, after commit, call `force_sigsegv(SIGSEGV)` and return 0, as `load_elf_binary` does for post-commit failures.

### K-5 [High] ntdll is loaded from a world-writable location with no trust checks
- `src/kernel/fs/binfmt_pe.c:622-631` (`pe_map_ntdll`): candidate paths are `/ntdll.dll`, `/windows/system32/ntdll.dll`, and `/tmp/ntdll.dll`. The file is opened with `filp_open(O_RDONLY)` and used with no owner, mode, or regular-file check.
- If the first two are absent, any local user can create `/tmp/ntdll.dll`. Its `LdrInitializeThunk` then becomes the entry point of every PE process (lines 851-855, 952-953).
- A FIFO at that path makes `filp_open` block inside exec (no `O_NONBLOCK`).
- Fix: remove the `/tmp` candidate. Accept only a root-owned regular file with no group or world write, open with `O_NOFOLLOW`, and check `S_ISREG` before reading.

### K-6 [High] Unbounded MAP_FIXED of ntdll sections
- **Status:** fixed in 539775a.
- `src/kernel/fs/binfmt_pe.c:701-713`: `vaddr` and `vs` are taken from the ntdll section table and used as `maddr = base + vaddr` with no check against `size_image`. The exe path does perform this check (303-310), so the two loaders are inconsistent.
- `vm_mmap(..., MAP_FIXED)` silently unmaps whatever is at that address, which can include the PEB, TEB, stack, or the exe image.
- Fix: apply the same bounds checks as the exe path (`vaddr < size_image`, `vaddr + vs <= size_image`, `rawptr + rawsize <= fsize`) before mapping.

### K-7 [Medium] SizeOfHeaders not bounded by SizeOfImage
- **Status:** fixed in b611a28.
- `src/kernel/fs/binfmt_pe.c:400` (`pe_map_sections`) and `682` (`pe_map_ntdll`) map `PAGE_ALIGN(SizeOfHeaders)` at the image base with `MAP_FIXED`. Nothing checks that `SizeOfHeaders <= SizeOfImage`. The check at 269 only covers the section table, and the read at 289 only needs the file to be that large.
- A header larger than the image reservation overwrites adjacent mappings.
- Fix: require `size_headers <= size_image` (and page-align the check) in `pe_read_file_headers` and `pe_map_ntdll`.

### K-8 [Medium] Relocation target not bounded to the image
- **Status:** fixed in dd6f4c8.
- `src/kernel/fs/binfmt_pe.c:531`: `target = actual_base + page_rva + offset` with `page_rva` straight from the file. The `copy_from_user` / `copy_to_user` pair at 533-535 then reads and writes that address in the new mm.
- The same applies at line 780 for ntdll.
- Fix: require `page_rva < size_of_image` and `page_rva + offset + 8 <= size_of_image` before use.

### K-9 [Medium] Per-exec memory leaks on the success path
- **Status:** fixed in db42c03.
- `src/kernel/fs/binfmt_pe.c:1034`: `state.mappings = NULL;` drops the `pe_mapping` list without freeing its nodes (`kzalloc`, line 105). One node is leaked per mapping per exec.
- `src/kernel/fs/binfmt_pe.c:1035`: `state.headers_buf = NULL;` drops the headers buffer (`PAGE_SIZE` or larger) without `kfree`.
- Fix: free the nodes without unmapping them (for example a `pe_state_free_nodes` helper), and `kfree` the buffer before clearing the pointer.

### K-10 [Medium] ntmm_add_region() return values ignored
- `src/kernel/fs/binfmt_pe.c:407`, `447`, `552`, `570`, `594`, `686`, `719`, `876`: each call is made under `#ifdef CONFIG_NT_PERSONALITY` and its `NTSTATUS` result is discarded.
- `ntmm_add_region` can fail with `STATUS_CONFLICTING_ADDRESSES` or `STATUS_NO_MEMORY` (`arch/x86/nt/mm.c:1660-1686`). The NT VAD table then silently disagrees with the real mappings. See the arch report, M-1, for why the table is global and not reset.
- Fix: check each result and fail the exec (before commit, see K-4) or at least log it.

### K-11 [Low] Relocation length check wraps; reloc_size is unbounded
- **Status:** partially fixed in 248eaa1: the u32 wrap is fixed at both sites. The `reloc_size` cap is still open.
- `src/kernel/fs/binfmt_pe.c:508`: `reloc_off + reloc_size > (u32)st->file_size` is computed in u32. `reloc_size` (from the data directory) is not capped before `kmalloc(reloc_size)` at 509, and line 761 has the same wrap.
- Fix: compare in `u64`, and cap `reloc_size` (for example to file size and a fixed maximum such as 16 MiB).

### K-12 [Low] Weak NtTerminateProcess ignores the handle; comment is wrong
- `src/kernel/fs/binfmt_pe.c:1043-1060`: the fallback definition calls `do_group_exit` on the caller for any handle value, including handles to other processes. The comment at 1044-1046 calls it a "strong override", but the definition is `__attribute__((weak))`.
- The strong definition in `arch/x86/nt/proc_thread.c:1545` is used when that object is linked. If it is not, the fallback kills the caller.
- Fix: make the fallback return `STATUS_NOT_IMPLEMENTED`, or honor the handle. Correct the comment.

### K-13 [Low] clear_user() return values ignored
- `src/kernel/fs/binfmt_pe.c:421`, `464`, `698`, `734`: a failed `clear_user` leaves the new mapping partly non-zero, which can leak bytes from the file's unused data into BSS.
- Fix: check the return and fail with `-EFAULT`.

### K-14 [Low] Export lookup for LdrInitializeThunk does no bounds checks
- **Status:** partially fixed in 63008f4: ordinal bound only. `num_names` cap and `func_rva` range check are still open.
- `src/kernel/fs/binfmt_pe.c:821-844` (`pe_map_ntdll`): `num_names` is unbounded (a u32 loop with a per-byte `copy_from_user`). `ord` (from the file) is not checked against `NumberOfFunctions` before indexing at 840. `func_rva` is not checked to lie within the image, so `func_rva == 0` gives `thunk = actual_base`, the PE header.
- Fix: cap `num_names`, check `ord < NumberOfFunctions`, and check `func_rva` lies in `[0, size_image)` and inside an executable section.

### K-15 [Low] Short ntdll files rejected for alignment reasons
- `src/kernel/fs/binfmt_pe.c:666-678`: the header re-read requires `nn >= PAGE_ALIGN(need)`. A valid ntdll shorter than the next page boundary fails.
- Fix: require only `need` bytes (or `min(need, fsize)`).

### K-16 [Low] Kernel pointers printed with %px
- **Status:** fixed in 3cc5903.
- `src/kernel/fs/binfmt_pe.c:1027`: `pr_err("... %px", state.teb)` prints an unhidden kernel address into the log. Fix: use `%p`.

### K-17 [Low] Duplicate and dead preprocessor blocks
- **Status:** fixed in 9ce57ae.
- `src/kernel/fs/binfmt_pe.c:25-34`: `#ifdef CONFIG_NT_PERSONALITY` is nested inside itself, and the inner `#else` can never be taken. Fix: collapse to one block.

### K-18 [Low] Unchecked return values on load and setup paths
- **Status:** partially fixed in 833e002: `register_binfmt` return is now checked. The ntdll-fallback policy is a design decision and is still open.
- `src/kernel/fs/binfmt_pe.c:1069` (`register_binfmt`) and `946` / `955` (ntdll failure is only logged, and the exe entry is used instead without ntdll). Silent degradation to a no-ntdll process is a policy decision that should be explicit.
- Fix: check `register_binfmt`. For the ntdll fallback, decide whether to fail the exec or keep the fallback and document it.

### K-19 [Info] Process parameters and command line are not built
- `src/kernel/fs/binfmt_pe.c:544-562` (`pe_build_process_parameters`): the comments describe populating the structure, but the function only maps a zeroed page. No argv, environment, or command line is passed to PE processes. `binfmt_pe_test.c:534` and `545` expect helpers that do not exist (see T-2).
- Fix: implement, or document the gap.

### K-20 [Checked, no finding] Checks that were looked at and found correct
- `pe_pick_base` (`binfmt_pe.c:332-361`) uses `MAP_FIXED_NOREPLACE` and falls back correctly.
- `pe_apply_relocs` (`binfmt_pe.c:514-538`) rejects non-DIR64 types and checks `cursor + block_size` against `reloc_size`.
- `pe_map_sections` checks `rawptr + rawsize <= fsize` at 311 (u64), so the later copy stays inside the file.
- `load_pe_binary` rejects setuid or setgid images (`binfmt_pe.c:887-889`).

## Test code

### T-1 [Medium] binfmt_pe_test.c cannot compile in userspace
- `src/kernel/fs/binfmt_pe_test.c:7-8` includes `kshim.h` and then `binfmt_pe.c`. `binfmt_pe.c:8-21` unconditionally includes `<linux/...>` headers, and the repository has no such include directory (a search finds no `linux/kernel.h`).
- `kshim.h:3-4` says `binfmt_pe.c` includes the shim when `PE_BINFMT_TEST` is defined. There is no such guard in `binfmt_pe.c` (grep finds none).
- Fix: move the pure parsing helpers into a header that does not depend on kernel headers, and add the `PE_BINFMT_TEST` guard the shim comment describes, or fix the comment.

### T-2 [Medium] Tests call functions that do not exist
- `src/kernel/fs/binfmt_pe_test.c:534` (`pe_build_command_line`), `545` (`pe_build_environment`), and `561-574` (`pe_utf8_to_utf16`) are not defined anywhere in `binfmt_pe.c` or elsewhere in the tree. The test binary would fail to link even with the include issue fixed.
- Fix: implement these helpers (and wire them into the loader, see K-19), or remove the tests.

### T-3 [Info] Tests do not exercise the exec-contract or bounds issues above
- No test covers e_lfanew wraparound (K-1), W+X protections (K-3), post-commit failure (K-4), or relocation bounds (K-8). Adding these would catch regressions.

## Shim header

### S-1 [Low] kshim.h has no guard against kernel inclusion
- `src/kernel/fs/kshim.h:69` defines `#define current ((struct task_struct *)kshim_current_task())`, and lines 136-148 define `printk`, `pr_info`, and `kmalloc`/`kfree` as userspace functions with no `__KERNEL__` guard. Any kernel translation unit that includes it would have its `current` and allocation calls replaced.
- Fix: wrap the whole header in `#ifndef __KERNEL__`, or rename the shim macros.

### S-2 [Info] Shim maps MAP_FIXED_NOREPLACE as MAP_FIXED
- `src/kernel/fs/kshim.h:264-265` and `278-281`: in the userspace shim `MAP_FIXED_NOREPLACE` is treated as `MAP_FIXED`, so conflicting mappings are silently replaced. This hides the conflict path the kernel depends on (see K-5 and K-7). Test-only, but it makes test results optimistic.

### kshim.c
- No findings. The file only defines three globals.

## Not verified / out of scope
- The kernel exec core behavior after a post-commit `load_binary` error (K-4) is from the general exec contract. The kernel's `fs/exec.c` is not part of this repository, so it was not read.
- `vm_mmap` semantics for `MAP_FIXED_NOREPLACE` on the target kernel version were not checked.
