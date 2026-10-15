# 0007: File handle table is global with no owner (IO-2)

Status: Proposed. Needs a policy for duplicates and inheritance. Partly implemented (2026-10-14, branch `haiku/hndl`): each slot records its owner process (`ntio_handle_entry.owner`), and process exit frees every slot the process owns, including the standard handles and any it leaked (`ntio_release_owned_handles`, called from do_exit when the last thread of the group exits). That fixes the exhaustion of the 1024-slot table (about 320 PE execs). Not implemented: the ownership check in lookup, peek and close (a process can still use another process's handle number), and the duplicate and inheritance rule. Option 1 is still the end state.
Finding: `docs/review/kernel-nt.md` IO-2 (Medium).
Date: 2026-10-12

## Context

File handles live in one array, `ntio_handles[NTIO_MAX_HANDLES]` (`src/kernel/nt/io.c`, struct at `:340-347`). `ntio_handle_create` (`io.c:366-395`) and `ntio_duplicate_file_handle` (`io.c:467-485`) store entries with no record of which process created them. Three consequences:

- `ntio_handle_close` (`io.c:486-505`) accepts any valid index and closes it, whichever process owns it.
- `ntio_handle_peek` (`io.c:452-465`) returns the object without taking a reference, so any caller can use another caller's file after the owner closes it.
- The table is reset as a whole by `ntio_reset_handle_table` (`io.c:355-362`), which is a test hook, but it shows the table has no per-process lifetime.

Object-manager handles are per-process (`handle.c`, `ObCreateHandle(proc, ...)`). File handles are not, so the two tables disagree about who owns a handle.

## Options

1. **Move file handles into the per-process table (recommended long term).** Register the file object as an `nt_object_type` and use `ObCreateHandle` / `ObReferenceObjectByHandle` like every other object. Tradeoff: largest change. It touches every call to `ntio_handle_*` (about 20 call sites across `io.c`, `iocp.c`, `nt_obj.c`, `io_test.c`) and needs the object-model change.
2. **Record an owner and check it.** Add `struct nt_eprocess *owner` to `struct ntio_handle_entry`, set it in `ntio_handle_create` and `ntio_duplicate_file_handle` from the current process, and check it in `ntio_handle_close` and in lookup and peek. Tradeoff: duplicates and inheritance need a rule (the duplicate gets the owner of the destination process; inheritance is not implemented here). The owner pointer must not outlive the process, so it needs cleanup at process exit.
3. **Owner as a process id.** Store the PID rather than a pointer. Tradeoff: PIDs are reused, so a stale entry can match a new process. Rejected.

## Recommendation

Option 2 as an interim fix, because it is local to `io.c` and does not touch the object model. Option 1 is the end state and belongs with the object-model review. Option 2 needs the duplicate rule settled first, which is why it is not implemented in this batch.

## Patch sketch (option 2)

- `io.c:340-347`: add `struct nt_eprocess *owner;` to `struct ntio_handle_entry`.
- `io.c:366-395` (`ntio_handle_create`) and `:467-485` (`ntio_duplicate_file_handle`): set `owner = nt_current_eprocess()` under `ntio_handle_lock`.
- `io.c:486-505` (`ntio_handle_close`): return `STATUS_INVALID_HANDLE` when `owner != nt_current_eprocess()`.
- `io.c:402-428` (lookup) and `io.c:452-465` (peek): return NULL on owner mismatch.
- Process exit: walk the table and close entries owned by the exiting process (called from the process destroy path in `proc_thread.c`).
- `ntio_count_handles_for_file` (`:507` area) is unchanged; it counts by object, not owner.

## Test plan

- `src/kernel/nt/io_test.c`: create a file in process A, switch the current process (test hook), and check that `NtClose` and `NtReadFile` from process B fail with `STATUS_INVALID_HANDLE`.
- Duplicate in the same process: works. Duplicate into another process: follows the rule chosen above.
- Process exit closes its file handles.
- `tests/kernel-host` is not used.

## Open questions

- The duplicate rule: the destination process owns the duplicate (Windows behaviour) or the source process keeps it. Needs a decision.
- Whether the test harness can switch the current process; if not, a test hook is needed.
