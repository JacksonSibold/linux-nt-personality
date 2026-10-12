# Progress log

Chronological summary of the commit history, grouped by theme. Generated from `git log --reverse --date=short` over the 99 commits that existed before this file was added (history ending at `e5ab0dc`).

Counts are of commits in that history. Each commit is listed once, under one theme. Merge commits are listed separately. Within a date, entries follow git's default log order, oldest first; `--date=short` does not give times.

## Counts

| Theme | Commits |
|---|---|
| Baseline snapshot | 1 |
| Tooling (`tools/`, `build/`, CI, generators) | 15 |
| Tests | 10 |
| Reviews and analysis (`docs/review/`, `docs/analysis/`, `docs/status/`) | 11 |
| Fixes (`src/`, `arch/`) | 35 |
| Documentation (`docs/`, `CONTRIBUTING.md`) | 17 |
| Merge commits | 10 |
| **Total** | **99** |

By date: 2026-10-01, 1 commit (baseline). 2026-10-12, 98 commits.

## Timeline

### 2026-10-01

- `5eb8026` Publish sanitized personal project source snapshot (baseline).

### 2026-10-12

#### Tooling

- `1f7cdd8` tools: add check_repo.py repository hygiene checker
- `03f57d7` tools: add ntstatus_lookup.py NTSTATUS lookup CLI
- `be85353` tools: add pe_info.py PE header summary CLI
- `a6db3a9` tools: document check_repo, ntstatus_lookup and pe_info
- `5fc8d4a` gen_ntdll_def: print the real export count
- `3998791` fix_gsbase: apply the TEB bounds check on the fallback path
- `f352792` fix_iosb: stop rewriting the accessor helpers
- `7ce7622` fix_iosb: rewrite comparison reads of Information
- `4a71467` ci: run unit tests and repo checks on push and PR
- `e5279cf` tools: add criteria_report.py status table for DoD criteria
- `ed17a53` gen_ntstatus_table: count every table entry and drop hard-coded 527
- `a3a84fe` build: add ntstatus-table rule and track generated ntstatus_table.inc
- `89f20cb` build: derive criteria.json path in validate_criteria.py
- `d7e2e0f` build: derive repository root in merge_dod.py
- `a1caba5` build: honour PYTHON override in ntstatus-table rule

#### Tests

- `017d7ad` Add unit tests for build/gen_stubs.py
- `cad5cb4` Add unit tests for build/gen_ntdll_def.py
- `45edb3a` Add unit tests for build/fix_iosb.py
- `aa78b3c` Add unit tests for build/fix_gsbase.py
- `6ea0082` Add unit tests for build/validate_criteria.py
- `bcc2207` Add unit tests for build/merge_dod.py
- `9c792d3` Add unit tests for gen_ntstatus_table.py
- `b8be4ab` tests(unit): cover criteria_report status, loading, rendering and real data
- `8a6b2c5` test(syntax): add userspace-shim syntax check for kernel-facing sources
- `5ef55bf` test(syntax): make check.sh executable

#### Reviews and analysis

- `056b2d6` docs(review): add loader PE parser review
- `ec2cf71` docs(review): add kernel fs PE loader review
- `5e88c69` docs(review): add arch/x86/nt review
- `5c83259` docs(review): add review index for loader, kernel fs, arch nt
- `182cbc8` docs(review): mark arch/x86/nt findings fixed by commit hash
- `0aaf41a` docs(review): mark loader and kernel fs findings fixed by commit
- `eeb6d7f` docs(status): add generated CRITERIA.md conformance status table
- `ce3ea7d` docs(analysis): document arch/x86/nt vs src/kernel/nt tree divergence
- `f23249e` docs(analysis): record step 1 port status for tree divergence
- `520c3c7` docs(review): add kernel NT static review (src/kernel/nt)
- `c0c4a57` docs(review): add kernel NT report to index and update totals

#### Fixes

- `6da43d2` fix(nt_io): remove early return that skipped NtReadFile read at nt_io.c:1899
- `79d1de2` fix(nt_io): bound FileNameLength in ntio_set_rename at nt_io.c:2272
- `d921888` fix(mm): reject section view offset past end and wrapping size at mm.c:1566
- `182ee9f` fix(proc_thread): zero PROCESS/THREAD_BASIC_INFORMATION before copy at proc_thread.c:1649 and 1814
- `16af7b7` fix(dispatch): zero NtQueryVirtualMemory bounce buffer at dispatch.c:258
- `3899c83` fix(proc_thread): close opened handle when copy_to_user fails at proc_thread.c:1593 and 1615
- `b8930c9` binfmt_pe.c:181 fix e_lfanew bounds check wrapping in u32
- `4ffc5e5` binfmt_pe.c:639 fix e_lfanew bounds check wrapping in u32 (ntdll path)
- `34b3fa8` fix(proc_thread): check current eprocess in Suspend/ResumeProcess at proc_thread.c:1472 and 1487
- `b205768` binfmt_pe.c:202 bound file-header and optional-header reads
- `54baad9` fix(proc_thread): probe ThreadHandle before creating the thread at proc_thread.c:1213
- `b611a28` binfmt_pe.c:249 and 639 reject SizeOfHeaders larger than SizeOfImage
- `539775a` binfmt_pe.c:~716 bound ntdll section VA/size against SizeOfImage
- `39de0fb` fix(nt_io): zero NtQueryInformationFile buffer before fill at nt_io.c:2139
- `248eaa1` binfmt_pe.c:519 and 774 compute reloc range end in u64
- `6f4e944` fix(proc_thread): reject image path too long for USHORT at proc_thread.c:1777
- `dd6f4c8` binfmt_pe.c:541 and 793 bound DIR64 relocation target to the image
- `3cc5903` binfmt_pe.c:1027 print TEB with %p instead of %px
- `833e002` binfmt_pe.c:1069 check register_binfmt() return in init_pe_binfmt
- `db42c03` binfmt_pe.c:1034 free mapping nodes and headers buffer on exec success
- `63008f4` binfmt_pe.c:~843 bound name ordinal against NumberOfFunctions
- `9ce57ae` binfmt_pe.c:25 remove duplicated CONFIG_NT_PERSONALITY conditional
- `ebf1cec` pe_parse.c:498 remove dead u64 overflow check in section validation
- `896f969` pe_parse.c:889 reject non-ordinal import thunks wider than 32 bits
- `ece6247` pe_parse.c:1027 check base-relocation directory fits in file before u32 walk
- `545b374` fix(dispatch): zero NtQueryVirtualMemory bounce buffer
- `ab11b7c` fix(dispatch): clear current nt_thread on personality exit
- `ecf6a9e` fix(proc_thread): probe ThreadHandle before creating the thread
- `c6f98dd` fix(proc_thread): check current eprocess in Suspend/ResumeProcess
- `0d931bd` fix(proc_thread): close opened handle when copy_to_user fails
- `c57c6ff` fix(proc_thread): zero BASIC_INFORMATION structs before fill
- `ba78f96` fix(proc_thread): reject image path too long for USHORT
- `40f6db0` fix(mm): reject section view offset past end and wrapping size
- `b125094` fix(io): declare unused LARGE_INTEGER off only under __KERNEL__
- `14f9fb8` fix(io): declare unused kbuf only under __KERNEL__

#### Documentation

- `b9e828c` docs: add object manager module reference
- `756ace2` docs: add handle table module reference
- `9e38854` docs: add registry module reference
- `4b50dd8` docs: add sync module reference
- `6e3ea5f` docs: add security module reference
- `ba8618b` docs: add virtual memory (mm) module reference
- `34deb2f` docs: add I/O manager module reference
- `2edbd7a` docs: add completion port and APC module reference
- `8d6c86a` docs: add process and thread module reference
- `590def1` docs: add syscall dispatch module reference
- `bcddf13` docs: add NTSTATUS-to-DOS module reference
- `dc0638e` docs: add module reference index
- `78e8631` docs: add development guide for tests, tools and ntstatus table
- `a1b7618` docs: consolidate known gaps and top review findings
- `a441a61` docs: add short contributing guide
- `4a778fd` docs: document ntstatus_table.inc generation and drop the 527 gap
- `874b8d5` docs: add Paths and environment section to DEVELOPMENT.md

#### Merges, in order

- `ef7d402` haiku/tests
- `84fa9e1` haiku/docs
- `35a7591` haiku/review
- `cc11e9c` haiku/iofix
- `48a960e` haiku/loaderfix
- `7eb64e6` haiku/diverge
- `2125f1b` haiku/ntstatus
- `5ad0233` haiku/paths
- `4ec7826` haiku/unitc
- `e5ab0dc` haiku/verify
