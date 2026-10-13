# vm/ overview

This document describes the shell scripts and small C sources in `vm/`, how they
are grouped, what each one reads and writes, and how they depend on each other.
It was written by reading the scripts; where a script's behaviour is surprising,
the text says so. For redundant scripts see `docs/vm/CLEANUP.md`.

Paths below are relative to the repository root. Host scripts run on the Linux
side (WSL Ubuntu in the author's setup). Guest scripts run inside the QEMU VM as
`sh <script>` under busybox, with the repository root mounted at `/mnt/share`.

## Syntax status

`bash -n` was run on all 78 tracked `*.sh` files (54 in `vm/`, the rest in
`tests/` and `build/`). All pass; no syntax fixes were needed. `vm/lint.sh`
now runs the same check over every tracked script, plus shellcheck when it is
installed. CI runs it.

Note on line endings: the working tree on the Windows checkout uses CRLF, but
the index stores LF (`core.autocrlf=true`). This is harmless for `bash -n`, but
a native Linux checkout should see LF only.

## Terms

- **Host script**: runs on the Linux side and drives QEMU, the kernel build, or
  the checks.
- **Guest script**: a `.sh` file that is passed to `vm/runvm.sh --test` and runs
  inside the VM. It sees `/mnt/share` (9p, the repo root) and `/tests_kernel/`
  (the PE test binaries packed into the initramfs).
- **Result protocol**: the guest writes its exit status to
  `.dodresult/result.exitcode` on the share. `vm/init.c` writes the test
  script's own exit status to that file after the script finishes, so any value
  a script writes there is overwritten. The authoritative value is the script's
  exit status. `runvm.sh` returns it.
- **M0**: the first milestone. `minimal.exe` is a PE that exits 42 through
  `NtTerminateProcess`. `minimal0.exe` is a zero-import PE. The M0 gate is
  `vm/check_m0.sh`.

## Build

| Script / file | Purpose | Inputs | Outputs / side effects | Env vars |
|---|---|---|---|---|
| `kbuild.sh` | The sanctioned kernel build. Restores missing NT sources into KDIR, checks the required CONFIG options and the 17 objects in `arch/x86/nt/Makefile`, runs `make -j$(nproc)`, runs the completeness check, then the M0 gate, then rebuilds the initramfs and stamps freshness. Uses `set -euo pipefail`. | `$KDIR/.config` (or `vm/kernel-config-ntpers` to restore it), `src/kernel/nt`, `arch/x86/nt`, `src/ntdll`, `src/kernel/fs/binfmt_pe.c`, `tests/kernel/check_kernel_complete.sh`, `vm/check_m0.sh` | Kernel build in KDIR, `/tmp/kbuild.log`, `/tmp/kconfig.bak` on recovery, plus everything the children write | `KDIR` (default `~/ntpers/linux-6.18`) |
| `rebuild_kdir.sh [--no-build]` | From-scratch rebuild. **Deletes the whole KDIR**, re-extracts the vanilla tarball, copies the NT sources in, patches Kconfig, Kbuild, `fs/Makefile`, `thread_info.h`, `sched.h`, `fork.c` and `syscall_64.c` with inline Python, installs the config, builds, runs the completeness check, rebuilds the initramfs and writes freshness. | `$TARBALL`, `vm/kernel-config-ntpers`, `arch/x86/nt`, `src/kernel/nt`, `src/kernel/fs/binfmt_pe.c`, `src/ntdll/*.h` | KDIR (recreated), `/tmp/rebuild_*.log`, `/tmp/freshness_write.log` | `KDIR`, `TARBALL` (default `~/ntpers/linux-6.18.tar.xz`) |
| `mkinitramfs.sh [out]` | Builds the initramfs. Compiles `init.c` statically with gcc, copies busybox and creates applet symlinks, adds PE test binaries, DLLs and the identity stamp, and writes a sorted `newc` cpio, gzipped. | `vm/init.c`, busybox (`/bin/busybox` or PATH), `tests/kernel/bin/*.exe` and `pmu_elf`, `build/*.dll`, and optional `/tmp/pe_min.exe`, `/tmp/minimal*.exe`, `/tmp/pe_min*.exe`, `/tmp/<dll>`, `/tmp/minimal.exe`, `$PROJECT/pe_min.exe`, `freshness.sh compute` | `vm/initramfs.cpio.gz` (default `out`), `vm/initramfs.digest`, `vm/initramfs.sha256`, and `vm/initramfs.identity.json`. The last file is tracked in git and rewritten on every run. | none (optional `$1` is the output path) |
| `golden_latest.sh` | Idempotently creates the symlink `tests/differential/golden/latest` pointing at `26200.9168`. Does nothing if `latest` exists. Called from `tests/harness/selftest.py`. | `tests/differential/golden/26200.9168` | one symlink | none |
| `init.c` | The guest PID 1. Mounts proc, sysfs, devtmpfs, debugfs, tracefs and the 9p share (`mount_tag=share`), enables dynamic debug for a few NT files, reads `dodtest=` from the kernel command line, runs the script through `/bin/sh`, writes the exit code to `.dodresult/result.exitcode`, then powers off. | kernel command line | compiled into the initramfs | none |
| `wait_test.c` | A fork/exec wrapper. It runs `argv[1]`, prints the raw wait status and decoded WIF* fields, and returns the child's exit code (or 128+signal). No script in the repo compiles it. `vm/test_wait2.sh` expects a prebuilt `build/wait_test`. | source only | none | none |
| `vm/kernel-config-ntpers` | **Missing from the repository.** `kbuild.sh` and `rebuild_kdir.sh` both copy it to KDIR when `.config` is absent. | | | |
| `initramfs.identity.json` | Tracked identity record of the last initramfs build. Regenerated by `mkinitramfs.sh`. | | | |

## Run

| Script | Purpose | Inputs | Outputs | Env vars / args |
|---|---|---|---|---|
| `runvm.sh --test <guest> [--share D] [--timeout S] [--log F] [--run-id ID]` | The canonical VM runner. First runs the freshness gate (`freshness.sh check`, refuses with exit 125 if stale). Then starts `qemu-system-x86_64` with KVM (TCG fallback), 4 GB, 4 vCPUs, serial to a log, the share over 9p, and `dodtest=<guest>` on the command line. Waits for `result.exitcode` (up to 10 s) and returns its value. Exit 124 on timeout, 125 for a stale build or missing initramfs, 1 if QEMU exited without a result. | `BZIMAGE`, `INITRD`, the guest script | `.dodresult/result.exitcode`, `.dodresult/serial-<UTC>.log` | `BZIMAGE` (default `~/ntpers/linux-6.18/arch/x86/boot/bzImage`), `INITRD` (default `vm/initramfs.cpio.gz`), `VM_TIMEOUT` (default 120) |
| `smoke.sh [--timeout S]` | End-to-end rig check: boots the kernel and runs `/mnt/share/vmtests/smoke.sh` through `runvm.sh`. The header comment says the guest checks that `uname` contains 6.18 and that a 9p marker round-trips, but that guest script is not in the repo, so this is unverified. Builds the initramfs if it is missing. | the kernel and initramfs above | `.dodresult/smoke.marker` | `BZIMAGE`, `INITRD`, `--timeout` (default 90) |
| `boot_smoke.sh [guest]` | Legacy entry point, kept for old callers. With no argument it execs `smoke.sh`. With a file argument it copies the file into `vmtests/legacy_guest.sh` under the repo root and runs it through `runvm.sh`. | as above | as above | `TIMEOUT` (default 120) |

## Check

Host-side gates and checks. The first four run on the Linux host.

| Script | Purpose | Inputs | Outputs | Env vars |
|---|---|---|---|---|
| `preflight.sh` | Pre-work sanity check. Verifies the required CONFIG options in `$KDIR/.config`, the 17 objects in the Makefile, that no strong symbol is defined in two `arch/x86/nt/*.o` (uses `nm`, with a specific check for `g_kshim_root`), and that `vmlinux` has `do_nt_syscall` (or builds `built-in.a` if there is no `vmlinux`). Exit 1 on failure. | `$KDIR` | none (may build objects in KDIR) | `KDIR` |
| `freshness.sh [check\|write\|compute [out]]` | Source-to-image freshness gate. `compute` hashes the tracked kernel, ntdll, arch/x86/nt and patch sources into a manifest. `check` (the default) compares the manifest and the mtimes against the stored manifest, the digest, bzImage sha, initramfs digest and sha, and DLL sidecars. `write` records all of this and is only allowed when `NTPERS_ALLOW_WRITE=1`, and only after the completeness check passes. | `$KDIR/arch/x86/boot/bzImage`, `$KDIR/vmlinux`, sources, `vm/init.c`, `vm/mkinitramfs.sh` | `.ntpers_build.{manifest,digest,identity.json}` in both the repo root and KDIR, `.ntpers_bzimage.sha256`, `vm/initramfs.{digest,sha256}`, `build/*.dll.{sha256,digest}`, `/tmp/fresh_*` scratch files | `KDIR`, `NTPERS_ALLOW_WRITE` |
| `check_m0.sh` | M0 hard gate. Rebuilds the initramfs (warnings only), runs `runvm.sh --test /mnt/share/vm/test_m0.sh`, then passes if `test_m0.json` reports PASS with exit 42, or if the result file is 42. | initramfs and kernel | `.dodresult/m0_gate_serial.log`, `.dodresult/test_m0.json` | `VM_TIMEOUT` (default 90) |
| `check_reg.sh` | REG gate for `reg_probe.exe`. Same shape as `check_m0.sh`, passes when the guest exit code is 0. Runs `test_reg.sh`. | as above | `.dodresult/test_reg.json`, `.dodresult/reg_serial.log` | `VM_TIMEOUT` (default 90) |
| `vm/lint.sh` | `bash -n` on every tracked `*.sh`, and shellcheck when on PATH. Run by CI. | the scripts | none | `SHELLCHECK_SEVERITY` (default `error`) |

The guest scripts `check_r3.sh`, `check_fs.sh`, `check_dispatch.sh` and
`check_after_section.sh` also start with `check_`, but they are guest-side
diagnostics, not gates. See the Diag section.

## Test (guest)

These run inside the VM. Each one is selected with `runvm.sh --test /mnt/share/vm/<name>.sh`
or from a host gate. They locate PEs in `/tests_kernel/`, run them, and usually
write `.dodresult/<name>.json` and `.log`. Those that use `set -eu` and keep a
PE's exit code in a variable use `set +e` around the PE run.

| Script | What it runs | Result |
|---|---|---|
| `test_m0.sh` | `minimal.exe`, falling back to `minimal0.exe`. Expects 42. | `test_m0.json`, exit 0 on PASS |
| `test_m0_minimal0.sh` | `minimal0.exe` only, expects 42. Near-duplicate of `test_m0.sh`. | `test_m0.json` (same name) |
| `test_reg.sh` | `reg_probe.exe`, expects 0. | `test_reg.json` |
| `test_validation.sh` | Counts `dispatch_count` and `host_count` around 1, 5, 10 and 100 launches of `minimal0.exe`. Always exits 0 and only prints PASS/FAIL lines. | `validation.json` |
| `test_counter.sh` | Counter deltas for `minimal0.exe`, `counter7.exe` (if present), and 100 launches. Exits 0 always. | `counter_validation.json` |
| `test_counter1.sh` | Delta for `counter1.exe`, passes when delta is 2. | `result.exitcode` |
| `test_arch_debug.sh` | 100 launches of `minimal0.exe` with counter reads. Writes result 0 unconditionally. | `kernel_arch.{json,log}` |
| `test_arch_simple.sh` | 100 launches, prints the HOST/NT ratio and whether it is under 0.05. Always exits 0. | `arch_simple.json` |
| `test_n4.sh` | `n4_thread.exe` (30 s timeout), expects 0. | `test_n4.json`, `n4_dmesg.log` |
| `test_sync_atomic.sh`, `test_sync_mutant.sh`, `test_sync_wait64.sh` | `sync_atomic.exe`, `sync_mutant.exe`, `sync_wait64.exe` (30 s), expect 0. Only `test_sync_mutant.sh` writes a JSON file. | `test_sync_mutant.json`, `sync_mutant_*.log`, `result.exitcode` |
| `test_debugfs.sh` | Runs `echo run > /sys/kernel/debug/ntpers/selftest` and passes if the output contains `PASS`. | `test_debugfs.json` |
| `test_f2.sh` | `f2_io.exe`, passes when exit (masked) is 0. | stdout |
| `test_mem_vad.sh` | `mem_vad.exe` (30 s), expects 0. | `result.exitcode` |
| `test_peb_walk.sh` | `peb_walk.exe` copied from `scratch/`, which does not exist in the repo. | `peb_walk_output.txt` |
| `test_r3.sh`, `test_r3b.sh`, `test_r3_patched2.sh`, `test_r3_protect.sh`, `test_r3_dmesg2.sh` | Run `r3_minimal.exe` (or a `.patched` variant) from `build/probes/` on the share, expecting 42. | `r3_output.txt` or result file |
| `test_image_vad_run.sh` | Runs `test_image_vad.exe` from `build/probes/` and passes on exit 42. | `result.exitcode` |
| `test_protect_run.sh` | Runs `test_protect.exe` from `build/probes/`, expects 42. | `result.exitcode` |
| `test_probe_path.sh` | Runs `probe_path.exe` from `tests/differential/build/`, expects 0. | `result.exitcode` |
| `test_wait2.sh` | Runs `build/wait_test` on `r3_minimal.exe` and `minimal.exe`. Always writes 0. | `result.exitcode` |
| `test_ls.sh` | Lists `/tests_kernel`, runs every PE and writes a PASS JSON unconditionally. | `test_ls.json` |
| `test_elf.sh` | Runs `/tmp/test_elf` (not produced anywhere) and `pmu_elf`, always writes 42. | `result.exitcode` |
| `test_hello.sh` | Writes `hello.log` and exits 42. A plumbing check for the result path. | `hello.log` |
| `test_both.sh`, `test_m0_debug3.sh`, `test_m0_verbose.sh`, `test_minimal_segfault.sh`, `test_debug.sh`, `test_debug2.sh`, `test_probe_debug.sh` | Mostly diagnostics; see the Diag section. | none or logs |

## Diag (guest)

Guest scripts that dump state (`dmesg`, debugfs, `/tests_kernel` listings) for
investigation. They are not gates and their exit codes are not meaningful.

| Script | Purpose |
|---|---|
| `diag_m0.sh` | Runs `minimal.exe` (fallback `minimal0.exe`), saves `dmesg` to `m0_diag_dmesg.log`, and exits with the PE status. |
| `m0_diag2.sh` | **Broken.** The file's variables were stripped (`mkdir -p` with no argument, `if [ -x  ]`, `RC=True`). It is syntactically valid sh, but it does not run correctly. |
| `test_m0_verbose.sh` | `set -x` run of `minimal.exe` and `minimal0.exe`, plus `peb_walk.exe` from `/tmp`. Its last line runs `minimal.exe` a third time. |
| `test_m0_debug3.sh` | `minimal.exe` under `strace` if present, plus `dmesg`. |
| `test_minimal_segfault.sh` | `minimal.exe` and a `dmesg` tail. Writes 0 unconditionally. |
| `test_both.sh` | `minimal0.exe` (expects 0) and `minimal.exe` (expects 42), with `dmesg` greps. |
| `test_debug.sh` | Lists ntdll locations, runs `minimal.exe`, greps dmesg. Writes 0 unconditionally. |
| `test_debug2.sh` | Greps dmesg for ntpers, lists debugfs, and reads `ntpers/selftest`. |
| `test_probe_debug.sh` | Background run of `probe_path.exe` with `dmesg` before and after. |
| `check_fs.sh` | Lists `/`, `/windows/system32`, reads `/kernel32.dll`, and always writes 0. |
| `check_dispatch.sh` | Reads `/sys/kernel/debug/ntpers/dispatch_per_ssn` and `dispatch_count`. |
| `check_after_section.sh` | Runs `probe_section_direct_vm.exe` and greps dispatch counts for section syscalls. |
| `check_r3.sh` | Runs `r3_minimal.exe` and `test_image_vad.exe` with `dmesg` and debugfs dumps. Writes the R3 result, which depends on the first run's exit code. |
| `run_section.sh` | Runs `probe_section_direct_vm.exe` and dumps JSON and `dmesg`. |

## Dependencies

```
kbuild.sh
 |-- tests/kernel/check_kernel_complete.sh   (completeness; after make)
 |-- vm/check_m0.sh                          (M0 gate, after the completeness check)
 |     |-- vm/mkinitramfs.sh
 |     |-- vm/runvm.sh
 |           |-- vm/freshness.sh check       (gate; skipped with a warning if not executable)
 |           |-- vm/init.c (compiled in mkinitramfs.sh)
 |           |-- guest: vm/test_m0.sh
 |-- vm/mkinitramfs.sh                       (after the M0 gate)
 |     |-- vm/init.c, freshness.sh compute
 |-- vm/freshness.sh write                   (NTPERS_ALLOW_WRITE=1; runs the completeness check)

rebuild_kdir.sh
 |-- tests/kernel/check_kernel_complete.sh
 |-- vm/mkinitramfs.sh
 |-- vm/freshness.sh write

check_reg.sh  -> vm/mkinitramfs.sh, vm/runvm.sh -> guest vm/test_reg.sh
smoke.sh      -> vm/mkinitramfs.sh (if missing), vm/runvm.sh -> guest /mnt/share/vmtests/smoke.sh
boot_smoke.sh -> vm/smoke.sh, or vm/runvm.sh with a copied guest script
runvm.sh      -> vm/freshness.sh check, qemu-system-x86_64, initramfs
preflight.sh  -> standalone (uses nm and make)
golden_latest.sh -> standalone; called by tests/harness/selftest.py
lint.sh       -> standalone; called by .github/workflows/ci.yml
```

Callers outside `vm/`:

- `tests/harness/vmtest.py`, `tests/harness/freshness.py`, `tests/harness/selftest.py`,
  `tests/harness/dualrun_wrapper.py`, `tests/kernel/harness/*.py`,
  `tests/differential/dualrun.py` call `vm/freshness.sh check` (through WSL),
  `vm/runvm.sh`, `vm/mkinitramfs.sh` and `vm/golden_latest.sh`.
- `tests/kernel/harness/*.py` run the guest scripts in `tests/kernel/guest/`
  through `runvm.sh` (`GUEST = "/mnt/share/tests/kernel/guest/run_*.sh"`).
- `tests/regression/test_smap_guard.py` asserts that `runvm.sh` does not contain
  `nosmap`, `clearcpuid=244` or `-smap`.

## Inputs that are not in the repository

Several scripts need files that are not tracked. They are listed here so the
failures can be explained.

- `vm/kernel-config-ntpers`: used by `kbuild.sh` and `rebuild_kdir.sh`. Missing.
- `vmtests/smoke.sh` (on the share as `/mnt/share/vmtests/smoke.sh`): the default
  guest script of `init.c`, `smoke.sh`, `vmtests/`-based `boot_smoke.sh` and
  `tests/harness/registry.yaml`. Missing. The `vmtests/` directory does not exist.
- `tests/kernel/bin/*.exe` and `tests/kernel/bin/pmu_elf`: built PEs packed into
  the initramfs. The directory is untracked and empty in the repo.
- `build/probes/*.exe`, `build/wait_test`, `build/*.dll`, `tests/differential/build/*.exe`,
  `tests/differential/golden/26200.9168`, `scratch/peb_walk.exe`: generated or
  not present.
- `create_kconfig_nt.py`, `add_suspend.py`, `fix_syscall.py`, `fix_syscall_orig.py`
  at the repo root, and `scratch/add_fs_makefile.py`, `scratch/add_fs_kconfig2.py`:
  called by `kbuild.sh` with `2>/dev/null || true`, so their absence is silent.
  `rebuild_kdir.sh` has inline Python fallbacks for the two `scratch/` helpers.
- `~/ntpers/linux-6.18` (KDIR) and `~/ntpers/linux-6.18.tar.xz`: the kernel tree
  and the vanilla tarball, outside the repo.
- Host tools: `gcc`, `strip`, `cpio`, `gzip`, `sha256sum`, `nm`, `make`,
  `python3`, `qemu-system-x86_64` with `/dev/kvm`, GNU `timeout`, busybox.

## Environment variables

| Variable | Used by | Default |
|---|---|---|
| `KDIR` | `kbuild.sh`, `preflight.sh`, `rebuild_kdir.sh`, `freshness.sh` | `~/ntpers/linux-6.18` |
| `TARBALL` | `rebuild_kdir.sh` | `~/ntpers/linux-6.18.tar.xz` |
| `BZIMAGE` | `runvm.sh`, `smoke.sh` | `$KDIR/arch/x86/boot/bzImage` (smoke uses `~/ntpers/...`) |
| `INITRD` | `runvm.sh`, `smoke.sh` | `vm/initramfs.cpio.gz` |
| `VM_TIMEOUT` | `runvm.sh`, `check_m0.sh`, `check_reg.sh` | 120 for runvm, 90 for the gates |
| `TIMEOUT` | `boot_smoke.sh` | 120 |
| `NTPERS_ALLOW_WRITE` | `freshness.sh write` (must be 1) | unset |
| `SHELLCHECK_SEVERITY` | `lint.sh` | `error` |
| `VM_TRANSPORT` | `runvm.sh`, `mkinitramfs.sh`, `init.c` build | `9p` (or `initrd-serial`; see [TRANSPORTS.md](TRANSPORTS.md)) |
| `VM_PAYLOAD_DIRS` | `mkinitramfs.sh` (initrd-serial) | `vm build/probes` |
| `VM_PAYLOAD_MAX_KB` | `mkinitramfs.sh` (initrd-serial) | 32768 |
| `PYTHON` | `runvm.sh` (initrd-serial collector) | `python3` |

## Files written by the scripts

- `.dodresult/` (on the share, repo root): `result.exitcode`, `serial-*.log`,
  per-test `*.json` and `*.log`, `current_run_id`, `m0_gate_serial.log`.
- `.ntpers_build.*` and `.ntpers_bzimage.sha256` in the repo root and in KDIR.
- `vm/initramfs.cpio.gz`, `vm/initramfs.digest`, `vm/initramfs.sha256`,
  `vm/initramfs.identity.json` (tracked; rewritten on every run).
- `/tmp`: `kbuild.log`, `rebuild_*.log`, `freshness_write.log`, `fresh_*.txt`,
  `kconfig.bak`, `check_nm.*`, `check_ar.txt`, and guest copies such as
  `/tmp/r3_test.exe`.

## Known problems

These were found while reading. Nothing was changed for them.

1. **Git file modes.** Every `.sh` file in the index is mode `100644` except
   `tests/syntax/check.sh` (`100755`) and `vm/lint.sh` (added in this review as
   `100755`). On a Linux checkout that keeps git modes,
   `runvm.sh` skips the freshness gate with a warning (`[ -x ... ]`), and
   `kbuild.sh` silently skips `mkinitramfs.sh` and the freshness stamp (also `-x`
   checks). `kbuild.sh` runs `check_m0.sh` directly and `check_m0.sh` runs
   `runvm.sh` directly (`check_reg.sh` uses `bash` instead), so those calls fail
   with "Permission denied" without the exec bit. The fix is
   `git update-index --chmod=+x vm/*.sh`. DrvFs mounts usually report files as
   executable, so the problem may not show under WSL.
2. **Missing `vm/kernel-config-ntpers`** (see above). `kbuild.sh` and
   `rebuild_kdir.sh` fail when KDIR has no `.config` and the file is absent.
3. **`vmtests/smoke.sh` is missing**, so the default guest script in `init.c`
   and `smoke.sh` cannot run. `registry.yaml` also refers to it.
4. **`run_pmu.sh` fallback is a no-op.** The primary and fallback PE paths are
   both `/tests_kernel/pmu_probe.exe`. The fallback was probably meant to be
   `pmu_elf`.
5. **Inconsistent expectation for `minimal0.exe`.** `run_m0.sh` and
   `test_m0_minimal0.sh` expect 42. `test_both.sh` expects 0. `test_arch_debug.sh`
   counts 0 as success. `test_validation.sh` accepts 0 or 42.
6. **`set -e` with PE exit codes.** `test_arch_debug.sh` and `test_counter.sh`
   run `minimal0.exe` under `set -e` without `set +e`. A non-zero exit ends the
   script before it writes its result. `test_validation.sh` wraps its launches in
   `set +e`, which is the pattern the others should follow.
7. **Scripts that always report success.** `test_ls.sh`, `test_arch_debug.sh`,
   `test_arch_simple.sh`, `test_counter.sh`, `test_validation.sh`, `check_fs.sh`,
   `test_wait2.sh`, `test_debug.sh`, `test_minimal_segfault.sh` and `test_elf.sh`
   write a fixed result or exit 0 or 42 without checking the outcome. They are
   not usable as gates.
8. **`check_m0.sh` log grep.** `grep -q "exit 42\|status=2a\|code 42\|2a"` also
   matches any `2a` in the serial log. It only prints a message, so it does
   not affect the verdict.
9. **`boot_smoke.sh` legacy path** copies into `vmtests/legacy_guest.sh` under the
   repo root, but `vmtests/` does not exist, so the `cp` fails under `set -e`.
10. **`test_n4.sh` and `test_sync_*.sh` run `dmesg -c`**, which clears the kernel
    log ring. Anything that reads the log later loses those messages.
11. **Hard-coded `/tmp` and `$HOME` paths** make several scripts depend on earlier
    runs leaving files in `/tmp`, for example `mkinitramfs.sh` reading
    `/tmp/pe_min.exe`. The results change with the state of the host.
12. **`rebuild_kdir.sh` deletes KDIR.** It is documented as the only recovery path,
    but it removes `~/ntpers/linux-6.18` without asking.
13. **Source-tree divergence.** `kbuild.sh` and `rebuild_kdir.sh` copy
    `arch/x86/nt` and `src/kernel/nt` over each other in different orders. See
    `docs/analysis/tree-divergence.md`.
14. **The M0 gate in `kbuild.sh` looks unable to pass after a source change.**
    This comes from reading the script order, not from running it. `kbuild.sh`
    runs `check_m0.sh`, which runs `runvm.sh`. `runvm.sh` refuses unless
    `freshness.sh check` matches the stored manifest. The manifest is written
    only later, in the stamping step (`freshness.sh write`). So after any
    change to a tracked source, the stored manifest is stale when the gate runs.
    Running `kbuild.sh` on a real tree would confirm or refute this.
