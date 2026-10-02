# Master Architecture — Native Windows Personality on Ubuntu 26.04 LTS

**Codename:** `ntpers` (NT personality)
**Inputs reconciled:** DEFINITION-OF-DONE.md (423 criteria) + research reports 1–4.
**Status:** Phase 2 ratified. Governs Phase 3 implementation.

---

## §0 The architectural thesis

The system supports **two first-class ABIs on one kernel**:

```
   Linux app                                    Windows app
      |                                              |
   ELF / glibc                                  PE / ntdll
      |                                              |
   syscall (Linux nr, -errno)          syscall (NT SSN, NTSTATUS)
      \                                              /
       \--------- entry_SYSCALL_64 (shared) ---------/
                          |
              TIF_NTABI ? do_nt_syscall : do_syscall_64
                          |
        NT object mgr / handle table / dispatcher objects   <-- IN KERNEL
                          |
         shared kernel subsystems (VFS, mm, net, DRM, sched)
                          |
                       hardware
```

**Why this is not the prohibited model.** The prohibition targets architectures whose
mechanism is a user-mode server translating Win32 calls into POSIX calls. Here the NT
service layer *is* the kernel's second syscall table; NT objects, handles, dispatcher
objects and waits are kernel-resident data structures operating directly on kernel
infrastructure — the same infrastructure the Linux ABI uses. Two ABIs sharing a page
cache is not one ABI translated into the other, exactly as two Linux syscalls sharing
the VFS are not translations of each other. The falsifiable test of this claim is
`DOD-ARCH-001`: kill every user-mode helper immediately before launch and 100/100 PE
launches must still succeed, with ≤0.05 host syscalls per NT call.

**Placement rule.** A component lives in the kernel iff (a) it holds cross-process
authoritative state (objects, handles, tokens, sections), or (b) user-mode placement
would force a per-call context switch. `ntdll`'s loader, heap and SEH dispatcher live
in user space — as they do on real Windows, where `LdrLoadDll` and `RtlAllocateHeap`
are user-mode. Copying Windows' own split is what makes us compatible, not what makes
us a shim.

---

## §1 Kernel components

| Component | Form | Responsibility |
|---|---|---|
| `binfmt_pe` | `fs/binfmt_pe.c` | PE32+ recognition, section mapping from page cache, ASLR, `IMAGE_REL_BASED_DIR64` relocation, PEB/TEB construction, GS base, transfer to `winldr` |
| NT dispatch | patch to `arch/x86/entry/` + `nt/dispatch.c` | `TIF_NTABI` test on the hot flags word; `mov r10,rcx` NT arg convention; args 5+ from `RSP+0x28`; return **unsigned NTSTATUS in RAX, never `-errno`** |
| `ntobj` | `nt/object.c` | Object manager: types, headers, ref vs handle counts, `\??`, `\Device`, `\BaseNamedObjects`, `\KnownDlls` |
| `nthandle` | `nt/handle.c` | Per-process handle table, inheritance, `DuplicateHandle`, pseudo-handles, close-pending semantics |
| `ntsync` | `nt/sync.c` | Dispatcher objects: events (manual/auto), **mutants with ownership + abandonment**, semaphores, timers, keyed events. Provides true **wait-all**, which futex cannot express |
| `ntmm` | `nt/mm.c` | Reserve/commit distinction, 64 KiB allocation granularity, `PAGE_GUARD`/`PAGE_WRITECOPY`, VAD tree, `VirtualQuery` fidelity, section objects |
| `ntio` | `nt/io.c` | IRP-shaped I/O, async completion, **IOCP**, user/kernel APCs, alertable waits |
| `ntsec` | `nt/security.c` | Token objects, SIDs, security descriptors, `SeAccessCheck`. PE processes never acquire POSIX rights |
| `dxgcore` | `drivers/gpu/dxgcore/` | D3DKMT-compatible `/dev/dxg`; allocations→GEM/TTM, fences→`drm_syncobj`, submission→`drm_sched`, sharing→PRIME, TDR |

**Dormancy (`DOD-ARCH`):** cost when no PE process exists is one predictable branch on
an already-hot flags word plus 16 bytes in `task_struct`. No daemons. Disableable via
`CONFIG_NT_PERSONALITY=n` or `ntabi.exec_enabled=0`.

**Syscall-number stability:** NT SSNs are unstable across Windows builds, so we pin a
single frozen reference build (Server 2022) in a build-time-generated, `const`,
append-only table. Binaries with hardcoded SSNs from other builds are a declared
compatibility limit, not a silent failure.

---

## §2 User-space components

| Component | Responsibility |
|---|---|
| `ntdll.dll` (ours) | Syscall stubs, `Ldr*` loader, loader lock, heap, `Rtl*`, SEH dispatcher, APC delivery, `KiUserExceptionDispatcher` |
| `winldr` | Import/IAT resolution, api-set schema (`api-ms-win-*`), forwarders, delay-load, `DllMain` ordering, TLS callbacks, SxS/manifests |
| `kernel32`/`kernelbase` | Win32 personality over NT services |
| `user32`/`gdi32` | Windows/message queues/DCs |
| `d3d9/11/12`, `dxgi` | Native D3D runtimes |
| per-vendor UMD | D3D→GPU-native command streams (AMD PM4 via `AMDGPU_CS`/userq first) |
| `ntafd` | Winsock/AFD semantics with exact WSA error codes |
| `services.exe` | SCM, running as a systemd guest so Ubuntu is unaffected |

---

## §3 Graphics

Hybrid per R3: kernel `dxgcore.ko` owns the D3DKMT object model over real DRM;
user-mode D3D runtimes + per-vendor UMDs emit native command streams. Shader path is
DXBC/DXIL → NIR/LLVM → vendor ISA with a disk cache. Reusing Mesa's compiler and
tiling libraries is *driver infrastructure sharing* (the same relationship an IHV UMD
has to its own compiler), not API translation. An in-process D3D→Vulkan path is
**prohibited as the mechanism** and permitted only as an explicitly flagged,
measured, temporary fallback for legacy D3D9/11 while native emitters are written —
never counted toward a DoD pass.

---

## §4 Anti-cheat and DRM policy

Per R4, and binding:

- **User-mode anti-tamper** (PEB flags, debug registers, syscall-stub hashing, QPC
  timing, exception probes, HV detection, self-hashing, TPM via TBS): satisfiable by
  faithful native implementation. This is engineering and is in scope.
- **Kernel-mode AC** (`.sys` from EAC/BattlEye/Vanguard): cannot ABI-execute because
  they bind `nt!` internals — `SwapContext`, HAL PMU hooks, build-pinned offsets —
  independent of signing. Vanguard additionally requires
  `GetRuntimeAttestationReport`, a Microsoft Secure-Kernel-signed artifact.
- **Policy: faithful implementation or honest failure. Never spoof.** We do not fake
  attestation, forge measurements, or lie about kernel identity. Spoofing would be
  both a security defect and a DoD violation (`DOD-ARCH` counterfeit detection).
- Gated items stay in the EXT-GATED register and are reported as gated, never as
  passing. The ungated substitutes (our own test-signing CA and driver stack, custom
  Secure Boot PK, MOK enrolment, user-mode-AC titles, DRM-free corpora) are what
  progress is measured against.

---

## §5 Linux non-regression

Ubuntu behaviour is protected by construction: no change to `do_syscall_64`'s Linux
path beyond one branch, no change to glibc, no change to systemd defaults, no
replacement of Mesa or the display server for Linux clients. `DOD-LINUXR` compares
against a permanent stock-Ubuntu witness image; any divergence is a failure.

---

## §6 Implementation order (M-milestones)

| M | Content | Gate |
|---|---|---|
| **M0** | Dispatch multiplex spike: `binfmt_pe` maps a static reloc-free PE, kernel sets GS/TEB/PEB, NT `NtTerminateProcess` observed | go/no-go for the whole thesis |
| M1 | Differential harness vs real Windows VM; freeze kernel↔ntdll contract (SSN table, PEB/TEB/KUSER offsets) | contract pinned |
| M2 | `ntmm` memory services, commit ledger, guard-page stack growth | `VirtualQuery` parity |
| M3 | `nthandle` + file objects; `NtCreateFile`/`NtReadFile`/`NtClose`; std-handle bridge | round-trip byte equality |
| M4 | `ntsync` (wait-any/wait-all/mutant abandonment) + threads (`clone3`+GS+TEB+TLS) + suspend gate | torture suite |
| M5 | SEH: signal→`EXCEPTION_RECORD`, `.pdata`/`RUNTIME_FUNCTION` unwind, C/C++ EH | unwind parity |
| M6 | `winldr` imports + apisets + `DllMain` → MSVC hello-world | real PE runs |
| M7 | IOCP + `ntafd` sockets; named pipes; COM; SCM | netgame semantics |
| M8 | `dxgcore` + D3D12 + AMD PM4 emitter + present | triangle → frames |
| M9 | Image build, VM boot, bare-metal install, full DoD run | portability |

M0 is load-bearing: if the dispatch multiplex cannot be made correct and cheap, the
architecture is wrong and must be redesigned before anything else is written.

---

## §7 Test authority

`tests/conformance/criteria.json` drives everything. Every claim is verified by the
master agent independently; agent reports are evidence, not authority. Counterfeit
detection (`DOD-ARCH-001/002`) runs every cycle: helper-kill launch matrix, host/NT
syscall ratio via ftrace, PMU fingerprint vs same-source ELF twin, CPUID hypervisor
bit, provenance scan for Wine/QEMU/DBT lineage.
