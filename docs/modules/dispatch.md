# Module: dispatch (NT system-call dispatch)

Sources: `src/kernel/nt/dispatch.c` (kernel-only, no header of its own), `src/kernel/nt/nt_service_list.h` (service table source), `src/kernel/nt/nt_internal.h` (shared constants and `nt_service` types)
Related: every other module, since dispatch resolves into their `nt_svc_*` entry points. `proc_thread.md`, `mm.md`, `io.md`, `iocp.md`, `registry.md`, `sync.md`, `ntstatus_kernel.md` are the main targets.

## Purpose

Routes NT system calls, identified by a system service number (SSN), to the `nt_svc_*` implementation in the kernel. It also provides per-CPU counters for dispatched calls, host-syscall calls and per-SSN calls, and exposes them through debugfs. The module has no public header. Its exported symbols are `do_nt_syscall`, the personality enter and exit hooks, and the counter accessors.

## Public API (as defined in dispatch.c)

Entry point:

- `void do_nt_syscall(struct pt_regs *regs, unsigned int ssn)` - called by the arch syscall path for NT tasks. It reads the first four arguments from registers (r10, rdx, r8, r9), reads arguments 5 to `argc` from the user stack, calls the service, and stores the NTSTATUS in `regs->ax`.

Personality hooks:

- `void nt_enter_personality(struct task_struct *tsk)` - sets `TIF_NTABI` and attaches a sync thread record (`nt_thread_create`) to the current `nt_ethread`.
- `void nt_exit_personality(struct task_struct *tsk)` - calls `nt_handle_table_destroy`, abandons owned mutants, and clears `TIF_NTABI`.
- `void nt_report_missing_service(unsigned int ssn)` - logs an unimplemented SSN.
- `bool nt_can_sysret(struct pt_regs *regs)` - always returns `true` in this file.
- `void nt_host_syscall_inc(void)` - increments the per-CPU host-syscall counter for NT tasks.

Counters:

- `u64 nt_sum_dispatch_count(void)` and `u64 nt_sum_host_count(void)` - sum the per-CPU counters over all possible CPUs.
- Per-CPU variables: `nt_dispatch_count`, `nt_host_syscall_count`, `nt_dispatch_per_ssn[NT_NR_SERVICES]`.

Service table (built in `dispatch.c` from `nt_service_list.h`): `struct nt_service` holds `fn`, `argc` and `name`. The table is `nt_service_table[NT_NR_SERVICES]`, indexed by SSN. Each entry is `[ssn] = { .fn = nt_svc_<name>, .argc = <n>, .name = "<name>" }`.

Debugfs: `ntpers/dispatch_count`, `ntpers/host_count` and `ntpers/dispatch_per_ssn` (created in `nt_counters_debugfs_init`, registered with `late_initcall`, `dispatch.c:378-392`).

## Key data structures

- `struct nt_service` - `nt_service_fn fn`, `unsigned int argc`, `const char *name`. The function type takes twelve `unsigned long` arguments.
- `NT_NR_SERVICES` = 476 (`nt_internal.h:16`), "0x1dc = max SSN 0x1db + 1".
- `NT_MAX_ARGS` = 12. `NT_SYSCALL_SKIP` = -1.
- `nt_service_list.h` contains 171 `NT_SERVICE(ssn, name, argc)` lines. Each line sets the table entry for one SSN.

## Control flow (`do_nt_syscall`)

1. Increment `nt_dispatch_count` and `nt_dispatch_per_ssn[ssn]` (when in range).
2. If `ssn >= NT_NR_SERVICES`, set `regs->ax = STATUS_INVALID_SYSTEM_SERVICE` and return.
3. Apply `array_index_nospec` to `ssn` and look up `svc`.
4. If `svc->fn` is NULL (the weak symbol was not defined), call `nt_report_missing_service` and return `STATUS_NOT_IMPLEMENTED`.
5. Read the arguments. If `nt_fetch_stack_args` fails (a `__get_user` fault), return `STATUS_ACCESS_VIOLATION`.
6. Call `svc->fn` with twelve arguments and store the result in `regs->ax`.
7. If `TIF_NTSUSPEND` is set, call `nt_thread_check_suspend()` before returning (`proc_thread.md`).

## Invariants (as implemented)

- Missing services do not crash. Declarations in `dispatch.c` are `extern NTSTATUS nt_svc_<name>() __attribute__((weak))`, so an unimplemented service resolves to NULL and goes through the `STATUS_NOT_IMPLEMENTED` path.
- Counters are per-CPU, incremented with `this_cpu_inc`/`__this_cpu_add`, and summed with `for_each_possible_cpu`. No lock is taken.
- The SSN bound is checked before the table is indexed, and `array_index_nospec` is applied to the index (Spectre v1 mitigation).
- Stack arguments are read from user memory with `__get_user`, and the function returns false on any fault.

## Known gaps (evident in code)

- Debug logging in the hot path: `do_nt_syscall` calls `pr_info` for six hard-coded SSNs (`0x53`, `0x1c8`, `0xc5`, `0x1c4`, `0x52`, `0x34`) on every call. These are not gated by a debug option.
- `nt_can_sysret` is a stub that always returns `true`.
- The table is sized for SSNs up to 0x1db, but only 171 entries are listed in `nt_service_list.h`. The rest of the 476 slots resolve to NULL and return `STATUS_NOT_IMPLEMENTED`.
- Some service names in `nt_service_list.h` have no `nt_svc_` definition anywhere under `src/kernel/nt/`. Checked: `NtAllocateVirtualMemoryEx`, `NtAlertResumeThread` and `NtWaitForAlertByThreadId`. These resolve to NULL and return `STATUS_NOT_IMPLEMENTED`. I did not check all 171 names.
- `dispatch.c` declares the native `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtProtectVirtualMemory` and `NtQueryVirtualMemory` as `extern` (`dispatch.c:151-154`) and also defines `nt_svc_NtAllocateVirtualMemory`, `nt_svc_NtFreeVirtualMemory`, `nt_svc_NtProtectVirtualMemory` and `nt_svc_NtQueryVirtualMemory` at `dispatch.c:156`, `185`, `212` and `243`. Those wrappers sit in the same file as the table, unlike the other modules' services.
- The header `nt_internal.h` provides the shared types. There is no `dispatch.h`, so callers outside this module have no declared interface other than the symbols listed above.
