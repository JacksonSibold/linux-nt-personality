# Review: src/loader (PE parser, protection mapping)

Scope: `src/loader/pe_format.h`, `pe_parse.c`, `pe_parse.h`, `pe_prot.c`, `pe_prot.h`.
Method: read-only static review. Every finding below was checked against the source text at the cited line. Nothing was compiled or run.

Severity: High / Medium / Low / Info.

## Summary

| Severity | Count |
|---|---|
| High | 2 |
| Medium | 3 |
| Low | 9 |
| Info | 1 |

The parser in this directory is careful about bounds for most reads (`pe_check_read`, byte-wise loads, u64 overflow checks in `pe_validate`). The problems are concentrated in the walkers (exports, imports, relocations) and in the fact that the kernel loader does not use this code at all (see L-0).

## Findings

### L-0 [High] Hardened parser is not used by the kernel loader
- `src/kernel/fs/binfmt_pe.c` does not include `pe_parse.h` and never calls `pe_validate`, `pe_imports_walk`, `pe_exports_walk`, `pe_relocs_walk`, or `pe_rva_to_offset`. A repo-wide search finds those symbols only in `build/test_ntdll_pe.c` (userspace test).
- The kernel loader re-parses headers with its own, weaker checks (see the kernel-fs report, K-1 to K-6). Several checks that exist here (data directory bounds, SizeOfHeaders vs SizeOfImage, section overlap, entry point in an executable section, import/export RVA sanity) are absent there.
- Fix: have `binfmt_pe.c` call `pe_validate()` on a buffer it has fully read (up to `SizeOfHeaders`) and use the `pe_*` walkers for imports, relocations and TLS, instead of the duplicated open-coded parsing. Then delete the duplicate helpers.

### L-1 [High] Quadratic export name lookup (CPU DoS on crafted input)
- `src/loader/pe_parse.c:964-1000`: the outer loop runs over `NumberOfFunctions` (line 964). For every function with a nonzero RVA the inner loop at 985 scans all `NumberOfNames` entries (985-1000) looking for an ordinal match. The inner loop only exits early on a match, so exports that have no name (or names that appear late in the table) cost a full scan.
- The validation at 937-946 only checks that each table fits in the file. It does not bound the product of the two counts.
- Cost is O(NumberOfFunctions x NumberOfNames). Both tables are bounded only by file size, so a single crafted file of about 1 MiB can force on the order of 10^10 inner iterations.
- Fix: make one pass over the name table and record `name_index_for_ordinal[]` (sized from `NumberOfFunctions`, which is bounded by file size, or keep a bitmap of ordinals that already have a name). Then iterate functions once. Alternatively, build a sorted index. Add a test with a 1 MiB export table.

### L-2 [Medium] Unbounded symbol-name scans
- `src/loader/pe_parse.c:341` (`pe_read_asciiz`) scans until a NUL byte or end of file. It is called for every import name (`pe_parse.c:897`) and every export name (`pe_parse.c:980`, `997`) with no length cap.
- Many thunks can point at the same long run of non-NUL bytes near the end of the file, so the total work is O(thunks x file size).
- Fix: cap name length (for example 512 bytes for imports, 1024 for exports) inside `pe_read_asciiz` or at the call sites, and return `PE_ERR_CORRUPT` when the cap is hit.

### L-3 [Medium] Walkers treat a bad directory as "absent"
- `src/loader/pe_parse.c:816` (imports), `920` (exports), `1020` (relocations), `1082` (exception): `if (s != PE_OK) return PE_OK;` after `pe_directory_get`. A directory that fails validation (`PE_ERR_DIRECTORY`, `PE_ERR_BOUNDS`) is reported as an empty directory with success.
- Callers that skip `pe_validate()` (for example a future loader path) silently get an empty import table, which is a correctness and security hazard.
- Fix: return `s` for any error except the explicit "no directory" case (`rva == 0 || size == 0`, already handled inside the walkers).

### L-4 [Medium] Relocation walker: u32 offset arithmetic can wrap
- **Status:** fixed in ece6247.
- `src/loader/pe_parse.c:1027-1033` and `1048-1049`: `reloc_off + cursor` and `reloc_off + cursor + sizeof(...) + i * 2` are computed in u32. `reloc_dir.size` may be up to `SizeOfImage` (up to 4 GiB), so the sum can exceed 2^32. The wrapped value then passes `pe_check_read`, which is itself a u32 offset check (`pe_parse.c:60-69`).
- The result is misparsed relocation entries read from the wrong file offset. It is not an out-of-bounds read, because every wrapped access still goes through `pe_check_read` against `ctx->size`.
- Fix: compute offsets as `u64`, compare to `ctx->size` before narrowing, and reject if the block lies outside the file.

### L-5 [Low] Import thunk truncation
- **Status:** fixed in 896f969.
- `src/loader/pe_parse.c:889`: `pe_rva_to_offset(ctx, (u32)thunk.u1.Ordinal, &ibn_off)` silently drops bits 32-63 of a 64-bit thunk that is not an ordinal.
- Fix: reject with `PE_ERR_CORRUPT` if `thunk.u1.Ordinal > 0xFFFFFFFFULL` in the non-ordinal branch.

### L-6 [Low] Dead overflow check in section validation
- **Status:** fixed in ebf1cec.
- `src/loader/pe_parse.c:498-499`: `(u64)sec.VirtualAddress + vsize < sec.VirtualAddress` compares a u64 sum with a u32 value. The sum is always at least the operand, so the branch never fires. The real guard is the next check at 500-501, which does work.
- Fix: delete the dead check, or write it as `vsize > UINT32_MAX - sec.VirtualAddress`.

### L-7 [Low] Unused IAT offset computation
- `src/loader/pe_parse.c:869-871`: `iat_off` is computed and never used. The error is returned only when `FirstThunk != 0`, which is misleading.
- Fix: delete the lines.

### L-8 [Low] Unexplained mask on RUNTIME_FUNCTION.EndAddress
- `src/loader/pe_parse.c:1100`: `.end = rf.EndAddress & ~0x3U`. No comment explains the mask. For a function whose end is not 4-byte aligned, the stored end moves down by up to 3 bytes, which can make the last bytes of the function fail a range lookup.
- Confirm against the PE/COFF specification whether x64 `EndAddress` has reserved low bits. If not, remove the mask. If yes, add a comment citing the spec.

### L-9 [Low] Public unchecked readers
- `src/loader/pe_parse.c:119-127` (`pe_file_u16`, `pe_file_u32`) are declared in `pe_parse.h` but do no bounds checking. All current internal callers check first, but the API invites misuse.
- Fix: make them `static`, or rename to `pe_file_u16_unchecked` and add a checked wrapper.

### L-10 [Low] Shared mutable diagnostic buffer and kernel log flooding
- `src/loader/pe_parse.c:22` (`pe_diag_buf`) is a file-scope buffer. Concurrent `pe_validate()` calls race on it. The kernel build also calls `pr_info` on every failure (`pe_parse.c:41`), which an attacker can trigger by repeatedly loading crafted files.
- Fix: pass a caller-owned diag buffer into `pe_validate`, and use `pr_info_ratelimited` (or drop the log) in the kernel path.

### L-11 [Low] Optional header read before size check
- `src/loader/pe_parse.c:408-409` reads the full 240-byte optional header with `pe_check_read(oh_off, 240)`. The check that `SizeOfOptionalHeader == 240` happens later at `pe_parse.c:415`.
- For a short `SizeOfOptionalHeader` the reader consumes bytes beyond the declared header, and a truncated file produces `PE_ERR_TRUNCATED` where `PE_ERR_CORRUPT` would be correct. No memory safety issue.
- Fix: validate `SizeOfOptionalHeader == 240` before reading, or read only `min(size, 240)`.

### L-12 [Low] Protection mapping has no W^X policy
- `src/loader/pe_prot.c:44-46` returns READ|WRITE|EXEC unchanged when a section requests all three. The comment at `pe_prot.c:5` mentions "the rare write+execute combination" but nothing enforces or flags it.
- `pe_prot.c:25-32` derives protections from the content class only when no MEM bits are set. A section with only `IMAGE_SCN_MEM_WRITE` gets `PROT_WRITE` without `READ`.
- Fix: decide policy explicitly. Reject or flag W+X, and set READ whenever WRITE is set.

### L-13 [Low] Relocation type not validated in the walker
- `src/loader/pe_parse.c:1053-1059` passes through any 4-bit type, including reserved values 6 and 11-15. The kernel consumer rejects non-DIR64 types (`binfmt_pe.c:526`), but the library contract does not say so.
- Fix: validate in `pe_relocs_walk` (accept ABSOLUTE and DIR64 for x64, reject others) or document it in `pe_parse.h`.

### L-14 [Info] u32 wrap in aligned_highest
- `src/loader/pe_parse.c:565`: `(highest + SectionAlignment - 1) & ~(SectionAlignment - 1)` is computed in u32 and can wrap when `highest` is within one alignment unit of 4 GiB. The 1 MiB slack check at 570-571 currently rejects the wrapped value, so there is no accepted bad case today.
- Fix: compute in u64.

## Not verified / out of scope
- No compile, no sanitizer run, no fuzzing. Overflow claims are from reading the arithmetic.
- `pe_format.h` was read for struct sizes (static asserts present for the main structures). No layout issues found.
