# Research Report 1 — Windows NT Architecture & NT System-Call Semantics

**Project:** WinAPI Native OS (Ubuntu 26.04 LTS / Linux kernel host)
**Phase:** 1 — Research
**Author:** Research Agent 1
**Date:** 2026-08-30
**Status:** Design-input research. Uncertainties are marked `[UNCERTAIN]` with the experiment (E-#) that resolves them.

---

## 0. Framing: the WSL1 mirror image

The target is a **second native execution personality inside the Linux kernel**, symmetric to the Linux personality — not Win32-in-user-mode-over-POSIX (the Wine model, prohibited by project framing).

The decisive existence proof is **WSL1** (`lxss.sys` + `lxcore.sys`, Microsoft 2016–2017). Documented facts (Microsoft "WSL Architectural Overview" 2018; "Pico Process Overview"; Stephen Hufnagel, "WSL System Calls", 2016):

- A Linux process was a **pico process**: a minimal NT process with an *empty user address space and no NT user-mode state*. The NT kernel routed **all syscalls and all exceptions** from that process's user mode to a registered **pico provider driver** (`lxcore.sys`), a clean-room implementation of Linux kernel interfaces.
- On `syscall`, `KiSystemCall64` recognized the pico process, **preserved register state**, called the provider's dispatch routine. The provider read `RAX` (Linux syscall number), serviced the request — calling **internal NT kernel APIs directly** (not through the user syscall gate) where a mapping existed, implementing semantics itself where none existed (e.g., Linux pipes, `fork` via internal Psp APIs). The return value was placed in `RAX` and the NT kernel exited via `sysret`.
- The provider relied on the host kernel for scheduling, memory management, and I/O primitives.
- WSL1's failure modes were *semantic corner cases* (rare ioctls, `/proc` behavior, signal/pty subtleties), **not** the dispatch mechanism. The dispatch mechanism was not the hard part; semantics were.

**Our mirror image:** a Linux kernel with an `ntcore` (name TBD; this report uses `nt`) personality where `entry_SYSCALL_64` recognizes NT tasks, preserves register state, dispatches through **our own NT service table**, services requests via **direct internal Linux kernel calls** (`vm_mmap`, `vfs_read`, `sock_recvmsg`, `hrtimer_start`, `file_lock`, …) — never re-entering the Linux syscall gate — and returns a full unsigned `NTSTATUS` in `RAX` through the normal return path.

Historical precedents (studied for mechanism only, not as dependencies):

| System | Direction | Mechanism | Lesson |
|---|---|---|---|
| iBCS2 / SCO / Solaris-brands on Linux (2.0–3.x kernels) | foreign ABI on Linux | `current->personality` tested in the i386 `system_call` path; alternate syscall table; `lcall7` IDT hack for x86 | Per-task personality + alternate table is a validated old idea on Linux. iBCS2 died from maintenance burden and fragile IDT patching, **not** from the concept. |
| FreeBSD **linuxulator** | Linux-on-BSD | per-process `sysentvec` registered ABI object; syscall entry reads the vec from the thread | Per-process syscall vector + ABI-registered exec is the cleanest known shape. We invert it. |
| Solaris 8 **lx brand** / branded zones | Linux-on-Solaris | brand module swaps syscall table at exec | Same pattern. |
| **SFU / Interix / SUA** (Windows) | Unix-on-Windows | subsystem EXE + `psxdll` talking to user-mode `pssdrv`/`psssrv` over LPC/ALPC | **Counter-example.** User-mode server model = syscall round-trip tax + split kernel state. WSL1's kernel-resident provider replaced it for exactly these reasons. We follow WSL1, not Interix. |
| **WSL1** | Linux-on-NT | pico process + kernel pico provider | Primary template, mirrored. |

## 1. The NT syscall interface on x86-64

### 1.1 Entry mechanism

- `syscall` loads `RIP ← IA32_LSTAR (0xC0000082)`, `RCX ← RIP_after_syscall`, `R11 ← RFLAGS`, `CS/SS ← IA32_STAR[47:32]/[63:48]`, `RFLAGS &= ~IA32_FMASK`, CPL→0. Windows sets LSTAR = `nt!KiSystemCall64` (plus `KiSystemCall64Start` KVA-shadow trampoline). Linux sets LSTAR = `entry_SYSCALL_64` in `syscall_init()` (arch/x86/kernel/cpu/common.c). **Both ABIs arrive at one MSR target per CPU** → multiplexing must happen inside the stub (§2), not at the MSR.
- Windows 64-bit deliberately does **not** use `MSR_CSTAR` in its design (compat-mode `syscall` is not supported on Windows x64 either — WoW64 transitions via `wow64cpu.dll` executing a far `retf` into 64-bit mode and then `syscall`).

### 1.2 ntdll stub and argument marshalling

Every 64-bit ntdll service stub has the shape:

```asm
mov  r10, rcx      ; arg1 parked away: syscall overwrites RCX with return RIP
mov  eax, <SSN>    ; System Service Number (build-dependent!)
syscall
ret                ; RAX = NTSTATUS
```

- In-kernel: `KiSystemCall64` swaps to the kernel stack via `KPRCB.RspBase` (`mov rsp, gs:[0x1A8]`), builds a `KTRAP_FRAME`, then on the dispatch path moves `R10 → RCX` so the `Nt*` service is a plain Windows-x64-ABI function: **arg1 RCX, arg2 RDX, arg3 R8, arg4 R9**.
- **Args 5+ live on the user stack.** Given 0x20 shadow space + 0x8 return address, arg5 = `[userRSP+0x28]`, arg6 = `+0x30`, … The dispatcher copies them per the service's stack-byte count (from `KiArgumentTable`) before calling the service. High-arity examples: `NtReadFile`/`NtWriteFile` (11), `NtDeviceIoControlFile` (10), `NtQuerySystemInformation` (4+ptrs). Our entry must replicate this copy under `copy_from_user_nofault` with probe-once semantics (TOCTOU note §11).

### 1.3 The SSDT and numbering instability

- `KeServiceDescriptorTable` / `KeServiceDescriptorTableShadow`: array of SERVICE_TABLE_ENTRY (Base, Counter, Limit, ArgumentCount). On x64, entries are **4-byte encoded relative offsets**: `target = table_base + (sign_extend(int32) >> 4)`; `ParamTableBase` gives per-service stack-byte counts. Win32k SSNs: `EAX` bits 0–11 = index, bits 12+ = table selector (`0x1000|idx` for win32u); some builds encode extra metadata in `EAX` high bits. This is RE'd, build-specific detail; **we must replicate only its externally visible contract, not its internals.**
- **SSN instability is real and documented** (j00ru/VXSecurity syscall comparison tables; geoffchappell.com SSDT notes): numbers reshuffle every release (`NtCreateFile` has been 0x2D/0x55/0x72/0xC5 across builds; services removed and slots reused). SSNs are **not** a published compatibility contract — ntdll is.

### 1.4 Proposed stable dispatch table for our OS

**We publish an NT-ABI conformance version and freeze numbering.**

- Pin SSNs to **one documented reference build**: proposal = Windows Server 2022 x64 (10.0.20348) ntdll SSN set — long-lived, widely targeted by tooling. `[UNCERTAIN: E1]`
- E1: harvest SSN tables from the ~15 most-deployed x64 builds (Chappell tables + disassembly of local ntdll copies), then choose the set maximizing hits against our first-party PE corpus. Measure, then freeze.
- The table is `const`, generated at build time from the same SSN manifest our ntdll uses — kernel and ntdll can never disagree.
- **Append-only forever.** Deprecated services keep reserved slots returning `STATUS_INVALID_SYSTEM_SERVICE (0xC0000096)`.
- Dispatch validation: `EAX` bits 0–11 index; bit 12 selects our reserved "GUI/shadow" table slot (win32k-equivalent, future); anything else → `STATUS_INVALID_SYSTEM_SERVICE`. No arg-count-in-EAX encoding.
- Return: full unsigned 32-bit NTSTATUS in RAX (upper 32 bits zeroed) — never sign-extended-negative errno style.
- **Direct-syscall compat risk:** binaries hardcoding *foreign-build* SSNs (protectors, EDR self-tests, malware) will misfire. Mitigations: our ntdll is the only supported SSN source; document the pinned build; future option = per-process "SSN translation table" set via a process-information class (for forensic/compat shims). **Do not build an SSN auto-detector** — correctness trap.

## 2. Coexisting a second syscall ABI on x86-64 Linux

### 2.1 MSR_LSTAR: not multiplexable, and we don't need it

One LSTAR per logical CPU, written at boot/hotplug by `syscall_init()`. Giving the two personalities different hardware targets would require WRMSR on every context switch (~50–100 ns + speculation-hygiene hazards: the LSTAR target is speculation-relevant, cf. Swapevents guidance in Documentation/arch/x86/spectre-meltdown-checker). **Rejected.**

**Adopted design: single shared entry stub + per-task personality bit + second service table.**

```
entry_SYSCALL_64 (arch/x86/entry/entry_64.S, modified)
  ... SWAPGS, kernel stack switch, pt_regs build — unchanged ...
  testb  $TIF_NTABI, <ti_flags>          ; flags word already loaded on this path
  jnz    win_syscall_entry               ; -> not_syscall slow-path region
  ... existing Linux dispatch unchanged ...
```

In-tree precedent: the *same stub* already multiplexes four ABIs (x86-64 native, x32 via `__X32_SYSCALL_BIT` mask on `eax`, ia32 via `TIF_IA32`/`regs->cs` checks, `int 0x80` via a separate IDT vector `entry_INT80_compat`). The kernel has never pretended these are one contract. Cost of the NT test: one `test/jcc` on an already-hot cache line, 100% branch-predicted per workload. Implement dispatch in C after `pt_regs` exists: `do_win_syscall(struct pt_regs *regs)` mirroring `do_syscall_64()` in arch/x86/entry/common.c, including the same work-processing hooks (see §2.6 for why NT needs those hooks differently).

- New `TIF_NTABI` (free bit in arch/x86/include/asm/thread_info.h). Set **once at PE execve**, never from user mode, inherited by `NtCreateThread`, checked in `copy_thread` so a Linux `fork()`/`clone()` from an NT task stays NT by design decision (or is disallowed at the syscall boundary — see §6 in the syscall doc cross-ref).
- Per-thread/per-process state: `task_struct->nt_proc` / `->nt_thread` pointers (NULL for Linux tasks; same lazy-pointer pattern as `io_uring` personality fields). Allocated only by the PE loader.

### 2.2 binfmt: `binfmt_misc` is insufficient — write `binfmt_pe.c`

`binfmt_misc` (fs/binfmt_misc.c) only maps magic→interpreter; it cannot build an address space. The right mechanism is a new `linux_binfmt` (`include/linux/binfmts.h`), registered parallel to `binfmt_elf.c`, found by `search_binary_handler()` (fs/exec.c). `load_pe_binary()` must:

1. Validate MZ + `PE\0\0`, `IMAGE_NT_HEADERS64`, `OptionalHeader.Magic == 0x10B`; reject `PE32` (WoW64 out of scope Phase 1) and reject .NET crossgen subtleties beyond the loader contract (user-mode Ldr problem, not binfmt's).
2. Parse `IMAGE_SECTION_HEADER` array → build NT memory-state regions (§5 of exec doc / this §7.2), map sections with NT `PAGE_*`→pgprot translation, honor `ImageBase`+relocation-not-applied (relocation is user-mode Ldr's job — **the kernel must NOT apply relocations**; real NT does not: `LdrpSnapThunk`/`LdrpProcessRelocationInformation` run in ntdll; the kernel maps and jumps to a loader parameter block contract).
3. Construct PEB/TEB, NT stack (with the `ImageBase+SizeOfStackReserve/Commit` values), TLS pointers, `KUSER_SHARED_DATA` page at 0x7FFE0000 (fixed VA, reserved in the NT-mm's ELF-incompatible mmap layout; NT processes never load Linux ELF so no collision), set `thread.gsbase = TEB` (`X86_GSBASE_ACTIVE` lazy path, arch/x86/kernel/process_64.c), set `TIF_NTABI`.
4. Enter user mode at **our ntdll's `RtlUserThreadStart`** with `RCX = thread start context` — exactly as real NT starts threads; the kernel knows nothing about the entry point semantics.
5. Gating: MZ magic collides with `file`/`strings` expectations? No — binfmt only runs on execve. Still gate exec behind policy: `ntabi.exec_enabled` sysctl + an LSM hook at `security_bprm_creds_from_file` so AppArmor can deny PE exec. binfmt registration order: after `binfmt_misc`, before `binfmt_elf`? Irrelevant (magic disjoint); register normally.

### 2.3 `personality(2)` — report-only, not the switch

`current->personality` is a user-writable u8 field (`sys_personality()`); if it gated dispatch, any NT process could flip itself into the Linux ABI mid-flight (ABI-confusion attack; iBCS2 had exactly this class of leak) and Linux tools could accidentally set it. Use the inaccessible TI flag instead. Optionally expose `PER_NTABI` bit (include/uapi/linux/personality.h has free high bits? PER_ flags are u8; if full, omit) purely so `ps`/`/proc/PID/personality` can display it.

### 2.4 `arch_prctl` and GS — the collision that isn't

Both OSes use identical MSR mechanics (SWAPGS + `MSR_KERNEL_GS_BASE`): kernel GS = per-CPU area (Linux) / KPCR (NT); user GS value is saved per-task and lazily restored (`thread.fsbase`/`thread.gsbase` + `X86_FSBASE_ACTIVE`/`X86_GSBASE_ACTIVE`, `x86_load_fsgs`/return-to-user WRMSR path in process_64.c). NT uses **GS for the TEB** and leaves FS at 0; Linux uses **FS for TLS** and (mostly) leaves GS at 0 (`arch_prctl(ARCH_SET_GS)` is available but rare). A task is one personality, so the two fields never conflict; the return path already restores **both** base addresses per task.

- Do **not** route NT TEB setup through `arch_prctl` from user mode; the kernel writes `thread.gsbase` at exec/thread-create. User-mode `ARCH_SET_GS` on an NT task should be rejected (`-EPERM`/`STATUS_…`) so the TEB anchor can't be spoofed away — differs from Linux behavior, deliberate, matches NT (where userspace cannot re-point GS at all).
- `[UNCERTAIN: E2]` Does real NT software ever read `%fs:` on x64 (e.g., legacy code ported from Linux/macOS-x64 where TLS=FS)? Experiment: disassembly pass over corpus PEs for `fs:` prefixes; expect ~zero, confirm.

### 2.5 SYSCALL vs interrupt gates

Legacy `int 0x2E` exists only for WoW64/32-bit; out of Phase-1 scope. One genuine x64 asymmetry: **return RFLAGS/R11 content**. NT and Linux set different `IA32_FMASK` (Linux clears TF|DF|IF|IOPL|AC|NT, see syscall_init), and MSRs are global — we cannot per-process FMASK. If any NT binary provably inspects `R11`/EFLAGS bits post-syscall (rare; `[UNCERTAIN: E3]` — test: single-step corpus apps checking `__read_eflags` behavior), the NT return path can force `iretq` (full RFLAGS control, already implemented in `entry_SYSCALL_64`'s `entry_SYSCALL_iret` path used for TF/single-step ptrace) instead of `sysretq` (~100 ns slower, only for affected calls). v1: use `sysretq` like Linux; revisit with data.

### 2.6 ptrace-free dispatch; seccomp; audit; tracing coexistence

- **seccomp** (kernel/seccomp.c): filters match `(AUDIT_ARCH, nr)`. Dispatching NT before `__seccomp_filter()` would silently bypass every sandbox — unacceptable. Design: NT dispatch stays **inside** the common pipeline after seccomp phase-1/2, and we mint `AUDIT_ARCH_X86_64_NT` (include/uapi/linux/audit.h). Existing containers' profiles fail closed on unknown arch (runc default `SCMP_ACT_ERRNO` for unmatched arches) → **Linux sandboxes refuse NT binaries until policy adds the arch** — correct default posture; add an `ntabi` profile vocabulary later.
- **ptrace/gdb:** NT SSNs would surface in `orig_ax`/`ax` to Linux-shaped tracers; syscall-restart (`-ERESTARTSYS` machinery in `do_syscall_64`) must **never** be used on NT paths (no NT analog; blocking resumption is expressed as `STATUS_ALERTED`/APC re-waits, §6). NT-personality debuggers will be an NT-aware gdbserver (user-mode, later phase). Keep `PTRACE_SYSCALL` coherent by simply exposing raw NT SSNs — no lying.
- **ftrace/bpf:** add `nt:sys_enter`/`nt:sys_exit` tracepoints decoding SSNs against the stable table (perf tooling under tools/testing + a `perf` python decoder). Cheap, high debug value.
- **livepatch/KLP:** NT table entries are plain `const` function pointers — patchable with `ftrace`-based klp like any other function. No constraint.

### 2.7 Per-mm vs per-task table — per-task, decisively

NT threads can share an mm with… only other NT threads (one personality per process by construction in our v1). But dispatch state must still be per-task because: (a) the entry path runs before any `mm` is guaranteed useful (kernel threads, `use_mm` patterns); (b) Linux's flag word lives at the kernel-stack tail (`TI flags`) — reachable in asm with zero extra loads; per-mm checks would race during execve mm swap. FreeBSD chose per-thread `td_sysentvec` for the same reason; copy that.

### 2.8 Dispatch coexistence bill of materials

1 `TIF_NTABI` bit + 1 branch in `entry_SYSCALL_64` + ~200 LOC entry glue in arch/x86 + new `kernel/nt/` subtree (service table, C services) + 1 `binfmt_pe.c` + 1 `AUDIT_ARCH` constant + tracepoints. **No MSR games, no IDT patching, no sys_call_table mutation, no livepatch tricks.** Everything reviewable as a normal subsystem.

## 3. NT Object Manager → kernel-resident object model

### 3.1 What it is (Windows Internals Part 1 7th ed + published RE)

- Every resource is an **object** prefixed by `OBJECT_HEADER`: `PointerCount` (strong refs), `HandleCount`, `NextObject` (lookaside), `HeaderInfo` bitfield (`NamePresent`, `HandleCountPresent`, `QuotaChanged`, `KernelObject`, `SecurityDescriptorPresent`, each bit push-lock-guarded), `CreatorBackwardInfo` (quota back to the creating process), `TypeIndex` (1-byte index into the type array — x64 handles embed type+sequence bits so a handle cannot name an object of the wrong type without the type check passing), `SizeOfBody`, then **variable-length info blocks** in fixed order: Name (`OB_NAME`: full path + parent dir short name), HandleInfo (cached `GRANT_ACCESS_MASK` per handle), ProcessInfo, QuotaInfo, EventTraceInfo, SecurityDescriptor.
- Two namespaces: the **name space** (object directories: hash + `ObpLockTree` push-locks; type-specific `ParseProcedure`s; symlink following with `OBJ_DONT_RESOLVE_LINK`), and the **handle space** (per-process, §4). A name is only a directory entry pointing at an object; named objects persist while the directory holds the pointer ref — this is why `CreateEvent("X")` then closing all handles then re-`OpenEvent("X")` succeeds. fds cannot be reopened by name.
- Directories we must materialize for compat: `\` root; `\ObjectTypes`; `\KernelObjects`; `\BaseNamedObjects` **per session** (`\Sessions\N\BaseNamedObjects` with Win32-level path substitution); `\GLOBAL??` (a.k.a. `\??`: DOS-device symlinks, `\NUL`, `\PIPE`, `\MAILSLOT`, COM/LPT); `\Device`; `\Driver`; `\FileSystem`; `\KnownDlls` (pre-built image **sections** the loader maps instead of re-reading disk — we reproduce it as a list of section objects pinned at boot from our ntdll set); `\RPC Control`; `\Windows` (LPC/ALPC ports); `\Security`; `\MSCTF.Asm`.

### 3.2 Mapping onto Linux kernel facilities

Linux has **no analog, and VFS inodes are the wrong substrate** (fd identity ≠ object identity, and most NT objects aren't files). Build it as a new subsystem from existing parts:

| NT concept | Implementation | Reused Linux facility |
|---|---|---|
| object allocation/lifetime | per-type `kmem_cache`, explicit destroy callback | slab allocator |
| strong ref (`PointerCount`) | dedicated 64-bit refcount with underflow trap (not `kref` — NT lets drivers observe `PointerCount` semantics incl. last-ref-to-zero destroy ordering and `ObReferenceObjectSafe`-style racing acquire; custom ops needed) | `refcount_t` internals as starting code |
| name directories | `struct nt_directory`: rhashtable of `nt_object_dir_entry` + rwlock; **lockref** (`lib/lockref.c`) is the same fastpath idea as NT push-locks for name lookups | rhashtable, lockref |
| path parse + symlink follow | recursive parse loop w/ per-type parse procs (~800 LOC) | custom; **do not** reuse fs path walk (link polarity + `OBJ_*` flag semantics differ) |
| case-insensitive objects/dirs | per-directory flag on lookup fn | ext4 **casefold** (fs casefolding API, DCACHE_OP_COMPARE) handles the *filesystem* side; `\??`/`\BaseNamedObjects` need our own CI compare |
| quota | per-process object+handle counters | `ucounts` pattern |
| `PreviousMode` | per-thread `USER_MODE`/`KERNEL_MODE` flag; every syscall entry sets USER; internal services take an explicit mode param (WSL1 lxcore did exactly this with `ExAcquire…` modes). Drives `ObReferenceObjectByHandle` probes and access checks. | 1 field, large compat payoff |
| session | v1: single session 0, `\Sessions\0\…` synthesized; map to `struct pid`/`user_namespace`/`session` conceptually, no hard binding | n/a |

Design rule (from WSL1's "translate where reasonable, implement where not"): every NT object is either (a) a pure nt-object (event/mutex/section/port/token…), or (b) a file wrapper holding a host `struct file *`/`struct inode *` (see §4 for the mapping). Never let a raw host `struct file` *be* an NT object identity for non-file types.

## 4. NT handle semantics — the precise delta list over Linux fds

Per-process **handle table**: 3-level directory (primary 4096 entries, secondary, tertiary pages), 16-byte entries on x64: qword0 = object fast-ref word (in-entry lock bit + 3-bit "handle protection" index since Win8 — `ProtectFromClose`/`ProtectFromChange`), qword1 = `GrantedAccess` (32) + `Attributes` (OBJ_INHERIT, OBJ_PROTECT_CLOSE, OBJ_INHERIT_ONLY, OBJ_OPENLINK-like per-entry flags). Free list = per-CPU lookaside → **entry reuse order is not FIFO and must not be assumed FIFO** (real NT recycles the most-recently-freed slot aggressively; apps that implicitly rely on value reuse do exist in the wild, rare). `[UNCERTAIN: E4]` exact x64 handle-value encoding (which bits carry table slot vs protection tag): pin against live VM with WinDbg `dt nt!_HANDLE_TABLE_ENTRY` + `!handle` corpus. Functionally: handles are 32-significant-bit opaque values storable in 64-bit `HANDLE`.

**Why fds are insufficient — enumerate:**

1. **Typed universality:** one HANDLE names files, pipes, registry keys, processes, threads, tokens, events, semaphores, sections, devices, ports, jobs, timers, symbolics, directories. `NtWaitForSingleObject(hEvent)`, `NtDuplicateObject(hProc,h)`, `NtQueryValueKey(hKey,…)` share one reference+access-check primitive: `ObReferenceObjectByHandle(h, ACCESS_MASK, POBJECT_TYPE, PreviousMode, &obj, &GrantedAccess)`. fds name open files only; futexes/timerfd/signalfd are per-feature patches, not a typed object contract.
2. **Sentinels:** NT `NULL` handle = 0 and `INVALID_HANDLE_VALUE` = `(HANDLE)(LONG_PTR)-1` both mean "nothing", while **fd 0 is valid (stdin)**. A `handle == fd` identity is structurally wrong at the most common API boundary (`CreateFile` failure returns -1). Our design makes this unrepresentable (separate table), but std-handle plumbing must synthesize genuine file handles wrapping host fds 0/1/2 (see §7.2, `RTL_USER_PROCESS_PARAMETERS.StandardInput/Output/Error`).
3. **Pseudo-handles:** `GetCurrentProcess() = (HANDLE)-1`, `GetCurrentThread() = (HANDLE)-2`; resolved specially *inside services* (`ObReferenceObjectByHandle` special-cases them), never table entries; `CloseHandle(-1)` must fail with `STATUS_INVALID_HANDLE (0xC0000008)` — apps rely on the exact code; pseudo-handles are not inheritable.
4. **Inheritance is per-handle opt-in and value-preserving.** NT children inherit *only* `OBJ_INHERIT`-flagged handles, and see the **same value**. Linux default is inverted (fd inheritance across exec is the legacy default; `FD_CLOEXEC` opts out). `CreateProcess(bInheritHandles=TRUE)` = duplicate-marked-handles into the child's *new* table before child runs (both tables locked). `OBJ_INHERIT_ONLY` (Win8+: flag set but not actually inherited) must work.
5. **Cross-process duplication is a kernel primitive:** `DuplicateHandle(srcProc,srcHst,dstProc,&dstH,Access,Inherit,Options)` incl. `DUPLICATE_CLOSE_SOURCE` (move) / `DUPLICATE_SAME_ACCESS`; authorized by `PROCESS_DUP_HANDLE` on the target process object — handle-table handle-stealing is a real security surface (lsass dumping, EDR, malware, and benign installers all use it). **Reproduce exactly; do not "improve."** No fd analog (SCM_RIGHTS requires both peers cooperating over a unix socket and cannot enumerate a target's table).
6. **Close = reference drop with object survival + file close-pending.** `CloseHandle` removes the entry immediately; the object dies at last `PointerCount`. A `FILE_OBJECT` closed while owning pending I/O enters **close-pending**: other handles to the same file object get `STATUS_FILE_CLOSED (0xC000012C)`; final teardown rides the I/O cancel path. Additions with no fd analog: `FILE_FLAG_DELETE_ON_CLOSE`, `NtSetInformationFile(FileDispositionInformation)` (delete on last close), `NtCancelSynchronousIoFile`.
7. **Per-handle granted-access + access checks at use time.** Every handle-using syscall rechecks `desired ⊆ granted`; type mismatch → `STATUS_OBJECT_TYPE_MISMATCH (0xC0000024)`; generic-mapping translation (`GENERIC_READ→FILE_READ_DATA|…` via the type's `GENERIC_MAPPING`) happens at open. fds carry zero introspectable access state; `EBADF` is fd-land's entire error vocabulary. This matters for apps that *re-derive* capabilities from handles (e.g., `QuerySecurityObject` on an inherited handle needs `READ_CONTROL` granted).
8. **Introspection:** `NtQueryObject` (`ObjectName`, `ObjectType`, `ObjectInformation` incl. real `HandleCount`/`PointerCount`, `HandleFlagInformation`) and `NtSetInformationObject` (event-signaling via handle!). Leak-detection tools (GFlags/AppVerifier) parse these counters — faking 1/1 breaks them. `/proc/self/fd` is not an equivalent contract.
9. **Leak behavior:** NT has no "EMFILE" class error — the table grows to tens of thousands (services historically leaked 100k handles). **Do not impose `RLIMIT_NOFILE` semantics on the handle table.** Use a very high per-process quota (NT-style quota failure only under memory pressure).
10. **Kernel-mode handles:** `OBJ_KERNEL_HANDLE`-created entries live in a system table, unreachable/uncloseable from user mode — our services called from kernel context must never leak user-visible handle values; keep an internal-table twin.

**Verdict:** a real handle table + object manager is mandatory (est. 6–10k LOC core). fd-identity shortcuts collapse points 1–9 and break apps on day one.

## 5. NTSTATUS — full space, Win32 mapping, and the errno traps

### 5.1 Structure

`typedef _Return_type_success_(return >= 0) LONG NTSTATUS;`
Bits: **31:30 severity** (0=SUCCESS, 1=INFORMATIONAL, 2=ERROR, 3=WARNING); 29 = customer flag; 28:16 facility; 15:0 code. Note the ordering: `STATUS_SEVERITY_WARNING = 0x8…` has the sign bit set while **INFORMATIONAL (0x4… and below) is "success-like"**: the universal idiom `NT_SUCCESS(s) == (s >= 0)` treats *all informational statuses as success*. Any kernel-internal code that uses signed comparison or boolean "ok" must replicate this partition exactly.

`RtlNtStatusToDosError` lives in **user mode** (our ntdll): a binary-search table + the well-known generic fallback (`(Status & 0xFFFF) | 0x00070000`-family → `ERROR_MR_MID_NOT_FOUND (1500)` for unmapped custom statuses). The full canonical status→Win32 mapping list is *published reference data* (Microsoft's documented NTSTATUS list); use it as spec input to a generator that emits our ntdll table. The kernel's duty is to **emit the correct NT status at the correct call site** — Win32 error codes are never a kernel concept (they don't exist pre-ntdll).

### 5.2 Concrete traps where naive errno mapping is wrong

| Status | Value | Win32 (GetLastError) | Trap |
|---|---|---|---|
| `STATUS_BUFFER_OVERFLOW` | 0x80000005 (**warning** = success by NT_SUCCESS!) | `ERROR_MORE_DATA (234)` | Means "we wrote what fit; the needed size is in the out-param; retry bigger". `RegQueryValueEx`/`GetMappedFileName`/`CmQueryValueKey` probe loops test `NT_SUCCESS` first — mapping to any errno (→ generic error) breaks the two-call probe idiom. |
| `STATUS_BUFFER_TOO_SMALL` | 0xC0000023 (**error**) | `ERROR_INSUFFICIENT_BUFFER (122)` | Collapsing with the above into one code is the single most common wrong result: probe loops either spin or false-succeed. **These two must remain distinct all the way to the Win32 layer.** |
| `STATUS_NO_MORE_FILES` | 0x80000006 | `ERROR_NO_MORE_FILES (18)` | FindNextFile loop terminator; must not collapse with `STATUS_NO_SUCH_FILE`. |
| `STATUS_MORE_PROCESSING_REQUIRED` | 0xC0000094 (error-severity but "keep going") | `ERROR_MORE_DATA`-ish contexts, `DeviceIoControl` internals | Used in pending-IRP/async create flows; errno "negative = failure" style treats it as failure → hangs. |
| `STATUS_TIMEOUT` | 0x00000102 (informational!) | `WAIT_TIMEOUT (258)` | Wait results are **statuses**; swallowing into `-ETIMEDOUT` corrupts the wait-state channel. |
| `STATUS_USER_APC` (0xC0), `STATUS_KERNEL_APC` (0x100), `STATUS_ALERTED` (0x101) | informational | `WAIT_IO_COMPLETION (240)` | §6.4 alertable-wait channel. |
| `STATUS_PENDING` | 0x00000103 | — | Async completion handshake with `IO_STATUS_BLOCK`; internal blocking code paths must never leak it. |
| `STATUS_SHARING_VIOLATION` | 0xC0000043 | `ERROR_SHARING_VIOLATION (32)` | No errno analog whatsoever (§11.3). |
| `STATUS_OBJECT_NAME_COLLISION`/`_NOT_FOUND`/`_PATH_NOT_FOUND` | 0xC0000035/0x34/0x3A | `ERROR_FILE_EXISTS/FILE_NOT_FOUND/PATH_NOT_FOUND` | POSIX collapses name-vs-path misses and treats them per-call; NT distinguishes at *object* layer. |
| `STATUS_REPARSE` (0x104), `STATUS_NOTIFY_ENUM_DIR` (0xC0), `STATUS_CANCELLED` (0xC000012A→`ERROR_CANCELLED 1223`) | | | structural statuses seen by ordinary apps. |

### 5.3 Design mandates

1. **Typed error channel:** internal services return `typedef s32 ntstat_t`; C helpers `STATUS_*` verbatim from our generated header; **CI lint rejects `ENOSYS/EINVAL/…` and `-errno` returns under `kernel/nt/`** (allow errno only at explicit host-call-site converters).
2. **errno→NTSTATUS conversion is contextual, not a function.** `-ENOENT` from `kern_path` may be `STATUS_OBJECT_PATH_NOT_FOUND` (a middle component died) vs `STATUS_OBJECT_NAME_NOT_FOUND` (last component) — decided by *which call* failed. Rule: convert at each call site's branch with the site's knowledge; provide a table only for the unambiguous codes (EACCES→`STATUS_ACCESS_DENIED` 0xC0000022, EAGAIN→`STATUS_IO_REQUEUE_APF`? no — EAGAIN→`STATUS_INSUFFICIENT_RESOURCES`? no: EAGAIN on nonblocking IO→`STATUS_PIPE_EMPTY`-family or `STATUS_TIMEOUT`-ish **per call**; enumerate per service). This table IS a deliverable; build it service-by-service, not globally.
3. **Never translate statuses back to negative errnos** on the NT return path (`regs->ax` is written unsigned; ensure the common exit path's `regs_set_return_value` sign-extension and `-ERESTART*` fixups don't touch the NT path).
4. `RtlNtStatusToDosError` fallback must match real NT behavior for unmapped codes (`0xC0000xxx` → `ERROR_MR_MID_NOT_FOUND` 1500; customer bit passes through) — apps compare `GetLastError()` to 1500 to detect unknown errors.

## 6. IRP / I/O manager, asynchronous I/O, completion ports, APCs, alertable waits

### 6.1 What to keep vs discard

The **IRP as WDM artifact** (32 major codes, layered device stacks, `IoCallDriver`/`IoCompleteRequest`, pending queues, MDL chains through filter drivers) **is not reimplemented** — no third-party NT drivers run here. What must survive is the *contract* that user mode observes through `NtReadFile/NtWriteFile/NtDeviceIoControlFile/NtFsControlFile` + the wait machinery:

- `NtReadFile(hFile, hEvent, PIO_APC_ROUTINE apc, PVOID apcCtx, PIO_STATUS_BLOCK iosb, PVOID buf, ULONG len, PLARGE_INTEGER byteOffset, PULONG key)` → sync: `STATUS_SUCCESS` + filled iosb; async (`FILE_FLAG_OVERLAPPED` handle, or `FileAsynchronous` set): `STATUS_PENDING`, completion later fills iosb, **sets hEvent, queues the APC, and posts to the handle's completion port** (order documented below).
- Byte offset `-1`/NULL semantics per file create flags; `FILE_SYNCHRONOUS_IO_ALERT` vs `_NONALERT` (position update atomicity + alertability) are observable.

### 6.2 Reused host facilities (real files/structs)

| NT need | Host facility | Verdict |
|---|---|---|
| sync file read/write/flush | `vfs_read`/`vfs_iter_read`, `filp_flush` (fs/read_write.c) | **Direct** (called in-kernel from the service with the wrapper `struct file`) |
| async completion engine | `io-wq` (fs/io-wq.c) worker model + `task_work_add` (kernel/task_work.c) for the on-return-to-user leg; or `struct io_kiocb`-shaped private request object — **v1: private `nt_io_request` on a dedicated `workqueue`/`io_worker` subset + `wait_queue`** to avoid coupling to io_uring internals | **Adapted** |
| blocking waits | `wait_queue_head_t`, `io_schedule`/schedule_timeout pattern | **Direct** |
| timers | `hrtimer` (kernel/time/hrtimer.c), `timer_slack` | **Direct**; `NtSetTimer` (100ns absolute/relative, period, TMR_MASK flags) → hrtimer + APC-or-event expiry |
| pipes | `fs/pipe.c`, unix sockets | Named pipes are **message-mode, bidirectional, duplex**; v1: AF_UNIX `SOCK_SEQPACKET` pair + our ioctl shim (**Adapted**); server-side `ConnectNamedPipe` overlapped = §6.3 |
| sockets | `net/socket.c` in-kernel `sock_create_kern`/`sock_recvmsg` | NT sockets are *not syscalls*: winsock (user-mode ws2_32/`mswsock`) → `NtDeviceIoControlFile(IOCTL_AFD_*)` at `\Device\Afd`. We implement an **Afd device object** translating to kernel socket ops (exact WSL1 mirror). **Adapted**, ~2k LOC translator (IOCTL list is RE'd spec data — see AFDDoc/OpenNT) |
| sections/views | shmem (`mm/shmem.c`, `shmem_file_setup`), `vm_mmap`/`do_mmap`, CoW via `vm_insert_page` | **Adapted** (Section object = file wrapper over shmem or host file; `SEC_*` flags → pgprot; page protection deltas D via `page protection` engine) |
| byte-range locks | `struct file_lock`/`posix_lock_file`/OFD (`fs/locks.c`) | **Adapted** — see §11.4 mandatory-lock asymmetry |
| futex | `kernel/futex/` | NT user-mode fast paths (critical sections, slim sync primitives) are **user-mode ntdll over `NtWaitForAlertByThreadId`** — that one syscall needs a real futex twin: map `NtWaitForAlertByThreadId` + `NtAlertThreadByThreadId` onto futex-wait/wake with NT's *alert* (not just wake) semantics; **Adapted, small** |

### 6.3 NEW facilities this area requires

**(a) I/O completion ports.** No host analog (epoll/io_uring are not kernel-producer consumable as an NT port). ~600–1000 LOC: `struct nt_iocp { llist_head queue; wait_queue; ULONG nthreads; EX_KEYED ...}`; `NtCreateIoCompletion`/`NtSetInformationFile(FileCompletionInformation)` binds a `FILE_OBJECT`→port with completion key; on request completion the I/O path posts a packet `{key, overlapped_ptr, status, information}`. Consumer: `NtRemoveIoCompletion(Ex)` = `llist_del_all` else interruptible sleep on the wait_queue w/ timeout. NT subtleties to reproduce: `NtRemoveIoCompletionEx` batched array + *reserve* count; worker-round-robin wake order is unobservable → reproduce return codes (`ERROR_QUEUE_EMPTY/NOT_EMPTY/ALL_ITEMS_RESERVED`) not scheduler fairness. **Keyed events** (`NtCreateKeyedEvent/WaitPulse`) = llist-per-key + wait_queue; ~300 LOC; used by ntdll's own loader-lock-free structures.

**(b) User APCs + kernel-alertable waits — genuinely new.** Closest precedent is **signal delivery**: `arch/x86/kernel/signal.c:setup_rt_frame()` already *builds a user-mode call frame on the user stack and redirects RIP to a kernel-known user entry (sigreturn trampoline)*. NT user APCs are the same trick with a different frame: queue `struct nt_user_apc{routine, arg1, arg2, iosb}` on `nt_thread->apc_queue` (sorted, `InsertQueueApc` priority: `IoApcPriority > ... > Normal`), set a new `TIF_NT_APC` work bit consumed in `exit_to_user_mode_loop()` (kernel/entry/common.c — the *existing* generic TIF-work hook: correct integration point, zero cost when bit absent), and rewrite the IRET frame: new RIP = `TEB->...` our ntdll's `KiUserApcDispatcher`, RCX/RDX/R8 = apc args, RSP = user APC frame with the previous context chained for **`NtTestAlert`/`RtlRestoreContext`**.
**Alertable waits:** `NtWaitForSingleObjectEx(bAlertable)`: sleep on the object's wait_queue **interrupted by** TIF_NT_APC work (pattern: add wait_queue_entry with custom `wake_func`, or use `prepare_to_wait_exclusive` + check of `apc_queue` non-empty as wake condition); on APC wakeup: *deliver APCs in kernel-ordered loop first* (return-with-`STATUS_USER_APC` contract: dispatcher runs queued APCs, then the wait is **re-entered**, not failed — NT's `KeWait…UserModeApc` contract, not the signal-EINTR contract). `NtTestAlert` = flush+`STATUS_SUCCESS`/`STATUS_ALERTED`. Getting this re-wait vs fail-back distinction wrong breaks every overlapped console/pipe app; it is a *semantic* difference from signals — call it out in the kernel patch.

**(c) Wait/status multiplexing:** wait services return `WAIT_0+i` (0x0–0x3F, informational statuses) or `STATUS_USER_APC`/`STATUS_TIMEOUT`/`STATUS_ALERTED`/`STATUS_IO_PENDING`… as §5.2; internal `-ERESTARTSYS` must be intercepted by the NT path and *never* reach user mode.

### 6.4 Verdict for §6

Memory/scheduler/timer/futex/wait-queue/VFS reuse: **direct or thin-adapted**. IRP: **discarded except its user-visible contract**. APCs+alertable waits: **new, on existing precedent** (signal frames + TIF work). Completion ports: **new, small**. Afd/ioctl device translation: **new shim, high LOC but low risk**. This does *not* extrapolate to §7–§8 (loader is the bigger problem).

## 7. KPCR / KPRCB / TEB / PEB and GS-relative access

### 7.1 What each is, and which we implement

- **KPCR/KPRCB**: kernel-side per-CPU state (Self pointer, `CurrentPrcb`, `RspBase` at gs:0x1A8 for syscall stack switch, DPC/processor state, and the **KiProcessorAccessors** table at `KPCR+0x320`: `SystemGetLocale`/`GetTickCount`-style thunk pointers user-mode used to call via `gs:` — modern ntdll avoids them). **We do not implement KPCR.** No user-mode code can address it (ring 0), and our syscall entry uses Linux per-CPU infra. The only NT-visible residue is accessor calls — `[UNCERTAIN: E5]` whether modern binaries still call KPCR accessors on x64 (legacy x86-era pattern; `gs:[0x1234]` thunk calls). Experiment: scan corpus for `call qword ptr gs:[...]` (vs `mov` reads of TEB offsets); expect zero. If nonzero, synthesize via TEB.
- **TEB (user GS anchor)**: `NT_TIB64` at +0x00: `ExceptionList(gs:[0x0], unused on x64)`, `StackBase +0x08`, `StackLimit +0x10`, `SubSystemTib +0x18`, `FiberData +0x20`, `ArbitraryUserPointer +0x28`, `Self +0x30`. Then `EnvironmentPointer +0x38`, `ClientId +0x40 {UniqueProcess, UniqueThread}`, `ActiveRpcHandle +0x50`, `ThreadLocalStoragePointer +0x58`, **`ProcessEnvironmentBlock +0x60`**, `LastErrorValue +0x68`, `CountOfOwnedCriticalSections +0x6C`, `CsrClientThread +0x70`, `Win32ThreadInfo`, `User32Reserved`, many reserved fields, `CurrentLocale`, `FiberData`, `DeallocationStack`, `TlsValues/FinalValues`, `TlsExpansionCounter`, `TlsBitmap(+Bits)`, **`TlsSlots[64]` at +0x5E0?** … **exact offsets are the ABI** for every compiler's TLS access (`gs:[0x2d80]` style static TLS) — `[E6]` transcribe from mingw-w64 `winnt.h` + ReactOS `ndk` `tntstruct.h`, then **verify against disassembly of a stock `C++ thread_local`/`__declspec(thread)` binary's gs-relative accesses on a real Windows VM** (the public headers are accurate for modern builds; still verify — this is the kind of thing an off-by-8 kills silently).
- **PEB**: `InheritedAddressFlags/BeingDebugged/NtGlobalFlag +0x02`, `CriticalSectionTimeout +0x08`, `HeapSegmentReserve/Commit`, `OSMajorVersion +0x118`, `OSMinorVersion +0x11C`, `OSBuildNumber +0x120`, `OSPlatformId +0x124`, `ImageBaseAddress +0x10`, `Ldr +0x18` (`PEB_LDR_DATA`: `InLoadOrderModuleList/InMemoryOrderModuleList/InInitializationOrderModuleList` — the module list every ETW/tlhelp/tooling walks), `ProcessParameters +0x20` (`RTL_USER_PROCESS_PARAMETERS`: `Reserved1[16]`, `ImagePathName`, `CommandLine`, `CurrentDirectory{DllPath:UNICODE_STRING, Handle}`, `StdHandle[3]`, `RuntimeModule? (Win11)`, `DefaultWindowStyle`, `Flags`, `ConsoleHandle/ScreenBufferSize/CursorSize/AttributeSize/Title`, **Desktop**, `ShellInfo`, `RuntimeData`, **`Environment` (PWSTR double-null block)**), `AddressOfReserved?` / `SystemReserved[512] +0xE8` (kernel/loader: `__readgsqword(peb+0x2d0? no)` — contains `PEB_KUSER_SHARED_DATA`? on x86 SystemReserved[2] was `PEB addresses`; on x64 loader stores **`KdCopyDataBlock`-era `KernelHandleIndexes`** — third-party code indexes into it; keep the array real-size 512 qwords, contents: loader-private → our ntdll's private meaning + documented zeros), `FlsBitmap(+HighBit)`, `FlsCallback/FlsListHead`, `BitField` (`IsVirtualized`, `TerminatingOnIdling?`), `ReadOrderVectorCount`, `ImageKey*, SystemReserved2[...][3]` (SxS activation contexts: **`SystemReserved2[1]` = `ACTCTX` array — apps read it**), `KpBlock`, `UtcCompiledTime`, `StaticUnicodeString/Buffer`. `BeingDebugged` is kernel-writable via `NtSetInformationProcess(DebugPort)`.
- **KUSER_SHARED_DATA at fixed `0x7FFE0000`** (read-only user page, mapped into every NT process; the 0x7FFDFFFF0000–0x7FFFFFFFF000 "shared user page" guard layout): `TickCount (ULONGLONG TickCount1580@0x320?)` — *exact offsets per public headers, verify E6* — `TickCountLow + TickCountMultiplier (0x320/0x328)`; **`SystemTime/TimeZoneBias` as `KSYSTEM_TIME` seqlock triples — the same readseqlock pattern Linux's vDSO `vdso/ts_clocksource.c` uses; we push values from `update_vsyscall()` (kernel/time/timekeeping.c) into our KUSER page — one store at an existing timekeeping point, zero polling**; `BootTime`, `TimeZoneInformation`, plus the compat-critical static fields: `MajorVersion/MinorVersion/BuildNumber`, `ServicePackMajor/Minor`, `CSDVersion`, `PlatformId=2`, **`NtProductType`** (1=NT Workstation/Server persona choice §11.7), `ProductType`, `SuitesMask`, `VendorInfo`, `MaximumStackSize/CommitSize`, `PageSize=0x1000`, `NumberOfPhysicalPages`, `AvailablePages`, `ProcessorFeatures`, `ActiveProcessorMask`, `NumberOfProcessors`, `ConsoleHandle`, `SharedUserDataPageFlags`, `MmCopyMemory/AvailableMemoryInformation`, **`SessionId`, `SessionStandbyCacheLimit`, `IsWown64Emulator`(!)**, `ProtectionMode`, `KdDebuggerEnabled`, `GuestAndHostState`, … ~40 fields matter for the apps we target.

### 7.2 Kernel's duties establishing PEB/TEB for a natively-loaded PE

(§2.2's `load_pe_binary` does this synchronously.) Kernel allocates and fills, **before first user execution**:

1. PEB page: `ImageBaseAddress` = mapped image base; `OSVersion` fields ← frozen version persona (§11.7); `Ldr` zeroed lists (our user-mode Ldr populates — kernel never builds module lists); `ProcessParameters` blob constructed **in the PEB page region** (NT allocates it right after PEB in the PEB allocation; the parent→child copy at `NtCreateUserProcess` is via the "PEB copy section" — for Linux-parent launches we synthesize directly); `NtGlobalFlag` ← policy (heap-verify bits for compat tools); `SystemReserved` zeroed; `StdHandle[0..2]` = **synthesized handles to new file objects wrapping host fds 0/1/2** (the one-time fd→handle bridge; `-1` if the fd is closed).
2. TEB (main thread): `StackBase/StackLimit` from our NT stack mapping; `Self`, `ArbitraryUserPointer=0`, `ClientId{UniqueProcess = kernel u32 id from our `nt_ida` (NT ids are 4-byte PspCidTable values — use `ida_alloc` for process/thread ids so `ClientId`/`WaitForMultipleObjects` handle identity matches NT's shape), UniqueThread same}`, `ProcessEnvironmentBlock`, `TlsSlots/TlsExpansionSlots` = 0 (loader/user-side `TlsAlloc` fills; kernel never), `PreferredNode`? reserved, `GuaranteedStackBytes`, `Vdm`? skip, `ActivationContextData/AssemblyStorageMap` = loader-private pointers (0), `LastError=0`.
3. `thread.gsbase = TEB` + `X86_GSBASE_ACTIVE` (existing lazy restore path); ELF-incompatible **mmap base for NT mms** so 0x7FFE0000 is reservable (set `mm->mmap_base` at load time to an NT-shaped value; verify `mmap` for NT tasks still respects hints).
4. KUSER page: shared, RO in user, kernel-writable; static fields filled at first NT exec (boot-time constants), dynamic time fields from the timekeeping mirror.
5. **Thread start contract:** `entry = ntdll!RtlUserThreadStart`, `RCX = ThreadStartContext pointer`, stack initialized with the `RTL_…` contract our ntdll expects; kernel also seeds the `TEB.StackLimit` to the *commit* end and installs a **guard-page→commit handler** in our NT fault path (§8 commit ledger; Linux's stack-growth machinery `expand_downwards` is per-VMA downward-growable; NT stack needs its own grower with `DeallocationStack` bound + `GuaranteedStackBytes` pre-commit for exception frames — a *new small* mm adapter, ~300 LOC in the NT fault path, reusing `__vm_mmap`-region manipulation).
6. `NtCreateThreadEx` later: kernel duplicates 2–5 per thread; `DbgSsReserved`, `HardErrorMode=0`, `HvPageTableKernel/VirtualRanges` (HV — 0).

### 7.3 Exception dispatch (GS-adjacent, kernel duty)

x64 PE SEH = `.pdata`/UNWIND_INFO tables + `RtlDispatchException` — **user mode**. Kernel duties: (a) on a trap from an NT task, translate `X86_TRAP_*`/`pt_regs` → `EXCEPTION_RECORD` + `CONTEXT` **on the user stack** and redirect to our ntdll `KiUserExceptionDispatcher` (same frame-injection precedent as `setup_rt_frame`; **new** hook in `arch/x86/kernel/traps.c`/`mm/memory.c` fault path gated by `TIF_NTABI`: `do_trap`→`nt_deliver_exception`, ~400 LOC + the `NtContinue`/`NtRaiseException` services); page faults → `STATUS_ACCESS_VIOLATION` with `ExceptionInformation[0]=read(0)/write(1)/execute(8)` mapped from `error_code` bits (`X86_PF_USER/PROT/INSTR/Rsrvd`); (b) unhandled → process teardown with `RtlExitUserProcess(status)` semantics + WER-style exit status (v1: exit code = status low bits); (c) `STATUS_GUARD_PAGE_VIOLATION (0x80000001)` must be *informational-warn* consumed by the stack/commit grower **before** becoming user-visible — this is exactly how NT's demand-zero stack commit works; our NT-stack grower rides this path; (d) **`DBG`/KUSER `KdDebuggerEnabled` consistency** (§11.8); (e) `NtQuery/setInformationThread` for `CONTEXT` = pt_regs↔CONTEXT converter (registers fully; **Dr0–7 debug regs via existing arch/dbg.c per-task hw_breakpoints — direct reuse**; `NtSetContextThread` = `user_regset_write`-style, reused).

## 8. Per-subsystem verdict: direct / new / adapted

Do NOT extrapolate one subsystem's answer. Subsystem-by-subsystem (D=direct on existing Linux facility, A=adaptation layer, N=NEW kernel facility, U=user-mode personality runtime, X=out of Phase-1 scope):

| NT facility | Verdict | Mechanism / host anchor |
|---|---|---|
| Syscall dispatch | **N** (small) | §1–2: TI flag + stub branch + const table |
| Process/thread create, join, exit codes | A | `copy_process`/`do_exit` internals are NOT directly callable per-instance; build NT process = `kernel_thread`-like `create_io_thread`-analog with custom clone flags + `wait_queue` on our `nt_process`; exit status = our field + `do_exit` hook (exit_code is NT-compatible: NT exit codes and Linux `WEXITSTATUS` don't collide if we carry full 32-bit via our own; `PTRACE` sees Linux exit code → known asymmetry) |
| Scheduler/priorities/affinity | D mostly; boost=A | EEVDF; 31 NT priorities→`sched_setscheduler` SCHED_OTHER nice/weights + RT for `TIME_CRITICAL`; **NT priority boost on event wake: unmapped to CFS → drift, document**; `KAFFINITY`→cpumask **D**; priority decays differ |
| Virtual memory (reserve/commit/probe/query) | A + N(commit ledger) | `vm_mmap`/`vm_munmap`/`vm_brk` **D**; NT `MEM_RESERVE`→`VM_IO`-less anon PROT_NONE mapping; `MEM_COMMIT`→mprotect+touch; `MEM_DECOMMIT`→`MAP_FIXED` anon reclaim — **per-commit ledger = N-small** (atomic64 counters, `CommitLimit` for `NtQuerySystemInformation(MemoryInformation)` honesty); NT allows unmapping partial regions → `VM_DONTFORK`? no — fine natively |
| Protection changes / `PAGE_GUARD` / `PAGE_WRITECOPY` | D | `mprotect`/`userfaultfd`? no — `mprotect(PROT_NONE)` + our fault handler for guard; WRITECOPY = `MAP_PRIVATE` CoW D |
| Stack growth | A+N | §7.2.5 grower on `STATUS_GUARD_PAGE_VIOLATION`, bounded by `DeallocationStack` |
| Sections/views | A | shmem/tmpfs + `vm_mmap` D; `SEC_*` + image-section (loader maps `PAGE_EXECUTE` views — Linux W^X? `mm` allows x after rw via mprotect; **no `VM_WX` panic on Ubuntu?** Ubuntu kernel has no CONFIG_STRICT? fine, verify module params) |
| Files (create/open/read/write/close) | D+A | `filp_open`/`vfs_*` D; **share-access enforcement = N-small** (inode→our share-registry; deny→`STATUS_SHARING_VIOLATION`); `FILE_ATTRIBUTE_*`, reparse points → `FILE_ATTRIBUTE_REPARSE_POINT` synthesized for symlinks **A**; DOS names/8.3 **X** (stub) |
| Directory enumeration | A | `iterate_dir`/dir_context D-core; the 7 `FILE_DIRECTORY_*` INFO layout converters **A** (exact field packing, `NextEntryOffset`, file index stability → inode number) |
| Change notifications (`NtNotifyChangeDirectoryFile`) | A | **dnotify/fsnotify (fs/notify/) as event source** — inotify/fanotify are user APIs; `fsnotify` kernel hooks (`fs/notify/mark.c`) are callable — subscribe per-watch, translate to `FILE_NOTIFY_INFORMATION`, pending-IRP style completion via §6.3 engine |
| File locks | A | `file_lock` core; **mandatory enforcement only within NT path** (§11.4) |
| Devices/ioctls/`\\Device` namespace | **N** (namespace+dispatch) + A | our `nt_device_ops` table; `NtDeviceIoControlFile` → handler; AFD/pipes/console are devices here |
| Named pipes | A | §6.2 |
| Sockets | A | Afd shim + `net/` in-kernel; **winsock itself = U** (ws2_32) |
| Atoms | **N-trivial** | rhashtable+freelist, 150 LOC |
| Tokens/access checks | N (v1 = DAC-lite) then N | token object wraps `cred`/uid/gid SIDs; **`Ob`-level `ACCESS_MASK` vs granted check D-small**; full `AccessCheck` w/ ACEs = U(ntdll RtlpAccessCheck?) no—kernel `SeAccessCheck`: N, large, deferred; impersonation (named pipes)= X v1 |
| **Registry** | **N (largest new subsystem after loader)** | hive reader (regf/hbin/kb/cell format = published spec data + libregf docs — original implementation) under `kernel/nt/registry/`; kernel-resident read cache; write = N (log-file replay — defer durability; v1 write-through with host `filemap`); `NtNotifyChangeKey` on our wait machinery; **no host analog** (cgroup sysctl? irrelevant) |
| Object manager/handles | **N** | §3–4 |
| Dispatcher objects (event/sem/mutex/timer/resource) | **N small each** | `wait_queue` primitives; executive-resource = rwlock+ownership-list analog |
| APCs / alertable waits / `NtAlert*` | **N** | §6.3(b) |
| Completion ports / keyed events | **N** | §6.3(a) |
| Time (`QueryPerformance*`, tick, timezone) | D | clocksource via KUSER mirror §7.1; `KeQueryPerformanceCounter` = TSC invariant path = same source as vDSO; QPC frequency via `cpu_khz` |
| `NtQuerySystemInformation` | A | per-class: task list (`for_each_process` D), CPU info (arch/cpuid + topology `arch_register_cpu`? just read `cpuinfo`), PerformanceCounter (D), HandleInformation (`NtQuerySystemInformation(SystemHandleInformation)` walks **all** nt handle tables — our subsystem provides it natively D) |
| Jobs objects | A | cgroup v2 (kernel/cgroup/) as limits backend: memory/CPU **A**; nested-job semantics = X v1 |
| KnownDlls | N-trivial | boot: pre-create section objects for our ntdll set + `\KnownDlls` dir (loader compat + WinSxS) |
| LPC / **ALPC** | ALPC = **N-large, deferred**; LPC = N-small (needed by conhost-era console) | console path v1: Linux **pty** as backend behind a synthesized `\Device\ConIn/ConOut` (A); `WriteConsole`-family behaviors = U |
| GUI (win32k, windows, messages) | **X** | needs §1.4 reserved shadow table + window server; out of scope; architecture keeps a slot so it isn't a redesign later |
| WoW64 / PE32 / ARM64 | **X** | separate design doc |
| ETW, WMI, .NET strong-name/CLR host, drivers | **X** | note: managed apps need CLR host = U-scale project; treat as Phase 3+ |

**Summary judgment:** NT semantics split roughly: file/process/thread/time/mem/pipe/socket/notify backends = *reuse* with translation; the entire **object-handle-wait-APC-completion spine = new but small and self-contained**; **registry + security model = new and large**; loader/heap/RTL/Win32 = *user-mode, huge, but zero kernel risk*. The risky claim "we just wrap VFS" is false for the spine (correctly new), and the claim "we need a new kernel" is false for the backends (correctly reused).

## 9. Kernel vs user space — the split and the reasons

**Kernel (`kernel/nt/`, `fs/binfmt_pe.c`, arch/x86 entry+traps+process_64 hooks):**
NT syscall entry/dispatch; object manager + handle tables; all dispatcher objects, waits, alertable semantics, user APCs, completion ports, keyed events; file/device/section objects + async engine + share access + commit ledger; `\Device`/`\??` namespace + ioctl dispatch incl. Afd and console-pty bridge; PE binfmt loader (address space, PEB/TEB/KUSER, GS anchor); exception→dispatcher frame delivery; registry service core; `NtQuerySystemInformation` data pulls; process/thread lifetime & ids.
*Why:* (1) every one of these must be reachable synchronously, in arbitrary process context, **with no cooperating user daemon** — Interix's user-server model is the documented failure; (2) they need privileged primitives (page tables, scheduler hooks, handle tables of *other* processes for `DuplicateHandle`, trap redirection); (3) they are where NT's own contracts (PreviousMode, quotas, seqlock time page) live — faking them from user mode reintroduces exactly the ABI-betrayal WSL1 proved avoidable.

**User mode (the personality runtime — our "ntdll-equivalent + baseldr + kernel32/ucrt bridge"):**
`Ldr*`: PE mapping *decisions* (relocations via `.reloc`, import/delay-load binding, **TLS directory callbacks**, activation contexts/SxS, module lists in PEB), heap manager (`RtlCreateHeap`… on our memory syscalls), all `Rtl*` (UNICODE_STRING, AVL/Splay trees, SID/ACL *manipulation*, `RtlImageDirectoryEntryToData`), ntdll syscall stubs (SSN-ABI surface), **SEH walker** (`RtlDispatchException`/`RtlVirtualUnwind` over `.pdata`), `KiUser*Dispatcher` trampolines, `RtlNtStatusToDosError` table, `NtRtl*` futex fast paths, Win32 layer (`kernel32`/`advapi32`/`user32` split), winsock provider.
*Why:* that is where *real* NT puts them (ntoskrnl vs ntdll), and compatibility observability follows the same boundary: module lists, heap debug flags, loader locks, `Rtl*` exports are inspected by apps and tools at these exact addresses. Keeping the boundary honest is what makes "native personality" not "Wine wearing a hat": **Wine's fault is not having user-mode libraries — NT has them too — it is that Wine's libraries implement Win32 over the *foreign* ABI's syscalls. Ours implement it over our own kernel's NT-semantics syscalls.**

## 10. Dormancy / zero cost when no PE runs

1. `CONFIG_NT_PERSONALITY` (default n): the tree, the binfmt, the TI bit consumption, the branch — compiled out → byte-identical Linux. Ubuntu kernel for this OS flavor enables it; it must remain possible to run *without*.
2. Built-in, no PE ever: cost = one never-taken `test/jcc` (flags word already on register — merge into the existing `Ti_flags` work check on the syscall slow path so it is literally the same instruction sequence for Linux tasks); two NULL pointers per task (`pahole` check: +16 B `task_struct`); one binfmt list node; a case label in seccomp/audit arch tables. **Nothing allocates, polls, wakes, or reserves.** KUSER/commit-ledger/handle tables appear only at PE exec.
3. `ntabi.exec_enabled=0` boot param/sysctl: loader refuses (`-EACCES`), dispatch returns `STATUS_NOT_SUPPORTED` — a kill switch without rebuild; plus the LSM hook so Ubuntu policy can deny PE exec per-apparmor-profile.
4. Perf gate in CI: `will-it-scale syscall` and `kernel selftests` benchmarks must show <0.5% regression config-on vs config-off on a Linux-only workload (the only measurable interaction is I-cache/branch-target pressure — measure, don't argue).
5. Security-surface argument: always-on additions = entry branch + PE parser. Parser hardened (bounded `memdup_user` reads of headers, reject > reasonable section counts, syzkaller descriptors from day one — precedent: `binfmt_elf` survives continuous fuzzing; ours must too).

## 11. Known compatibility traps (beyond those already noted)

1. **Probe-once argument copying:** NT copies stack args into the trap frame *once*; services must use the copy (a user thread freeing the args mid-call is benign on NT, UAF on a lazy-read design). Our arg-copy at entry (per service stack-byte count) reproduces it — do not read user memory twice without `ACCESS_OK` discipline.
2. **`IO_STATUS_BLOCK` is an out-param written by the *kernel* at completion** including on the sync path *and* on failure paths (many Win32 layers `RtlNtStatusToDosError(Iosb.Status)` and ignore the syscall return — both must be consistent; sync services that return `STATUS_SUCCESS` must also write iosb).
3. **Share access vs host daemons:** journald-style processes holding our exe/registry files open must use compatible share masks; our own init plumbing (log fds) uses NT-side opens with `FILE_SHARE_READ|WRITE` — enforce NT rules *between NT peers only*; Linux peers ignore share masks (asymmetric but matches real mixed environments where SMB clients misbehave — document).
4. **Mandatory file locks:** Linux OFD locks are advisory to non-locking openers; NT locks are mandatory *within our enforcement set* only. Cross-personality file locking is broken by construction; same as WSL1-era drvfs behavior — document, never silently vary.
5. **Console:** `\Device\ConIn/ConOut` over host pty: NT console semantics (modes via `SetConsoleMode` = U layer; `ReadConsole` filtering, ctrl-C events → **`CTRL_C_EVENT` delivery = user APC into the console-owning thread with `STATUS_ALERTED` channel** — do not use Linux SIGINT).
6. **8.3 names, alternate data streams** (`:$DATA` suffix parsing! path `file.txt:stream` is legal POSIX but NTFS-ADS syntax; our parser must treat `:` per DOS rules on the NT side only) — v1: no streams, reject ADS with `STATUS_NOT_SUPPORTED` honestly (some installers use `::$DATA` on *every* path — must accept the trivial stream name).
7. **Version persona:** freeze one (proposal 10.0.20348 NT Win persona, `NtProductType=NTWinNT`, `SuitesMask` conservative, `IsWown64Emulator=FALSE`); **every version gate in every component (PEB, KUSER, registry `CurrentVersion`, `Rtl*VersionApis`, `VerSetConditionMask` in user-mode) must agree** — disagreement is what trips updaters and DRM; test with `Get-VersionReport` corpus tool.
8. **Debug/anti-debug triad:** `PEB.BeingDebugged` + `NtGlobalFlag` (FLG_HEAP_* bits) + `KUSER.KdDebuggerEnabled`/`NTGlobalFlag` mirror must be *internally consistent with an actually-absent NT debugger* (v1: always 0/0/FALSE until an NT-aware gdbserver exists, then update all three atomically with debug-port attach — `NtSetInformationProcess(ProcessDebugPort)`).
9. **Case-insensitive filesystems vs NT case preservation:** store exact names, compare CI; host ext4/xfs case sensitivity means `\\ntfs-like mounts` of Linux-native dirs must not break CI lookups — our path translation does CI compares at *our* VFS layer when the underlying superblock lacks `S_CI` (casefold). Perf trap, not correctness trap.
10. **Direct-syscall hardcoders** (§1.4).
11. **`STATUS_` vs `WAIT_` in every API boundary:** `WaitForSingleObject` returns DWORD WAIT_* derived from `RtlNtStatusToDosError`-adjacent mapping **but also** `WAIT_ABANDONED_0+i` from `STATUS_ABANDONED (0x8000001A)` — abandoned mutexes are *warning-severity success-like*; collapsing to error breaks `AcquireMutex` wrappers.
12. **Thread stack `Committed` vs `Reserved` for the *initial* thread:** `NT_TIB.StackLimit` must reflect *commit*, not reserve, or CRT `_alloca` probes walk garbage.
13. **PE header fields with kernel meaning:** `DllCharacteristics` (`HIGH_ENTROPY_VA` → ASLR entropy choice; `NX_COMPAT` → enforce DEP; `GUARD_CF` → **user CET: Linux already has user shadow-stack/IBT via `arch_prctl(ARCH_ENABLE_SHADOW_STACK)` (kernel ≥5.17? [UNCERTAIN: E7 — verify in Ubuntu 26.04 kernel tree and whether `NT_GUARD` page type + `RtlGuardCheckIcall` user plumbing maps to CET hardware — experiment: build CF-guarded PE, run on Linux CET-enabled, check `msr` state])** v1: ignore CFG, it's a user-mode check pattern anyway (guard functions are in the binary). `TERMINAL_SERVER_AWARE` (session concept) → honor as no-op-true.
14. **`Zw` vs `Nt` prefix aliasing:** `Zw*` stubs in ntdll set SSN and *kernel* mode flag — on real NT `ZwXxx` called from user mode executes with `PreviousMode=Kernel`?? (myth: user-mode Zw is identical to Nt; kernel-mode Zw calls *are* the Kernel-PreviousMode path). For us the distinction only matters kernel-internal: our services take explicit mode param; ntdll exports both aliases with same SSN. Document to defuse the myth-driven "Zw must differ" compat bug.

## 12. Proposed architecture (consolidated)

```
User space
├─ Linux ELF processes          PE processes ("NT personality")
│   libc → syscall(RAX=__NR)    our ntdll/ldr/heaps/win32 (Phase2+, U)
│                  │                       │  syscall(RAX=NT SSN, mov r10,rcx)
└──────────────────┴───────────┬───────────┴────────────────────────
Linux kernel (arch/x86/entry/entry_64.S: test TIF_NTABI → branch)
 ├─ do_syscall_64 (Linux, unchanged fast path)      └─ do_nt_syscall (new)
 │                                                    const nt_service_table[]
 │                                                    arg marshalling (R10→RCX, user-stack copy)
 ├─ kernel/nt/  (NEW subsystem):
 │   entry.c  objmgr.c  handletbl.c  objobs(event/sem/mutex/timer/res)
 │   apc.c    iocp.c   keyedevt.c   fileobj.c (share mgr, close-pending, ADS-reject)
 │   devmgr.c (\?? \Device \BaseNamedObjects \KnownDlls)  afd.c  conpty.c
 │   section.c memstate.c(commit ledger)  pe_load.c(PEB/TEB/KUSER)  except.c
 │   registry/(hive reader+cache)  token.c  time.c(KUSER seqlock mirror)
 ├─ fs/binfmt_pe.c (NEW)
 └─ all backends reuse: mm (vm_mmap/mprotect), fs (VFS, fs/notify, fs/locks),
    net (sock_create_kern…), kernel/time/hrtimer, kernel/entry work hooks,
    arch/x86 lazy FSGSBASE path, wait_queue/task_work/signal-frame precedents
```

## 13. Dependencies

- **Hard:** Linux ≥6.17-class tree (Ubuntu 26.04) — `kernel/entry/common.c` TIF-work hooks, `task_work`, `io-wq` or own workqueue, `rhashtable/lockref/xarray`, ext4 `casefold`, lazy GS restore, `ida_alloc` for cid space, `seqlock` time mirror in `timekeeping`. User-CET (CONFIG_X86_USER_SHADOW_STACK) presence to be checked (E7).
- **Spec/reference data (not runtime deps):** published NTSTATUS/winerror tables; PE/COFF spec; published RE corpora for AFD ioctls / SSDT sets (Chappell, j00ru, OpenNT, reactos `include/ndk` *as format spec only* — original implementations required, mind licenses: ReactOS is GPL — **use headers-as-data with counsel review, do not copy code**); mingw-w64 headers (zlib — safe as spec).
- **Adjacent teams:** Phase-1 agent 2/3 (Win32 API surface & loader inventory) — the `ntdll` user-mode runtime is on our critical path: **the kernel is useless to a PE until an original ntdll-equivalent with Ldr/SEH/TLS exists**. The exec contract between kernel (`RtlUserThreadStart` entry, gs=TEB) and that DLL is the single interface that must be frozen first (§14 item 3).

## 14. Implementation order (risk-first, each step independently testable)

1. **Spike: dispatch multiplex (3–5 days).** Patch `entry_64.S` + `TIF_NTABI` + one no-op NT service (`NtSetInformationThread(ThreadZeroInformation?)` no — invent `NtWinabiPing`) + hand-written asm test PE (flat binary w/ `syscall`, no headers) under QEMU. Measure syscall-overhead delta (will-it-scale). **Kill/continue decision gate.**
2. **Differential harness on real Windows (parallel, not code):** build a WinDbg/script rig on a local Win11 VM + Server 2022 VM that records ground-truth behavior for (a) every E-# experiment above, (b) per-syscall return-code inventories we'll need later. This harness is a permanent project asset.
3. **Exec contract freeze:** document kernel↔ntdll ABI (entry thunk, PEB/TEB construction contract, KUSER fields, SSN table, status channel). Emit generated headers consumed by both sides.
4. **binfmt_pe + PEB/TEB/KUSER + NtTerminateProcess/ExitThread + minimal `NtWriteFile`** → "runs a hand-written asm PE that writes to a console handle inherited from fd 1 and exits with code 42". *First demoable milestone.*
5. **Memory services** (`NtAllocate/Protect/Query/FreeVirtualMemory`, commit ledger, guard-page stack grower).
6. **Object spine:** handle table + ObReference/MakeHandling + file objects (CreateFile→`filp_open`, Read/Write/Close, QueryInformation basics, StdInfo enumeration) → PE that opens/reads a file and prints it. *This is where "native" becomes undeniable.*
7. **Threads + waits:** `NtCreateThreadEx`, TEB-per-thread, `NtWaitForSingleObject`, event/semaphore/mutex, `NtAlertResumeThread`, keyed events.
8. **APC + alertable waits** (§6.3b) → then **IOCP + async file I/O + `NtCancelIoFileEx`**.
9. **Sections/views + KnownDlls** → loader's mmap arm ready for ntdll team.
10. **Afd socket shim + named pipes + console-pty.**
11. **Registry (read-only hives first).** 12. Exception delivery + `NtContinue` (before C++/SEH-heavy binaries). 13. Tokens/DAC-lite + `NtQuerySystemInformation` classes pulled by corpus. 14. Atoms, misc, jobs=cgroup, LPC-lite.

## 15. Testing methodology

- **Differential testing vs real Windows** (the project's ground truth): harness (item 2) auto-generates "behavior vectors": for a corpus of hand-written *syscall-level* test programs (asm/PE, no CRT — isolates kernel from loader), capture (input class → returned status, object-name strings, PEB/TEB field effects, wait outcomes incl. alert races) on Server 2022 vs our kernel; diff. NT's *unwritten* behaviors (exact statuses in obscure races) are what compat hides — only real-machine capture finds them.
- **Syzkaller port:** NT syscall table + PE-header generators as syzlang descriptors — this is also our security gate for the new subsystem; upstream-visible commitment.
- **kselftests:** `tools/testing/selftests/winabi/` — dispatch/masking, status channel, handle tables (dup/close-pending/inherence matrices), waits+APC races (stress `NtTestAlert` vs `CancelSynchronousIo`), commit ledger vs `MemoryInformation` honesty, KUSER seqlock torn-read torture (read while timekeeping updates — same suite pattern as `tools/testing/selftests/timens`?).
- **Race/alarm discipline:** all dispatcher objects pass lockdep (use real `spinlock_t`/`wait_queue`, no custom locks unless proven) + CONFIG_PROVE_LOCKING CI; APC-vs-wake races get targeted `stress` loops.
- **Perf gates:** syscall latency parity with Linux path (<+2 ns), will-it-scale regression gate (§10.4), NT-side throughput gates per subsystem (IOCP ping-pong vs NT baseline measured on the Windows VM as reference).
- **Corpus ladder** (with loader team): hand-written syscalls → MSVC-built CRT-free → MinGW → real OSS PEs → commercial installers (last; legal review).

## 16. Areas requiring original implementation (and where specs suffice)

Original code, spec data available: syscall entry multiplex (unique — no one has shipped NT-on-Linux; iBCS2 is the only precedent at all); object manager/handle tables (NT's own structures are documented but x64 handle encoding gaps exist → E4 fills them with first-party observation); user APC + alertable wait engine (only two shipped designs in history: NT's, WSL1's reverse — neither published as code); completion ports (fully observable API contract → straightforward original); PE binfmt (spec complete); commit ledger (original by necessity — Linux has no analog); KUSER mirror in timekeeping (original but tiny); hive reader (format documented, original impl); Afd/ioctl translators (RE-documented interface lists).
Specs suffice, no research risk: every status code (published), every structure layout (published; E6 pins), SSN sets (published per-build), UNICODE_STRING/Ldr contracts (published), Win32↔NT mapping (published).

## 17. Bottom line

The dispatch problem is *solved-by-precedent* (WSL1 proved the model end-to-end; the Linux tree already carries every mechanism we need to invert it: TIF-flag dispatch, binfmt, per-task GS, task_work/TIF-work return hooks, signal-frame injection, seqlock time mirroring). The expensive, risky, irreplaceable work is: the **object/handle/wait/APC spine** (new, medium, high-certainty), the **registry + security model** (new, large, medium-risk), and the **user-mode ntdll/ldr/SEH runtime** (new, very large — owned by the parallel agent, gated by our §7 contracts). The Linux-kernel-host architecture does **not** require a fork of the exec/entry/mm model: it adds one flag, one branch, one loader, one subsystem tree, and zero cost when idle.

---

## Appendix A — Experiment queue

| # | Question | Method | Blocks |
|---|---|---|---|
| E1 | Best SSN reference build | Chappell tables + corpus SSN histogram | §1.4 |
| E2 | Does x64 NT SW use `%fs:` | objdump corpus scan | §2.4 |
| E3 | Post-syscall R11/RFLAGS inspection in the wild | corpus scan + single-step tests | §2.5 |
| E4 | x64 handle value ↔ table entry encoding; OBJECT_HEADER layout on 20348 | WinDbg `dt nt!_HANDLE_TABLE_ENTRY`, `dt nt!_OBJECT_HEADER`, `!handle` + scripted `NtQueryObject` probes on Server 2022 VM | §4 |
| E5 | KPCR accessor (`call gs:[...]`) still used? | corpus disasm scan | §7.1 |
| E6 | TEB/PEB/KUSER exact offsets | mingw/ReactOS header transcription + disassembly verification (gs offsets used by stock binaries) | §7 |
| E7 | Ubuntu 26.04 kernel: user CET (ARCH_ENABLE_SHADOW_STACK) present? | check tree + boot test | §11.13 |
| E8 | Zw/Nt user-mode equivalence check | run stubs both ways on VM, trace PreviousMode-visible behavior | §11.14 |
| E9 | LSTAR entry-patch spike: actual overhead of personality bit test | spike #1 (must be <2 ns) | gate |

## Appendix B — Key sources consulted
(WSL1: Microsoft WSL Architectural Overview 2018; Pico Process Overview; "WSL System Calls" S. Hufnagel 2016.) (NT x64 syscall/SSDT: ired.team SSDT notes; x64 calling convention docs; WinInternals P1 7th ed.) (Linux entry: Documentation/arch/x86/entry_64; kernel-internals syscall-entry; entry_64.S/common.c/process_64.c; docs.kernel.org.) (Precedents: Linux-ABI iBCS2 project history; FreeBSD linuxulator sysentvec design (open source for pattern reading); Solaris lx branded zones; SFU/Interix docs.) (Instability data: Chappell SSDT notes; j00ru VXSecurity syscall tables.) (Structures: mingw-w64 headers, ReactOS ndk headers *as format spec only*, Windows Internals.)
