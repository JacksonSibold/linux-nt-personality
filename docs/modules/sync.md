# Module: sync (Dispatcher objects and wait engine)

Sources: `src/kernel/nt/sync.c`, `src/kernel/nt/sync.h`
Related: `object.md` (dispatcher objects are object-manager bodies), `proc_thread.md` (thread lifecycle, alerts), `iocp.md` (`NtTestAlert` and APC delivery), `nt_obj.c` and `nt_services.c` (the event, mutant and semaphore wrappers, which are not in this file).

## Purpose

Implements NT dispatcher objects (events, mutants, semaphores, waitable timers, keyed events), the wait engine for single and multiple objects with WaitAll and WaitAny semantics, address-based waits, and the per-thread alert state. The file comment (`sync.c:4-16`) states that WaitAll atomicity depends on a fixed lock order.

The file builds for two targets: a userspace shim that maps kernel primitives to pthreads (`#ifndef __KERNEL__`), and the kernel build (`__KERNEL__`) with `spinlock_irqsave` and wait queues. Most of the file is shared; the lock and wait primitives differ.

## Public API (sync.h)

Thread state:

- `NTSTATUS nt_thread_create(struct nt_thread **out)` (`sync.c:145`) and `void nt_thread_destroy(struct nt_thread *t)` - create or destroy the sync-side thread record. Destroy abandons the thread's owned mutants.
- `void nt_thread_alert(struct nt_thread *t)` - sets `alerted`. Every alertable wait tests it.
- `void nt_thread_queue_apc(struct nt_thread *t)` - sets `alert_pending`.
- `void nt_thread_clear_alert(struct nt_thread *t)` - clears both flags.
- `bool nt_thread_alert_pending(struct nt_thread *t)` - reads `alerted` without changing it.
- `bool nt_thread_consume_alert(struct nt_thread *t)` - reads `alerted` and clears it. `NtTestAlert` (`iocp.c:773`) and the alertable waits in `nt_obj.c` (`:2109`, `:2126`) use it.

Object creation and release: `nt_create_event`, `nt_create_mutant`, `nt_create_semaphore`, `nt_create_timer`, `nt_create_keyed_event`, and the matching `nt_put_*` functions that drop a reference and free at zero.

Signalling: `nt_set_event`, `nt_reset_event`, `nt_pulse_event`, `nt_release_mutant`, `nt_release_semaphore`, `nt_set_timer`, `nt_cancel_timer`, `nt_wait_for_keyed_event` and `nt_release_keyed_event`.

- `nt_set_timer(timer, due_time, period_ms, apc_routine, apc_ctx)` refuses a non-NULL `apc_routine` with `STATUS_NOT_IMPLEMENTED` before any timer state changes. The check is at `sync.c:903` (shim) and `sync.c:1770` (kernel), and again in `nt_svc_NtSetTimer` (`sync.c:2603`).

Waiting: `nt_wait_for_single_object`, `nt_wait_for_multiple_objects` (`wait_type` is `WaitAll` (0) or `WaitAny` (1)), `nt_wait_on_address`, `nt_wake_by_address_single`, `nt_wake_by_address_all`, `nt_abandon_owned_mutants`, and the test helper `nt_dispatcher_is_signaled`.

Service entry points defined in `sync.c` (raw-argument form, `ULONG_PTR a0, ...`, one definition each, as dispatch calls them):

- Waits: `NtWaitForMultipleObjects` (`sync.c:2286`), `NtSignalAndWaitForSingleObject` (`:2751`).
- Timers: `NtCreateTimer` (`:2509`), `NtOpenTimer` (`:2538`), `NtSetTimer` (`:2576`), `NtCancelTimer` (`:2621`), `NtQueryTimer` (`:2692`).
- Keyed events: `NtCreateKeyedEvent` (`:2856`), `NtOpenKeyedEvent` (`:2886`), `NtWaitForKeyedEvent` (`:2943`), `NtReleaseKeyedEvent` (`:2951`).
- Scheduler: `NtYieldExecution` (`:2961`).

`NtWaitForSingleObject`, the event, mutant and semaphore creation services are in `nt_obj.c`. The event and mutant and semaphore query and set services are in `nt_services.c`. `NtSetTimerResolution` and `NtQueryTimerResolution` are in `nt_services.c` too.

## Key data structures

- `enum nt_disp_type` - `NT_OBJ_EVENT`, `NT_OBJ_MUTANT`, `NT_OBJ_SEMAPHORE`, `NT_OBJ_TIMER`, `NT_OBJ_KEYED_EVENT`.
- `struct nt_dispatcher_header` - `type`, a per-object lock (`pthread_mutex_t` in the shim, `spinlock_t` in the kernel build), a `waiters` list, `signal_state`, `manual_reset`, and a refcount.
- `struct nt_event`, `struct nt_semaphore` (adds `limit`), `struct nt_mutant` (adds `owner`, `recursion`, `abandoned`, and `owned_entry`, which links it into the owner's `owned_mutants`), `struct nt_timer`, `struct nt_keyed_event`.
- `struct nt_thread` - two definitions, one per build (`sync.h`). The shim one has `pt`, `tid`, a `pthread_mutex_t lock`, `alerted`, `alert_pending`, `owned_mutants` and `dying`. The kernel one has a `global_entry` and the task.
- `struct nt_wait_block` and `struct nt_wait_ctx` - one waiter record per object, queued on the object's `waiters` list.
- `struct nt_addr_wait` - one entry per address being waited on.

## Invariants and locking (as implemented)

- Multi-lock paths take object locks in ascending address order and release them in reverse (`sync.c:453`, `:506`). The sort is `sort_by_addr` (`sync.c:426`), applied when `wait_type == WaitAll` (`sync.c:608`). The same order is required in the acquisition loop, the undo-on-partial-failure path, waiter insertion and removal, and the zero-timeout poll.
- Waiter insertion happens while all object locks are held, followed by blocking on a per-waiter context. Signalers wake that context. This prevents lost wakeups.
- Shim globals guard the wait lists: `g_keyed_lock` protects `g_keyed_waiters` and `g_keyed_releasers` (`sync.c:59`), and `g_addr_lock` protects `g_addr_waiters` (`sync.c:62`).
- Timer signalling takes the timer lock and then the header lock (`timer_thread_func`, `sync.c:860`).
- The shim timer thread waits on a condition variable with an absolute deadline (`pthread_cond_timedwait`). Periodic timers skip missed periods rather than firing a burst.
- Thread exit calls `nt_abandon_owned_mutants`, which abandons mutants the thread still owns.
- Alert semantics: an alertable wait that finds the flag set returns `STATUS_ALERTED` and clears it. The alert flag is one field on this module's `nt_thread`, shared with `NtAlertThread` in `proc_thread.c` (audit pass 4 item 6, `a1d128c`).
- Lock counts (whole file, not per build): 50 `spin_lock_irqsave` and 61 `spin_unlock_irqrestore` calls; 66 `pthread_mutex_lock` and 85 `pthread_mutex_unlock` calls. I did not map each call to its field.

## Known gaps (evident in code)

- Timer APC routines are not implemented in either build. `NtSetTimer` returns `STATUS_NOT_IMPLEMENTED` when a routine is given (`sync.c:2603`).
- The header comment (`sync.c:4-16`) names a kernel sort helper, `kernel_sort_by_addr`. No such function exists in this tree. The only sort is `sort_by_addr`, so the comment is stale.
- Keyed events use global lists (`g_keyed_waiters`, `g_keyed_releasers`) rather than per-object state. The comment on the struct in `sync.h` still says the global table holds waiters.
- The kernel build's wait-queue path and its timer mechanism are not exercised by `sync_test.c`, which runs on the userspace build. The kernel path needs a kernel build.
- The kernel `NtDelayExecution` honours the alert when the delay is alertable (`nt_obj.c`). That path needs a kernel build and is not run on the host.
- Access checks were added to `NtSignalAndWaitForSingleObject` (signal and wait rights, `accf1ce`) and to the keyed-event WAIT and RELEASE services (`7c6f281`). The rights of the other sync services were not re-audited in this pass.
