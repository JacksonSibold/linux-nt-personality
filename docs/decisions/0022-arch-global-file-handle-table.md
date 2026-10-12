# 0022: Handle table is global; handles are shared across processes (arch IO-3)

Status: Proposed. Arch source (`arch/x86/nt`). Recorded only in this batch, not implemented. Decide together with 0007.
Finding: `docs/review/arch-x86-nt.md` IO-3 (High).
Date: 2026-10-12

## Context

`arch/x86/nt/nt_io.c:348` declares one `static struct ntio_handle_entry ntio_handles[NTIO_MAX_HANDLES]` for the whole system. `ntio_handle_lookup` (`402-430`), `ntio_handle_close` (`488-505`) and `NtClose` check no owner. Any NT process can read, write, query, close or duplicate another process's file handles if it knows the number. This is the arch counterpart of kernel-nt IO-2 (0007).

## Options

1. Put the table in `nt_eprocess` and check ownership on each lookup. Tradeoff: moves the table and every caller. Large change. End state.
2. Keep the global table, store the owning process in each entry, and reject foreign handles in `ntio_handle_lookup`. Tradeoff: smaller change, but still one shared table. Interim.
3. Use the object manager's handle table (`ObCreateHandle`, as `proc_thread.c` does). Tradeoff: depends on the object model; the same decision as 0007.

## Recommendation

Option 2 now. Option 3 as the target, so arch and kernel-nt use one handle system.

## Patch sketch

- Add `owner` (process id) to `struct ntio_handle_entry`, set it on create.
- In `ntio_handle_lookup` and `ntio_handle_close`, return `STATUS_INVALID_HANDLE` when `owner` differs from the caller.

## Test plan

- Kernel test: process B passes process A's file handle number to `NtReadFile` and `NtClose`. Both fail with `STATUS_INVALID_HANDLE`.
- Kernel test: a process's own handles still work.
- Host: not applicable.

## Not verified

Not built or run. Inheritance and duplication policy (0007) decides whether shared handles are allowed later.
