# Debug instrumentation in the NT kernel sources

Date: 2026-10-12. Branch: `haiku/dbgprints`. Scope: `src/kernel/nt`, `src/kernel/fs`, `arch/x86/nt`.

Search: `git grep -nE 'W1_INSTR|KERN_EMERG|KERN_ALERT|printk\(|pr_emerg|fprintf\(stderr|DBG|TRACE'`, plus `pr_info|pr_err|pr_debug` in the hot-path files.

No `KERN_EMERG`, `KERN_ALERT`, `pr_emerg` or direct `printk(` trace calls exist in these trees. The noisy output is `pr_info`/`pr_err` with `W1_INSTR` or per-call labels.

## Mechanism

`NT_TRACE(...)` (pr_info) and `NT_TRACE_ERR(...)` (pr_err) are defined in `src/kernel/nt/kshim.h`. They compile to nothing unless `NT_DEBUG_TRACE` is defined. Message text is unchanged, so `-DNT_DEBUG_TRACE` restores the previous output.

The macro lives in `kshim.h`, not `nt_internal.h`. `nt_obj.c` and `proc_thread.c` cannot include `nt_internal.h`: its `nt_svc_*` prototypes conflict with the definitions in those files. `arch/x86/nt` has no call sites to gate (its trace lines are already `pr_debug`), so no macro was added there.

With the macro off the arguments are not evaluated. None of the gated calls has side effects in its arguments.

## Inventory

Gated = now behind `NT_TRACE`/`NT_TRACE_ERR`. Kept = unchanged, with reason.

### src/kernel/nt/nt_obj.c (commit b2b3e3b)

| Line | Level | Text | Hot path | Action |
|---|---|---|---|---|
| 561 | ERR | `NtCreateMutant: st=...` | per call | gated |
| 1250, 1254, 1259 | ERR | `NtWaitForSingleObject: ...` (userspace branch) | per call | gated |
| 1309 | INFO | `W1_INSTR: NtReleaseMutant ...` | per call | gated |
| 1341, 1345 | ERR | `wait_on_ethread: ...` | per wait | gated |
| 1443, 1447, 1449, 1452 | ERR | `NtWaitForSingleObject: ...` (kernel branch) | per call | gated |
| 1478, 1486, 1489 | INFO | `W1_INSTR: NtDelayExecution ...` | per call | gated |

### src/kernel/nt/proc_thread.c (commit 8140aec)

| Line | Level | Text | Hot path | Action | Reason |
|---|---|---|---|---|---|
| 1259 | INFO | `NtCreateThreadEx: StartRoutine=...` | per thread creation | kept | `vm/test_n4.sh` greps `NtCreateThreadEx` in dmesg |
| 1290 | INFO | `W1_INSTR: ObCreateHandle ...` | per thread creation | gated | |
| 1338-1357 | INFO | `W1_INSTR: child creation ...`, `after set_tsk_thread_flag`, `after nt_setup_child_regs`, `before/after wake_up_new_task` | per thread creation | gated | |
| 1317 | ERR | `NtCreateThreadEx: copy_process failed` | error path | kept | error, not per call |
| 1372, 1374, 1379 | INFO | `W1_INSTR: copy_to_user ...`, `NtCreateThreadEx success` | per thread creation | gated | |
| 1419-1472 | INFO | `W1_INSTR: NtSuspendThread ...`, `NtResumeThread ...` (and `thr->terminated` variants) | per call | gated | |
| 1437, 1479 | INFO | `NtSuspendThread: tid=...`, `NtResumeThread: tid=...` | per call | gated | |
| 1561, 1564 | ERR | `NtTerminateThread: ...` | per call | gated | |
| 481, 484, 510, 513 | stderr | `Suspend: ObReference failed`, `Suspend: find thr failed`, `Resume: ...` | error path | kept | fprintf on failure only |
| 1119-1204 | DEBUG | `nt_alloc_user_trampoline`, `nt_setup_child_regs` | per thread creation | kept | already `pr_debug` |

### src/kernel/nt/dispatch.c

| Line | Level | Text | Hot path | Action | Reason |
|---|---|---|---|---|---|
| 311 | INFO | `nt: missing service %u` | per unimplemented call | kept | Follow-up: make ratelimited like `arch/x86/nt/dispatch.c:311`. Not done here because `pr_info_ratelimited` is missing from `kshim.h`, and the syntax check would add a failure. |
| 114 | DEBUG | `do_nt_syscall: ssn=...` | per call | kept | already `pr_debug` |

### arch/x86/nt/proc_thread.c

| Lines | Level | Text | Action | Reason |
|---|---|---|---|---|
| 1245-1334, 1395-1472 | DEBUG | `W1_INSTR: ...` | kept | already `pr_debug`; silent unless dynamic debug is enabled |
| 464, 467, 493, 496 | stderr | Suspend/Resume error paths | kept | error path |

### Other sites reviewed, kept

| File:line | Level | Text | Reason |
|---|---|---|---|
| src/kernel/nt/io.c:1872, arch/x86/nt/nt_io.c:1852 | INFO | `nt console[%ld]: %s` | product behaviour (STD_ERROR routed to the kernel log), not debug |
| arch/x86/nt/kernel_stubs.c:20 | INFO | personality banner | boot banner |
| src/kernel/nt/kshim.c:113 | stderr | `LEAK SITE ...` | leak report only |
| src/kernel/nt/mm.c:72, arch/x86/nt/mm.c:63 | DEBUG | `kshim_mprotect: ...` | already `pr_debug` |
| src/kernel/fs/kshim.h:193-195, src/kernel/nt/kshim.h | macros | `pr_err`/`pr_warn`/`pr_info` shims | definitions, not calls |
| `*_test.c` in src/kernel/nt, src/kernel/fs | stderr | `FAIL ...` | test harness output |

## Script dependencies

| Script | Looks for | Effect of gating |
|---|---|---|
| `vm/test_n4.sh` | dmesg grep `W1_INSTR\|do_syscall_64\|do_nt_syscall\|NtCreateThreadEx\|binfmt_pe` | Its verdict is from the exit code only. The `NtCreateThreadEx` line is kept. The `W1_INSTR` lines from `src/kernel/nt` disappear unless the guest is built with `-DNT_DEBUG_TRACE`. Build with that flag to get the old capture. |
| `tests/kernel-host/README.md` | documents the `W1_INSTR`/`NtWaitForSingleObject:` flood | updated to describe the macro |

No other script in `vm/` or `tests/` greps for the gated strings.

## Verification

- `bash tests/syntax/check.sh`: same 5 failures before and after (`src/kernel/nt/dispatch.c`, `src/kernel/nt/kshim.c`, `src/kernel/fs/binfmt_pe.c`, `src/kernel/fs/binfmt_pe_test.c`, `arch/x86/nt/dispatch.c`). Warning total unchanged at 452.
- Each gated file was also compiled with `-DNT_DEBUG_TRACE`; warning counts match the default build.
- `make check` in `tests/kernel-host`: all tests pass except `mman_test`, which exits 126 because Windows Application Control blocks the freshly built exe. This is pre-existing and was not bypassed.

## Follow-ups

1. Make `src/kernel/nt/dispatch.c:311` ratelimited, which needs `pr_info_ratelimited` in `kshim.h`.
2. Decide whether `vm/test_n4.sh` should set `NT_DEBUG_TRACE` so its dmesg capture keeps the `W1_INSTR` lines.
