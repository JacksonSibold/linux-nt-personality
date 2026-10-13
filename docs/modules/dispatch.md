# Module: dispatch (NT system-call dispatch)

Sources: `src/kernel/nt/dispatch.c` (kernel-only, no header of its own), `src/kernel/nt/nt_service_list.h` (service table source), `src/kernel/nt/nt_internal.h` (shared constants and `nt_service` types)
Related: every other module, since dispatch resolves into their `nt_svc_*` entry points. `proc_thread.md`, `mm.md`, `io.md`, `iocp.md`, `registry.md`, `sync.md`, `security.md`, `object.md` and `ntstatus_kernel.md` are the main targets. The service-by-service status is in `docs/status/SERVICES.md`.

## Purpose

Routes NT system calls, identified by a system service number (SSN), to the `nt_svc_*` implementation in the kernel. It also provides per-CPU counters for dispatched calls, host-syscall calls and per-SSN calls, and exposes them through debugfs. The module has no public header. Its exported symbols are `do_nt_syscall`, the personality enter and exit hooks, and the counter accessors.

## Public API (as defined in dispatch.c)

Entry point:

- `void do_nt_syscall(struct pt_regs *regs, unsigned int ssn)` (`dispatch.c:110`) - called by the arch syscall path for NT tasks. It reads the first four arguments from registers (`r10`, `rdx`, `r8`, `r9`), reads arguments 5 to `argc` from the user stack (`nt_fetch_stack_args`, `dispatch.c:89`), calls the service, and stores the NTSTATUS in `regs->ax`.

Personality hooks:

- `void nt_enter_personality(struct task_struct *tsk)` (`dispatch.c:393`) - sets `TIF_NTABI` and attaches a sync thread record to the current `nt_ethread`.
- `void nt_exit_personality(struct task_struct *tsk)` (`dispatch.c:413`) - destroys the handle table, abandons owned mutants, clears the current thread, and clears `TIF_NTABI`.
- `void nt_report_missing_service(unsigned int ssn)` (`dispatch.c:427`) - logs an unimplemented SSN.
- `bool nt_can_sysret(struct pt_regs *regs)` (`dispatch.c:432`) - always returns `true`.
- `void nt_host_syscall_inc(void)` (`dispatch.c:68`) - increments the per-CPU host-syscall counter for NT tasks.

Counters:

- `u64 nt_sum_dispatch_count(void)` (`:49`) and `u64 nt_sum_host_count(void)` (`:58`) - sum the per-CPU counters over all possible CPUs.
- Per-CPU variables: `nt_dispatch_count`, `nt_host_syscall_count`, `nt_dispatch_per_ssn[NT_NR_SERVICES]`.

Service table: `nt_service_table[NT_NR_SERVICES]` (`dispatch.c:83`), indexed by SSN. Each entry is `[ssn] = { .fn = nt_svc_<name>, .argc = <n>, .name = "<name>" }`, built from `nt_service_list.h`.

Debugfs: `ntpers/dispatch_count`, `ntpers/host_count` and `ntpers/dispatch_per_ssn`, created in `nt_counters_debugfs_init` (`dispatch.c:497`) and registered with `late_initcall` (`dispatch.c:511`).

Virtual-memory wrappers defined in `dispatch.c`: `nt_svc_NtAllocateVirtualMemory` (`:164`), `NtFreeVirtualMemory` (`:193`), `NtProtectVirtualMemory` (`:220`), `NtQueryVirtualMemory` (`:251`), `NtReadVirtualMemory` (`:283`), `NtWriteVirtualMemory` (`:301`), `NtFlushVirtualMemory` (`:319`) and `NtAllocateVirtualMemoryEx` (`:352`). They call the native-signature functions in `mm.c`, which are declared `extern` at `dispatch.c:151` to `:162`. Every other service wrapper is defined in its own module (see the module index).

## Key data structures

- `struct nt_service` - `nt_service_fn fn`, `unsigned int argc`, `const char *name`. The function type takes twelve `unsigned long` arguments.
- `NT_NR_SERVICES` = 476 (`nt_internal.h:16`), "0x1dc = max SSN 0x1db + 1".
- `NT_MAX_ARGS` = 12. `NT_SYSCALL_SKIP` = -1.
- `nt_service_list.h` contains 171 `NT_SERVICE(ssn, name, argc)` lines.

## Control flow (`do_nt_syscall`)

1. If the SSN is one of six hard-coded values (`0x53`, `0x1c8`, `0xc5`, `0x1c4`, `0x52`, `0x34`), call `pr_debug` (`dispatch.c:114`).
2. Increment `nt_dispatch_count` and `nt_dispatch_per_ssn[ssn]` (when in range).
3. If `ssn >= NT_NR_SERVICES`, set `regs->ax = STATUS_INVALID_SYSTEM_SERVICE` and return.
4. Apply `array_index_nospec` to `ssn` and look up `svc` (`dispatch.c:125`).
5. If `svc->fn` is NULL (no definition was linked), call `nt_report_missing_service` and set `STATUS_NOT_IMPLEMENTED`.
6. Read the arguments. If `nt_fetch_stack_args` fails (a `__get_user` fault), return `STATUS_ACCESS_VIOLATION`.
7. Call `svc->fn` with twelve arguments and store the result in `regs->ax`.
8. If `TIF_NTSUSPEND` is set, call `nt_thread_check_suspend()` before returning (`dispatch.c:144`; see `proc_thread.md`).

## Invariants (as implemented)

- Missing services do not crash. The `NT_SERVICE` macro declares each `nt_svc_` symbol as an extern weak function (`dispatch.c:74`), so an unimplemented service resolves to NULL and goes through the `STATUS_NOT_IMPLEMENTED` path.
- Counters are per-CPU, incremented with `this_cpu_inc` and `__this_cpu_add`, and summed with `for_each_possible_cpu`. No lock is taken.
- The SSN bound is checked before the table is indexed, and `array_index_nospec` is applied to the index (Spectre v1 mitigation).
- Stack arguments are read from user memory with `__get_user`, and the function returns false on any fault.

## Known gaps (evident in code)

- Debug logging on the hot path: `do_nt_syscall` calls `pr_debug` for six hard-coded SSNs (`dispatch.c:114`) on every call. The calls are silent unless dynamic debug is enabled, but the branch and the format arguments stay in the hot path. They are not gated by a compile-time option. (An earlier per-call `pr_info` was removed in `8ccc960`.)
- `nt_can_sysret` is a stub that always returns `true`.
- The table is sized for SSNs up to 0x1db, but only 171 entries are listed in `nt_service_list.h`. The other slots resolve to NULL and return `STATUS_NOT_IMPLEMENTED`.
- Names in `nt_service_list.h` with no `nt_svc_` definition: 27 services, all `MISSING` in `docs/status/SERVICES.md`. Examples are `NtCreateSectionEx`, `NtMapViewOfSectionEx`, the port services and the debug and job-object services.
- `NtAllocateVirtualMemoryEx` and `NtAlertResumeThread` now have definitions (`dispatch.c:352` and `proc_thread.c`). The earlier notes that they were missing are stale.
- The virtual-memory declarations in `dispatch.c` are `extern` prototypes for the functions in `mm.c`. There is no `dispatch.h`, so callers outside this module have no declared interface other than the symbols listed above.
- `tools/check_syscall_tables.py` checks the names, SSNs and argument counts across the copies of the table. It does not compare the C signatures of the `nt_svc_` wrappers with `argc`.
