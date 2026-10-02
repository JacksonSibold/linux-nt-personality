# Research Report 2 — PE Loading, Process/Thread Model, Synchronization, Exceptions, Memory, DLL Infrastructure

**Scope:** How PE becomes a first-class execution personality on the Linux kernel: format facts, binfmt design, process/thread/sync/exception/memory semantics, DLL loading, and the precise kernel↔user split.
**Framing constraint (restated):** Not Wine. No POSIX-emulating Win32 shim as the mechanism. Windows ABI is a second personality with its own kernel-supported objects where required. Where the Linux kernel lacks the semantics (dispatcher wait-all, reserve/commit, APCs, debug regs for self), we specify NEW kernel facilities rather than papering over in user mode.

---

## 1. PE32+ format: loader-relevant detail

All offsets/sizes from `winnt.h` (MinGW-w64 headers, MIT — usable directly). Only x64 (`IMAGE_FILE_MACHINE_AMD64` = 0x8664) is in scope for Phase 1; 32-bit PE (`0x14C`) is **rejected at load** with a diagnostic (IA-32-in-AMD64 adds a second exception model (chain-based `fs:` SEH) and paging-mode complications; deferral is a sizeable scope cut, not a risk).

### 1.1 Headers

- **DOS header** `IMAGE_DOS_HEADER` (64B): magic `MZ`, `e_lfanew` (0x3C) = RVA (file offset) of NT headers. Loader must validate `e_lfanew` bounds (< SizeOfHeaders sanity, < file size, page-aligned reads).
- **NT headers**: signature `PE\0\0`, `IMAGE_FILE_HEADER` (20B): `NumberOfSections`, `TimeDateStamp`, `SizeOfOptionalHeader`, `Characteristics` (`IMAGE_FILE_DLL` 0x2000 → not executable as a process image; `IMAGE_FILE_EXECUTABLE_IMAGE` 0x0002).
- **`IMAGE_OPTIONAL_HEADER64`** (magic `0x20B`, 240B) — loader-relevant fields:
  - `AddressOfEntryPoint` (RVA; for DLLs it is the DllMain trampoline).
  - `ImageBase` (preferred VA, typically `0x140000000` for MSVC x64 exes, `0x180000000` for DLLs).
  - `SectionAlignment` (VA alignment, usually 0x1000 on x64), `FileAlignment` (usually 0x200).
  - `SizeOfImage` (total VA size, rounded to SectionAlignment), `SizeOfHeaders`.
  - `SizeOfStackReserve`/`SizeOfStackCommit`, `SizeOfHeapReserve`/`SizeOfHeapCommit` (default 1MB/4KB commit-ish; 1MB reserve/commit for heap).
  - `Subsystem` (2 = WINDOWS_GUI, 3 = WINDOWS_CUI; reject 1=NATIVE, 10=AppContainer variants unless implemented). GUI vs CUI decides console-allocation behavior (dependency on Agent 3's console design).
  - `DllCharacteristics`: `DYNAMIC_BASE` (0x40, ASLR capable), `NX_COMPAT` (0x100), `NO_ISOLATION` (0x200), `NO_SEH` (0x400), `GUARD_CF` (0x4000), `TERMINAL_SERVER_AWARE` (0x8000).
  - 16 data directories (`NumberOfRvaAndSizes`, cap at 16).

### 1.2 Section table → page protections

`IMAGE_SECTION_HEADER` (40B each): `VirtualAddress`, `VirtualSize`, `SizeOfRawData`, `PointerToRawData`, `Characteristics`:
- `IMAGE_SCN_MEM_EXECUTE` (0x20000000), `_MEM_READ` (0x40000000), `_MEM_WRITE` (0x80000000).
- Content hints `CODE`/`INITIALIZED_DATA`/`UNINITIALIZED_DATA` (same bit patterns as EXECUTE/READ/WRITE — decode masks separately; that's not a bug, it's the spec).
- `CNT_UNINITIALIZED_DATA` (0x80000000): mapped as zero pages (BSS), no file bytes.
- Share bits `IMAGE_SCN_GK_RW` (0x00F00000, deprecated, treat as private), alignment bits `IMAGE_SCN_ALIGN_*` (compiler hint only).
- Mapping rule: `prot = (EXEC?PROT_EXEC:0)|(READ?PROT_READ:0)|(WRITE?PROT_WRITE:0)`; file bytes mapped `MAP_PRIVATE` from `page_align_down(PointerToRawData)`; zero-fill tail between `VirtualSize` and page end; the gap between `SizeOfRawData` end and next section is `NOACCESS`.
- **Overlapping sections** (RW data folded over RO data — packers like MPRESS/Themida): Windows permits overlap if at least one side is non-exec. Linux `mmap` with `MAP_FIXED` over a mapped RX range converts it; must emulate: later mapping that overlaps an RX page requires the RX→RWX→split dance. **Loader validation pass must tolerate this** or reject-with-fingerprint (decide per policy; packers also anti-debug — Phase 4 issue).

### 1.3 Base relocations (`.reloc` / `IMAGE_REL_BASED_*`)

`IMAGE_BASE_RELOCATION` blocks: `{PageRVA, BlockSize}`, entries `u16 = (type<<12)|offset12`:
- `ABSOLUTE`(0)=padding, `HIGH`(1), `HIGHLOW`(3, x86 classic), `DIR64`(10, x64), `HIGHADJUST`(7, ARM64-only).
- Apply delta = `actualBase − ImageBase` to `*(u64*)(base+PageRVA+offset)`.
- Absent reloc block + `DYNAMIC_BASE` set + base unavailable = load failure (matches Windows `STATUS_INVALID_IMAGE_NOT_PREFERRED_BASE`→relocate fail).
- Relocated sections must be writable during fixup: `mprotect(PROT_READ|WRITE)` per affected page, restore afterward (COW of a file map, so kernel-side fixup = `copy_present_pages` on first write under `MAP_PRIVATE`; no special handling needed if done in-kernel before any CPU touches it).

### 1.4 Imports (`.idata`)

`IMAGE_IMPORT_DESCRIPTOR[]` (null-terminated): `{OriginalFirstThunk (ILT), TimeDateStamp, ForwarderChain, Name RVA, FirstThunk (IAT)}`.
- ILT entries `IMAGE_THUNK_DATA64`: high bit `0x8000000000000000` = import by ordinal (low 16 bits) else RVA→`IMAGE_IMPORT_BY_NAME{u16 Hint, CHAR Name[]}`.
- IAT: same array laid down in a writable section; loader overwrites with final target address. Windows: pages containing IAT are made RW during fixup then RX/RW per section flags (CFG: some IAT pages become "guard-checked").
- **Hint** is advisory (skip name lookup if hint correct) — we can ignore or use as perf.
- **Bound imports** (`TimeDateStamp != 0xFFFFFFFF`, `IMAGE_BIND_IMPORT_DESCRIPTOR` snapshot form with `SnapByOriginalFirstThunk`, `MaximumTagCount`, Uuid `0xFFFFFFFFFFFFFFFF` for snapshot): **Phase 1 policy: validate and ignore** (resolve normally). Using stale bounds without a Timestamp check against the target DLL's `FileHeader.TimeDateStamp` is a known exploit vector; Windows only trusts bound IAT for system DLLs via loader snaps in debug builds.
- **API sets**: names like `api-ms-win-core-synch-l1-2-0.dll` resolve via the **API set schema** (see §8) — must be handled inside the import resolver, not at file-open time.

### 1.5 Delay-load imports

`IMAGE_DELAYLOAD_DESCRIPTOR` in `.didat`/`.data`: `{Attributes|DllNameRVA, ModuleHandleRVA, ImportAddressTableRVA (IAT), ImportNameTableRVA (ILT), BoundIATRVA, UnloadIATRVA}`. Runtime: `__delayLoadHelper2` (in CRT) calls `LoadLibrary(dll)` then `GetProcAddress` on first call, patches IAT slot. **Entirely user-mode**; zero kernel work beyond `LoadLibrary`+`GetProcAddress` being correct. Failure mode: raises `VcppException(ERROR_SEVERITY_NONFATAL, ...)` / SEH code if DLL missing — replicate the "return 0x1" / raise behavior per CRT flavor (UCRT vs MSVCRT differ; bind to chosen CRT, §14).

### 1.6 Exports (`.edata`)

`IMAGE_EXPORT_DIRECTORY`: `{Base, NumberOfFunctions, NumberOfNames, AddressOfFunctions, AddressOfNames (RVAs to ASCII), AddressOfNameOrdinals (u16→function index)}`; export VA = `AddressOfFunctions[i]`; **if export RVA lands inside the export directory VA range → it's a forwarder** (ASCII string `"DllName.ExportName"` or `"#ordinal"`, possibly `"!"` prefix = load as explicit, `"."` at end = name-only). Ordinal lookup = `Base + i` (or `#ordinal` import → index = ordinal−Base; if `AddressOfFunctions[index]==0` but name table maps → check forwarded-name ordinal trick where `AddressOfNameOrdinals` points past `NumberOfFunctions` = forwarded ordinal). All user-mode loader logic in `LdrGetProcedureAddress`/`GetProcAddress`.

### 1.7 TLS directory + callbacks (`IMAGE_TLS_DIRECTORY64`)

`{StartAddressOfRawData, EndAddressOfRawData, AddressOfAddressOfCallbacks (ptr to null-terminated callback array), AddressOfIndex (ptr to TLS slot index), SizeOfZeroFill, Characteristics (low 4 bits = alignment `IMAGE_TLS_ALIGN_*`)}`.
- Template block = `[Start, End)` + `SizeOfZeroFill` zero bytes; **per-thread, per-module** copy; static `__declspec(thread)` vars compile to `ThreadLocalStoragePointer[Slot]` indirection — `TEB.ThreadLocalStoragePointer` (x64 offset 0x58) is a `PVOID*` array indexed by slot, i.e. **`mov rax, gs:0x58; mov rax, [rax+slot*8]`**. This means the TEB array must exist from the first user instruction of every thread.
- Slot allocation: index from a per-process bitmap (`PEB.TlsBitmap`, 64 slots, then `TlsExpansionBitmap`/`TlsExpansionSlots` for >64). Windows allocates the **main exe's** index in the kernel (`PsProcess->TlsBits` era pre-Vista; modern = ntdll `RtlAcquireSRWLock` bitmap ops). Our split: **kernel allocates slot 0 for the exe at execve and lays down its block in the initial TEB** (needed before stub runs; keeps GS invariant trivially true); **loader stub allocates slots for all other modules at load** and for each new thread copies all module templates (array of pointers per thread).
- Callbacks: `TLS_CALLBACK(PEB, DLL_THREAD/PROCESS_...)` — wait, process-start form is `PVOID Reason` (0=ATTACH,1=DETACH; Vista+ signature), called **before entry point / before DllMain**, one set per module **in load order**; detach callbacks at process/thread exit. Ordering subtlety: DLL callbacks run before dependent DLLs fully initialized? Windows order: system DLL callbacks, then per-module in InLoadOrder with the exe last? Actual documented-by-behavior order: EXE TLS callbacks before any DllMain; DLL TLS callbacks during that DLL's process-attach pass, before DllMain of that DLL... **This is a compatibility trap area** — resolve empirically on real Windows VM (test matrix §17) and encode results into a conformance test.

### 1.8 `.pdata` — exception directory (x64 table-based SEH)

Sorted `IMAGE_RUNTIME_FUNCTION_ENTRY[]` in `.pdata`: `{u32 BeginAddress, u32 EndAddress, u32 UnwindInfoAddress}`. **Low 2 bits of `EndAddress` are special-entry flags** (0=normal, 1=epilog-in-function, 2=special handler entry e.g. JIT-provided sentinel) — table search masks them.
`UNWIND_INFO`: `{Version(1):3, Flags:5 (UNW_flag_EPILOG 0x1, UWIND 0x2, CHAININFO 0x4, EHANDLER 0x8, EHANDLER3 0x10), SizeOfProlog, CountOfCodes, FrameRegister:4, FrameOffset:4, UNWIND_CODE[CountOfCodes], (ChainInfo), (ExceptionHandler RVA)}`; codes `(offset, opcode)` with `UWOP_PUSH_NONVOL 0, ALLOC_SMALL 1, ALLOC_LARGE 2, SAVE_NONVOL 3, FRAME_OFFSET 4, ... 0x11 PUSH_MSR, SET_FPVAR 0x10 ...`. `UWIND` flag = legacy nonstandard unwind (old JITs; rare). `EHANDLER` → `ExceptionHandler` is `_C_specific_handler` (C `__try`) or `__CxxFrameHandler3` (C++ EH) — the handler itself lives in the CRT, OS just dispatches.
All **walked in user mode** by our `RtlLookupFunctionEntry`/`RtlVirtualUnwind`/`RtlDispatchException` (offset into the mapped `.pdata` known from the directory entry; JITs register via `RtlAddFunctionTable` user-mode lists). The kernel only needs to (a) map `.pdata` (it's just a section — RX) and (b) record its bounds for the load-info block (used for security checks / debug). **No kernel exception logic required** beyond what §6 covers.

### 1.9 Load config directory (`IMAGE_LOAD_CONFIG_DIRECTORY64`)

`{Size, TimeDateStamp, GlobalFlagsMask/Clear/Set, CritSectionDefaultTimeout, DeCommitFree/TotalBlockThreshold, MinorRuntimeVersion, ImageVersion..., SEHandlerList/Count (32-bit only), GuardCFFunctionPointer?, GuardCFCheckFunctionPointer, GuardCFDispatchFunctionPointer, GuardCFFunctionTable/Count, GuardFlags, CodeIntegrity{Flags, Catalog..., Reserved1}, GuardAddressTakenIatEntryTable/Count, GuardLongJumpTargetTable/Count, DynamicValueRelocTable, CHPEMetadataPointer, GuardRFFailureRoutine..., GuardEHContinuationTable/Count, ...}`.
- The Guard* function-pointer fields are **themselves relocated with DIR64 entries** (marked `IMAGE_REL_FLAG...` in newer relocs; actually the loader special-cases: they get relocated like normal relocs — kernel fixup covers it).
- `IMAGE_GUARD_CF_INSTRUMENTED` (0x100) / `IMAGE_GUARD_CF` (0x400) in `GuardFlags`: binary compiled with `/guard:cf`. **Phase 1 policy: parse, validate, do not enforce** (log a warning, `winctl`-sysctl `cfg_mode=0|1|2`). Enforcement = hardware CET (IBT+shadow stack) or software emulation at `__guard_dispatch_indicator` sites; CET path discussed §6/§13.
- `GuardEHContinuationTable`: `/guard:ehcont` — EH continuation tokens (`__CxxFrameHandler4`, VS2019+); only matters to the C++ EH handler in our CRT choice (UCRT ships it — we get it free if we ship UCRT).

### 1.10 Resources, debug dir

`.rsrc`: `IMAGE_RESOURCE_DIRECTORY` tree (TYPE→NAME→LANG) with `RT_MANIFEST`=24, `RT_VERSION`=16, `RT_ICON`=3, `RT_GROUP_ICON`=14, `RT_STRING`=6, `RT_ACCELERATOR`=9, `RT_DIALOG`=5, `RT_RCDATA`=10. Read via `FindResource`/`LoadResource` — **user-mode** parser over mapped `.rsrc` (mapped RWX-ish; it's READ|DATA — fine). Manifest (XML in RT_MANIFEST) feeds SxS (§8).
Debug directory (`IMAGE_DEBUG_DIRECTORY`): `CODEVIEW`(2) RSDS = `{O7Guid, Age, PdbFileName}` — used by debuggers & crash tooling; loader validates & ignores. `VC_FEATURE` (16) = /RTC. `ILCMETA`(11) = .NET.

### 1.11 Kernel-vs-user split for loading (justified)

| Task | Where | Why |
|---|---|---|
| Header/section validation | **kernel** (binfmt) | Address space must not exist if image is malformed; ENOEXEC atomically. |
| Map sections with final prot | **kernel** | execve is the atomic "replace address space" operation; demand paging from page cache is kernel MM. |
| ASLR base choice (MAP_FIXED_NOREPLACE @0x140000000 or random) | **kernel** | Only the kernel sees the hole map + randomization state at this instant. |
| Base relocations | **kernel** | Mechanical delta; atomicity: never expose a half-relocated image; Windows does it in Ldr but we can't trust a not-yet-running stub ordering race. (Keep a `winctl` mprotect path anyway.) |
| Build PEB/TEB skeleton, initial TEB, GS base, static-TLS array slot 0 | **kernel** | Invariant `gs:[0x30]==TEB` and stack-probe `gs:0x28/0x30` fields must hold from the first instruction; `ARCH_SET_GS` at exec is the only pre-runtime place. |
| UTF-16 command line + environment block assembly | **kernel** (from bprm argv/envp) | Available exactly once, at execve; stub shouldn't need a Linux argv ABI. |
| Import resolution (ILT→IAT), api-set + forwarder chains, DllMain, TLS callbacks beyond exe's, delay loads | **user loader stub (winldr.dll / our ntdll)** | Mirrors real Windows (Ldr\* is user-mode; kernel only has NtCreateSection/NtMapViewOfSection); needs filesystem namespace, search order, PATH; keeps kernel attack surface minimal. |
| Export tables, resources, activation contexts | **user** | Above. |
| VirtualAlloc/VirtualFree/commit tracking | **user** | Windows itself: `ntdll!RtlUser*` + kernel Mi\* — but the VAD bookkeeping we need is a user-mode tree over an address space Linux mm doesn't model (§7); the kernel just does mprotect/mmap syscalls. |
| Exception dispatch | **user** | §6: table walk is pure user computation once signals are delivered. |

**The split is defensible on the "genuine ABI personality" framing**: this is exactly the Windows split (kernel = section objects, MM, dispatcher; user = ntdll Ldr), with the single deviation that execve-time work (map/relocate/TEB/params) moves into the kernel binfmt because Linux's `execve` is the process-creation primitive, not `NtCreateUserProcess` + `MapViewOfSection`.

---

## 2. PE as a first-class binfmt (`binfmt_pe`)

### 2.1 What `binfmt_elf` does (reference points, `fs/binfmt_elf.c`)

`load_elf_binary(struct linux_binprm *bprm)`: `elf_validity_checker` → read phdrs (`kernel_read`, backed by `bprm->buf`/pages from `binfmt_elf_readmin`) → `search_binary_handler` recursion for the ELF interpreter (ld.so) → `elf_map` (`vm_mmap` with `MAP_PRIVATE|MAP_DENYWRITE`, `MAP_FIXED` for reloc) per PT_LOAD → `bf_load_elf_headers` → `load_elf_phdrs` → relocations are not done (ELF is ET_DYN-linked at 0) → `SET_PERSONALITY` (`personality(PER_LINUX|...)`, `SET_UNMAPPED_BASE` for 32-bit-on-64) → `setup_arg_pages` (move stack, `vma_shift_exec_stack`, guard page, `RLIMIT_STACK` sizing) → `create_elf_tables` (strings + auxv: `AT_PHDR/AT_ENTRY/AT_SECURE/AT_HWCAP/AT_EXECFN/AT_RANDOM`) → jump to entry (`start_thread` → `RET_IN_USER_MODE` path).

### 2.2 `binfmt_pe` mirror design

```
new file: fs/binfmt_pe.c
struct linux_binprm *bprm  /* bprm->buf has first 1024B; kernel_read for SizeOfHeaders */
load_pe_binary():
  1  read_pe_headers(): MZ, e_lfanew, PE\0\0, Machine==0x8664, Magic==0x20b,
     !(Characteristics & IMAGE_FILE_DLL), DllCharacteristics policy gates,
     subsystem ∈ {2,3}; SizeOfHeaders sanity; NumberOfSections <= 96; bloat check
     -> else return -ENOEXEC (never oops: every read bounds-checked; syzkaller target)
  2  validate_section_table(): monotonic-ish VA, within SizeOfImage, raw ranges within
     i_size (with the file-alignment slack Windows tolerates)
  3  choose base:
     if !(DllCharacteristics & DYNAMIC_BASE): base = ImageBase via
        vm_mmap(MAP_FIXED_NOREPLACE) or fail -ENOMEM on conflict (Windows: reloc
        without ASLR flag also fails; match)
     else: try preferred ImageBase with MAP_FIXED_NOREPLACE first (determinism for
        non-ASLR-aware tools/debug); if taken: elf_mmap-style random base from
        mmap_base randomization extended to the 47-bit space — see base-collision
        analysis below.
  4  map headers: MAP_PRIVATE @base, length page_up(SizeOfHeaders), PROT_READ (+WRITE
     briefly for reloc/fixup)
  5  map each section: pe_map() == elf_map() with rounded file offsets; zero-fill for
     VirtualSize>SizeOfRawData (clear_user of tail); overlap handling per §1.2
  6  apply relocs: walk .reloc, per-page mprotect(RW) via get_unmapped_area paths,
     copy-on-write is automatic for MAP_PRIVATE; then restore prot
  7  TLS: if TLS directory present: alloc slot 0 from per-mm bitmap
     (new: mm->pe_tls_bitmap — 64+64 bits, trivial), copy template+zero-fill into an
     anon block, align per Characteristics; will be referenced by
     TEB->ThreadLocalStoragePointer[0]
  8  create PEB+TEB+ProcessParameters+env/command-line: anon mappings, fill UTF-16
     command line (utf8→utf16 from bprm->p argv, quoting per Windows rules), env block
     (Linux environ UTF-8→UTF-16, sorted case-insensitively per Windows convention;
     ACP trap: see §18), write TEB fields: StackBase/Limit (from RLIMIT or fixed),
     ClientId{pid,tid}, TlsSlots/TlsExpansion zeroed, ThreadLocalStoragePointer (per
     §1.7), ProcessEnvironmentBlock, GS invariant
  9  initial stack at top (setup_arg_pages equivalent): Windows entry gets no args;
     the stub is entered with rsp = top; put win_exec_info ptr in rcx (Windows:
     RtlUserThreadStart gets PEB ptr in RDX/RCX? Actual Windows ABI: initial thread
     entered with RCX = PEB pointer, RSP = stack) — we enter our **loader stub**
     (installed from a fixed, signed path on the personality's root — see 2.4) with
     RDP = win_exec_info; stub completes load (imports etc.) then calls entry
 10  arch_prctl-style GS setup: set task->thread.gsindex/gsbase = TEB (kernel-side
     equivalent of set_user_gsbase(); same code path used by ARCH_SET_GS —
     `x86_fs_gs` `arch_prctl(ARCH_SET_GS)` internals, `wrmsrl(MSR_GS_BASE)` deferred
     via `thread.in_ia32...` — concretely: call `set_user_gsbase(current, teb)` —
     real function in arch/x86/kernel/ptrace.c)
 11  mark mm: mm->context pe flag (new MMF_PE bit in mm->flags? MMF_ bits exist —
     `set_bit(MMF_PE_IMAGE, &mm->flags)`) used by: fork denial, exec-of-ELF-of-PE
     (allowed: execve clears state), core dump, /proc/pid/maps formatting
 12  SET_PERSONALITY(PER_PE) (new personality bit) → per-syscall policy checks
```

**Base-collision analysis**: Linux x86-64 default layout (`randomize_va_space=2`) puts `mmap_base` at `TASK_SIZE − max(128MB gap, stack_guard) − random` (top-down allocation in `get_unmapped_area`/`vm_unmapped_area`), heap via `brk` near the ELF data of *our* stub (irrelevant — the PE process has no ELF), stack near top. The 47-bit space has ~4881GB of unmapped room below 0x7Fxxxxxxxxxx; **0x140000000 (5GB) is a hole in practice** — first-fit `MAP_FIXED_NOREPLACE` succeeds >99.9% of the time. On collision (huge `RLIMIT_AS` ELF-less... rare: vDSO/vVAR at top, `[stack]`, `[heap]` if a PE process ever brks), fall back to randomized `MAP_FIXED_NOREPLACE` probe loop (same primitive bionic `linker` uses). Windows-visible consequence: apps that hardcode `0x140000000` with a reloc-less image = broken on real Windows too; apps with relocs are base-agnostic. `mmap_min_addr` (65536) doesn't collide. **Risk to retire early**: confirm nothing in Linux places mappings at 5GB (KASLR is kernel text; `vdso` is top; hugetlbfs default mount maps anywhere but only on request).

**Section alignment > page size**: `SectionAlignment` may be 0x10000 or 0x200000 on x64 for some images (rare; .NET single-file). Rule: map whole aligned pages, pad — VA gaps between sections are `NOACCESS`; `SizeOfImage` rounded to SectionAlignment; `get_unmapped_area`'s alignment arg (`get_unmapped_area(file, 0, size, 0, ...)` then `MAP_FIXED_NOREPLACE` at returned VA handles >page alignment natively).

**Demand paging from page cache**: sections are `MAP_PRIVATE` file-backed → `do_read_fault`→`filemap_fault` → page cache, shared across concurrent instances of the same PE — **a genuine win over Wine** (text pages dedup system-wide exactly like ELF and like Windows image-section sharing). File mutation mid-run: Linux holds the inode (old pages keep serving) vs Windows `STATUS_IN_PAGE_ERROR`/sharing-violation semantics → divergence, accepted (§18). `MAP_DENYWRITE` → `deny_write_access()` already prevents write-to-running-image — closer to Windows than plain ELF loading realizes (ELF clears it post-exec! `bprm` keeps it until `flush_old_exec`... actually `exec_mmap`→`set_binfmt`... the `bprm->removed_by_reopen`... — detail: `deny_write_access` persists for the life of the `vma->vm_file` for shared mappings of executables? For ELF the count is dropped in `begin_new_exec`. For PE keep it held — new flag or keep `vm_file` open w/ write denied. Resolving experiment: hold `struct file` ref in `mm->pe_sections` list.)

**binfmt registration**: `module_binfmt(&pe_format)` with `pe_format.load_binary = load_pe_binary`; `CONFIG_BINMISC`-style Kconfig `CONFIG_PE_EMULATION` → but we ship as an **in-tree Ubuntu kernel patch series**, plus `FS_M`-level mount policy: refuse `S_NOSUID`? `bprm_fill_uid`/setuid semantics for PE: **disable suid PEs** (Windows has no setuid; return `-EPERM` if uid changes pending). `AT_SECURE` analog → win_exec_info flag.

### 2.4 Loader stub installation

Stub (call it `winldr`) is a **user-space PE-free ELF shared object** the kernel maps? Two options:
- (a) ld.so-style: kernel maps an ELF `.so` (our `winldr.so`) into the new PE address space (like `load_elf_interp` does for ELF interpreters) and jumps to it with `win_exec_info`. Pro: reuses `load_elf_interp`, no exec-time path lookup races... Con: the stub is ELF while the image is PE — philosophically fine (Windows' own initial-mode transition is similar; the stub is part of the OS, not the app), but mixing ELFs into "PE-only" address spaces complicates §7 hole bookkeeping.
- (b) Stub is itself a **signed PE DLL** (`winldr.dll`) placed at a deterministic kernel-known path; kernel maps it via the same PE mapper (self-hosting: proves the mapper) and jumps to it. **Chosen**: dogfoods the PE path, keeps address spaces pure, lets the stub be updated as a normal OS package; integrity = kernel-side `MODULE_SIG`-style: distro key signs the file, kernel verifies hash against `winldr.dll`'s Authenticode (Authenticode verify in kernel = new facility; Phase-1 fallback: dm-verity/IMA root-of-trust of the path + `deny_write_access`). Authenticode (WIN_CERTIFICATE in `.data`/SECURITY directory) verification in kernel is listed in §13-new but not on the critical path.

---

## 3. Windows process creation semantics

### 3.1 Windows model (what must be observable)

`CreateProcessW → kernel32!CreateProcessInternalW → ntdll!NtCreateUserProcess`: **one syscall** creates process + initial thread, both suspended. Kernel: `PspCreateProcess` (address space, PEB, handle table), `Mi...` maps the **section object** for the image (`NtCreateSection` over the file — this is how Windows shares image pages: the *process references the section*, not the file directly), `PspAllocateProcessStorage`, `PspCreateThread` for initial thread (TEB, kernel stack), passes `USER_PROCESS_PARAMS` pointer; **user-mode post-creation**: parent does `ResumeThread`; child begins in `ntdll!LdrpInitializeProcess` which does imports, TLS, DllMains, activation contexts, then `NtResumeThread`/`RtlUserThreadStart`→entry. Debugger case: `CREATE_SUSPENDED` lets parent `DebugActiveProcess` before any app code. `CREATE_NEW_CONSOLE`/`DETACHED`/`NEW_PROCESS_GROUP` = console-agent dependencies.
`RTL_USER_PROCESS_PARAMETERS`: `{MaximumLength, Length, Flags(1=DELEGATE_DEBUGGER), ConsoleHandle, ConsoleFlags, StandardInput/Output/Error (3 handles), CurrentDirectory{HandlingFileHandle, DosPath UNICODE_STRING}, DllPath, ImagePathName, CommandLine, WindowTitle, DesktopInfo, ShellInfo, RuntimeData, CurrentDirectories, Environment (UTF-16 block), ...}`. All strings UTF-16 `UNICODE_STRING` (Length, MaximumLength, Buffer).
**No fork.** `CreateProcess` = path+cmdline+env+handles. Handle inheritance: `HANDLE_FLAG_INHERIT` bit in the handle table entry survives exec via the *object*, not an fd number. Exit status: process object holds `ExitStatus` (u32) until last close; `GetExitCodeProcess` works after death (object refcounted).

### 3.2 Mapping onto Linux without fork (the design)

- **`CreateProcess`** implemented in our user-mode runtime as:
  1. Open image path (`openat` under resolved search rules) → `NtCreateSection` = **new `winctl` facility** (§13) creating a kernel `win_section` wrapping the `struct file` (`shmem_file_setup`/`alloc_file_pseudo` for page-file sections; `f_path` hold for file sections).
  2. Create `win_process`/gate: `winctl(WINCTL_SPAWN_BEGIN, ...)` returns an anon-inode fd (non-CLOEXEC) carrying spawn descriptor (params blob, std-handle mapping plan, CREATE_SUSPENDED flag). **Suspended-at-first-user-instruction** is guaranteed because `binfmt_pe` maps `winldr` and the stub's *first action* is `winctl_wait_gate(gatefd)` — no app instruction retires before parent calls `NtResumeThread`. (Alternative rejected: `ptrace`-based initial trap — races and slows exec.)
  3. `execve(path, NULL, envp)` — kernel `load_pe_binary` builds PEB/TEB/params (it reads the spawn descriptor from the gate fd attached to `bprm` — pass via `fd 254`? cleaner: `bprm->security`-style `bprm->buf`? Use: **`bprm->pe_spawn = fget(gatefd)` via `prctl(PR_SET_PE_SPAWN_FD)` (new `prctl`, ~40 lines) before execve**; prctl-set value is cleared on execve of ELFs).
  4. Parent maps child std handles: inherited `win_handle`s cross execve because the handle table is rebuilt from non-CLOEXEC fds; `winctl` handle = fd → inheritance is exactly fd inheritance with the CLOEXEC bit mirroring `SetHandleInformation(HANDLE_FLAG_INHERIT)`.
  5. `NtResumeThread(initial)` → gate fires.
- **Why fork() is a poor fit (analysis, as demanded):** mechanically a COW copy *would* reproduce user state, but (a) PEB.ClientId/UniqueProcessId, TEB.ClientId, every `win_process`/`win_thread` handle's `struct pid` links, and gate/spawn kernel objects would be stale → would need `copy_process`-time fixup of **user memory from kernel** (patching PEB after COW): hostile to KASLR-safe copy semantics and to the "kernel never writes userspace except at exec" invariant; (b) Windows-visible state (thread *IDs*) cannot be preserved without cloning the whole thread set — `fork()` of a multithreaded Windows process isn't Windows behavior at all; (c) `atfork` handlers don't exist; (d) races: another CPU can't mutate during `copy_page_range` but user-mode loader state expects a static process. **Decision: `fork`/`vfork`/`clone(SIGCHLD)` from a PE process return `-ENOSYS`** via a `MMF_PE` check in `kernel/fork.c::kernel_clone` (or cleaner: personality `PER_PE` check in `SYSCALL_DEFINE0(fork)` wrapper). Only `CreateProcess` (path-based) and our `NtCreateUserProcess`-equivalent exist. Child processes of PE processes must be PE *or* Linux ELF (execve of ELF by a PE app happens via `CreateProcess("...cmd.exe")` → our `cmd.exe` shim; ELF interop is Agent-1/3's call; kernel supports: execve clears `MMF_PE`).
- **Job objects**: `CreateJobObject` → `winctl` job = cgroup v2 cgroup (`/sys/fs/cgroup/jobs/<id>`) + kernel object tracking member `win_process` list; `JOB_OBJECT_LIMIT_ACTIVE_PROCESS` (member counter w/ deny-on-attach), `PROCESS_MEMORY` (`memory.max`), `KILL_ON_JOB_CLOSE` (kernel: last-handle-close → `cgroup_kill`/SIGKILL members — cgroup has no auto-kill; `winctl` implements via `css_get` ref on job struct, `do_exit` hook → kill), nested jobs (Windows: child inherits membership) = cgroup nesting. UI restrictions (clipboard, desktop) = console-agent territory. `SetInformationJobObject` per-type switch. **Class: NEW kernel object, built on existing cgroups.**
- **Exit codes**: `ExitProcess(u32)` → `do_exit` must store the status in the `win_process` object → new facility: `winctl_process_exit_status` set in the exit path via `release_task` hook (or stub writes to a shared kernel object before `exit_group` — user-mode-first: stub `winctl(WINCTL_SET_EXIT_STATUS, code)` then `exit_group`; kernel object also updated on signal death: `128+?` no — Windows maps to `STATUS_...` codes; our `winctl_wait` synthesizes). `GetExitCodeProcess` after death reads the stored u32. `pidfd`-like: our `win_process` object is a **waitable dispatcher object** (§5) whose signaled state = process exited.

---

## 4. Windows thread semantics

### 4.1 `NtCreateThreadEx` → `winctl` + `clone3`

`CreateThread(dwCreateFlags, ...)` → `RtlCreateUserThread`/`NtCreateThreadEx`: kernel builds TEB (`PspAllocateThread`: kernel stack = `guard_stack`-like, `KTHREAD` with `SuspendCount`, alertability `UserAffinity`...), entry point is always `ntdll!RtlUserThreadStart` (TEB+`StartAddress`/`Parameter` stored at `TEB+0x358/0x360` x64 — offsets from public dbg symbols; verify by experiment), user stub calls `NtCallbackReturn`, CRT sets up SEH-less stack (`gs:0 = 0`), calls `ThreadProc`, `ExitThread(ret)`.
**Our mapping**: one Windows thread = one Linux `task_struct` created with `clone3` **by the loader stub** (fork denial applies only to *app*-initiated fork). `CLONE_VM|CLONE_THREAD...` — use raw `clone3` with no `CLONE_SETTLS` (FS stays stub-owned; **GS is the TEB**: stub does `arch_prctl(ARCH_SET_GS, teb)` post-clone — permitted since kernel uses `swapgs` shadow; verify `paranoid` entry: `cpu_current_base` restore works for any user GS value; experiment: PE thread doing GS vs syscalls+signals, §17). The new task must inherit `MMF_PE` via its mm (shared). Then `winctl_register_thread(tid, teb)`.
**TEB layout fidelity** (x64 offsets, must match MSVC/glibc-independent binaries): NT_TIB: `ExceptionList 0x00 (x64: always 0 — table-based SEH doesn't chain here, but `RtlpExecuteHandlerForException`... keep writable for compat APIs), StackBase 0x08, StackLimit 0x10, SubSystemTib 0x18, FiberData 0x20, ArbitraryUserPointer 0x28, Self 0x30`; `TLS: ThreadLocalStoragePointer 0x58? (verify: widely cited 0x58), TlsSlots 0x600? (verify: `TEB64`: `LastErrorCode? ...`)** — **Action: derive the full table from compiling against public headers + empirical probe binaries; do not trust any single web offset table.** MSVC `/GS` cookie: gs:0x28 on x64? — conflict with `ArbitraryUserPointer` (that collision is *real* and is the reason `TlsAlloc`-era docs warn on x64; some references give `0x17E00`... resolve: **run compiled `int main(){} /GS` and disassemble `__security_init_cookie`/cookie reads** — 10-min experiment). Known-solid: stack-probe `gs:0x30`?? No — stack guard check uses `gs:[StackLimit]` and cookie `gs:[0x28]`; pin all via §17 test-1.
Fields we must implement in our headers & kernel init: above + `ClientId 0x40 {UniqueProcess, UniqueThread, SubSystemTib?}` (x64 ClientId at 0x40), `ImageAddressInfo? 0x48`, `KernelCallbackTable 0x58`?? — **offset uncertainty is exactly why we build `teb_probe` corpus** — `ThreadErrorMode?`, `LastErrorValue`, `LastStatusValue`, `TlsExpansionSlots`, `FlsData`, `CriticalSectionCount?`, `SrvServerBuffer?...`.
**GS = TEB** implies every Win64 binary's `/GS`, SEH `__security_check_cookie`, and stack probes read *our* memory layout → the TEB is an **ABI contract**: ship our exact field offsets in `docs/teb-peb-abi.md`, freeze them, version via `PEB.CompatibilityBlock`? Windows apps never version-check TEB → **get it right once**.

### 4.2 Suspend/Resume counts — the real gap

Windows `KTHREAD.SuspendCount` (u8, max 255? 63?): thread does not run while >0; `NtSuspendThread`/`NtResumeThread` return previous count. `CreateThread(CREATE_SUSPENDED)` = born with 1.
Linux has **no per-task gate other than `STOP` (SIGSTOP group-stop)**. Options analyzed:
- (a) `ptrace`-style stop: hijacks group-stop semantics, breaks debuggers (Windows debugger also uses suspend — collision), leaks via `wchan`. Rejected.
- (b) **Futex gate in stub**: thread's first user code blocks in `winctl_wait_gate` on its TEB-embedded count — but "before executing any app instruction" again requires our stub as trampoline (it is — `RtlUserThreadStart` equivalent), so this works for app-created threads: `NtSuspendThread` on a *running* thread, however, must stop it **at any instruction** (apps use it to freeze a thread to change priority/affinity/debug) — user-gate can't stop an arbitrary loop.
- (c) **NEW kernel facility — `winctl_suspend(tid)`**: add a per-PE-task gate in `task_struct` (or store in the `win_thread` kernel object): implemented as `signal->group_exit?` no — cleanest existing hook: `get_signal()` path + `schedule()` preemption check? A *preemptive* arbitrary-instruction stop needs kernel cooperation at return-to-user: new TIF flag `TIF_PE_SUSPEND` checked in `exit_to_user_mode_loop` (arch/x86 entry) → if set, `do_task_dead`? no — block: `win_thread_suspend_block()`: sets state, adds to per-thread `wait_queue`, `schedule()`. Resume clears flag + wakes. ~150 lines: one TIF bit, one `exit_to_user_mode_loop` case, two `winctl` ops, refcounted per-thread object. This is unavoidable if SuspendThread is to be honest; **without it, apps that suspend-while-modifying get corrupted state — silent misbehavior is worse.**
- **Critical interplay**: suspend a thread that holds the loader lock or `RtlpHeapLock` = deadlock on resume? Windows has the same flaw (documented: don't suspend threads arbitrarily) → fidelity = implement faithfully, warn in docs. `TerminateThread` = kernel kill: `send_sig` of a new internal exit to single tid (`do_exit` of one task — `kernel/exit.c` has per-task exit via `de_thread`? no that's thread-group leader; kill one thread: `zap_other?` — implement: `winctl(TERMINATE_THREAD)` sets `TIF_PE_EXIT_ONE` → `exit_to_user_mode` → `do_exit(SIGKILL-equivalent internal)`; **must run thread detach? Windows TerminateThread skips TLS callbacks & SEH (documented)** → faithful: kernel kill, no user unwinding. Trap: leaks (honest to Windows!).

### 4.3 Priorities, affinity, fibers, TLS

- **Priority classes** (`REALTIME/HIGH/ABOVE/NORMAL/BELOW/IDLE`) × thread deltas (11 levels +2..−2, TIME_CRITICAL/HIGHEST): map → (nice −2..19 for below-normal classes via `setpriority`) + (`IDLE` = nice 19 + `SCHED_BATCH`? Windows idle = below everything, above nothing — SCHED_IDLE on Linux is closest but SCHED_IDLE ≠ nice-19; **choose SCHED_IDLE for IDLE class, SCHED_BATCH? no**) + REALTIME via `sched_setattr(SCHED_FIFO/RR)` **requires `CAP_SYS_NICE`** → distro policy: allow via ambient? — document: `REALTIME` fails gracefully (apps fall back; games assume it — risk). 0x100..0x400 deltas. `SetThreadPriority(THREAD_MODE_BACKGROUND_BEGIN)` = `SCHED_IDLE`+io nice → `ioprio_set(IOPRIO_CLASS_IDLE)` — direct. Windows priority *boosting* (I/O, wait-exit +1..+15, scheduler decides) — not reproducible; accept as fidelity gap (list §18).
- **Affinity**: `SetThreadAffinityMask` (u64 per group) + **Processor Groups** (>64 CPUs, Win64 groups of ≤64) → `sched_setaffinity` with 64-bit mask; group API = our user-mode shim over `winctl` exposing `KAFFINITY` semantics; NUMA node≈group approximation on >64-CPU hosts. `GetSystemInfo.wProcessorsMask` must report per-group u32s — `GetActiveProcessorCount(NtCurrentGroup())`.
- **Fibers**: pure user: `CreateFiber` = reserved stack + saved CONTEXT; `SwitchToFiber` = `swapcontext`-equivalent written in asm + **`ARCH_SET_GS(newFiberTEB)`** (Windows swaps TEB per fiber — confirmed: fibers have own TEB w/ new GS) + update `gs:StackLimit/Base`. No kernel work; no APC (Windows fibers get APCs — QueueUserAPC alertable... our APC §5.5 is on alertable waits incl. fiber switches? Defer fiber+APC combo). `ConvertThreadToFiber` = grab current TEB/stack.
- **TLS slots**: static = §1.7 (kernel slot 0 + stub per-thread array copy from module list — stub keeps `LoadedModules[]` w/ template ptr+len+slot: on thread create allocate array, `memcpy` templates — **callback `DLL_THREAD_START` order: TLS copy before callbacks, Windows-verified**); dynamic `TlsAlloc`: bitmap in **PEB** (`TlsBitmap/TlsBitmapBits`, `TlsExpansionBitmap`) — user-mode over kernel? Windows: bitmaps are PEB-owned, user-mode `RtlSetBits` in shared heap → ours: PEB-resident `RTL_BITMAP` + `InterlockedBitTestAndSet` = lock-free, no kernel object. `Fls*` (fiber-local): PEB `FlsBitmap` + `FlsData` array on TEB — user-mode.
- **`GetCurrentThread`/Process pseudohandles**: `0xFFFFFFFFFFFFFFFE/FFFFFFFF` — trivial in every syscall wrapper.

---

## 5. Synchronization

### 5.1 Dispatcher object model

Windows: `KEVENT (SynchronizationEvent=auto, NotificationEvent=manual)`, `KSEMAPHORE (MaxCount)`, `KMUTANT (SignalState: −1 owned..+N recursive, OwnerThread, Abandoned)`, `KTIMER`, `KTHREAD`/`KPROCESS` (signaled on exit/term), all derived from `DISPATCHER_HEADER {SignalState, ...}` — `WaitForSingleObject` = one object; `WaitForMultipleObjects` handles wait-any **and wait-all** (`bWaitAll`); `KMUTANT` in wait-all = **atomic acquisition of all mutexes in handle order** (`KeWaitForMultipleObjects` AllObjects path w/ undo list) + `WAIT_ABANDONED_0+i` result encoding; thread termination of owner → abandonment. Alertable waits (`WaitForSingleObjectEx(...,TRUE)`) + kernel-mode APC queue + user APCs (`QueueUserAPC`) fire only on alertable waits & `NtTestAlert`.
Timeouts: **absolute = 100ns ticks from 1601-01-01; negative relative**; `0` = poll; `INFINITE`.

### 5.2 What Linux has

`futex` (FUTEX_WAIT_PRIVATE/FUTEX_WAKE on a u32 value): **wait-any on one address only**; `futex_waitv()` (Linux 5.16, `SYSCALL_DEFINE(futex_waitv)`, `struct futex_waitv[]`): **multiple addresses, still wait-ANY-only — wait-all mode was explicitly removed from the design** (kernel commit docs). `FUTEX_LOCK_PI`/rtmutex = ownership+priority-inheritance but single-futex. `pidfd`+`epoll` = process-exit waits. `timerfd`/`POSIX timers` = timers. `io_uring` has multishot wait patterns, no wait-all. **Nothing models mutant ownership/abandonment with wait-all.**

### 5.3 Analysis: wait-all (demanded)

Options:
- **User-mode-only emulation of `WAIT_ALL`** (increment per-object pending counter then wait-any, signal decrements pending for *all wait-all waiters*...): racy against "signal then wait-all should succeed immediately" and breaks mutant mutual exclusion (two wait-all waiters can each grab a disjoint subset → deadlock). No correct pure-user scheme exists for **wait-all over mutants** (classic result; Wine's approach is precisely this hack + known hangs). Rejected as primary.
- **NEW kernel facility — `winobjs`**: implement the 6 real object types as refcounted kernel objects under the `winctl` syscall surface:
```
winctl(CREATE, WT_EVENT|WT_SEMAPHORE|WT_MUTANT|WT_TIMER|WT_PROCESS|WT_THREAD,
       {initial, max, name?, manual?}) -> handle(u32, table-backed)
winctl(SET/RELEASE/PULSE/... )
winctl(WAIT, {handles[N], bWaitAll, timeout_100ns, alertable})
     -> WAIT_OBJECT_0+i | WAIT_ABANDONED_0+i | WAIT_TIMEOUT | WAIT_ALERTED | 0x80..
```
  Internals: per-object `raw_spinlock` + `wait_queue_head_t` + typed state word; wait-all = acquire-with-undo exactly like `KeWaitForMultipleObjects`: for `bWaitAll`, take objects in **sorted handle order** (deadlock avoidance), rollback on contention; return `WAIT_ABANDONED_0+k` if a mutant's owner died. Mutant: `owner_tid`, `recursion` (u32), abandonment → wake all waiters w/ `STATUS_ABANDONED`, and **owner-death detection in `do_exit`** (per-thread `win_thread` object → release owned mutants — real code: hook `exit_signals()`/`release_task`). Timers: `hrtimer` + `timer_settime`-equivalent absolute semantics. Waiters also get **alertable** hook (§5.5 APC).
  Sizing: ~1–1.5k lines, no changes to core kernel structures except the `do_exit` hook. **This is the single most important new kernel object subsystem; it is not Wine-replaceable.**
- Hybrid: implement events/semaphores in user futex space and only mutants/timers in kernel — rejected: wait-all needs uniform acquisition protocol across *all* types (`WaitForMultipleObjects` can mix types).

### 5.4 User-mode-primitive sync (exact Windows split)

- **CRITICAL_SECTION** (`RtlInitializeCriticalSection*`, `RtlpWaitForCriticalSection`): Windows fast path = user atomics (`LockCount`, `OwningThread`, `RecursionCount`, `ContentionEvent` — the `RtlAcquireSRWLock`... `RtlpEnterCriticalSectionContended` → `NtWaitForAlertByThreadId`/futex). We implement over **futex directly** (glibc `pthread_mutex` is a known-correct futex use but has no ownership-recursion-with-abandon... write ours; it's 200 lines). `TryEnterCriticalSection` = atomic `cmpxchg` fast path. Timeout → `WAIT_TIMEOUT` from `SEH`? `RtlpWaitForCriticalSection` raises `STATUS_ABANDONED_WAIT_0`? (abandon semantics only via mutant — CS has no abandonment; owner-death = undefined hang on Windows too, unless `RtlInitializeCriticalSection...` w/ spin count + debug heap `RtlpDeadlockMonitor` — stub.)
- **SRW locks** (64-bit `SRWLOCK` = single word: exclusive waiter bit, shared bits, `+WAKE...`): user-mode futex-based port of the *documented structure* (Windows internals: `PsGetProcessWow64Process`... `EX_RESOURCE`-less: `srwlock.c` layout public via symbols: bit0 exclusive-owner-waited, bit1 shared-waited, bits[3:2] wake/upgrade, bits 63:4 = shared count / owner... **write from the observable structure, verify against MS headers**); needs "wake by address" = `futex(FUTEX_WAKE, addr)` — direct. Windows's `NtWaitForAlertByThreadId` (used for SRW shared-wait) has alertability → our kernel object wait path.
- **Condition variables** (`SleepConditionVariableCS/SRW`, `WakeConditionVariable`): Windows internals = **user-mode pushlock + wait-list + `NtWaitForAlertByThreadId`** → futex wait list in user, kernel only via winobjs for timed/alertable. Straight build.
- **`WaitOnAddress`/`WakeByAddressSingle/All`**: `futex(FUTEX_WAIT/FUTEX_WAKE)` 1:1 — `WaitOnAddress(addr, cmpval, size, timeout)`: **must re-check value atomically** — futex *requires* the u32 value to still equal cmp at wait else `EAGAIN` — exactly the needed semantics (compare in kernel = the uaddr read inside futex). Size 1/2/4? Windows takes u8/u16/u32/u64: normalize into u32 slots (futex is 32-bit; for 64-bit values use two-slot protocol or `FUTEX_...`? — use `FUTEX_WAIT` on the u32 pair emulation trick? Simpler: for size 8, `FUTEX_WAIT_BITSET`? no. **Original bit: 64-bit WaitOnAddress needs a u64 futex op** — implement with a per-address spin+double-check + 32-bit futex on high word: standard; or add `FUTEX64` (there was an upstream `FUTEX64` proposal, stalled) — **mark as original implementation; Phase 2, user-mode double-check is sufficient** (compare u64 in user, then `FUTEX_WAIT` on low word after re-reading — race window closed by retry loop: value must equal at wait, kernel u32 recheck on low 32 catches most changes, full u64 recheck after return = correct wait-any semantics with rare spurious wakes).
- **Keyed events** (`NtReleaseKeyedEvent/NtWaitForKeyedEvent` — used by CRT heap, `TerminateThread` cleanup, `FlsFree`, condition-variable impls on Windows): kernel = `KEVENT`-per-(ptr,key) with queues on both sides, no close. Implement inside `winobjs` as `WT_KEYED_EVENT` with an open-addressed hash `{object, key}`→wait queue — **~100 lines once winobjs exists; required for our CRT's correctness** (UCRT heap... UCRT uses its own locks — but many apps' statically-linked MSVCR use keyed events: **needed**).
- **Slim events / one-shot**: `SYNCHRONIZATION`... covered.
- **Spinlocks** (`ExAcquire...` kernel-only — user: `RtlAcquire...`), `Interlocked*` = `LOCK`-prefixed atomics = compiler intrinsics, nothing OS.

### 5.5 APCs / alertable waits

`QueueUserAPC(func, hThread, dwData)`: delivered when thread enters an **alertable wait** or calls `NtTestAlert` (`SleepEx(0,TRUE)` trick). Linux: nothing (timer signals via `SIGEV_THREAD` = userspace — wrong semantics: Windows APCs run on the target's stack with its locks; SIGRT-based hacks break loader lock).
**NEW kernel facility (inside winobjs)**: per-thread kernel APC queue (`list_head` in `win_thread`), `TIF_PE_USER_APC` → in `exit_to_user_mode` if `Alertable && queue empty`? no — APC delivery happens when thread parks in `winctl(WAIT..., alertable=TRUE)`: pop → build a frame on the user stack via... kernel writing user stack = exactly what Windows `PsExecuteUserApc` does (constructs a user-mode call frame). ~200 lines incl. stack probe on target. **Phase 3.** Compatibility impact assessment: rare in apps but present in: `TerminateThread`-adjacent cleanup, some anti-cheat, `ReadFileEx`/`WriteFileEx` (APC-based I/O → our async file I/O story), `SetEndOfFile`... — `ReadFileEx` completion routines are **documented as APC** → any app doing async file I/O needs it. Accept the work.

---

## 6. Windows exceptions on x64

### 6.1 The Windows mechanism (recap of what we must present)

Fault in kernel → `KiDispatchException` builds `EXCEPTION_RECORD {ExceptionCode (STATUS_...), ExceptionFlags, ExceptionRecord chain, ExceptionAddress, NumberParameters, ExceptionInformation[15]}` + `CONTEXT` (full 1232-byte `CONTEXT64`: Dr0-7 0x00-0x38, ContextFlags @0x30 (10007=CONTROL,10002=INTEGER,10007|0x10=EXCEPTION(0x1000F),FULL=0x10017... — pin exact: `CONTEXT_AMD64=0x100000`, CONTROL=0x100001, INTEGER=0x100002, ... 0x400000 EXCEPTION =0x100040? **no**: CONTROL=0x100001, INTEGER=0x100002, CONTROL=0x100010? — the real constants: `CONTEXT_CONTROL 0x00000001`? on AMD64: `CONTEXT_AMD64 0x00100000`, `CONTEXT_CONTROL (CONTEXT_AMD64|1) = 0x100001`, INTEGER=0x100002, FLOATING=0x100004, SEGMENTS=0x100010, EXCEPTION=0x100040? EXCEPTION=(CONTEXT_AMD64|0x40)=0x100040, FULL=0x10010F? FULL=CONTROL|INTEGER|CONTROL|EXCEPTION... verify-all-in-`ctx_probe` test) → user stack frame → `KiUserExceptionDispatcher(EXCEPTION_RECORD*, CONTEXT*)` (ntdll) → `RtlDispatchException`: **1st pass** vectored handlers (`AddVectoredExceptionHandler` list) → **2nd pass** SEH table walk from faulting frame: `RtlLookupFunctionEntry(Rip, &Imagebase, &Establisher)` in `.pdata`/`RtlAddFunctionTable` lists → `handler(Record, EstablisherFrame, ContextRecord, DispatcherContext)` returns `ExceptionContinueExecution(0)/ContinueSearch(1)/NestedCall(2)/CollidedUnwind(3)` → **3rd pass** unhandled → `NtRaiseHardError`/WER or die with `STATUS_...` exit code (e.g. `0xC0000005`). **Unwinding for `__finally`**: `RtlUnwindEx(TargetFrame, TargetIp, ..., 1=EH_UNWINDING): same walk calling handlers with `EH_UNWINDING(2)|EH_EXIT_UNWIND(4)|EH_TARGET_FRAME(0x20)` — handler (`_C_specific_handler` / `__CxxFrameHandler3`) executes `__finally`/destructors, may `ExceptionContinueExecution` with modified CONTEXT. `__try/__except` codegen = compiler (scope records) — our shipped CRTs provide them; OS only dispatches.
`EXCEPTION_...` codes: ACCESS_VIOLATION `0xC0000005` (params: [access 0R/1W/8DEP, addr]), ILLEGAL_INSTRUCTION `0xC000001D`, INT_DIVIDE_BY_ZERO `0xC0000094`, STACK_OVERFLOW `0xC00000FD`, BREAKPOINT `0x80000003` (param: address), SINGLE_STEP `0x80000004`, DATATYPE_MISALIGNMENT `0x80000002`, PRIV_INSTRUCTION `0xC0000096`, ARRAY_BOUNDS_EXCEEDED...

### 6.2 Our conversion design: signal → EXCEPTION_RECORD (user-mode, in winldr)

winldr installs handlers via `sigaction` on **all** exception-mappable signals at process init; `SA_SIGINFO|SA_ONSTACK` (sigaltstack per thread — set in thread bootstrap so stack-overflow `SIGSEGV` can run the *probe*).
Mapping table (per fault type; `CONTEXT` reconstructed from `ucontext_t` `mcontext.gregs[REG_RIP/RSP/RAX/... + FPSTATE via REG_? ]` — need FXSR/XSTATE: use `sigcontext.__fpregs_mem` and `arch_prctl(ARCH_GET_XCOMP_...)`/`PTRACE_GETREGSET`? user-side: `ucontext->uc_mcontext.fpregs` exists (glibc exposes `struct _libc_fpstate`) — verify ymm via `XSAVE` area pointer (`fpregs` extended; on modern glibc `uc_mcontext.fpregs` = pointer to `user_i387_struct`; YMM0-15 are in `__glibc_reserved`... **known Linux shortcoming: `ucontext` does not expose full XSTATE (only legacy FP + no AVX beyond XMM?) — actually since 4.14, `sigcontext.__reserved` holds full `XSAVE` incl. AVX — read via `restore_xstate` layout; experiment needed**); plus **GS segment base** for the CONTEXT's `SegmentGs` and to reach TEB for handler dispatch.

| Source | Windows | Notes |
|---|---|---|
| `SIGSEGV` `SEGV_MAPERR/ACCERR` @`si_addr` | `0xC0000005` | params[0]: **R/W not in siginfo** — see below |
| `SIGBUS` (BUS_ADRERR) | `0xC0000005` (or `IN_PAGE_ERROR`) | file-map SIGBUS |
| `SIGFPE` | `0xC0000094` INT/`0xC000008E` FLOAT divide; check `si_code` FPE_INTDIV vs FLTDIV; INVALID→`0xC0000090` | x86 SIGFPE `si_code`s give the class — good |
| `SIGILL` ILL_ILLOPN/ILL_ILLADR | `0xC000001D` | |
| `SIGTRAP` TRAP_BRKPT | `0x80000003` param=RIP−1 | int3; `EXCEPTION_BREAKPOINT` addr points *at* int3 (Windows: address of INT3 = fault rip, and debugger adjusts −1 for ContinueStatus) |
| `SIGTRAP` + `DR6` single-step (TRAP_TRACE) | `0x80000004` | Linux `si_code=TRAP_TRACE` for TF; **DR6 not exposed in siginfo** → kernel facility (§6.4) |
| `SIGBUS` `MCEERR*` | `0xC0000409`? no — `BUS_MCEERR_AR`→`STATUS_NONCONTINUABLE_EXCEPTION`? keep: report fatal | |
| misaligned `movaps` #AC | Linux: SIGSEGV (`si_addr`=instr) / SIGBUS? kernel may fix... `0x80000002` | verify |

**The R/W access-type gap (concrete, verified problem):** x86 page-fault error code bit1 = write, but `SEGV_ACCERR/MAPERR` in `siginfo_t` carries no R/W; Windows `EXCEPTION_RECORD.ExceptionInformation[0]` = 0(read)/1(write)/8(DEP-execute). Many apps *branch on it* (JIT page-protection schemes — e.g. .NET `PROTECT_VIRTUAL` writes, V8 `wasm`: `1W` vs `0R` decides remap vs trap; `IsBadWritePtr`-style SEH probes rely on write-flags... `0R` faults also = probing). Solutions:
- (a) Decode the instruction at `RIP` (`ndisasm-lite` in winldr: ~500 lines for common stores; must also resolve RIP-relative operand → faulting-address = decoded access, and segment-less x64 → mostly deterministic) — **V8/JSC do exactly this in their crash handlers**; wrong for DEP (`DEP` param 8 vs exec faults — `si_code` can't tell either; instruction-decode covers).
- (b) **NEW kernel facility (tiny, honest): extend x86 `force_sig_info`→`SIGSEGV` with `si_code`-carrier**: new codes are ABI-frozen, but `siginfo._sifields._sigfault.si_addr_lsb` exists (`SF_ADDR_LSB`); **better: add a new `SEGV_ACCRW`**? cleanest upstream-shaped: `siginfo` has `__bindgen`... use `si_addr` + a **new `si_code`**: `SEGV_MAPERR/SEGV_ACCERR` plus `si_addr_lsb`... none carry R/W. Proposal: distro patch adds `union { ... } si_fault_ext` with `si_ferror` = page-fault error code bits (16 bits of room; error-code bit 1,2,4,5 = WR/PROT/USER/I-D). **Do this** — original work, small, testable (`t_rw_siginfo`): if we refuse to patch, (a) is mandatory anyway for DEP-classification. Resolve by building (b) and falling back to (a) if policy forbids kernel changes? No — kernel changes are first-class here. Ship (b), keep (a) for DEP-execute (`PF instruction-fetch: error bit 4=I-D` — (b) also solves DEP).
- (c) **`EXCEPTION_ILLEGAL_INSTRUCTION` vs CET/`#CP`** later.

**Dispatch flow**: handler (sigaction) runs on sigaltstack → build `EXCEPTION_RECORD`+`CONTEXT` on a saved *normal* stack region (Windows: `KiUserExceptionDispatcher`'s stack = current thread stack; a SEGV due to stack exhaustion can't run our frame → Windows handles with `STATUS_STACK_OVERFLOW` special: it commits + marks stack non-growable and lets handler run ~8KB of reserve; our design: preallocate 32KB of "rescue stack" per thread via sigaltstack, and when the fault is inside the guard of a *growing* stack → §6.3) → `RtlDispatchException` (vectored → table walk) → outcome:
- `ExceptionContinueExecution`: `sigreturn` with **modified CONTEXT** (write back into `ucontext_t` — RIP/RSP/GP/FP + `fpregs`) — works, that's exactly how JIT guard-region tricks function;
- `ContinueSearch` all the way: Windows unhandled → WER → `NtRaiseHardError` → terminate with the STATUS code: our → `winctl(FATAL_EXCEPTION, code)` → kernel stores exit status (§3.2) + `kill(self, SIGKILL)`-ish (must *not* produce a core dump as "signal death" for ELF tools? decide: report 128+? no — Windows-exit-code: our process object records it; `winctl` waiters get it; kernel-side exit status = raw u32 via `winctl(SET_EXIT_STATUS)`).
- **Nested exception during dispatch** → `STATUS_NONCONTINUABLE_EXCEPTION` (0xC0000409... `__fastfail`/`STATUS_STACK_BUFFER_OVERRUN 0xC0000409` used by /GS failure → `__fastfail(5)`: **`int 0x29` → Linux SIGSEGV? kernel: `ldt_gdt`? `0x29` is unallocated gate → SIGSEGV/ILL** → map `int29` → `SIGILL` w/ `si_code=ILL_ILLOPC`? Detect in handler: RIP-2 opcode `CD 29` → synthesize `0xC0000409` fatal, **not catchable** (Windows `__fastfail` is uncatchable) → straight to kernel exit.

### 6.3 Guard pages & stack growth (demanded)

Windows commit model: TEB `StackBase/StackLimit`; each commit extends `StackLimit` down; touching the page below `StackLimit` (guard-marked) → kernel **commits** one `StackCommitChunk` (0x1000) (or for MSVC large `sub rsp, big` probes: MSVC `__alloca`/large-frame probing walks in 4KB steps so each guard page is touched sequentially; >48KB frames use `__chkstk` walking in page steps — both fine with page-at-a-time commit). Fault *below* the commit beyond `StackReserve` → `STATUS_GUARD_PAGE_VIOLATION (0x80000001)` delivered **once** to the app, then `STATUS_STACK_OVERFLOW`.
Our implementation (mostly user-mode + 1 kernel assist):
- winldr `SIGSEGV` handler: if `si_addr ∈ [StackLimit − Page, StackLimit)`: `mmap(MAP_FIXED, len=commitChunk, anon)` under the **per-thread stack-growth lock** (thread is the only writer of its own stack → lock-free per current TEB: just check-and-mmap; **race with `VirtualAlloc` over the hole** → growth path must take the VAD allocator's lock (§7) — **this is the one place needing a user-mode kernel object lock shared with the allocator; fine, it's in-process**) → update `TEB.StackLimit` → `sigreturn` retry. Matches Windows observably (fault is *retryable*).
- `VirtualFree(...MEM_DECOMMIT)` inside a thread stack: Windows refuses (`STATUS_...`) — refuse too.
- `SetThreadStackGuarantee` → winctl adjusts the guard size → keep an extra reserved gap: track in `win_thread` — **needs kernel cooperation only for the "guarantee" commit region; approximate with user reserved gap.**
- `CreateThread(dwStackSize)`: stub maps `dwStackCommit`... Windows commits full `dwStackSize`? default 0 → exe's `SizeOfStackReserve/Commit` apply; thread stacks are **fully committed reserve** (default 1MB, `StackCommit=0x1000` grow — same guard model). Implement identically: `mmap(PROT_NONE)` full + grow-as-described, `sigaltstack` inside the committed part.
- **Thread stack overflow kills the thread** (Windows: `STATUS_STACK_OVERFLOW` unhandled in a thread → thread dies w/ exit code; process continues if `UnhandledExceptionFilter`... default: WER → whole process dies — actually: unhandled → `NtTerminateProcess`; `SetThreadStackGuarantee`... document: match Windows: unhandled stack overflow = **process** termination 0xC00000FD.)

### 6.4 Debugging exceptions / hardware breakpoints

- `int3` → §6.2 table (works with no kernel change); `DebugBreak` inside a debugged process → `0x80000003` to SEH/vectored first (if app), then debugger.
- `CONTEXT` fields `Dr0-Dr3, Dr6, Dr7` + `EXCEPTION_SINGLE_STEP` + `Debug registers`:
  - **Self-set watchpoints** (`SetThreadContext` with Dr0... on your own thread — used by anti-debug & sanitizers): `ptrace` cannot touch the running thread's own DRs. **NEW kernel facility (small): `prctl(PR_GET_DEBUGREGS/PR_SET_DEBUGREGS)` or `winctl(GET/SET_DEBUG_REGS)`** writing `thread->thread.debugreg0-7` with the `arch/x86/kernel/ptrace.c` `set_debugreg()` guards (DR7 local-enable bits, kernel-owned reserved slots — Linux reserves 1 HW slot? `arch/x86/kernel/ptrace.c::reserve_debugreg`? modern x86 has `per-task dr7` + `x86_debug...`: `hw_breakpoint` API reserves DR0-3 for its own (kprobes/perf). Conflict management via existing `arch_install_hw_breakpoint`. ~80-line winctl façade over existing arch code; the arch support is already complete.)
  - `Dr6` reporting in the single-step exception record: `EXCEPTION_RECORD.ExceptionInformation[0]=Dr6` — Linux `TRAP_TRACE` siginfo has no DR6 → same winctl read-on-fault (`ptrace`... user handler can't read DR6 → **winctl(GET_DEBUG_STATUS)`** reads `current->thread.debugreg6` (`get_debugreg` macro).
  - External debuggers (`DebugActiveProcess`) = **ptrace bridge**: winldr's debug-API implements `WAIT_DEBUG_EVENT` by `PTRACE_EVENT_STOP`/`PTRACE_SINGLESTEP` translation + `GET/SETREGS`→CONTEXT + `GET/SET_THREAD_AREA`(GS) — significant user-mode work (Debugger subsystem, ~3–4k lines), Phase 4.

---

## 7. Memory management

### 7.1 Reserve/commit — the core mismatch (precise analysis)

Windows: `VirtualAlloc` two-axis state: `{Free | Reserved (VA named, no backing) | Committed (backed)} × PAGE_* prot`; `MEM_RESERVE` alone = **zero pages, zero PTEs**, address usable later by `MEM_COMMIT`; `VirtualQuery` reports `State: MEM_FREE/RESERVED/COMMITTED`, `AllocationBase`, `AllocationProtect`; `MEM_COMMIT` requires prior reservation (or auto-reserves). Working-set/`CommitCharge` accounting real (pagefile-backed).
Linux: `mmap` = allocate VA + PTEs lazily; no VA-only state — `PROT_NONE` **is** a reservation-like state: `MAP_NORESERVE PROT_NONE` = named hole, no page tables materialized for the guard? `PROT_NONE` mappings still create VMAs (kernel `vm_area_struct` present) → **visible in `/proc/pid/maps`, counted in `VmSize`/commit cgroup** — differs from Windows "reserved" (invisible to `VirtualQuery`? no — Windows *reports* reserved in VirtualQuery — that's fine: our user-side VAD tracks it anyway). The real problem with PROT_NONE-as-reserve: **you cannot `mprotect` a subrange's commit state independently of... actually you can: `mprotect(PROT_NONE↔RW)` = commit↔decommit 1:1**, no VMA merging issue? `MERGE`-able VAs make `VirtualQuery`'s "contiguous same-state region" *coarser* than Windows (Windows: reserved+committed region reports committed-run correctly). Also **accounting**: PROT_NONE pages aren't charged to overcommit (good, matches: Windows reserves aren't charged).
**Decision — hybrid (analyze, don't dogmatize):**
- Primary: **user-mode VAD-like allocator** (call it `winmm` in winldr): red-black tree (reuse `tree.c`? user: implement) of intervals `{Base, RegionSize, State, Protect, Type: MAPPED|PRIVATE|IMAGE}`; reserves = **holes** (no mapping at all → Windows-exact invisibility to Linux `mmap` allocation, holes found via `get_unmapped_area` semantics by probing `MAP_FIXED_NOREPLACE`); commits inside a reservation = `mmap(PROT_x, MAP_FIXED)`; decomits = `mmap(PROT_NONE? no — munmap + re-hole)`; but then a 16MB reserve with 4KB commit = `mmap(PROT_NONE)` for the rest... **the honest cost model**: `MAP_FIXED PROT_NONE` per decommit = VMA per run (Windows VAD trims similarly); `/proc/maps` divergence accepted for committed-state fidelity.
- 64KB allocation granularity (`WIN64` only for *allocation suggestions*; the actual VirtualAlloc granularity = **64KB on x64 for reserved regions**) enforced purely in `winmm` alignment logic; `dwAllocationType` MEM_TOP_DOWN = search from hole-map top.
- `PAGE_WRITECOPY` = `MAP_PRIVATE` file view (Linux's private file mappings **are** writecopy at the PTE level — exact hardware match; only `VirtualQuery`'s Type must say `MEM_MAPPED` — tracked). `PAGE_WRITECOMBINE`: user-space WC impossible without `PAT` control — **NEW facility or accept**: `mmap(..., MAP_? )` none; later: `winctl(SET_MEM_ATTRIBUTE, WC)` using `pgprot_writecombine()` (`arch/x86/mm/pat: `memcpy_to_msr`? `vm_pgprot` — via new `PROT_WRITECOMBINE`? upstream rejected; distro patch adds `MAP_SYNC?` no — `PROT_WRITECOMBINE` as `pkey`? **pkeys can't express WC.**) — consumers: GDI blits (ours: normal WB, perf-only), GPU `Map` (`WDDM`/direct3D apps: `D3D11_MAP_WRITE`... they `VirtualAlloc PAGE_WRITECOMBINE` for staging! → **real-compat risk; schedule the distro `mprotect(PROT_WRITECOMBINE)`-style patch in Phase 3**; fallback `PAGE_NOCACHE`-as-WB (correctness OK, perf hit, some drivers actually demand UC — document).)
- `MEM_RESET` → `madvise(MADV_FREE)`+next-touch-zero? Windows: contents undefined (often zero) — use `MADV_DONTNEED` (guaranteed zero next touch; costlier, correct-by-construction).
- `MEM_REPLACE_HANDLE`/`MEM_LARGE_PAGES`: `MAP_HUGETLB` — requires `vm.nr_hugepages`/cgroup-hugetlb reservation; `MEM_LARGE_PAGES` demands `SeLockMemoryPrivilege` → distro policy; `VirtualAlloc(...MEM_LARGE_PAGES)` fail gracefully with `ERROR_NOT_ENOUGH_MEMORY`. **AWE** (`AllocateUserPhysicalPages`): 32-bit-era physical-page lock — **no mapping; stub → `ERROR_NOT_SUPPORTED`** (XP-era only; ~0 apps).

### 7.2 Section objects / file mapping

`NtCreateSection(SECTION_PAGE_FILE|FILE|IMAGE)` + `NtMapViewOfSection` (+ `SEC_NOCACHE/COMMIT_RANGE/IMAGE`) + `CreateFileMapping` (backed by `INVALID_HANDLE_VALUE` = pagefile) ↔ kernel: `win_section` = `{struct file *}`: pagefile → `shmem_file_setup("win-sec", size, ...)` (existing); named/file → `kern_path`+`filp_open`; image → exec-time section. `NtMapViewOfSection` in another process handle table (cross-process shared memory, `OpenFileMapping` by name) → `winctl(MAP_SECTION, process_handle|−1, view...)`: kernel uses `mmget_task_mm` + `install_mapping`? cross-process map = existing `process_vm`-like? **Kernel: map via `winctl` with `mm` from target's `win_process` object + `shmem_file`** (`vm_mmap`-style in target context requires `use_mm` — `kthread_use_mm` (real, exported for in-kernel mmap-on-behalf) — **existing facility**; name-lookup namespace = `winobjs` named objects (BaseNamedObjects analog: `winctl(CREATE_NAMED)` — implement with a kernel hash table + simple DAC: `GLOBAL\` prefix = cross-session — Phase 3: **`BaseNamedObjects` = a `winctl` directory of named winobjs/win_sections**; `tmpfs` under `/run/winobj` as a *store* is the alternative — kernel-hash preferred (permissions/leases).

### 7.3 `VirtualQuery`, working sets, `MEMORY_BASIC_INFORMATION`

`winmm` VAD answers `VirtualQuery` (`Type`: `MEM_IMAGE` for mapped binaries — kernel tells loader which ranges are images via `win_exec_info`). `SetProcessWorkingSetSize` → `mlock`-ish? Windows trims working set — `madvise(MADV_PAGEOUT)` (exists, 5.4+) direct. `GetProcessWorkingSetSize` → `read `/proc/pid/smaps_rollout`? (`smaps`) sum of RSS — user-mode read of `/proc/self/smaps` (slow; cache). `QueryWorkingSet(Ex)` (page-granular presence + fault counts) → **`mincore()` (exists!)** for present bits; perf counters (`CycleCount` per page) unsupported→zeros (`ERROR_...`). `EmptyWorkingSet` → `madvise(MADV_PAGEOUT)`.

### 7.4 Misc memory facts

`Heap*`: `RtlCreateHeap` over our `VirtualAlloc` + segmented heap (Windows heap = nt!RtlHp — user-mode: implement the **Segment Heap** (Win10+ default) = bitmap-based, per-size-class, on VirtualAlloc — large but pure user; UCRT's `ucrtbase` calls `RtlAllocateHeap` → winldr must ship a faithful `Rtl*Heap` set with `HEAP_NO_SERIALIZE/GROWABLE/...` flags & `SetHeapInformation(HeapEnableTailChecking?)` (debug heap = WER territory).
`GetSystemInfo.lpMinimum/MaximumApplicationAddress` → `0x10000`/`0x7FFFFFFFFFFFFF` (Linux's `mmap_min_addr` is 65536 = Windows's 64KB floor — **lucky match**). Top: `0x7FFFFFFFFFF8`? Windows x64 app max = `0x7FFFFFFDEFFF`? (~ `0x7FFFFFFF0FFF`) — report exactly.
Page-file/commit limits: `NtQuerySystemInformation(SystemBasicInformation.NumberOfPhysicalPages...)` → `/proc/meminfo`-backed, cached by a `winctl` sysinfo façade (kernel, so it can enforce commit limits: `MemTotal+Swap` ≈ Windows commit ceiling; our cgroup memory.max = commit limit → **enforce in kernel: winctl commit accounting via existing `mm->pinned_vm?` or just let Linux OOM/`memory.max` do it; MEMORY_ALLOCATION_FAILURE synthesis: on `mmap` ENOMEM return `ERROR_NOT_ENOUGH_MEMORY` — behaviorally Windows-like**).

---

## 8. DLL loading

### 8.1 Search order (Windows canonical, SafeDllSearchMode=1 default since XP SP2)

1. DLL already loaded (by basename, case-insensitive; the loaded-module list is keyed on `BaseDllName` — **no path distinction after first load**).
2. If app-dir special: the application directory.
3. `SetDllDirectory` dir (if set) else: `%SystemRoot%\System32`, then Windows dir, then 16-bit, then Current directory (note: CWD after System32 in safe mode), then `PATH`.
4. `App Paths` registry key for the app (our distro: file DB), `DllRedirect`/`.local` (dead), `KnownDLLs` (`%SystemRoot%\System32` pinned set — ntdll/kernel32/advapi32/user32/gdi32/shlwapi/kernelbase/...: **load from our fixed "system store" directory only, ignore app-dir copies** — implement as a list + path override), `KnownDlls` + **DLL redirection** (SxS `dependentAssembly` in manifest for versioned system DLLs → `winsxs` store; we model: our "system store" package tree with manifests, §8.3).
5. API sets (`api-ms-*`, `ext-ms-*`) short-circuit to schema map **before** any disk search; loaded under their **alias names** but backed by a host DLL's export table with **forwarder-based** stubs.
`LoadLibraryEx` flags: `DONT_RESOLVE_DLL_REFERENCES` (map without DllMain/IAT — **kernel-assisted `winctl(MAP_IMAGE, nofix)` = just skip user fixup**), `LOAD_WITH_ALTERED_SEARCH_PATH`, `LOAD_LIBRARY_AS_DATAFILE` (no exec prots: map as data — prot override flag to the mapper), `LOAD_LIBRARY_SEARCH_*` (app-local/system32/... explicit sets).
**Divergence risk**: PATH semantics = Windows PATH env + our app-bundle `bin/`; we define distro policy: our loader also exposes a "win-lib path" (env `PATH` interpreted Windows-style `;` separators — the *environment block is UTF-16 with Windows PATH* — Linux's `:` PATH invisible to PE apps; fine).

### 8.2 Loader lock & `DllMain` ordering

`LdrpLoaderLock` = global per-process SRW. `LdrLoadDll`: hold → map → **append to lists with `DllMain(1/*PROCESS_ATTACH*/) called with loader lock held** → release. In-load-order = dependency order (exports resolved depth-first; a DLL's DllMain runs **after** its import dependencies' DllMains — initialization order list = by dependency; circular imports: Windows allows (partially-initialized forward references; the callee's IAT entry for a not-yet-`DLL_PROCESS_ATTACH`-completed DLL is still *patched* (addresses known from exports) — IAT fixup ≠ init order → **faithfulness checklist**: three lists (`InLoadOrder`, `InMemoryOrder`, `InInitializationOrder`) + per-module `{Loaded, DllMainCalled, TlsCallbacksCalled}` flags; deadlock on re-entrant `LoadLibrary` from DllMain = matches Windows (document).
`LdrShutdownProcess` DETACH order = reverse init. `DisableThreadLibraryCalls` observed.
`NtCurrentTeb()->...` — TLS callbacks per §1.7 in InLoadOrder, before respective DllMain? Windows: DLL's TLS callbacks fire at **that DLL's load**, process TLS callbacks of all loaded... empirically pinned in §17.

### 8.3 SxS / activation contexts / manifests / api sets (our native design)

- `CreateActCtx`/`ActivateActCtx`/`QueryActCtxs...` + `Ldrp激活` stack on TEB (`ActivationContextStackPointer` @TEB 0x3A0): implement **a real (not emulated) SxS engine**: `winldr` parses `RT_MANIFEST` (XML — small embedded parser? no: ship a minimal XML subset parser in winldr — original work ~1k lines for the manifest grammar only), assembly identity `{name, version, publicKeyToken, processorArchitecture, type(win32)}`; resolution order: process default ctx → thread ctx → explicit → app-dir loose manifest (`<exe>.manifest`) → **policy** (manifest `application→dependency→dependentAssembly→assemblyIdentity + publisherPolicy` — Phase 3; Phase 1: side-by-side store only).
- **Assembly store = OS-level**: distro packages install assemblies to `/usr/lib/x86-windows-assemblies/<name>/<version>/` + a compiled index (a "fusion cache" of ours, kernel-visible via `winctl(ASSEMBLY_OPEN)` — **the OS does the version resolution, not a user-mode hack**: this is our first-class-infrastructure argument; simplest: winldr over a plain path store with exact-version dirs — the *manifest spec* demands version matching (exact or binding-policy "roll-forward": implement `autoInplace` = highest-minor/patch in store).
- `Microsoft.Windows.Common-Controls` v6 (comctl32 v6 vs v5.8 — **the single most common SxS dependency**: manifests that request v6 must get our `comctl32.dll` v6-equivalent from store; missing → v5 silently = subtle UI bugs — **top-30 compat item**, coordinate with Agent-3 (GUI)).
- CRT versions: `Microsoft.VC90.CRT` (msvcr90.dll SxS — VS2008 apps ship it in-proc: app-local dir wins anyway), `VCRUNTIME140`/`MSVCP140`/`UCRT` (`api-ms-win-crt-*` = the api-set for UCRT — **UCRT's api-sets are schema entries: `api-ms-win-crt-runtime-l1-1-1.dll → ucrtbase.dll` forwards** — ship ucrtbase as one of our system DLLs, resolving most VC14+ apps for free.)
- **API set schema** (`ApiSetNamespace` registry blob → PEB `ApiSetMap`): flattened binary format `{ApiSetNamespaceValueCount, entries: name→(flags, host-module-index, aliases)}`; our source of truth: a repo-maintained `apiset.json` → compiled to the binary schema shipped at `/usr/share/win-personality/apisetschema.bin` + installed into PEB by winldr; host DLLs (ntdll=win32u? Windows: api-ms\* → `win32u.dll`/`ntdll.dll`/dlls) forward exports `LdrpSnapThunk` — **we make every api-ms alias an actual tiny PE DLL with forwarder exports (`.edata` forwards to ntdll/base) OR resolve transparently in the import path without materializing a module — Windows materializes (`LdrpSnapIAT` follows schema to host module); we do host-resolution-without-materialization for perf (apps can still `LoadLibrary("api-ms-win-core-x")` → return a forwarder DLL)**. Phase 2 decision w/ A/B test (`t_apiset_loadlibrary` — apps calling `GetProcAddress(LoadLibrary("api-ms-win-..."))` — must return real functions).
- `GetProcAddress(GET_PROC_ADDRESS_HANDLE?... )` (`LdrGetProcedureAddressByHint`), `DllMain` fwding, `#pragma comment(linker,"/export:foo=bar.foo")` app-local forwarder chains — export-table code handles all (§1.6).

---

## 9. Classification matrix (DIRECT / ADAPT / NEW)

| # | Item | Class | Mechanism |
|---|---|---|---|
| 1 | binfmt_pe: map/validate/reloc/params/GS | **NEW** (in-tree, modeled on `binfmt_elf`) | `fs/binfmt_pe.c` |
| 2 | Image page sharing via page cache | DIRECT | `filemap_fault`/`MAP_PRIVATE` |
| 3 | PEB/TEB/ProcessParameters init | **NEW** (small, inside binfmt) | anon maps + `set_user_gsbase` |
| 4 | VirtualAlloc reserve/commit/VAD | **ADAPT + USER-NEW** | hole tracking + `MAP_FIXED`+`PROT_NONE` + `winmm` user tree |
| 5 | PAGE_GUARD | DIRECT | `PROT_NONE` (Windows PAGE_GUARD = commit-kept-no-access: `PROT_NONE` w/ committed VMA — exact-ish: Linux `mprotect(PROT_NONE)` keeps VMA = commit survives ✔) |
| 6 | PAGE_WRITECOPY | DIRECT | `MAP_PRIVATE` file view |
| 7 | PAGE_WRITECOMBINE/NOCACHE | **NEW kernel patch** | PAT/`pgprot_writecombine` exposure (Phase 3) |
| 8 | Section objects / named shared mem | **NEW** (thin) | `winctl` + `shmem_file_setup` + `kthread_use_mm` cross-map |
| 9 | Event/Semaphore/Mutant/Timer + WaitAny/WaitAll | **NEW kernel facility** | `winobjs` (futex has no wait-all/mutant/abandon) |
| 10 | WaitOnAddress/WakeByAddress | DIRECT (1:1) | `futex(FUTEX_WAIT/_WAKE)` |
| 11 | Critical sections / SRW / CV | USER-NEW (over futex; alertable→winobjs) | winldr |
| 12 | Keyed events | **NEW** (inside winobjs) | keyed hash queues |
| 13 | APC + alertable waits | **NEW** | winobjs `TIF_PE_USER_APC` |
| 14 | Thread create/suspend/resume/term | **NEW kernel bits** (gate TIF flag, kill-one-thread) + `clone3` | winobjs/task hooks |
| 15 | Thread/process exit waitables | ADAPT | `pidfd`-style or win_process exit-state (NEW thin) |
| 16 | Priorities/classes | ADAPT | nice/`SCHED_FIFO`/`SCHED_IDLE`/`ioprio_set` (RT needs CAP_SYS_NICE — policy) |
| 17 | Affinity/groups | ADAPT | `sched_setaffinity`, 64-bit masks, group shim |
| 18 | Fibers | USER | asm switch + `ARCH_SET_GS` |
| 19 | TLS static+dynamic | USER (+ kernel slot-0 at exec) | PEB bitmaps, TEB `ThreadLocalStoragePointer` |
| 20 | SEH/x64 table dispatch, vectored, unwinding | USER (winldr) | sigaction + `.pdata` walk |
| 21 | Access-type (R/W/DEP) in AV record | **NEW kernel patch** (siginfo error-code) | small; fallback = user instruction decode |
| 22 | Guard-page stack growth | ADAPT (user handler + `MAP_FIXED` commit) | §6.3 |
| 23 | HW breakpoints/DR | **NEW** thin | `winctl(GET/SET_DEBUGREG*)` over `arch/x86` debug API |
| 24 | Debug/exception exit codes | ADAPT | `winctl(SET_EXIT_STATUS)` + `PR_SET_DUMPABLE(0)` |
| 25 | CreateProcess (path-based) | **NEW** (gate + inherit + params; *not* fork) | `winctl(SPAWN_BEGIN/WAIT/RESUME)` + `execve` + `prctl(PR_SET_PE_SPAWN_FD)` |
| 26 | Handle table (u32 handles, inherit bit) | **NEW thin** over `fdtable` | winctl handle⇄fd encoding (anon inodes as object bodies) |
| 27 | BaseNamedObjects | **NEW thin** | winctl name hash (+perms) |
| 28 | Job objects | ADAPT-NEW | cgroups v2 + winctl membership + KILL_ON_CLOSE hook |
| 29 | `VirtualQuery`/`QueryWorkingSet` | ADAPT | winmm VAD; `mincore`; `/proc/self/smaps` cache |
| 30 | `SetProcessWorkingSetSize`/`EmptyWorkingSet` | DIRECT | `madvise(MADV_PAGEOUT)`/`MADV_DONTNEED` |
| 31 | Large pages | ADAPT | `MAP_HUGETLB` + cgroup hugetlb policy |
| 32 | AWE | STUB | `ERROR_NOT_SUPPORTED` |
| 33 | CFG enforcement | DEFER→NEW | CET `arch_prctl(ARCH_SHSTK_ENABLE/ARCH_ENABLE_CET)` (exists 6.x!) + IBT; `winctl(CFG_SET_MODE)`; Phase 3 |
| 34 | Memory protection restore on execve mix | policy | execve(ELF) clears PER_PE |
| 35 | Console/`GetStdHandle`/UTF-16 env | USER + Agent-3 | process params |

## 10. Kernel↔user split (summary & justification)

**Kernel**: binfmt_pe (map/validate/reloc/ASLR/base-invariants), PEB/TEB skeleton + GS + initial static-TLS + UTF-16 params, `winobjs` (dispatcher objects incl. wait-all/mutant/APC/suspend gates), `win_section`/handle objects/named namespace, exit-status capture, siginfo fault-detail + debug-reg exposure, `MMF_PE` fork/exec policy hooks, (later) WC-pgprot + CFG/CET wiring.
**User (winldr + win32 + our CRT)**: import resolution (IAT/ILT/ordinals/forwarders/api-sets/bound-validation/delay-load), exports, resources/manifests, SxS activation contexts + assembly store resolution, DllMain/loader-lock protocol, TLS callbacks & per-thread template copies, VirtualAlloc VAD bookkeeping & granularity & commit state, Heap*, all SEH dispatch (table walk, vectored, unwind), CS/SRW/CV fast paths, fibers, TLS/FLS slots, GetCommandLine/CMD parsing, search order.
**Justification**: mirrors real Windows (kernel = `Mi*`/`Ob*`/`Ke*`; ntdll = `Ldr*`); minimizes kernel attack surface to format parsing + object scheduling; every piece that *requires* the file namespace / XML / string processing stays in user. The three things that deviate from upstream Linux (dispatcher objects, TIF gates, exec-time personality init) are precisely the places where user-mode faking either deadlocks or misbehaves — the "second ABI" thesis in practice.

## 11. Proposed architecture (components)

```
 [user] win32u.dll/kernel32-ish shim ── calls ──> winldr.dll (Ldr*, ntdll-equivalent:
        Rtl*/Nt* wrappers, SEH dispatcher, winmm VAD, heap, TLS, SxS, search order)
                     │ syscall: execve / winctl / futex / m* / clone3 / prctl
 [kernel] fs/binfmt_pe.c ── maps EXE+winldr.dll, PEB/TEB, relocs, GS, spawn gate
 [kernel] security/winobjs/ (or drivers/win/): winobjs.c (event/sem/mutant/timer/
        keyed/apc/thread-gate), winhandles.c (fd-backed handles), winsection.c
        (file/shmem sections), winctl syscall (or /dev/winctl ioctl multiplexer —
        start as miscdevice ioctl: faster to iterate, later promote to syscall)
 [kernel] fs/exec.c hooks: PER_PE personality, fork denial, MMF_PE, exit-status
 [kernel] arch/x86: siginfo error-code patch (small), set_user_gsbase use
 [distro] /usr/share/win-personality/{apisetschema.bin, system-dlls/, assemblies/}
```
`/dev/winctl` **ioctl-first** decision: iteration speed + easy `seccomp`/LSM mediation; the numbers are irrelevant to apps (they call `Nt*` wrappers); promote to real syscalls when stable. LSM hook: AppArmor/SELinux policy for handle-object DAC.

## 12. Existing Linux facilities reused (named)

`binfmt` core (`register_binfmt`, `search_binary_handler`, `linux_binprm`, `bprm_fill_uid`), `vm_mmap`, `get_unmapped_area`, `MAP_FIXED_NOREPLACE`, `deny_write_access`, `filemap_fault`, page cache, `shmem_file_setup`, `kthread_use_mm`, `vm_insert_pfn`?, `raw_spinlock`/`wait_queue_head_t`, `hrtimer`, `futex` (`futex_waitv` for wait-any multi — used by winobjs *as backend* for pure events? we'll implement natively, but `WaitOnAddress` path uses futex directly), `clone3`, `set_user_gsbase`, `TIF_` flag machinery + `exit_to_user_mode_loop`, `pidfs` (`pidfd_open`/`pidfd_getfd` — **handle stealing for cross-process: `pidfd_getfd` (5.6) = exactly `DuplicateHandle` cross-process over our fd-backed handles! big existing-facility win**), `ptrace` (`PTRACE_GETREGSET/NT_X86_DEBUGREGS`, `GETREGS`), `cgroups v2` (`memory.max`, `cpu.max`, `pids.max`, hugetlb), `madvise`(`MADV_PAGEOUT`), `mincore`, `mprotect`, `mlock`, `timerfd` (`TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET` for timers + **dynamic-timechange awareness**), `membarrier`? (no), `process_vm_readv/writev` (debugger + `ReadProcessMemory` on other-process: hmm Windows `ReadProcessMemory` same-process trivial; cross-process → `process_vm_*` **if permitted by Yama ptrace_scope** → winctl proxy read preferred: `pidfd_getfd`/winctl(MEM_READ,handle,addr) using `access_process_vm` (real, exported to? used by proc — internal; expose via winctl) — **existing function, new façade**). `vDSO` pattern for a future `winvdso` (time).

## 13. Required NEW kernel facilities (spec sketch + LOC estimate)

1. **`binfmt_pe`** (~1.5k): as §2.2. Kconfig, sysctls: `pe_allow_rwx_sections`, `pe_deny_fork`, `pe_require_nxcompat?` (default warn-only).
2. **`winobjs` + `winctl`** (~2.5k): dispatcher objects (§5.3), keyed events, APC queues + `TIF_PE_USER_APC`, thread gates `TIF_PE_SUSPEND`, timers, wait-all acquisition w/ undo ordering, named namespace, exit-status, `win_process/win_thread` lifetime via `struct file`+`release` hooks in `do_exit`.
3. **`win_section`/`win_handles`** (~0.8k incl. `pidfd_getfd` glue for DuplicateHandle).
4. **fork/personality policy** (~100): `MMF_PE` bit (mm flags), `PER_PE` in `personality`, `kernel_clone` check, `execve(ELF)` clear.
5. **siginfo fault extension** (~50): expose PF error code bits in `siginfo` (new `si_addr_lsb`? no — `si_trapno`?? AMD64 `si_trapno` **exists in siginfo and is currently unused on x86** → **verify**: `struct siginfo` `_sigfault` has `si_trapno` (added for ARM) — x86 could populate it w/ error code! **Resolving experiment t_si_trapno**: check `force_sig_info` path; if `si_trapno` free & visible in `ucontext`... `ucontext` exposes `sigcontext` not si_trapno → user handler reads `siginfo` only via `SA_SIGINFO` handler args — we get `si_trapno` from our `winldr` sigaction → **no ucontext needed**! If this works: **zero kernel patch needed for R/W** — distro: set si_trapno=error_code in x86 fault path (`do_user_addr_fault` has `error_code` at hand — 5-line patch, likely upstreamable as generic). Excellent: demote risk.
6. **debug-reg self access** (~150): `winctl(GET/SET_DEBUGREGS)` over `arch/x86/kernel/ptrace.c` primitives (respecting `reserve_debugreg` conflicts w/ perf/kprobes).
7. **WC pages** (~100, later): `winctl(SET_MEM_ATTRIBUTE WC)` → `apply_to_page_range`/`change_page_attr`-based, `pat_enabled` guarded.
8. **CFG/CET wiring** (Phase 3): existing `arch_prctl(ARCH_ENABLE_SHSTK_IBT?)` `cet` uapi exists (6.x); expose via winctl gating by image GUARD_CF + load config CET flags.
9. (Optional) Authenticode verify for `winldr.dll`+system DLLs (~400, pkcs7 already in-tree for module signing — reuse `verify_pkcs7_signature`).

**Upstream posture**: these are distro-kernel patches first (Ubuntu patch port, like the existing 150+ patches); design them so `winobjs` lives under `security/` or a self-contained `drivers/win/` dir for rebaseability. No changes to core sched/mm except the listed hooks.

## 14. Dependencies

- **MinGW-w64** headers/libs (MIT/zlib) for build + test corpus; **MSVC**-built binaries for conformance (need a Windows VM/CI w/ VS Build Tools — the only trustworthy ground truth).
- **UCRT (MIT, Microsoft's own source)** + **mingw-w64 CRT**: pick one as the *shipped* CRT surface; UCRT is the modern default (VC14+ apps import `api-ms-win-crt-*` → our `ucrtbase.dll` = recompiled MIT UCRT against **our ntdll-equivalent** — massive leverage: UCRT source is open, tested, and apps' `msvcrt.dll` expectations covered by mingw's open-compat msvcrt. Decision gate: UCRT-first + a small `msvcrt.dll` shim over it (export-name remap DLL).
- Agent-1: Win32 base API set (handles, strings, UNICODE/UTF-16 infra, NTSTATUS codes). Agent-3: console subsystem (`ConErr`, std handles, `AllocConsole`), GUI (comctl32 SxS dependency §8.3).
- Distro: packages `win-personality-loader` (winldr.dll+system store), `win-personality-kernel` patch series, `win-assembly-store` (manifest dirs), udev/sysusers for `/dev/winctl` perms; `deny_newgrp`? no.
- Build: `x86_64-w64-mingw32-gcc` for PE components (winldr is built as PE with `-nostdlib` + our own CRT-free entry); kernel = standard Ubuntu `linux-kernel` source + quilt patches (repo has `src/kernel/`).

## 15. Technical risks (ranked)

1. **TEB/PEB/CONTEXT exact-offset errors** → every /GS binary faults silently. *Sev: critical, early*: pin with probe binaries + MS-debug-headers cross-check + Windows-VM differential (§17 T1).
2. **wait-all + mutant abandonment misdesign → deadlocks** in multithreaded apps (DB drivers, installers). *Sev: high*: implement winobjs with a formal-ish doc first, stress-suite w/ randomized acquire patterns; diff against Windows behavior.
3. **fork-denial breakage**: statically-linked-Go PE? (Go built for windows = our ABI = PE; fine), but some installers use `CreateProcess`+`DebugActiveProcess` flows and assume `CREATE_SUSPENDED` can be entered *without* winldr having mapped the real stub's gate — third-party protection wrappers (`packer stubs`) detect the gate wait (anti-debug heuristics see our winldr in module list). *Sev: medium*: make winldr `Ldr`-invisible (unlink from `InLoadOrder` like Windows' ntdll? Windows shows ntdll; packers expect ntdll → we provide `ntdll.dll` name).
4. **CFG/JIT conflict**: V8/.NET5+/Java w/ CFG-on + our no-enforcement → OK; but CET-on-kernel (`ARCH_SHSTK` inherited?) must stay **disabled** for PE mm or JITs fail (`int3`/mprotect RX after write → `XCR`?); risk = kernel defaults changing. *Mitigate*: clear `ARCH_SHSTK` user-permission bits at PE exec.
5. **APC-based file I/O (`ReadFileEx`) + overlapped-I/O design collision**: we need an IOCP-ish story (Agent-1 scope) but `ReadFileEx` is APC — if we defer APC, async file I/O silently hangs. *Sev: medium-high*: schedule winobjs-APC before "file I/O apps" milestone.
6. Loader-lock fidelity: apps that intentionally race the loader (hooking frameworks — Detours, game mods, Discord overlay) depend on exact ntdll internals (`LdrpLoaderLock` *exported* by ntdll on Windows!) — hook libs patch `ntdll!LdrLoadDll`: our `LdrLoadDll` must be **present, patchable, in a known exported symbol set** — document "supported hook surface"; otherwise overlays silently crash → mass real-world breakage. *Sev: high for desktop adoption*.
7. **siginfo/DIAGNOSTICS gaps** (misalignment ACs, `STATUS_IN_PAGE_ERROR` for network FS) — degrade gracefully; medium.
8. Performance of user-mode VAD (global lock contention in JITs doing millions of VirtualProtect) — mitigate: per-reservation sharded SRW.
9. Ubuntu kernel rebase churn (each LTS point-release; `exit_to_user_mode_loop`, `futex_waitv` internals shift) — mitigate: TIF surface minimal, CI boots matrix on hwe kernels.
10. Unicode/CP confusion (§18) — medium, mostly Agent-1 but the *env block* conversion is ours.

## 16. Implementation order (milestones)

- **M0 (spike, 2wk)**: binfmt_pe maps a **statically-linked, reloc-disabled, import-free MinGW PE** with kernel-set GS+TEB; `winldr` stub does nothing but `exit(0x2A)` → parent observes exit code. Proves `MAP_FIXED_NOREPLACE@0x140000000`, section prots, reloc pass, gate-fd resume. **Go/no-go for the whole thesis.**
- M1: winldr real: PEB lists, `LdrLoadDll` of `ntdll/kernel32/kernelbase` minimal (self-hosting: winldr maps more PEs through kernel `winctl(MAP_IMAGE, nofix)`), **static import fixup**, `GetProcAddress` w/ forwarders, DllMain+loader-lock, `WriteFile(GetStdHandle)` → hello-world.exe (MSVC-compiled!) runs.
- M2: winobjs v0 (events/semaphores + WaitAny/WaitAll/pidfd-backed process waits), threads (clone3+GS+TEB copy, RtlUserThreadStart, CS over futex, TlsAlloc, LastError), **Multi-threaded PE runs**.
- M3: exceptions: signal→EXCEPTION_RECORD, `.pdata` walk + `_C_specific_handler` + C++ EH via UCRT, stack-grow guard handler, exit codes; stress: thrower/fault-injector corpus.
- M4: process: `winctl(SPAWN_*)`+params/CommandLineToArgvW/env UTF-16, CreateProcess/`DETACHED_PROCESS`, handle table+inheritance+`pidfd_getfd`, jobs-lite.
- M5: memory: winmm VAD VirtualAlloc/Protect/Query full-state matrix, sections+FileMapping, heaps (`Rtl*Heap`), `MEM_RESET`, WC patch.
- M6: DLL infra: search order+KnownDLLs+api-set schema+`ucrtbase`+`msvcrt` shim, delay-load, bound-import validation, manifest+SxS lite (common-controls), fibers, keyed events, APC-lite.
- M7: hardening: CFG parse+CET opt-in, debug-regs + Debugger bridge (Phase 4), TLS-fidelity pass, ASLR policy, /dev/winctl LSM/seccomp policy.
- M8: compat triage loop (§17 corpus), perf pass (vwinvdso time, futex fast-paths), docs freeze (`teb-peb-abi.md`).

## 17. Testing methodology

- **T1 offset-fidelity suite (first, blocking)**: probe PEs compiled by MinGW & MSVC: `/GS` (cookie slot), stack-probe (`gs:0x30` vs TEB StackLimit? verify: MSVC probes `gs:0x30`? x64 uses `StackLimit @ TEB+0x10` — **the probe test is the ground truth, not the docs**), TLS dynamic+static, SEH (`CONTEXT` roundtrip with `RtlCaptureContext` field-by-field vs Linux `ucontext`), `Fls`. Run on **real Windows VM** for expectations + on ours → golden diffs.
- T2 kernel: kselftests for binfmt_pe (malformed-image fuzz: `syzkaller` + a PE-header fuzzer w/ `afl++` over the parser; invariant: **any input ⇒ `-ENOEXEC` or a working/safe process, never oops**), winobjs: wait-all torture (N threads × M objects × random acquire/signal/timeout/abandon/owner-death), APC ordering, suspend gates vs exit races (`KCSAN`), `pidfd_getfd` cross-process handle dup.
- T3 conformance corpus: build `build/build_pe_corpus.sh`-style matrix: MinGW + MSVC samples per feature (imports/bound/delay/exports-ordinal/forwarders/TLS-callbacks/reloc-less/reloc-dir64/overlap-sections/64KB-align/pdata-EH + C++ EH + SEH + filter return codes). Automate exit-code + stdout diff vs Windows reference captures.
- T4 behavioral differential: instrumented "API trace" (our `winctl` audit log) replayed against Windows ETW traces for selected real apps (ProcessMonitor-style); target: identical syscall-object sequences for `notepad`-class.
- T5 third-party: run a fixed app tier list (7-zip, Notepad++, vim-nt, python-embeddable **as phase targets** — python = stress for TLS/heap/CV/exceptions; .NET9 deferred) + **SPEC CPU? no** perf micro: import-count, VirtualAlloc churn, wait ping-pong latency vs Windows bare-metal (goal: ≥80% of native Windows for sync hot paths — futex winobjs should beat Windows in pure user→kernel→user... measure).
- T6 determinism/ASLR: `ADDR_RANDOMIZE=0` for CI, full for nightly; `MAP_FIXED_NOREPLACE` collision injection (`/proc/sys/vm/mmap_min_addr`? no: pre-map the base via LD_PRELOAD? — PE-only: add test hook sysctl `pe_prefer_no_base=1`).
- Long-haul: suspend/resume(3) host with live PE processes (winobjs state across hibernate — hrtimer absolute vs CLOCK_MONOTONIC: **timer semantics trap: Windows relative timers use `KeQueryInterruptTime`? monotonic; absolute = system time — affected by suspend; verify `TFD_TIMER_CANCEL_ON_SET` use**).

## 18. Known compatibility traps (aggressive list)

1. `argv[0]` trust: Windows `CommandLineToArgvW` vs MSVC CRT parsing rules (backslash-quote sequences; `GetCommandLineW` must be *exact string* user passed — our execve path: parent-built command line from gate fd must preserve the *verbatim* Windows command line; env→UTF-16 conversion assumes UTF-8→W; **legacy-ACP apps** (`GetCommandLineA` = `CP_UTF8`→W→A roundtrip garbles `Shift_JIS` args) — provide per-process ACP config (`win-personality` ACP policy); same for filenames (ANSI APIs use ACP not UTF-8 — win32 must expose the active code page).
2. `GetVersion`/`NtQueryVersion` lies: apps break past Win10 1909 unless appcompat shims — implement a **shim database** (env `WINVER`? our `PEB.OSMajorVersion=10.0.26100` + `PEB` appcompat flags `AppCompatFlags` from distro config; `kernel32!GetVersionEx` fails for manifests w/o `supportedOS` GUIDs — exact behavior: returns 6.2 unless manifest lists Win8.1/10 GUID `{...}` — implement verbatim).
3. `PEB.BeingDebugged`/`NtGlobalFlag`/`KernelDebugInformation` anti-debug checks: defaults `BeingDebugged=0`, `NtGlobalFlag=0`, `NtQSystemInfo(DebugActive)=0` — and **winobjs must not appear in module lists**; `rdtsc` timing traps: not our problem (but vwinvdso later).
4. `0x140000000` hardcoded (non-reloc DLLs `LoadLibrary`ed into ASLR'd base → fail like Windows but our hole may differ) — log loudly.
5. **Bound imports trusted stale** → we ignore (validated-then-ignored) — *except* `Kernel32` `LoadLibraryExW` bound-skip optimizations some loaders assume... none real. Low.
6. TLS callback signature confusion (Win2003-era `(PVOID hDll, reason, reserved)` 3-arg vs Vista 2-arg): Windows ignores extras; we pass 2 + keep r8/r9 = reason? Windows calls with RCX=Process/Thread, RDX=Reason — pin in T1.
7. `DllMain`-during-`LdrpInitializeProcess` calling `LoadLibrary` = loader-lock re-entrant success (Windows allows same-thread re-entry; SRW not re-entrant! Windows uses a separate loader-lock CS for reentrancy — **implement loader lock as re-entrant CS exactly**) + `TerminateProcess` from DllMain (Windows: works, kills).
8. Circular DllMain deadlock: two threads loading A↔B — Windows serializes via loader lock (one waits *without* deadlock; init order interleaves) — test in T2 torture.
9. `VirtualQuery` on **gaps of reservations** (`AllocationBase=0, State=MEM_FREE`) vs Linux's PROT_NONE-VMA-as-holes — winmm answers from VAD tree not smaps ✔ but `VirtualAlloc(0-size)` query semantics + `MEM_RELEASE` needs whole-allocation, exact-base arg → VAD must track allocation boundaries (not commit ranges) — implement like Windows (two-layer tree: region vs allocation).
10. `MEM_DECOMMIT` on an image-section (apps do it for `SizeOfImage` slack) → `mmap PROT_NONE` into file-backed region kills `Type=MEM_MAPPED` continuity → VAD must *reconstruct* the section region (we do, but `QueryWorkingSet` on it = lies). Accept.
11. `FILE_FLAG_SEQUENTIAL_SCAN`/prefetch → `posix_fadvise WILLNEED`; `NO_BUFFERING` requires O_DIRECT alignment (512) — `CreateFile` side (Agent-1).
12. `STATUS_ACCESS_VIOLATION` **read of `0xFFFF0000`** (`IsBadPointer` probes; `NULL+0x10000` guard region Windows reserves: Windows `NtAllocateUserPhysicalPages`? no: Windows's 64KB-low **NULL-guard = `MM_LOWEST_VALID_ADDRESS`** — our `mmap_min_addr=65536` matches ✔ but guard must be **always-present even in ELF-mixed execs** ✔.
13. `TerminateThread` leaks/abandons SRW/mutants: mutant abandonment via kernel ✔; our CS: Windows leaks too ✔ — but `RtlRestoreContext`-based `_seh_longjmp_unwind@4` interactions w/ `SetUnhandledExceptionFilter`-returning-`EXCEPTION_CONTINUE_EXECUTION` after terminate-thread — pin by test not reasoning.
14. `Wow64`-isms in 64: `%WINDIR%\SysWOW64` refs in hardcoded paths → alias in our store; `IsWow64Process` must return TRUE for a 32-bit? we have no 32-bit → return FALSE honestly (apps then use 64 paths ✔).
15. `GetSystemPowerStatus`, `SetTimer(QUser)` — Agent-3.
16. **Registry absence**: `Advapi32` Reg* APIs need a backing store (Agent-1 territory) — but *we* need it for KnownDLLs/App Paths/appcompat/`HKLM\System\...\Image File Execution Options` (`Debugger` key used by some launchers!) — winldr reads "registry-lite" file DB; flag dependency on Agent-1's registry store design NOW.
17. Locale: `LOCALE_USER_DEFAULT` collation (`CompareStringEx`, `SortHandle`) tables for filenames/strings (ICU exists on Linux — bridge, but Windows sorting orders must match **NLS** for compat (ICU ≠ NLS sort order → wrong dir listings vs Windows expectations... rare but real for installers) — Phase 2.
18. `KERNELBASE`-vs-`kernel32` **forwarding graph**: apps resolve some exports expecting them in `kernel32` via `GetProcAddress(GetModuleHandle("kernel32.dll"), "SetThreadDescription")` — our DLL split boundary must **match Windows's exact export→dll placement** (documented in MS headers) — conformance test per exported function (exported-symbols.json + map).
19. `LoadResource`/`LockResource` returns pointers into mapped `.rsrc` (no copy) — prots & VAD `MEM_IMAGE` ✔.
20. `RtlP4..`/`DbgPrint`... `OutputDebugString` → our winobjs→debugger bridge ✔ + syslog fallback.
21. **`DuplicateHandle` cross-process with `DUPLICATE_SAME_ACCESS` on thread handles** → `pidfd_getfd` ✔ but Windows grants `DuplicateObject` access rights — map rights→fd perms (our rights are advisory anyway until winobjs DAC).
22. `ExitThread` while holding heap lock (UCRT) — fine; but `ExitProcess` with other threads mid-`DllMain` detach — Windows races and apps rely on "usually survives" — exact Windows quirk: `LdrShutdownProcess` runs only in the exiting thread; implement identical.
23. File-locked-image vs Linux unlink (§2.3) — installers that delete-then-replace running exe **succeed on Linux-fail on Windows**; our `deny_write_access` keeps the *incompatible* half; document.
24. `SuspendThread` in DllMain of self (some obfuscators) → our gate: thread is already running → TIF gate suspends self — legal, resume by same thread ✔ test.
25. `__CxxFrameHandler3` v1/v2 (`/EHsc` old vs EH-metadata in load config `GuardEHContinuation`) — UCRT-only path ✔, mingw's `libstdc++` differs → **ship one C++ runtime story**.
26. `CONTEXT.SegGs`/`Arch` fields: x64 CONTEXT includes `SegGs` — user code may check ==0x2B? (Windows user GS selector=0x2B; our kernel swapgs user base with selector... `GSbase`-based, selector= `__USER_DS` 0x2B ✔ same!).
27. `rdpid`/`getcpu` inlined in new MSVC? — kernel `RDPID` enabled ✔ same values.
28. `XSTATE` in `CONTEXT`: `XSTATE_FEATURE...` — `XRSTORE` restore on `SetThreadContext` — Linux sigreturn does xstate restore ✔ (verify AVX-512 `xcr0` mismatch trap → `SIGSEGV`? pin in T3).
29. `HEAP_TAIL_CHECKING...` debug flags via `NtGlobalFlag` (apps check `RF_...`) → support GFLAGS read/write (winctl) for WER-less triage.
30. **Windows 11 `RtlAreBitsSet`... trivial** vs **`RtlSetProcessIsCritical` (process die-on-unhandled... `RtlSetProcessIsCritical(1)` = kernel dies on exception → our: mark mm fatal-on-exception flag in winctl ✔).

## 19. Original implementation required (no external code to reuse)

- `binfmt_pe.c`, `winobjs`/`winctl`, PEB/TEB init + TEB-ABI doc, winldr import/TLS/SEH-dispatcher/winmm-VAD/Rtl\*Heap, api-set schema tooling, SxS manifest engine, api-ms forwarder DLL build system, signal→EXCEPTION translator + instruction-decoder (DEP classification), APC frame builder (kernel), `RtlVirtualUnwind`, `CONTEXT` marshalers, Debugger bridge (`DebugActiveProcess` semantics), `winldr` itself (PE, `-nostdlib`, self-loading), fuzzing harnesses, Windows-expectation golden capture pipeline (Windows VM automation — legal & fine).
- **Clean-room note**: semantics from Microsoft docs + observable behavior + MinGW `winnt.h` (MIT) + ReactOS headers (GPL — **exclude from MIT/BSD components**); do **not** derive code from Wine (project constraint & license culture: Wine is LGPL, our target tree is MIT/Apache-style).

## 20. Open questions → resolving experiments

| Q | Experiment |
|---|---|
| Exact TEB/PEB/CONTEXT offsets for /GS, probes, TLS ptr, FLS | T1 probe-binary disasm + Windows VM (`WinDbg dt ntdll!TEB`) golden dump |
| Does x86 `si_trapno` survive `SA_SIGINFO` w/o ucontext loss? | `t_si_trapno`: 5-line error-code patch + handler print on AV R/W/DEP |
| Preferred-base collision frequency in real distro workloads | T6 injection hook + `perf`-stat 10k execs `MAP_FIXED_NOREPLACE@0x140000000` fail rate |
| GS vs syscall/signal/clone stability | `t_gs_matrix` (GS set, 10k syscalls, 10k signals, spawn) |
| Windows TLS-callback order w/ dep graph | T3 capture on VM (`DllMain`+TLS print, 6 dep topologies) |
| `SCHED_IDLE` vs Windows IDLE class in practice | T5 micro-bench co-runner latency |
| `pidfd_getfd` enough for `DuplicateHandle` incl. access-mask | T2 |
| AVX/XSTATE `CONTEXT` fidelity | T1 w/ AVX-512 probe, `SetThreadContext` roundtrip |
| Suspend-at-arbitrary + loader-lock deadlocks | winobjs torture + KCSAN |
| WC necessity for GPU path (dxvk-style? native?) | profile staging-buffer usage of top-50 apps when GUI lands |
| CET/shadow-stack interaction w/ JIT-heavy apps | Phase-3 `ARCH_SHSTK` matrix |

---
**Bottom line**: ~5k lines of new kernel code (`binfmt_pe` + `winobjs`/`winctl` + policy hooks) + ~15k lines of winldr (loader/SEH/VAD/heap-lite) carry the entire execution-personality thesis to "MSVC C++ multithreaded console app with SEH, TLS, threads, mutexes runs natively at full speed". The three irreducible kernel requirements are: **PE exec-time address-space construction**, **dispatcher-object wait-all/mutant/APC semantics**, and **fidelity of the exception/gate signals** — none is achievable by pure user-mode layering without silent misbehavior, which is exactly what "not Wine" means operationally.
