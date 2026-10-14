# NT personality kernel: build, boot and M0 status

Kernel: Linux **6.18.56** (newest 6.18.y in kernel.org `sha256sums.asc`), built in the Debian 12 guest
(`~/ntpers/linux-6.18.56`) with the NT wiring from `tools/ntpers_kpatch.py`. Session date 2026-10-13.

## Min profile (2026-10-14): smaller kernel, same M0 and S1-S4 result

The default `.config` is now `PROFILE=min` (`vm/make-kernel-config.sh`, BUILD-RECIPE 6). It drops the
networking stack, the block layer, the 9p and virtio transports, BPF, VT and the input stack, the extra
decompressors and CPU vendors, and modules. Measured in a separate guest tree, `~/kfast/linux-src`, extracted
from the same `linux-6.18.56.tar.xz` (the other agents' trees were not touched):

| | full | min |
|---|---|---|
| enabled options | 611 | 469 |
| compile steps, `make bzImage` (unique objects, dry run) | 1044 | 608 |
| wall time, `make -j2`, ccache cold | estimate about 3 h 40 min | 7,720 s (607 compile lines, 2 h 8 min) |
| `bzImage` | 1.9 MB (earlier) | 1.1 MB |

Boots on host QEMU (`tools/linux-env/boot-m0-host.ps1`, TCG, `-kernel`, the initramfs images from `~/ntpers`,
`step-initramfs.sh` and `m0-initramfs.sh`), run twice: once after the first build, and again on the image
produced by the incremental rebuild (BUILD-RECIPE 8a), which is the final state:

| Test | Initramfs | Expected | Result (final image) | Serial line |
|---|---|---|---|---|
| M0 | `m0-initramfs.cpio.gz` | 42 | PASS | `M0 exit status: 42 (expected 42)`, `M0 PASS` |
| S1 | `s1-initramfs.cpio.gz` | 0 | PASS | `hello from NT` |
| S2 | `s2-initramfs.cpio.gz` | 0 | PASS | `hello from NT via ntdll` |
| S3 | `s3-initramfs.cpio.gz` | 0 | PASS | `hello from kernel32`, `file payload from S3` |
| S4 | `s4-initramfs.cpio.gz` | 0 | PASS | `hello from msvcrt 4 printf`, `hello from msvcrt puts` |

The userspace images (PEs, ntdll, kernel32, msvcrt) were not rebuilt; they are the ones that passed S1-S4 on the
full kernel. Only the kernel changed.

Incremental rebuild (`vm/incremental-kernel.sh`): a one-file edit in `src/kernel/nt/handle.c`, then the revert:
2 min 39 s each run, one NT object recompiled, and the manifest brought the guest back to the committed source
(the first version of the script compared with git and missed the revert; fixed before the timed run).

Unit suite (`python3 -m unittest discover tests/unit`, in the guest): 758 tests, 1 failure, 1 skipped (WSL).
The failure is `test_golden_accepted_builds.ShippedAcceptedListTest.test_shipped_list_validates_and_matches_corpus`.
The accepted list records the sha256 of the CRLF form of `windows-26200.9457/version.json` and
`manifest.json` (`822d95...`). The guest copy is LF, because `tools/linux-env/sync-repo.sh` normalises
line endings, so it fails there. The same test passes on a CRLF copy of the pre-change commit `6582899`.
This is not caused by the profile change, but the test needs a CRLF checkout to pass.

The completeness gate (`tests/kernel/check_kernel_complete.sh`) was not run on the min image; the open
threshold question in the section below is unchanged.

## Summary (the five-line report)

1. Downloaded: yes. `linux-6.18.56.tar.xz`, SHA256 matches the kernel.org checksum file. The PGP signature on that file is **not** verified (see LINUX-ENV.md).
2. Patched: yes. `ntpers_kpatch.py --check` reports 75 applicable, 0 blocked, 0 todo; the apply wrote 69 files.
3. Configured: yes. `vm/make-kernel-config.sh` (tinyconfig base, 64-bit, PASS).
4. Built: yes. `arch/x86/boot/bzImage` (1.9 MB) links. The repo's completeness gate `tests/kernel/check_kernel_complete.sh` **fails** (see "Completeness gate").
5. Booted: yes, under the host QEMU with `-kernel`. Banner: yes. M0: **PASS** (exit status 42).

## Boot and M0 result (serial, verbatim)

From `C:\Users\Jackson\repos\linux-env\kernel-6.18.56\m0-serial.log`, the boot that passed:

```
NT personality: ntpers v1.0 banner
Run /init as init process
=== M0 initramfs init (no 9p share) ===
binfmt_pe: headers ok imagebase=0x140000000 entry=0x1000 size=0x6000 sections=5 stack_res=0x200000
binfmt_pe: ntdll not mapped, falling back to exe entry
binfmt_pe: loading PE /tests_kernel/minimal.exe pid=25
ObRegisterThread: thr=00000000bfaeda1c tid=25 obj=00000000a4f0359c
binfmt_pe: after nt_enter TIF_NTABI=1
M0 exit status: 42 (expected 42)
M0 PASS
sysrq: Power Off
reboot: Power down
Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000000
```

The final panic line is printed because `/init` exits as the power-off completes. It comes after the
verdict and does not change it.

## Deviations from the recipe (and why)

- **Init and runner.** `vm/mkinitramfs.sh` and `vm/runvm.sh` need the QEMU 9p share, which QEMU for
  Windows does not provide (LINUX-ENV.md blocker 4). The M0 run used `tools/linux-env/m0-initramfs.sh`,
  whose `/init` runs `/tests_kernel/minimal.exe`, prints the status, and powers off. It boots with
  `tools/linux-env/boot-m0-host.ps1` (host QEMU, `-kernel`, TCG, `-nic none`). `vm/check_m0.sh` and
  `vm/test_m0.sh` were **not** run; their result file is written to the 9p share.
  Later, `VM_TRANSPORT=initrd-serial` and `tools/linux-env/run-vm.ps1` ran the same M0 payload
  through the repository's own test script and collector; see `docs/vm/TRANSPORTS.md`.
- **Test binary.** `tests/kernel/bin/minimal.exe` built in the guest with `x86_64-w64-mingw32-gcc`
  (PE32+, x86-64). The guest copy of the repo was LF-normalised before the build.
- **Config base.** tinyconfig with the required list, not defconfig (`BASE` in the script; BUILD-RECIPE G19).

## Fixes made in this session (one commit each)

| Commit | Change | Why |
|---|---|---|
| 9512d20 | `kpatch`: NT branch uses `syscall_enter_from_user_mode()`; enter.c todo removed | G3/G6: old branch left IRQs off and skipped seccomp, ptrace and audit; 6.18 declares `syscall_trace_enter()` |
| efa2ae7 | `vm/make-kernel-config.sh`: tinyconfig base, 64BIT, `bash scripts/config`, PRINTK/TTY/SHMEM/VIRTIO_MENU/NET/BLOCK | G19; tinyconfig is 32-bit and omits these |
| 31010f4 | `src/kernel/nt/kshim.h`: userspace `kstrdup` guarded by `!__KERNEL__` | conflicting `kstrdup` prototype (`int` vs `gfp_t`) |
| 5b32fca | `src/kernel/fs/binfmt_pe.c`: `register_binfmt()` returns void in 6.18 | compile error: void value not ignored |
| 883e59e | `job.c`, `debugobj.c`: `<stddef.h>`/`<string.h>` guarded | not available in a kernel build |
| 11f4fdd | `arch/x86/nt/nt_services.c`: `NT_MAX_HARDERROR_PARAMETERS` moved out of the userspace guard; `TRUE`/`FALSE` removed from the kernel branch | undeclared identifiers |
| c905982 | `src/kernel/nt/sync.c`: `bool equal;` declared | undeclared identifier |
| fbe6add | `src/kernel/nt/kshim.h`: `#include <linux/uaccess.h>` | link: undefined `copy_to_user`, `copy_from_user`, `access_ok` (warnings hidden by `-w`) |
| 743e4f3 | `src/kernel/nt/nt_obj.c`: kernel copy of `k_copy_oa` | link: undefined `k_copy_oa` (its only definition is static in unbuilt `registry.c`, G9) |
| 6a9da7f | `arch/x86/nt/nt_io.c`: `ntio_file_section_size`/`_extend` | link: undefined; the definitions were only in unbuilt `io.c` |
| c2813ac | `vm/make-kernel-config.sh`: `BINFMT_SCRIPT` | `/init` is a shell script; exec returned ENOEXEC (-8) |
| d9465a3 | `tools/linux-env/m0-initramfs.sh`, `boot-m0-host.ps1` | the 9p-free M0 path |

Docs: 7eeb614 (download log), 6836970 (BUILD-RECIPE: G1 partly, G3 and G4 closed, G19, 5i, 5l).

## Completeness gate (`tests/kernel/check_kernel_complete.sh`): FAIL

Run against `vmlinux` in the guest (`KDIR=~/ntpers/linux-6.18.56`). Exit status 1.

```
OK: do_nt_syscall ... OK: nt_banner_init            (14 symbols OK)
nt_svc_* count: 131 (expected >=350)
MISSING: nt_svc_* count 131 < 350 (expected 355)
built-in.a object count: 20 (expected 17)
OK object: dispatch.o ... OK object: kernel_stubs.o   (all 17 named objects OK)
ERROR: NT kernel incomplete - missing 1 symbols/objects: nt_svc_count:131
```

Analysis, not yet acted on:

- The service table `arch/x86/nt/dispatch.c` is generated from `arch/x86/nt/nt_service_list.h`, which has
  **171** `NT_SERVICE` lines. The gate expects 355. The two numbers disagree in the repo, so the threshold
  cannot be met from this list alone (similar to G8).
- 131 `nt_svc_*` symbols are defined. 43 listed services are weak, undefined, and therefore NULL in the
  table; the dispatcher returns `STATUS_NOT_IMPLEMENTED` for them (`dispatch.c`, `!svc->fn`). The M0 services
  (`NtTerminateProcess`, `NtWriteFile`, `NtClose`, `NtCreateFile`, `NtReadFile`) are all defined.
- `built-in.a` has 20 objects (the `arch/x86/nt/Makefile` list). The gate's 17 is the G8 disagreement.

Decision needed (owner): fix the threshold and object list to match the repo, or implement the missing services.
This session did not change the gate.

## Completeness gate (after)

The gate was rewritten to derive its expectations from the repository (commits
`f030cf2`, `62543ae`, `ec51f20`, `9272761`, `2d60b0e`); see `tools/kernel_gate.py`,
the header of `tests/kernel/check_kernel_complete.sh` and BUILD-RECIPE 7. Run in the
guest on 2026-10-13: `cd ~/gate && bash tests/kernel/check_kernel_complete.sh
~/ntpers/linux-6.18.56/vmlinux`. `~/gate` is a `git archive` of `tools/`,
`src/kernel/nt/`, `arch/x86/nt/`, `tests/kernel/` and `docs/status/SERVICES.md` at
`2d60b0e`. The kernel tree was only read. Exit status 1.

```
Checking NT personality in /home/ntpers/ntpers/linux-6.18.56/vmlinux (KDIR=/home/ntpers/ntpers/linux-6.18.56, repo=/home/ntpers/gate) ...
NT kernel completeness gate
  repo:     /home/ntpers/gate
  nm:       /home/ntpers/ntpers/linux-6.18.56/vmlinux
  table:    171 services (src/kernel/nt/nt_service_list.h)
  expected: 159 kernel-branch strong handlers (IMPLEMENTED 154, REFUSES 0, STUB-NOT-IMPLEMENTED 5)
  present:  124 strong nt_svc_* symbols in the binary
SHORTFALL: 35 expected service(s) not strong in the binary
  NtAlertThread                    IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtAlertThreadByThreadId          IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtAlertResumeThread              IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtWaitForAlertByThreadId         IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtFlushBuffersFileEx             IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryDirectoryFile             IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryDirectoryFileEx           IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryVolumeInformationFile     IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtSetVolumeInformationFile       IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryEaFile                    IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtSetEaFile                      IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtLockFile                       IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtUnlockFile                     IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtCancelIoFile                   IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtCancelIoFileEx                 IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtDeviceIoControlFile            IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtFsControlFile                  IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtDeleteFile                     IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryAttributesFile            IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtQueryFullAttributesFile        IMPLEMENTED  absent                   src/kernel/nt/io.c
  NtCreateKey                      IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtOpenKey                        IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtOpenKeyEx                      IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtDeleteKey                      IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtFlushKey                       IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtEnumerateKey                   IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtEnumerateValueKey              IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtQueryKey                       IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtQueryValueKey                  IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtSetValueKey                    IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtDeleteValueKey                 IMPLEMENTED  absent                   src/kernel/nt/registry.c
  NtQuerySystemTime                IMPLEMENTED  absent                   src/kernel/nt/nt_services.c
  NtContinue                       IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtContinueEx                     IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
  NtRaiseException                 IMPLEMENTED  absent                   src/kernel/nt/proc_thread.c
KERNEL GAPS: 12 service(s) with no kernel-branch strong handler (not failing)
  NtAllocateVirtualMemory          WEAK-FALLBACK            defined, weak binding
  NtAllocateVirtualMemoryEx        WEAK-FALLBACK            absent
  NtFreeVirtualMemory              WEAK-FALLBACK            defined, weak binding
  NtProtectVirtualMemory           WEAK-FALLBACK            defined, weak binding
  NtQueryVirtualMemory             WEAK-FALLBACK            defined, weak binding
  NtReadVirtualMemory              WEAK-FALLBACK            absent
  NtWriteVirtualMemory             WEAK-FALLBACK            absent
  NtFlushVirtualMemory             WEAK-FALLBACK            absent
  NtLockVirtualMemory              WEAK-FALLBACK            absent
  NtUnlockVirtualMemory            WEAK-FALLBACK            absent
  NtReplyWaitReceivePortEx         MISSING                  absent
  NtCloseObjectAuditAlarm          MISSING                  absent
KERNEL STUBS: 5 expected service(s) that return not-implemented (not failing)
  NtCreateProcess                  src/kernel/nt/proc_thread.c
  NtCreateProcessEx                src/kernel/nt/proc_thread.c
  NtGetContextThread               src/kernel/nt/proc_thread.c
  NtSetContextThread               src/kernel/nt/proc_thread.c
  NtExtendSection                  src/kernel/nt/mm.c
note: 3 strong nt_svc_* symbol(s) not in the table: NtOpenProcess NtOpenThread NtSetInformationDebugObject
entry points: 9 expected, 0 not strong
built-in.a: 20 expected objects from arch/x86/nt/Makefile, 0 missing
VERDICT: FAIL (35 expected service symbol(s) not strong in the binary)
EXIT=1
```

Reading the result:

- The gate now FAILS, as the rule says it should: 35 services that the kernel-branch
  inventory classifies as implemented are not in vmlinux. The object check passes:
  all 20 `arch/x86/nt/Makefile` objects are in `built-in.a`. The entry points are strong.
- The 48 services the kernel does not implement are 35 shortfalls, 8 with no handler
  symbol (6 weak-only handlers absent from the build, 2 with no handler in the
  repository), and 5 stubs. Four `dispatch.c` virtual-memory handlers run through
  weak binding. Pointers and the causes
  are in [KERNEL-GAPS.md](KERNEL-GAPS.md): 27 services sit in `io.c` and `registry.c`,
  which the kernel build does not compile; 8 are in `proc_thread.c` and `nt_services.c`,
  where the build uses the `arch/` copies.
- The old numbers could not pass. 355 matches no count the repository produces: there are
  171 table entries, and 1217 lines in the handler and arch sources mention `nt_svc_`.
  17 matches no list either: `arch/x86/nt/Makefile` has 20 objects. Two inventory defects had also inflated the
  expectation: the per-branch class was not computed, and a definition after the weak
  `NT_SERVICE` declaration in `dispatch.c` was counted as strong, which vmlinux shows as
  `W`. Both are fixed (`f030cf2`, `62543ae`).

## Remaining failures and gaps

1. **Completeness gate** now derives its expectations from the repository and FAILS on 35 expected
   handlers that vmlinux does not link. See "Completeness gate (after)" and [KERNEL-GAPS.md](KERNEL-GAPS.md).
   Closing those gaps is the open work; the gate itself is no longer the blocker (G8 closed).
2. **Warnings hidden.** `arch/x86/nt/Makefile` sets `ccflags-y += -w`. That hid the implicit-declaration
   errors that the link later reported. A build with warnings on (drop `-w`) is needed before trusting the
   NT sources. Not done here.
3. **G24 (which copy is canonical).** `src/kernel/nt/mm.c`, `mm.h` and `nt_service_list.h` overwrite
   `arch/x86/nt/` copies in the kpatch run (WARN G24). The kernel uses the `src` versions. Not decided.
4. **Seccomp self-test** from patch 0002 (`tools/testing/selftests/ntpers/seccomp_arch.c`) does not exist.
   The NT path now runs seccomp, but nothing tests it.
5. **Signature of `sha256sums.asc`** is not verified (LINUX-ENV.md).
6. **Userspace shim check** (`make -C tests/kernel-host check-quick`) fails **before and after** this session:
   at base commit `30f7e2b` it fails with the same nine errors: eight in `shim/khost_prelude.h` (`pread`/`pwrite`
   macros) and one in `shim/sys/stat.h` (`redefinition of 'struct stat'`) against glibc. Not caused by this
   session. The host has no make/gcc, so the check ran in the guest. Not fixed here.
7. **Boot path.** `vm/runvm.sh`, `vm/check_m0.sh`, `vm/freshness.sh` (stamp) and `vm/guest-bootstrap.sh`
   were not used. The freshness stamp (BUILD-RECIPE 10) was not written.
8. **Unit suite** (`python3 -m unittest discover tests/unit`, run in the guest, Python 3.11.2): 665 tests,
   1 error, 1 skipped (WSL). The error is `test_check_repo_extra.PerFileTests.test_nul_byte_past_probe_is_a_compile_failure_without_line_none`:
   `tools/check_repo.py` line 105 calls `compile()`, which raises `ValueError: source code string cannot contain
   null bytes` on this Python (3.11). The check only reports `SyntaxError`, so the error escapes instead of
   becoming a `py-compile` failure. (Python 3.12 and later are reported to raise `SyntaxError` here; not checked
   in this session.) Neither file changed in this session
   (`git diff 30f7e2b HEAD` is empty for both). The kpatch tests (19) pass, including the rewritten G3 and
   todo tests.

## Resume commands

Guest (SSH via `tools/linux-env/ssh-guest.sh`; the build runs detached, log in `~/ntpers/`):

```sh
cd ~/ntpers-research                                   # sync first: tools/linux-env/sync-repo.sh <repo>
sed -i 's/\r$//' vm/make-kernel-config.sh tests/kernel/check_kernel_complete.sh
python3 tools/ntpers_kpatch.py --tree ~/ntpers/linux-6.18.56          # idempotent; copies changed sources
KDIR=$HOME/ntpers/linux-6.18.56 bash vm/make-kernel-config.sh         # PASS expected
cd ~/ntpers/linux-6.18.56 && make -k -j4 ARCH=x86_64 bzImage          # the pass that compiled the remaining ~400 objects took about 1 h under TCG; reruns recompile only changed objects
KDIR=$HOME/ntpers/linux-6.18.56 bash ~/ntpers-research/tests/kernel/check_kernel_complete.sh ~/ntpers/linux-6.18.56/vmlinux
sh ~/ntpers-research/tools/linux-env/m0-initramfs.sh ~/ntpers-research ~/ntpers/m0-initramfs.cpio.gz
```

Host (copy `bzImage` and `m0-initramfs.cpio.gz` from the guest with `scp -P 2222` into
`C:\Users\Jackson\repos\linux-env\kernel-6.18.56\`, then):

```powershell
powershell -ExecutionPolicy Bypass -File tools\linux-env\boot-m0-host.ps1 -TimeoutSec 240
```

Expected last lines: `M0 exit status: 42 (expected 42)` and `M0 PASS`.

## Unification (2026-10-13)

Branch `haiku/kgaps`, after the merge of `origin/main` (`37f21c5`). The kernel was built in
`~/kgaps/linux-6.18.56` (a private copy of the kernel tree, `nice -j2`), with
`tools/ntpers_kpatch.py` applied. `src/kernel/nt` is the only NT source tree; the `arch/x86/nt`
duplicates are deleted (see [tree-divergence.md](../analysis/tree-divergence.md), section 7).

**Completeness gate: PASS** (`tests/kernel/check_kernel_complete.sh`, `EXIT=0`):

```
table:    171 services (src/kernel/nt/nt_service_list.h)
expected: 171 kernel-branch strong handlers (IMPLEMENTED 160, REFUSES 0, STUB-NOT-IMPLEMENTED 11)
present:  171 strong nt_svc_* symbols in the binary
SHORTFALL: 0 expected service(s) not strong in the binary
KERNEL GAPS: 0 service(s) with no kernel-branch strong handler
KERNEL STUBS: 11 expected service(s) that return not-implemented (explicit refusals)
built-in.a: 21 expected objects from arch/x86/nt/Makefile, 0 missing
VERDICT: PASS
```

Services in the kernel: **171 of 171** have a strong handler in vmlinux. 160 are implemented. 11 are
explicit refusals: NtCreateProcess, NtCreateProcessEx, NtGetContextThread, NtSetContextThread,
NtExtendSection, NtAllocateVirtualMemoryEx, NtReadVirtualMemory, NtWriteVirtualMemory,
NtFlushVirtualMemory, NtReplyWaitReceivePortEx and NtCloseObjectAuditAlarm. They return
`STATUS_NOT_SUPPORTED`; the reasons are in `src/kernel/nt/dispatch.c` and `docs/status/KERNEL-GAPS.md`.

**M0: PASS** (`M0 exit status: 42 (expected 42)`, `M0 PASS`), booted on the host with
`tools/linux-env/boot-m0-host.ps1`.

**Console steps: S1 PASS, S2 PASS, S3 PASS, S4 PASS** (`STEP exit status: 0 (expected 0)`, `STEP PASS`),
each booted from `tools/linux-env/step-initramfs.sh` on the same `bzImage`. The initramfs for S2–S4 packs
`ntdll.dll`, `kernel32.dll` and `msvcrt.dll` under `/windows/system32`.

**Decisions recorded in this change:**
- Registry: in memory only in the kernel build (`NT_REGISTRY_HOST_FILES 0`). It does not survive a reboot.
- VAD coalescing: off, the src behaviour (matches the probe oracle). Owner decision, delegated.
- Virtual-memory services that have no kernel implementation refuse (see above).

**Unit suite** (`python3 -m unittest discover tests/unit`, guest, Python 3.11): 789 tests. Three failures,
none in the kernel:
- `test_check_doc_links`: the link to this section was missing; fixed by this section.
- `test_service_inventory`: `docs/status/SERVICES.md` was stale; regenerated.
- `test_golden_accepted_builds`: the accepted-list hash for `windows-26200.9457/version.json` matches the
  CRLF bytes of the Windows worktree, and the committed blob is LF. The test fails in the Linux guest
  on the base commit too. Not changed here.

## Merged kernel (kgaps + main)

Branch `haiku/integrate5`: merge `haiku/kgaps` (`ada9ce0`) into `main` (`6582899`), merge commit `6f91c3f`, then `db4ee08`
(README file count). Conflicts: `arch/x86/nt/proc_thread.c` (deleted by kgaps, modified on main) took the deletion after
checking that `src/kernel/nt/proc_thread.c` already has main's TEB-in-user-memory fix (`nt_alloc_teb_stack_kernel`,
`nt_new_thread_peb`, `nt_teb_set_tid`, same bodies as the arch copy). `README.md` took main and was regenerated.
Regenerated: `SERVICES.md`, `TRACEABILITY.md`, `CRITERIA.md`, `CRITERIA-ROADMAP.md` (the roadmap changed: three
`WI-POLICY-DOCS` rows now `HAS-HARNESS-ENTRY`), and the README counts.

Build: private tree `~/integ5/linux` (a copy of the built `~/kgaps/linux-6.18.56`), `tools/ntpers_kpatch.py --tree`
(74 applied, 11 applicable, 10 files written), `vm/make-kernel-config.sh` PASS, `make -k -j20 ARCH=x86_64 bzImage`
(incremental after the copy). Test PEs and DLLs built in `~/integ5/ntpers-research` (`tests/kernel`, `src/ntdll`).

**Completeness gate: PASS** (`tests/kernel/check_kernel_complete.sh`, exit 0): 171 services in the table, 0 shortfall,
0 kernel gaps, 11 explicit refusals, 21 built-in objects present, 9 entry points strong.

Boots (host `tools/linux-env/boot-m0-host.ps1`, the QEMU VM runs the NT kernel and the PE inside it; the Windows host
does not execute any PE):

| step | exit status (expected) | verdict |
|---|---|---|
| M0 | 42 (42) | PASS |
| S1 console_s1 | 0 (0) | PASS |
| S2 console_s2 | 0 (0) | PASS |
| S3 console_s3 | 0 (0) | PASS |
| S4 console_s4 | 0 (0) | PASS |
| S5 console_s5 | 53 (53), `STATUS_DLL_NOT_FOUND` | PASS |
| S6 console_s6 | 0 (0) | PASS |
| S7 console_s7 | 0 (0) | PASS |

NONVM probes (`NONVM=1 tools/linux-env/probes-initramfs.sh`, boot with `log_buf_len=16M`): all 15 run to
`NTPERS_ALL_DONE`. 14 exit 0; `probe_exception` exits 139 (as before). Matched at the verdict level: **1 of 15**
(`probe_sharemode`, 458857 of 458857 fields). Against the last recorded main run (the "after" table above) there is no
regression: every field count is the same or higher. Improved: `probe_directory` 0 to 19 of 67, `probe_fileinfo` 129 to
165 of 221, `probe_peloader` 0 to 33 of 34, `probe_registry` 0 to 128 of 140.

Other checks on the merged tree: unit suite `python -m unittest discover -s tests/unit -p "test_*.py"` 789 tests OK;
`check_doc_links.py` 0 broken; `update_doc_counts.py --check` current; `check_syscall_tables.py` parity for 171 services;
`tests/harness/lint_integrity.py` PASS; `dodrun.py`: DOD-SEC-A008 PASS, 3 ERROR (DOD-ARCH-A001, A002, B001: WSL not
installed on the host, environmental, not regressions), 0 regressions.

## Main 0d19a62: flake fixes and reproof (2026-10-14)

Source: `main` at `0d19a62` (flake fixes `675cc54`, merged as `069c9f5`; thread-exit hook `d7edb90`). Main has since moved (`252fa28` and later are not in this build).
Kernel: a private copy of `~/integ5/linux` in the guest (`~/main-proof-kernel/linux-6.18.56`), with
`tools/ntpers_kpatch.py` applied from `~/main-proof` at `0d19a62` (76 applied, 0 blocked, 0 todo) and
`vm/make-kernel-config.sh` (PROFILE=min, PASS). Built `make -j20 ARCH=x86_64 bzImage` (BUILD_RC=0).
Userspace PEs and DLLs built from the same tree (ntdll, kernel32, msvcrt, advapi32, user32 with mingw,
tests/kernel, probes). Boots on host QEMU through `tools/linux-env/boot-m0-host.ps1`.

**Flake fixes (userspace shim, `src/kernel/nt`).** The wait engine lost a wakeup when a signal landed
between a failed acquire and the wait-block link, and dropped waiters beyond 64 on one object. Thread-exit
ordering published `terminated` before the mutants were abandoned. Both fixes apply to the kernel branch of
`sync.c` too; the proc_thread ordering fix is userspace only (see follow-ups).

Loops (`build/port_test.exe` and `build/proc_thread_test.exe` in the guest, 4 in parallel, 60 s per run):

| test | before | after |
|---|---|---|
| port_test, 200 runs | 9 of 40 runs deadlocked (rc 124) | 200 of 200 pass |
| proc_thread_test, 200 runs (run 1) | not measured | 11 of 200 fail (`self_terminate`, `thread_exit`) |
| proc_thread_test, 200 runs (run 2) | 11 of 200 fail | 200 of 200 pass |

Regression tests: `sync_extra_test` `wake_all_waiters_beyond_64` and `auto_reset_handoff_loop` fail on the
old engine (`FAIL wake_all: 64 of 100 waiters woke`, `FAIL handoff: lost wakeup`) and pass on the new one.
`make -C tests/kernel-host check-quick` (Linux guest, before the thread-exit merge): all 22 programs rc=0.

**Completeness gate (`tests/kernel/check_kernel_complete.sh`): PASS** (exit 0, 171 services, 0 shortfall,
0 kernel gaps, 9 entry points strong, 21 built-in objects).

**Boots (host QEMU, serial logs in the private Windows dir `linux-env\kernel-d7edb90`):**

| step | exit status (expected) | verdict |
|---|---|---|
| M0 | 42 (42) | PASS |
| S1 console_s1 | 0 (0) | PASS |
| S2 console_s2 | 0 (0) | PASS |
| S3 console_s3 | 0 (0) | PASS |
| S4 console_s4 | 0 (0) | PASS |
| S5 console_s5 | 53 (53) | PASS |
| S6 console_s6 | 0 (0) | PASS |
| S7 console_s7 | 0 (0) | PASS |

**NONVM probes** (`NONVM=1 tools/linux-env/probes-initramfs.sh`, boot with `log_buf_len=16M`): all 15 reach
`NTPERS_ALL_DONE`. 14 exit 0; `probe_exception` exits 139 (as before).

Matched against `tests/differential/golden/windows-26200.9457` (`probe_results.py split` and `compare`), fields
matched of golden leaves:

| probe | matched / golden | verdict |
|---|---|---|
| probe_directory | 19 / 67 | FAIL |
| probe_exception | 0 / 76 | FAIL |
| probe_fileinfo | 165 / 221 | FAIL |
| probe_handles | 82 / 111 | FAIL |
| probe_namedpipe | 3 / 45 | FAIL |
| probe_ntstatus | 2190 / 2199 | FAIL |
| probe_path | 51355 / 72523 | FAIL |
| probe_peloader | 33 / 34 | FAIL |
| probe_process_thread | 0 / 182 | FAIL |
| probe_registry | 128 / 140 | FAIL |
| probe_section | 128 / 129 | FAIL |
| probe_sharemode | 458857 / 458857 | PASS |
| probe_sync | 0 / 74 | FAIL |
| probe_teb_peb | 79 / 104 | FAIL |
| probe_virtualquery | 128 / 139 | FAIL |

Verdict level: **1 of 15** (`probe_sharemode`), the same as the last recorded main run. For the four probes
named in the previous section (directory, fileinfo, peloader, registry) the counts are the same as recorded there.

**Follow-ups (not fixed here):**
- Kernel branch of `NtTerminateThread`/`do_exit` still publishes `terminated` before abandoning mutants.
  The userspace fix is in `proc_thread.c`; the kernel path needs the same ordering and a kernel build check.
- Userspace `NtTerminateThread` on a peer thread calls `nt_abandon_for_thread`, which abandons the caller's
  mutants, not the target's. Mutants of the peer are abandoned by its cancel cleanup, which runs after
  `terminated` is published.
- `nt_pulse_event` has a 64-entry snapshot in the same shape as the fixed wake path. Not changed.
