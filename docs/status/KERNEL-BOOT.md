# NT personality kernel: build, boot and M0 status

Kernel: Linux **6.18.56** (newest 6.18.y in kernel.org `sha256sums.asc`), built in the Debian 12 guest
(`~/ntpers/linux-6.18.56`) with the NT wiring from `tools/ntpers_kpatch.py`. Session date 2026-10-13.

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

## Remaining failures and gaps

1. **Completeness gate** fails as described above (threshold and object-count disagreement, G8).
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
