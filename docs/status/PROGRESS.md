# Progress log

Chronological summary of the commit history, grouped by theme. Generated from `git log --reverse --date=short` over the 259 commits from the baseline (`5eb8026`) to `bb13e80` (inclusive). Regenerate by repeating that log, not by hand.

Counts are of commits in that history. Each commit is listed once, under one theme: subject prefix first (`docs(review)` and similar, then `tools`, `build`, `ci`, `vm`), then the paths the commit touches (`src/` or `arch/` as fixes, `tests/` as tests, `docs/` as documentation). Merge commits are listed separately. Within a date, entries follow git's log order, oldest first; `--date=short` does not give times.

## Counts

| Theme | Commits |
|---|---|
| Baseline snapshot | 1 |
| Tooling (`tools/`, `build/`, CI, generators) | 39 |
| Tests | 52 |
| Reviews and analysis (`docs/review/`, `docs/analysis/`, `docs/status/`, `docs/decisions/`) | 27 |
| Fixes (`src/`, `arch/`) | 60 |
| Documentation (`docs/`, `CONTRIBUTING.md`, `README.md`) | 52 |
| Merge commits | 28 |
| **Total** | **259** |

By date: 2026-10-01, 1 commit; 2026-10-12, 258 commits.

## Timeline

### 2026-10-01

#### Baseline snapshot

- `5eb8026` Publish sanitized personal project source snapshot

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
- `4ec4530` build: write validate_criteria output as UTF-8 on cp1252 consoles
- `b282025` build: derive Makefile PROJ from the Makefile location
- `024f573` build: take the kernel tree path for fix_gsbase.py from NTPERS_LINUX
- `f39bdf5` build: take the kernel tree path for fix_iosb.py from NTPERS_LINUX
- `aea2e83` build: resolve m0_diag.sh project root from the script location
- `5236f77` ci: run host-test (gcc) for src/loader
- `e791b60` build: resolve build_pe_corpus.sh project root from the script location
- `2641ddf` build: find syscall_table.def under NTPERS_ROOT in test_ntdll_pe.c
- `4db8dbf` vm: stop hardcoding /root in check_reg.sh
- `7c8cc23` tools: add check_doc_links.py for Markdown links and anchors
- `22d2148` ci: run the documentation link checker
- `ed1476c` fix(tools): keep the outer tracer when coverage measurement nests
- `4c6fbb0` fix(tools): parse cast and suffixed NtStatusToDosErrorTable entries
- `4d72102` fix(tools): omit "line None" for compile errors without a line
- `f0624fe` fix(tools): report bad criteria file and unwritable --out as errors
- `aee11e9` chore(vm): use strict mode in golden_latest.sh
- `6e138af` feat(vm): add lint.sh for shell script syntax and shellcheck
- `4995fe8` ci: run vm/lint.sh shell script checks
- `0cadee5` vm: set executable bit on tracked host and guest scripts
- `5296b38` build: set executable bit on tracked build scripts
- `6b81605` vm: mark m0_diag2.sh deprecated
- `56c5c76` feat(tools): add provenance_scan.py factual inventory tool
- `8515b1c` ci: check SPDX tag syntax with provenance_scan --check
- `a9bc215` scripts: set executable bit on shebang scripts added after the mode fix

#### Tests

- `017d7ad` Add unit tests for build/gen_stubs.py
- `cad5cb4` Add unit tests for build/gen_ntdll_def.py
- `45edb3a` Add unit tests for build/fix_iosb.py
- `aa78b3c` Add unit tests for build/fix_gsbase.py
- `6ea0082` Add unit tests for build/validate_criteria.py
- `bcc2207` Add unit tests for build/merge_dod.py
- `9c792d3` Add unit tests for gen_ntstatus_table.py
- `b8be4ab` tests(unit): cover criteria_report status, loading, rendering and real data
- `25c568a` tests(host): add gcc host harness for src/loader pe_parse
- `312c882` tests(host): walkers must report a corrupted data directory
- `8a6b2c5` test(syntax): add userspace-shim syntax check for kernel-facing sources
- `5ef55bf` test(syntax): make check.sh executable
- `d89de50` tests(host): exception EndAddress must not be truncated
- `24278ca` tests(host): short optional header must be reported as corrupt
- `2f88656` tests(host): make sanitizer reports fatal
- `c533695` tests: resolve test_pe_parse.c probe paths under NTPERS_ROOT
- `b9c66f4` tests(host): symbol names and forwarders must be length-capped
- `a69558a` tests(host): export table counts, ordinals and name lookup (L-1)
- `7acbcc1` fix(differential): use NTPERS_ROOT instead of personal path in check_exception
- `b8ebf56` fix(differential): use NTPERS_ROOT instead of personal path in check_ntstatus
- `62f08d8` fix(differential): use NTPERS_ROOT instead of personal path in check_path
- `196a9d3` fix(differential): use NTPERS_ROOT instead of personal path in check_process_thread
- `9377a75` fix(differential): use NTPERS_ROOT instead of personal path in check_section
- `8263e28` fix(differential): use NTPERS_ROOT instead of personal path in check_sync
- `77cea49` fix(differential): replace personal output paths in probe_path_direct
- `c08bd28` fix(differential): replace personal output paths in probe_process_thread_direct
- `b2f0fe5` fix(differential): replace personal output paths in probe_sharemode_direct
- `423af6f` fix(differential): replace personal output paths in probe_sync_direct
- `1e793c3` tests(unit): cover check_doc_links.py
- `e4e881d` fix(differential): replace personal output paths in probe_virtualquery_direct
- `969a0a0` fix(differential): drop personal input paths from probe_peloader
- `5b96adf` fix(kernel): replace personal output paths in probe_sharemode_direct
- `4f3ec4f` test(syntax): cover arch/x86/nt and record pass/fail matrix
- `11d9830` fix(host): derive temp dir for link step instead of hardcoding user path
- `b96e103` fix(differential): derive WSL staging path in harness instead of hardcoding user path
- `77b5cce` test(tools): add stdlib line-coverage report and CI step
- `1a02f37` test(tools): cover pe_info.py branches and CLI exit codes
- `abbcb5a` test(kernel-host): host build of src/kernel/nt against kshim
- `01cc02d` test(kernel-host): security_test passes on host (41 ntsec, 609 negative)
- `bfceb70` test(kernel-host): registry_test passes on host (10/10)
- `fe97c49` test(kernel-host): i2_test passes on host (7/7)
- `837d294` test(kernel-host): io_test builds and passes on host (11/11 in most runs)
- `42393ab` test(kernel-host): mm_test builds on host; 12 of 14 pass
- `4187bf4` tests(syntax): add linux/ core, memory, error and lock shims
- `a6883e7` tests(syntax): add sched, thread, percpu and debugfs shims
- `b59758a` tests(syntax): add fs and binfmts shims
- `58574d6` tests(syntax): add asm/ shims
- `cd306b6` test(kernel-host): sync_test passes on host (21/21, no leaks)
- `1665553` tests: set executable bit on tracked test scripts
- `7356802` test(tools): unit tests for provenance_scan.py
- `52909dc` tests: assert tracked shebang scripts are executable in the index
- `5304c09` test(syntax): declare inode->i_mode and i_uid_read in fs.h shim

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
- `1f46266` docs(review): mark fixed kernel-nt findings with commit hashes
- `7fcc8ac` docs(status): add chronological progress log grouped by theme
- `90badbd` docs(decisions): design records for deferred kernel-nt findings
- `8e0dfa9` docs(review): mark IO-1 write half and IOCP-2 fixed; link design records
- `a476278` docs(analysis): mark dispatch.c and proc_thread.c identical after port
- `91c034e` docs(review): mark loader L-1 and L-2 fixed
- `d74d410` docs(analysis): drop personal toolchain path from tree-divergence notes
- `1dc5dd3` docs(decisions): records 0011-0027 for open review findings
- `da9f0a2` docs(review): link open findings to decision records 0011-0027
- `71d4653` docs(review): record implementation status and blocked notes for 0003-0027
- `9d58d1b` docs(decisions): name the commit that implements record 0006
- `05124e9` docs(decisions): name the commit that implements record 0010
- `9d993c8` docs(decisions): add index of records with derived status
- `e8707e4` docs(review): mark loader L-3, L-8 and L-11 fixed by commit
- `94ec641` docs(review): record H-2 as partly fixed in kernel-nt report
- `ed1cb1c` docs(review): add fix status per report and mark top findings

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
- `68fe25b` fix(nt/handle): reject handle indices above HANDLE_MAX_INDEX
- `0254b09` fix(nt/registry): zero-init buffers returned to user space
- `b125094` fix(io): declare unused LARGE_INTEGER off only under __KERNEL__
- `14f9fb8` fix(io): declare unused kbuf only under __KERNEL__
- `65ca268` pe_parse: walkers propagate data directory errors
- `904b5d1` pe_parse: do not mask RUNTIME_FUNCTION.EndAddress
- `6f58d04` fix(nt/sync): type-check handles in NtWaitForMultipleObjects
- `04ed6a9` pe_parse: check PE32+ optional header size before reading it
- `30b200a` fix(nt/object): unregister freed process and thread in kernel builds
- `1d160e8` fix(nt/io): require FILE_READ_DATA in NtReadFile
- `80b6fea` fix(nt/io): require FILE_WRITE_DATA or FILE_APPEND_DATA in NtWriteFile
- `d25d66a` pe_parse: cap symbol names at 4096 bytes (L-2)
- `0b7921b` fix(nt/iocp): cap packets queued on one completion port
- `c021ca9` pe_parse: linear export walk and binary-search name lookup (L-1)
- `d7adc94` fix(kernel/nt): H-2 quarantine closed handle slots in a FIFO free list
- `83e3349` fix(kernel/fs): K-12 weak NtTerminateProcess fallback returns NOT_IMPLEMENTED
- `13a4357` fix(kernel/fs): K-5 do not load ntdll from /tmp or a non-root file
- `8eac32c` fix(kernel/fs): K-4 SIGSEGV instead of an error return after exec commit
- `7729288` fix(kernel/fs): K-10 check ntmm_add_region results in the PE exec path
- `419f675` fix(kernel/fs): K-13 check clear_user results in the PE exec path
- `7f11af9` fix(arch/nt/mm): remove KERN_EMERG debug printk from NtProtectVirtualMemory
- `f03a4d7` fix(fs/binfmt_pe_test): include loader/pe_prot.h
- `5c1d29c` fix(fs/kshim): match kernel_read and loader-used members, document TIF flags
- `3679688` fix(fs/binfmt_pe): skip no-op nt_enter_personality under KSHIM_H
- `6efb8b7` fix(nt/kshim): portable stat timestamps (st_atim is glibc-only)
- `efae2d3` fix(nt/mm): demote kshim_mprotect entry trace from KERN_EMERG to pr_debug
- `ea3c20a` fix(arch/nt/mm): demote kshim_mprotect entry trace from KERN_EMERG to pr_debug

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
- `38a2c9b` docs: update Paths and environment for the path cleanup
- `e5a9560` docs(phase2): map ARCHITECTURE.md components to implementation status
- `6836cb5` docs(readme): add test commands, docs index, CI badge and current state
- `0275eaf` docs(development): correct unit test module and test counts
- `1d60551` docs(contributing): fix dead anchor and ntstatus regeneration step
- `abde759` docs(known-issues): correct review totals and mark fixed findings
- `6d70de6` docs(tools): ntstatus_table.inc is tracked, not generated-only
- `a068346` docs(differential): use NTPERS_ROOT instead of personal paths in DUAL_TARGET
- `ee8f712` docs(vm): replace /path/to placeholder with NTPERS_ROOT in README
- `d923325` docs(differential): document NTPERS_ROOT, staging dir and syntax-check status
- `35d319b` docs(development): document the doc link checker and new test count
- `8326cdc` docs(contributing): add the doc link check to the pre-change checks
- `f4ef1aa` docs(tools): describe check_doc_links.py
- `f067b8b` docs(development): list check_doc_links.py among unit-tested scripts
- `3374621` docs(syntax): record shim results and remaining failures
- `793bd6b` docs(vm): describe the vm scripts, their dependencies and known problems
- `69fb2bc` docs(vm): list redundant vm and guest scripts for cleanup
- `7643139` docs(kernel-host): record host status per module and missing kernel deps
- `47378e9` docs(vm): record the M0 gate order check in CLEANUP.md
- `4ea548b` docs(provenance): generate file provenance inventory
- `3ce3768` docs(syntax): update shim matrix after kshim fixes
- `202920a` docs(known-issues): TIF_NTABI/TIF_NTSUSPEND sources and syntax gaps
- `211801d` docs(syntax): reconcile matrix after fs.h shim; record binfmt_pe_test helpers
- `ec3c426` docs(known-issues): update handle, io, kernel-flag and top-finding status
- `c1a26da` docs(modules): handle free list is FIFO quarantine since d7adc94
- `de9dc38` docs(modules): iocp line refs and queue quota invariant
- `76eb032` docs(modules): io line refs and NtClose definitions
- `d971d6d` docs(modules): mm free-path line refs
- `dfbaa59` docs(modules): dispatch exit path and debug log line refs
- `4869cea` docs(modules): proc_thread kernel stub line refs
- `2640997` docs(phase2): refresh line counts and references in architecture status
- `eb335c1` docs(readme): refresh current state, test counts and syntax result
- `780882d` docs(syntax): record 2026-10-12 re-run and correct TIF and line refs
- `573c89b` docs(kernel-host): record the 2026-10-12 build failure and correct ntstatus and nt_io claims
- `bb13e80` docs(development): correct unit test module and test counts

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
- `985a495` haiku/host
- `5f43f8b` haiku/ntfix
- `acecb07` haiku/paths2
- `797de67` haiku/readme
- `4b19469` haiku/adr
- `bff524f` haiku/exports
- `5779442` haiku/recheck
- `938a70d` haiku/docsfix
- `7e75069` haiku/diff
- `916620d` haiku/adr2
- `06e1d91` haiku/uni2
- `7e3b6df` haiku/linuxshim
- `2aa354d` haiku/vmrev
- `f0daf4e` haiku/kshim
- `b58b682` haiku/prov
- `b023c14` haiku/kshimfix
- `ec49863` haiku/vmfix
- `ff01c73` haiku/stale
