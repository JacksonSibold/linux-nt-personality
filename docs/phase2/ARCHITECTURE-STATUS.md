# Architecture status: components in ARCHITECTURE.md

Date: 2026-10-12. Branch: `haiku/docs5` (line counts, service counts and references refreshed from the code at this commit; service counts from [status/SERVICES.md](../status/SERVICES.md)).
Scope: each component named in [ARCHITECTURE.md](ARCHITECTURE.md) sections 1 to 4, mapped to the
file or files that implement it in this repository, with a status.

Method: static reading of the source files and the module reference in
[docs/modules/](../modules/INDEX.md), plus the review reports in [docs/review/](../review/INDEX.md).
Line counts are from `wc -l` on the files as they are in this tree. Nothing was built, linked,
booted or run. Status describes what the code contains, not whether it works.

## Status definitions

| Status | Meaning |
|---|---|
| implemented | The code covers the responsibility described in ARCHITECTURE.md. Not verified by running. |
| partial | Some of the responsibility is in the code. Other parts are missing, stubbed, unwired or known to be unverified. |
| stub | Present only as a placeholder or a minimal surface. |
| missing | No implementation file found in the repository. |

No component is rated `implemented`. Every kernel component has at least one gap listed in
[docs/KNOWN-ISSUES.md](../KNOWN-ISSUES.md) or in the review reports.

## 1. Kernel components (ARCHITECTURE.md section 1)

| Component | Implementation file(s) | Status | Basis |
|---|---|---|---|
| `binfmt_pe` | `src/kernel/fs/binfmt_pe.c` (1202 lines); test driver `src/kernel/fs/binfmt_pe_test.c` | partial | Has ASLR base selection (`pe_randomize_base`, `binfmt_pe.c:335`), relocation walk that accepts only `IMAGE_REL_BASED_DIR64` (`pe_apply_relocs`, `:489`), PEB and TEB construction (`pe_init_peb` and `pe_init_teb`, `:583` and `:608`) and a GS base write (`:1127-1128`). Transfer to `winldr` is not in this file; `winldr` is user-space code in `src/ntdll/ldr.c`. Review findings K-1 (`e_lfanew` bounds wrap) and K-3 (writable and executable sections) are recorded in [docs/review/kernel-fs.md](../review/kernel-fs.md). The kernel loader does not use the hardened parser in `src/loader/pe_parse.c` (review L-0). |
| NT dispatch | `src/kernel/patches/0001-nt-personality-dispatch.patch` (adds `TIF_NTABI` and the branch in `arch/x86/entry/syscall_64.c`); `src/kernel/nt/dispatch.c` (511 lines) and `arch/x86/nt/dispatch.c` (399 lines; the two copies differ) | partial | Patch adds the single `TIF_NTABI` branch that ARCHITECTURE.md describes. Arguments come from `r10` (`dispatch.c:131`) and stack arguments from `sp + 0x28` (`dispatch.c:97`). Status is written to `regs->ax` as an unsigned value, not `-errno`. `nt_can_sysret()` is a stub that returns `true` (`dispatch.c:420-423`). Six hard-coded SSNs call `pr_debug` on every call (`dispatch.c:113-115`). The service table holds 171 entries: 140 have a handler (134 classed IMPLEMENTED, 2 with a kernel stub, 1 refusing in userspace, 3 stubs in both builds) and 31 are missing ([status/SERVICES.md](../status/SERVICES.md)). There is no frozen reference-build SSN table in the repository. |
| `ntobj` (object manager) | `src/kernel/nt/object.c` (1719 lines), `src/kernel/nt/object.h` | partial | Object headers, types, reference and handle counts, namespace and process and thread objects are present. `ObCreateObjectType` is declared but not defined. Security descriptors are stored but not enforced. A symbolic link in a middle path component is not followed (`object.c:434`). The token type is defined in `object.c`; the section type is defined in `mm.c` and registered on first use. |
| `nthandle` (handle table) | `src/kernel/nt/handle.c` (705 lines), `src/kernel/nt/handle.h` | partial | Three-level per-process table and pseudo-handles are described in the file header and in [modules/handle.md](../modules/handle.md). Inheritance into a new table is all or nothing (`ObInheritHandleTable`). No dedicated handle test exists. No access check on handles (review arch IO-3, IO-4, P-1). |
| `ntsync` (dispatcher objects) | `src/kernel/nt/sync.c` (2885 lines), `src/kernel/nt/sync.h`, test `src/kernel/nt/sync_test.c` | partial | Events, semaphores, timers, keyed events, mutants with abandonment on owner exit (`nt_abandon_owned_mutants`, `sync.c:1200`) and a WaitAll path with a documented lock-ordering rule are present. Timer APC routines are refused with `STATUS_NOT_IMPLEMENTED`. Keyed events use one global waiter list. The kernel wait-queue path is not exercised by `sync_test.c` ([modules/sync.md](../modules/sync.md)). |
| `ntmm` (virtual memory) | `src/kernel/nt/mm.c` (2655 lines), `src/kernel/nt/mm.h`; `arch/x86/nt/mm.c` (1709 lines) | partial | Reserve and commit, 64 KiB allocation granularity (`mm.h:111`), VAD structures and sections are present. The shim VAD list and the kernel rb-tree are separate code paths. `NtMmHandleGuardFault` is a simulation hook (`mm.c:1654`). The two `mm.c` copies differ in this tree (see [docs/analysis/tree-divergence.md](../analysis/tree-divergence.md)). The arch copy returns a kernel object address as a section handle (`arch/x86/nt/mm.c:1554`, review arch M-2, record 0021). Of the 19 memory-area services, 8 are dispatched for virtual memory, the section services have wrappers, 5 names are missing and `NtExtendSection` is a stub ([status/SERVICES.md](../status/SERVICES.md)). `SEC_IMAGE` sections are built in the userspace build from `image_plan.h` and `image_reloc.h`; the kernel build refuses them. |
| `ntio` (I/O and IOCP) | `src/kernel/nt/io.c` (4064 lines), `src/kernel/nt/io.h`; `arch/x86/nt/nt_io.c` (2584 lines); `src/kernel/nt/iocp.c` (791 lines), `iocp.h` | partial | File services, share-access checks, delete-on-close and console pseudo-handles are present. Async I/O is an "IRP-light" request that runs synchronously in this build (`io.c` 12-14); no workqueue calls exist. IOCP has a concurrency limit and an APC queue. `NtQueryIoCompletion` fills its outputs (`iocp.c:634`). Review findings IO-1 (rename length) and IO-2 (`NtReadFile`) have fix commits (`79d1de2`, `6da43d2`); they were not re-verified. All 30 io-area services have a handler. The object-directory and symbolic-link services are in `nt_obj.c` and are implemented in both builds (SERVICES.md). No `iocp_test.c`. |
| `ntsec` (security) | `src/kernel/nt/security.c` (3399 lines), `src/kernel/nt/security.h` | partial | SIDs, tokens, ACL and ACE, security descriptors and `se_access_check` are implemented in the module. `se_access_check` is called by the token and descriptor services in `security.c`. The object, handle, registry, io and mm paths do not call it, and one of the ten security-area services (`NtCloseObjectAuditAlarm`) is missing ([modules/security.md](../modules/security.md)). |
| `dxgcore` (`/dev/dxg`, D3DKMT) | none | missing | No `/dev/dxg`, GEM, TTM, `drm_syncobj`, `drm_sched` or PRIME code in the repository. The name appears only in docs and `tests/conformance/criteria.json`. |

Process and thread creation has no row of its own in ARCHITECTURE.md section 1. Its status is in
[process-creation.md](../architecture/process-creation.md) (milestones M0 to M7) and in
[modules/proc_thread.md](../modules/proc_thread.md). Two host-tested helpers belong to it:
`process_params.h` (the parameter block, not called by any service) and `proc_create_args.h`
(the argument copy-in, called by the `NtCreateUserProcess` stubs, which then refuse).

## 2. User-space components (ARCHITECTURE.md section 2)

| Component | Implementation file(s) | Status | Basis |
|---|---|---|---|
| `ntdll.dll` (ours) | `src/ntdll/ntdll_main.c` (826 lines), `ldr.c` (1850), `except.c` (587), `unwind.c` (712), `syscall_stub.S` (34), `ntdll.def`, `ntdll_runtime.c`, `syscall_table.def`; generator `build/gen_stubs.py` | partial | Syscall stubs use the 11-byte layout in `syscall_stub.S` (`mov r10,rcx`, `mov eax,SSN`, `syscall`, `ret`). The loader (`ldr.c`) and an SEH dispatcher and unwinder (`except.c`, `unwind.c`) are present. `KiUserApcDispatcher` is an empty function (`ntdll_main.c:46-48`). No `RtlAllocateHeap` exists in `src/`; the only allocator is a minimal bump allocator in `kernel32.c` (`kernel32.c:296-297`). |
| `winldr` (import and loader) | `src/ntdll/ldr.c` | partial | Import resolution (`ldr_resolve_imports`), forwarder buffers in the resolver signatures, DllMain attach ordering (`ldr_process_attach`), TLS directory handling and an API-set schema loader with a built-in fallback are present. A block of stub functions is compiled in one branch (`ldr.c:1825-1850`). No side-by-side assembly or manifest handling was found. |
| `kernel32` / `kernelbase` | `src/ntdll/kernel32.c` (645 lines), `src/ntdll/kernel32.def`; `src/ntdll/kernelbase.c` (20 lines), `kernelbase.def` | partial | `kernel32.c` describes itself as a "minimal Win32 surface" (about 100 top-level function definitions by a line-pattern count), forwarding to ntdll and setting last-error from `RtlNtStatusToDosError`. `kernelbase.c` is a 20-line stub file. |
| `user32` / `gdi32` | `src/ntdll/user32.c` (137 lines), `src/ntdll/user32.def`; no `gdi32` file | stub | `user32.c` contains `DllMain` and the `wsprintf` family only. No windows, message queues or device contexts. No `gdi32` source. |
| `d3d9/11/12`, `dxgi` | none | missing | No implementation files. |
| Per-vendor UMD (AMD PM4 first) | none | missing | No implementation files. |
| `ntafd` (Winsock and AFD) | none | missing | No implementation files. |
| `services.exe` (SCM) | none | missing | No implementation files. |

## 3. Graphics (ARCHITECTURE.md section 3)

Covered by `dxgcore`, the D3D and DXGI runtimes and the per-vendor UMD rows above. The
D3D-to-Vulkan fallback described in section 3 is not present in the repository.

## 4. Anti-cheat and DRM policy (ARCHITECTURE.md section 4)

| Component | Implementation file(s) | Status | Basis |
|---|---|---|---|
| Syscall stub layout kept stable for anti-tamper hashing | `src/ntdll/syscall_stub.S` | partial | The header states the fixed 11-byte stub layout. Nothing else in the repository implements the listed user-mode anti-tamper checks. |
| OS identity policy (`os-identity-policy.json` and version canary) | none | missing | Named in `tests/conformance/criteria.json` (DOD-AC-B001). No file in the repository. |

## 5. Linux non-regression (ARCHITECTURE.md section 5)

| Component | Implementation file(s) | Status | Basis |
|---|---|---|---|
| One-branch change to the Linux syscall path | `src/kernel/patches/0001-nt-personality-dispatch.patch` | partial | The patch adds one `TIF_NTABI` branch in `syscall_64.c`. It has not been applied to a kernel tree or built here. The stock-Ubuntu witness image named in DOD-LINUXR is not in the repository. |

## 7. Test authority (ARCHITECTURE.md section 7)

| Component | Implementation file(s) | Status | Basis |
|---|---|---|---|
| `tests/conformance/criteria.json` and runner | `tests/conformance/criteria.json` (423 criteria); `tests/harness/dodrun.py`, `registry.yaml` | partial | The criteria set and runner are present. [docs/status/CRITERIA.md](../status/CRITERIA.md) lists all 423 criteria as `unknown`. Unregistered criteria are reported as unimplemented by the runner ([docs/DEVELOPMENT.md](../DEVELOPMENT.md)). |

## Not assessed

- Section 6 (M0 to M9 milestones) is a schedule of gates, not a component. None of the gates
  has a recorded result in the repository. The M0 dispatch spike was not run here.
- Counterfeit-detection suites (DOD-ARCH-001/002) were not assessed beyond their presence in the
  criteria file.
