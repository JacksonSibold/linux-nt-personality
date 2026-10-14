# NT kernel gaps (the built kernel)

> **Status, 2026-10-13:** this report describes the kernel before the unification. The gate now PASSES:
> 171 of 171 services are strong in vmlinux, and 11 of them refuse with STATUS_NOT_SUPPORTED. See
> [KERNEL-BOOT.md](KERNEL-BOOT.md#unification-2026-10-13). The sections below are the earlier record.

What the NT personality in the built kernel (`~/ntpers/linux-6.18.56/vmlinux`) lacks,
compared with the repository. Measured on 2026-10-13 by the completeness gate,
`tests/kernel/check_kernel_complete.sh` (full output in
[KERNEL-BOOT.md](KERNEL-BOOT.md#completeness-gate-after)). Pointers are `file:line`
in `src/kernel/nt/` unless marked otherwise. Where a service has two definitions, the
first is the kernel-branch form (the one the inventory classifies).

## Totals

The table has 171 entries (`src/kernel/nt/nt_service_list.h`). 159 have a strong
kernel-branch handler, so the gate expects them to be in vmlinux. 124 are. The other 35
are shortfalls, and the gate fails on them.

| Group | Services | Kernel result |
|---|---:|---|
| Shortfall: expected handler not linked (section 1) | 35 | NULL table entry, returns NOT_IMPLEMENTED |
| No handler symbol at all (section 2, MISSING and absent weak) | 8 | NULL table entry, returns NOT_IMPLEMENTED |
| Stub: linked, returns NOT_IMPLEMENTED (section 3) | 5 | NOT_IMPLEMENTED |
| Weak-bound handler with a body, runs (section 2) | 4 | works (weak binding, nothing overrides it) |
| **Services the kernel does not implement** | **48** | 35 + 8 + 5 |

Userspace-only handlers: none. No service has a strong definition only in the
userspace branch. The inventory finds none with a userspace class that is strong and a
kernel class of MISSING.

## 1. Shortfalls: expected handler not linked into vmlinux (35)

### 1a. Source file not compiled into the kernel (27)

`arch/x86/nt/Makefile` lists 20 objects. Neither `io.c` nor `registry.c` is one of
them, no quoted `#include` of either file exists, and `tools/ntpers_kpatch.py` copies
only what the Makefile and its quoted includes reach. `registry.c` is excluded on
purpose (BUILD-RECIPE G9). `io.c` is also unbuilt: commit `6a9da7f` records that its
definitions were missing at link time (KERNEL-BOOT.md, fixes table), and no decision
on building it is recorded.

- `io.c` (16): NtQueryAttributesFile `:2790`, NtDeviceIoControlFile `:3093`,
  NtFsControlFile `:3111`, NtQueryEaFile `:3152`, NtSetEaFile `:3184`,
  NtFlushBuffersFileEx `:3215`, NtCancelIoFile `:3268`, NtCancelIoFileEx `:3274`,
  NtDeleteFile `:3289`, NtQueryFullAttributesFile `:3325`, NtLockFile `:3383`,
  NtUnlockFile `:3452`, NtQueryDirectoryFile `:3897`, NtQueryDirectoryFileEx `:3914`,
  NtQueryVolumeInformationFile `:3961`, NtSetVolumeInformationFile `:4010`.
- `registry.c` (11): NtCreateKey `:1563`, NtOpenKey `:1581`, NtOpenKeyEx `:1593`,
  NtDeleteKey `:1605`, NtFlushKey `:1610`, NtEnumerateKey `:1615`,
  NtEnumerateValueKey `:1629`, NtQueryKey `:1643`, NtQueryValueKey `:1657`,
  NtSetValueKey `:1674`, NtDeleteValueKey `:1690`. Each also has a typed second
  definition at `:1735` to `:1863`.

### 1b. Source file replaced by the `arch/` copy in the kernel build (8)

`tools/ntpers_kpatch.py` `ARCH_WINS` (lines 52 to 55) makes the build use
`arch/x86/nt/proc_thread.c` and `arch/x86/nt/nt_services.c` in place of the `src/`
files. The arch copies do not contain these definitions. `arch/x86/nt/proc_thread.c`
has 44 `nt_svc_` definitions; `src/kernel/nt/proc_thread.c` has 49.

- `proc_thread.c`: NtContinue `:2190`, NtContinueEx `:2199`, NtRaiseException `:2219`,
  NtAlertThread `:2469`, NtAlertThreadByThreadId `:2482`, NtWaitForAlertByThreadId
  `:2496`, NtAlertResumeThread `:2521`.
- `nt_services.c`: NtQuerySystemTime `:366` (kernel form; `:136` is the host-build form).

## 2. No linked handler, or only a weak one (12)

Ten services are defined only as weak symbols in `src/kernel/nt/dispatch.c`. The
`NT_SERVICE` macro at `dispatch.c:75` declares the whole table weak before the
definitions, and C gives the later definitions weak binding. Four of them still run,
because `arch/x86/nt/dispatch.o` holds their bodies with weak binding and nothing
overrides them. The other six have no definition in the kernel build at all.

- Weak binding, runs: NtAllocateVirtualMemory `dispatch.c:167`, NtFreeVirtualMemory
  `:196`, NtProtectVirtualMemory `:223`, NtQueryVirtualMemory `:298`.
- Absent from the build: NtAllocateVirtualMemoryEx `:399`, NtReadVirtualMemory `:330`,
  NtWriteVirtualMemory `:348`, NtFlushVirtualMemory `:366`, NtLockVirtualMemory `:286`,
  NtUnlockVirtualMemory `:292`. `arch/x86/nt/dispatch.c` defines only the four above.

Two services have no handler in the repository: `src/kernel/nt/nt_service_list.h:151`
(NtReplyWaitReceivePortEx, SSN 0x002b) and `:157` (NtCloseObjectAuditAlarm, SSN 0x003b).
Their table entries are NULL.

## 3. Stubs linked and returning NOT_IMPLEMENTED (5)

- NtCreateProcessEx `src/kernel/nt/proc_thread.c:1448`, NtCreateProcess `:1449`
  (one-line bodies, kernel branch; the typed forms at `:476` and `:485` are the
  userspace branch).
- NtGetContextThread `:2038`, NtSetContextThread `:2039` (one-line bodies).
- NtExtendSection `src/kernel/nt/mm.c:3530`.

## 4. Strong symbols with no table entry (3)

NtOpenProcess (`src/kernel/nt/proc_thread.c:1680`), NtOpenThread (`:1705`) and
NtSetInformationDebugObject (`src/kernel/nt/debugobj.c:639`) are defined in vmlinux
but have no `NT_SERVICE` line, so no system call number reaches them. The gate lists
them as extra symbols and does not fail on them.

## Open decisions (owner)

1. `io.c` and `registry.c` (27 services): build them (add the objects to
   `arch/x86/nt/Makefile` and copy them in `ntpers_kpatch.py`), or record that they are
   out of the kernel scope. Building `io.c` may need the G24 canonical-copy decision too.
2. `arch/` or `src/` as canonical for `proc_thread.c` and `nt_services.c` (G24). The
   arch copies lack 8 services that `src/` defines.
3. The six absent `dispatch.c` handlers and the two services with no handler: implement
   or record as not planned.
4. The five stubs: they are reported as stubs and do not fail the gate. Implement or
   accept as not planned.
