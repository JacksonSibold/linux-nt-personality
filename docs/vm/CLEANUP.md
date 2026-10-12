# vm/ cleanup candidates

This is a review of duplicate and near-duplicate scripts in `vm/`, with related
copies in `tests/kernel/guest/` and `build/`. **Nothing has been deleted.** The
verdicts are recommendations for a later change. See `docs/vm/OVERVIEW.md` for
what each script does.

## Evidence used

- A grep of the repository for each script name, excluding `vm/` and
  `docs/vm/`, found no references to any `vm/test_*.sh`, `vm/diag_m0.sh`,
  `vm/check_r3.sh`, `vm/check_fs.sh`, `vm/check_dispatch.sh`,
  `vm/check_after_section.sh`, `vm/run_section.sh` or `vm/boot_smoke.sh`.
- Inside `vm/`, the only test scripts that are called by name are
  `test_m0.sh` (from `check_m0.sh`) and `test_reg.sh` (from `check_reg.sh`).
- The guest scripts in `tests/kernel/guest/` that are called by the Python
  harnesses are `run_arch.sh`, `run_handles.sh`, `run_i2.sh`, `run_mem.sh`,
  `run_ntstatus.sh`, `run_pmu.sh`, `run_provenance.sh`, `run_smc.sh` and
  `run_sync.sh`. Two of the `tests/kernel/guest` scripts are also cited by
  `tests/harness/lint_integrity.py` and `tests/harness/selftest.py`.
- A repository-wide grep found no reference to `tests/kernel/guest/run_m0*.sh`,
  `run_debug.sh`, `run_sync_atomic.sh`, `run_sync_events.sh`,
  `run_sync_mutant.sh`, `run_sync_semaphore.sh`, `run_sync_timing.sh` or
  `run_sync_wait64.sh`.

The grep does not prove that a script is unused. Someone may run these by hand
inside the VM with `runvm.sh --test`. The verdicts assume a script is only worth
keeping when it does something no other script does.

## Verdicts

Verdict key:
- **keep**: canonical; nothing else does the same job.
- **merge**: same job as another script; keep one copy and fold in the rest.
- **delete**: broken, or a strict subset of another script. Delete after the
  "before deleting" steps below.
- **fix**: useful, but has a defect that should be fixed first.

### 1. M0 PE checks and diagnostics

Each of these runs `minimal.exe` or `minimal0.exe` and dumps `dmesg`.

| Script | Overlaps with | Verdict | Reason |
|---|---|---|---|
| `test_m0.sh` | `test_m0_minimal0.sh` | **keep** | Referenced by `check_m0.sh`. Falls back from `minimal.exe` to `minimal0.exe`. |
| `test_m0_minimal0.sh` | `test_m0.sh` | **delete** | Same script with the PE fixed to `minimal0.exe`. Both write `test_m0.json`, so the last one run wins. |
| `diag_m0.sh` | `test_m0_verbose.sh`, `test_m0_debug3.sh`, `test_minimal_segfault.sh`, `test_both.sh`, `build/m0_diag.sh` | **keep** one | Smallest of the group; saves `dmesg` to a file. The others add `set -x`, `strace`, `peb_walk` or a second run. |
| `m0_diag2.sh` | `diag_m0.sh` | **delete** | Broken. Its variables were stripped (for example `mkdir -p` with no argument, `if [ -x  ]`, `RC=True`). It parses but cannot do its job. |
| `test_m0_verbose.sh` | `diag_m0.sh` | **merge**, then delete | `set -x` variant. Its last line runs `minimal.exe` a third time for no reason. |
| `test_m0_debug3.sh` | `diag_m0.sh` | **merge**, then delete | Adds `strace`. Keep only if `strace` is used in the image, which `mkinitramfs.sh` does not add. |
| `test_minimal_segfault.sh` | `diag_m0.sh` | **delete** | Runs `minimal.exe` and dumps `dmesg`. Writes 0 whatever happens. |
| `test_both.sh` | `test_m0.sh` | **delete** | Checks both PEs, but the expected exit codes disagree with other scripts (see below). |
| `test_debug.sh` | `diag_m0.sh` | **delete** | Lists ntdll locations, runs `minimal.exe`, writes 0 whatever happens. |
| `build/m0_diag.sh` | `diag_m0.sh` | **merge** | Different job: greps the newest `.dodresult/serial-*.log` on the host. Keep it under `build/` if that is the intended use, and say so in its header. |
| `tests/kernel/guest/run_m0.sh` | `run_m0_42.sh` | **keep** one | Runs `minimal0.exe`, expects 42. |
| `tests/kernel/guest/run_m0_42.sh` | `run_m0.sh` | **merge** | Runs `minimal.exe`, expects 42. The same check with a different PE. |
| `tests/kernel/guest/run_m0_debug2.sh`, `run_m0_debug3.sh` | `run_m0.sh` | **delete** | Same check as `run_m0.sh` (`minimal0.exe`, expects 42) with `dmesg` dumps added. Not called by any harness. |

**Expected exit code disagreement.** `run_m0.sh` and `test_m0_minimal0.sh` expect
`minimal0.exe` to exit 42. `test_both.sh` expects 0. `test_arch_debug.sh` counts
0 as success. `test_validation.sh` accepts either. Pick one value and document
it before merging any of these scripts.

### 2. Counter and dispatch-count loops

| Script | Overlaps with | Verdict | Reason |
|---|---|---|---|
| `test_validation.sh` | `test_counter.sh`, `test_arch_simple.sh`, `test_arch_debug.sh` | **keep** | Checks exact deltas for 1, 5, 10 launches and writes JSON. It is also the only one that wraps the PE runs in `set +e`. |
| `test_counter.sh` | `test_validation.sh` | **merge**, then delete | Same counters. It runs `minimal0.exe` under `set -e` without `set +e`, and always exits 0. |
| `test_arch_simple.sh` | `test_arch_debug.sh`, `test_validation.sh` | **merge** | The same 100-launch ratio loop. |
| `test_arch_debug.sh` | `test_arch_simple.sh`, `run_arch.sh` | **fix**, then merge | Writes result 0 unconditionally and runs the PE under `set -e`. |
| `test_counter1.sh` | none | **keep** | Only script that runs `counter1.exe` and checks delta 2. Rename it if it is kept. |
| `check_dispatch.sh` | `test_debug2.sh`, `test_debugfs.sh` | **merge** | Just prints two debugfs files. Fold into one diagnostic script. |

### 3. R3 and image/probe runners

Most of these copy one probe from `build/probes/` into `/tmp`, run it under
`timeout`, and print `dmesg`. They differ in the file they copy and the
`dmesg` grep.

| Script | Overlaps with | Verdict | Reason |
|---|---|---|---|
| `test_r3.sh` | `test_r3b.sh`, `test_r3_dmesg2.sh`, `check_r3.sh` | **merge** | Runs `r3_minimal.exe` twice. Its first `RC=$?` is dead code. Keep one parameterised runner. |
| `test_r3b.sh` | `test_r3.sh` | **delete** | Same probe and expected exit as `test_r3.sh`, with less output. |
| `test_r3_dmesg2.sh` | `test_r3.sh` | **merge** | Same probe; greps for allocation and VAD messages. |
| `test_r3_protect.sh` | `test_protect_run.sh` | **merge** | Same probe; greps for `NtProtect` messages and always exits with the PE status. |
| `test_r3_patched2.sh` | `test_r3.sh` | **merge** | Same, but copies `r3_minimal.exe.patched`. Keep only if that file is still produced. |
| `check_r3.sh` | `test_r3.sh`, `test_image_vad_run.sh` | **merge** | Runs `r3_minimal.exe` and `test_image_vad.exe` with debugfs dumps. Its `RC1` decides the result. |
| `test_protect_run.sh` | `test_r3_protect.sh` | **merge** | Runs `test_protect.exe` and expects 42. |
| `test_image_vad_run.sh` | `check_r3.sh` | **merge** | Runs `test_image_vad.exe` and passes on 42. |
| `test_probe_path.sh` | `test_probe_debug.sh` | **merge** | Runs `probe_path.exe`. `test_probe_debug.sh` is the same run in the background with `dmesg` before and after. |
| `test_probe_debug.sh` | `test_probe_path.sh` | **delete** | As above; the background `sleep 2` does not change the outcome. |
| `run_section.sh` | `check_after_section.sh` | **merge** | Both run `probe_section_direct_vm.exe`. One dumps JSON and `dmesg`, the other greps dispatch counts. |
| `check_after_section.sh` | `run_section.sh` | **merge** | See above. |
| `test_wait2.sh` | none | **fix** | Needs `build/wait_test`, which nothing builds. Writes 0 unconditionally. Keep only with a build step for `wait_test.c`. |

### 4. Synchronisation tests

| Script | Overlaps with | Verdict | Reason |
|---|---|---|---|
| `test_sync_atomic.sh` | `tests/kernel/guest/run_sync_atomic.sh` | **merge** | Runs the same PE. The `vm/` copy writes no JSON. |
| `test_sync_mutant.sh` | `tests/kernel/guest/run_sync_mutant.sh` | **merge** | Same PE. The `vm/` copy writes `test_sync_mutant.json`. |
| `test_sync_wait64.sh` | `tests/kernel/guest/run_sync_wait64.sh` | **merge** | Same PE. |
| `test_n4.sh` | `test_sync_*.sh` | **keep** | Different PE (`n4_thread.exe`) and writes JSON. Same shape as the sync scripts, so a shared runner could take all four. |
| `tests/kernel/guest/run_sync.sh` | `run_sync_*.sh` | **fix** | Always exits 1 (its status is hard-coded to FAIL with a note). Its PE fallback is `minimal0.exe`, which is not a sync test. |
| `tests/kernel/guest/run_sync_events.sh`, `run_sync_semaphore.sh`, `run_sync_timing.sh` | none | **keep** | No `vm/` counterpart and not called by the Python harness. Decide whether they are wanted. |

Note that the `vm/` copies of the sync tests call `dmesg -c`, which clears
the kernel log. That affects anything that runs after them.

### 5. Debugfs and dispatch dumps

| Script | Overlaps with | Verdict | Reason |
|---|---|---|---|
| `test_debugfs.sh` | `test_debug2.sh` | **keep** | Runs the debugfs selftest and writes JSON. |
| `test_debug2.sh` | `test_debugfs.sh`, `check_dispatch.sh` | **merge**, then delete | Dumps the same debugfs files. |

### 6. Plumbing and always-pass scripts

These write a fixed result or exit 0 or 42 without checking. They are not
usable as gates.

| Script | Verdict | Reason |
|---|---|---|
| `test_hello.sh` | **keep** or **delete** | Writes `hello.log` and exits 42. Useful as a result-path check for `init.c`, but nothing calls it. |
| `test_elf.sh` | **delete** | Runs `/tmp/test_elf`, which nothing creates, and always writes 42. |
| `test_ls.sh` | **delete** | Runs every PE in `/tests_kernel` and writes PASS whatever happened. |
| `check_fs.sh` | **delete** | Lists directories and always writes 0. |
| `test_f2.sh` | **keep** | Checks exit 0 of `f2_io.exe`. It is the only check of that PE. |
| `test_mem_vad.sh` | **keep** | Only check of `mem_vad.exe` in `vm/`. `tests/kernel/guest/run_mem.sh` covers the same PE, so decide which copy is canonical. |
| `test_peb_walk.sh` | **fix** | Copies `scratch/peb_walk.exe`, which is not in the repository. |

### 7. Harness and build entry points

| Script | Overlaps with | Verdict | Reason |
|---|---|---|---|
| `boot_smoke.sh` | `smoke.sh` | **delete** | Legacy wrapper. With no argument it execs `smoke.sh`. With a file argument it copies into `vmtests/`, which does not exist. |
| `smoke.sh` | none | **keep** | Referenced by `tests/harness/registry.yaml`. Its guest script `vmtests/smoke.sh` is missing, so it cannot pass today. |
| `check_m0.sh` | `check_reg.sh` | **merge** | Both gates have the same skeleton: rebuild the initramfs, run `runvm.sh`, read `result.exitcode` and a JSON file. A single parameterised gate would cover both. |
| `check_reg.sh` | `check_m0.sh` | **merge** | See above. |
| `preflight.sh` | `kbuild.sh` | **merge** | Both check the required CONFIG options and the 17 objects in `arch/x86/nt/Makefile`. The lists are duplicated. Keep one copy in a sourced file. |
| `kbuild.sh` | `rebuild_kdir.sh` | **merge** | Both restore or patch the same Kconfig, Kbuild, `fs/Makefile`, `thread_info.h`, `sched.h`, `fork.c` and `syscall_64.c` files, in different orders. See `docs/analysis/tree-divergence.md`. |
| `golden_latest.sh` | none | **keep** | Small, idempotent, called by `selftest.py`. |
| `wait_test.c` | none | **keep** | Only used through `test_wait2.sh`. Needs a build rule if it stays. |

## Before deleting or merging anything

1. Check the verdict against the `tests/harness` and `tests/kernel/harness`
   callers. The names above are the ones with no caller found by grep.
2. Run the M0 gate (`vm/check_m0.sh`) and the REG gate (`vm/check_reg.sh`). They
   use `test_m0.sh` and `test_reg.sh`, which this review keeps.
3. Delete in one commit per script, so that a revert is simple.
4. Keep `m0_diag2.sh` out of any merge. It is broken, and its only value is as a
   record of the stripped variables.

## Not in scope

- The PE binaries under `tests/kernel/bin/` and the probes under `build/probes/`
  are not in the repository, so the scripts that run them cannot be tested
  here. This review did not run any guest script.
