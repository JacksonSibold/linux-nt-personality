# First-boot checklist

What a first build and boot of the NT personality kernel should print, in the
order it appears, and what each line proves. Each entry names the script that
prints it. The build steps are in `docs/vm/BUILD-RECIPE.md`. Nothing here has
been observed yet. Where a line depends on a known gap, the entry says so.

Conventions:

- **Must** lines are asserted by a script. If one is missing, the step failed.
- **Should** lines are printed by a script but are not asserted.
- **Must not** lines are the failure signatures. Seeing one means stop.

## Phase 1: kernel build (`make bzImage`, then the completeness check)

Source: `tests/kernel/check_kernel_complete.sh`, run by `guest-bootstrap.sh`
step 7 and by `freshness.sh write`.

1. **Must** `make` exits 0 and `arch/x86/boot/bzImage` and `vmlinux` exist.
2. **Must** `Checking NT symbols in .../vmlinux (KDIR=...) ...`
3. **Must** one `OK: <symbol>` line for each of these, in this order:
   `do_nt_syscall`, `nt_service_table`, `nt_enter_personality`,
   `nt_exit_personality`, `ObCreateHandle`, `ObReferenceObjectByHandle`,
   `nt_create_event`, `se_access_check`, `nt_svc_NtAllocateVirtualMemory`,
   `nt_svc_NtCreateFile`, `nt_svc_NtCreateIoCompletion`,
   `nt_svc_NtCreateMutant`, `nt_svc_NtQueryInformationFile`, `nt_banner_init`.
   A `MISSING: <symbol>` line on stderr is a failure.
4. **Must** `nt_svc_* count: <N> (expected >=350)` with N at least 350.
5. **Must** `CONFIG_NT_PERSONALITY=y - checking .../arch/x86/nt/built-in.a objects...`
   and `built-in.a object count: <N> (expected 17)`, with N at least 17.
   The file `arch/x86/nt/built-in.a` is produced by the build; the Makefile
   lists 20 objects (G8).
6. **Must** one `OK object: <name>` line for each of `dispatch.o proc_thread.o
   mm.o handle.o kshim.o iocp.o object.o sync.o security.o nt_obj.o nt_io.o
   ntstatus_kernel.o nt_services.o debugfs_tests.o stubs.o weak.o kernel_stubs.o`.
7. **Must** `PASS: All NT symbols present - kernel complete`.

Must not: `ERROR: NT kernel incomplete`, `MISSING OBJECTS`, or
`WARNING: .../built-in.a not found` (the last means the object check was
skipped).

## Phase 2: initramfs and freshness stamp

Sources: `vm/mkinitramfs.sh`, `vm/freshness.sh write`.

8. **Should** `Building hand-rolled /init ...` (from `gcc`, then `strip`).
9. **Should** `Added /tests_kernel/minimal.exe` and `Added /tests_kernel/minimal0.exe`
   (or whichever PEs `tests/kernel/bin/` holds; each `Added` line is one file).
   If this line is missing, the M0 gate cannot pass (G12).
10. **Should** `Stamped initramfs identity: digest <64 hex> commit <sha|unknown>`.
    `commit` is `unknown` in a git worktree, because `.git` is a file there and
    the script tests for a directory.
11. **Must** `Initramfs ready: .../vm/initramfs.cpio.gz (<bytes> bytes)`.
12. **Must** `freshness: recorded digest <64 hex> (<N> files) commit <sha|unknown> bzImage <64 hex>`
    from `freshness.sh write`. This line comes from a successful write only.
    The bzImage hash is the sha256 of `arch/x86/boot/bzImage`.

Must not: `freshness write refused`, `ERROR: freshness write failed`.

## Phase 3: host side before QEMU starts

Source: `vm/runvm.sh`. Its freshness check (`freshness.sh check`) prints nothing
when it passes, and its exit status is the only signal.

13. **Must** the `runvm:` header, in this order:
    `runvm: kernel=<.../arch/x86/boot/bzImage>`, `runvm: initrd=<.../vm/initramfs.cpio.gz>`,
    `runvm: test=/mnt/share/vm/test_m0.sh`, `runvm: share=<repo root>`,
    `runvm: timeout=90s`, `runvm: log=<serial log path>`.
    (The timeout is 90 s when run through `check_m0.sh` or `guest-bootstrap.sh`,
    120 s when `runvm.sh` is run directly.)

Must not: `FRESHNESS GATE REFUSED` (exit 125). See the failure table.

## Phase 4: serial console, M0 gate

Source: `vm/init.c` (PID 1), `vm/test_m0.sh` (guest script). The console is
`ttyS0` at 115200 (`runvm.sh` `-serial file:`).

Kernel messages: the command line contains `quiet`, so only KERN_WARNING and
more severe messages reach the console. Most `pr_info` lines are not on the
serial log (G11). The lines below come from the guest's own `printf`/`echo`
and from `pr_err`. Other kernel boot text is not asserted.

14. **Must** `init: running test script: /mnt/share/vm/test_m0.sh` (`init.c`, `printf` to stdout).
15. **Must** `=== M0 PE test (exit 42) ===` (`test_m0.sh`).
16. **Must** `Running /tests_kernel/minimal.exe` (`test_m0.sh`). The fallback
    `minimal0.exe` is used only if `minimal.exe` is not executable.
17. **Should** `binfmt_pe: mapped ntdll at 0x<hex> LdrInitializeThunk 0x<hex>`
    (`binfmt_pe.c`, `pr_err`, so visible under `quiet`). It is printed when the
    ntdll mapping succeeds (G22).
18. **Must** `PE exit RC=42 (masked 42)` (`test_m0.sh`). This is the PE's
    `NtTerminateProcess(42)` observed through `binfmt_pe`.
19. **Must** the JSON block printed by `test_m0.sh`, which contains
    `"test": "m0",`, `"status": "PASS",`, `"pe": "/tests_kernel/minimal.exe",`,
    `"exit_code": 42,`, `"exit_masked": 42,` and a `"timestamp"` line.
20. **Must** `init: test exited with code 0`. The script returns 0 on PASS,
    and `init.c` writes that value to `.dodresult/result.exitcode`.
    The PE's 42 is in the JSON, not in `result.exitcode` (see BUILD-RECIPE
    section 11).
21. **Should** the kernel powers off (`sysrq` `o`, then `reboot(RB_POWER_OFF)`).
    The exact final kernel line is not known. Treat its absence as a warning
    only.

Must not, anywhere in the serial log:

- `init: failed to mount 9p share` (the share is missing, so no result is written)
- `init: exec test failed`
- `binfmt_pe: %s failed after exec commit ... sending SIGSEGV` (`pr_err`)
- `NT personality: ntpers v1.0 banner`. This line is expected from
  `nt_banner_init`, but nothing calls that function (G10). Its presence would be
  a surprise, and its absence is not a failure.

## Phase 5: host verdict

Sources: `vm/runvm.sh` (exit status) and `vm/check_m0.sh` or
`guest-bootstrap.sh` (verdict).

22. **Must** `runvm: guest exited with code 0` (from `runvm.sh`).
23. **Must** the JSON file `.dodresult/test_m0.json` contains `"status": "PASS"`
    and `"exit_code": 42`.
24. **Must** `=== M0 gate PASS: minimal.exe exited 42 (via JSON) ===` (from
    `check_m0.sh`), or `M0 gate PASS: minimal.exe exited 42` (from
    `guest-bootstrap.sh`). The bootstrap prints the JSON before the verdict.

## Phase 6: optional, diagnostic dmesg

Source: `vm/diag_m0.sh`, run with `--test /mnt/share/vm/diag_m0.sh`.
This is the only route in the repository that gets `dmesg` onto the serial log
(G11).

25. **Should** `=== M0 diag ===`, `Running /tests_kernel/minimal.exe`, `PE exit RC=42`.
26. **Should** the tail of `dmesg` (300 lines) on stdout, including
    `binfmt_pe: headers ok imagebase=0x... entry=0x... size=0x... sections=<N> stack_res=0x...`
    (`pr_info`).
27. **Should** `.dodresult/m0_diag_dmesg.log` exists afterwards.
28. Expected absent here: the banner line (G10). Do not expect `binfmt_pe: reject:`
    lines unless the PE was rejected.

## Phase 7: optional REG probe (`RUN_REG=1`)

Sources: `vm/test_reg.sh`, `guest-bootstrap.sh` step 12.

29. **Must** `=== REG probe test ===`, `Running /tests_kernel/reg_probe.exe`,
    `PE exit RC=0 masked 0`, then a JSON block with `"test": "reg"` and
    `"status": "PASS"`.
30. **Must** `REG gate PASS` (from `guest-bootstrap.sh`). `check_reg.sh` says
    `=== REG gate PASS ===`. Its verdict is `result.exitcode == 0`.

## Failure table

| Symptom | Exit / message | Meaning | Action |
|---|---|---|---|
| `runvm.sh` exits 125 | `FRESHNESS GATE REFUSED`, `STALE BUILD DETECTED - no freshness manifest found` | `freshness.sh write` has not run for this tree | Run step 10 (BUILD-RECIPE) |
| 1 | `ERROR: kernel bzImage not found` | step 7 did not produce `bzImage` | Repeat step 7 |
| 125 | `ERROR: initramfs not found` | step 9 did not produce `vm/initramfs.cpio.gz` | Repeat step 9 |
| 125 | `STALE BUILD DETECTED - kernel image is stale relative to sources` | a file under `src/kernel/`, `src/ntdll/` or `arch/x86/nt/` changed after the stamp | Rebuild, then stamp again |
| 124 | `runvm: TIMEOUT after <N>s (result file never written)` | the guest hung, or TCG is too slow (G16) | Check the serial log; try a larger `VM_TIMEOUT` |
| 1 | `runvm: ERROR - no result file and QEMU exited with code <N>` | the guest exited without writing `result.exitcode`, for example the 9p mount failed | Read the serial log |
| 0 but gate fails | `M0 gate FAIL: expected 42` | `test_m0.json` lacks PASS or exit 42 | Read `test_m0.json`, then the PE's `dmesg` via Phase 6 |
| any | `init: failed to mount 9p share` | the `share` tag is missing from the QEMU command, or `NET_9P`/`9P_FS` are not built in | Check the config (`make-kernel-config.sh` output) |

## What this checklist does not cover

- The patch 0002 seccomp and audit behaviour. Its self-test
  (`tools/testing/selftests/ntpers/seccomp_arch.c`) does not exist in the
  repository, and 0002 is not wired into the build (G3). No line of this
  checklist depends on it.
- The `smoke.sh` guest script, which is missing (G18).
- Performance. No timing is asserted, apart from the timeouts.
