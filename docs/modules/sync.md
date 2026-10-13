# Module: sync (Dispatcher objects and wait engine)

Sources: `src/kernel/nt/sync.c`, `src/kernel/nt/sync.h`
Related: `object.md` (dispatcher objects are object-manager bodies), `proc_thread.md` (thread lifecycle and suspend), `iocp.md` (alertable APC delivery).

## Purpose

Implements NT dispatcher objects (events, mutants, semaphores, waitable timers, keyed events), the wait engine for single and multiple objects with WaitAll and WaitAny semantics, address-based waits (`WaitOnAddress` style), and thread alert state. The file-level comment states that WaitAll atomicity depends on a fixed lock order, described below.

The file builds for two targets: a userspace shim that maps kernel primitives to pthreads (`#ifndef __KERNEL__`), and the kernel build (`__KERNEL__`) using `spinlock_irqsave` and wait queues.

## Public API (sync.h)

Thread state:

- `NTSTATUS nt_thread_create(struct nt_thread **out)` / `void nt_thread_destroy(struct nt_thread *t)` - create or destroy the sync-side thread record.
- `void nt_thread_alert(struct nt_thread *t)` - set the alerted state and wake an alertable wait.
- `void nt_thread_queue_apc(struct nt_thread *t)` - mark an APC pending (`alert_pending`).
- `void nt_thread_clear_alert(struct nt_thread *t)` - clear alert state.
- `nt_current_thread()` / `nt_set_current_thread(t)` - thread-local current pointer.

Object creation and release:

- `nt_create_event(struct nt_event **out, int type, BOOLEAN initial_state)`
- `nt_create_mutant(struct nt_mutant **out, BOOLEAN initial_owner)`
- `nt_create_semaphore(struct nt_semaphore **out, LONG initial_count, LONG maximum_count)`
- `nt_create_timer(struct nt_timer **out, int type)`
- `nt_create_keyed_event(struct nt_keyed_event **out)`
- `nt_put_event`, `nt_put_mutant`, `nt_put_semaphore`, `nt_put_timer`, `nt_put_keyed_event` - drop a reference and free at zero.
- `nt_header_addref`, `nt_header_release` - reference helpers on `nt_dispatcher_header`.

Signalling:

- `nt_set_event(struct nt_event *, LONG *prev_state)`, `nt_reset_event(...)`, `nt_pulse_event(...)`.
- `nt_release_mutant(struct nt_mutant *, LONG *prev_count)`.
- `nt_release_semaphore(struct nt_semaphore *, LONG release_count, LONG *prev_count)`.
- `nt_set_timer(struct nt_timer *, const LARGE_INTEGER *due_time, LONG period_ms, void (*apc_routine)(void *), void *apc_ctx)` and `nt_cancel_timer(struct nt_timer *, BOOLEAN *current_state)`. A non-NULL `apc_routine` is refused with `STATUS_NOT_IMPLEMENTED` before any timer state changes, in both builds (`sync.c:876` shim, `sync.c:1721` kernel).
- `nt_wait_for_keyed_event(...)` and `nt_release_keyed_event(...)` - keyed-event rendezvous with `key`, `alertable`, `timeout`.

Waiting:

- `nt_wait_for_single_object(struct nt_dispatcher_header *obj, BOOLEAN alertable, LARGE_INTEGER *timeout)`.
- `nt_wait_for_multiple_objects(ULONG count, struct nt_dispatcher_header **objects, ULONG wait_type, BOOLEAN alertable, LARGE_INTEGER *timeout)` - `wait_type` is `WaitAll` (0) or `WaitAny` (1).
- `nt_wait_on_address(void *addr, void *cmp, size_t size, LARGE_INTEGER *timeout)`, `nt_wake_by_address_single(void *addr)`, `nt_wake_by_address_all(void *addr)`.
- `nt_abandon_owned_mutants(struct nt_thread *)` - thread-exit hook that abandons mutants the thread still owns.
- `nt_dispatcher_is_signaled(struct nt_dispatcher_header *h, struct nt_thread *for_thread)` - test helper.

Service entry points defined here (`nt_svc_*` in `sync.c`, 12 in total):

- Waits: `NtWaitForMultipleObjects` (`sync.c:2237`), `NtSignalAndWaitForSingleObject` (`sync.c:2679`).
- Timers: `NtCreateTimer` (`:2460`), `NtOpenTimer` (`:2489`), `NtSetTimer` (`:2527`), `NtCancelTimer` (`:2567`), `NtQueryTimer` (`:2633`).
- Keyed events: `NtCreateKeyedEvent` (`:2774`), `NtOpenKeyedEvent` (`:2804`), `NtWaitForKeyedEvent` (`:2856`), `NtReleaseKeyedEvent` (`:2864`).
- Scheduler: `NtYieldExecution` (`:2874`).

`NtWaitForSingleObject`, `NtCreateEvent`, mutant and semaphore services are in `nt_obj.c`. Event query and mutant/semaphore query services are in `nt_services.c`. `NtSetTimerResolution` and `NtQueryTimerResolution` are in `nt_services.c` too; there is no separate system unit, so they have no module page of their own.

## Key data structures

- `enum nt_disp_type` - `NT_OBJ_EVENT`, `NT_OBJ_MUTANT`, `NT_OBJ_SEMAPHORE`, `NT_OBJ_TIMER`, `NT_OBJ_KEYED_EVENT`.
- `struct nt_dispatcher_header` - `type`, a per-object lock (`pthread_mutex_t` in the shim, `spinlock_t` in the kernel build), `waiters` list, `signal_state`, `manual_reset`, and an atomic refcount.
- `struct nt_event`, `struct nt_semaphore` (adds `limit`), `struct nt_mutant` (adds `owner`, `recursion`, `abandoned`, `owned_entry` linked into the owner thread's `owned_mutants`), `struct nt_timer`, `struct nt_keyed_event`.
- `struct nt_thread` - `tid`, `alerted`, `alert_pending`, `owned_mutants`, `dying`, and a lock. The shim version adds `pt` (pthread id). The kernel version adds `task` and a `global_entry`.
- `struct nt_wait_block` and `struct nt_wait_ctx` - per-waiter records queued on each object's `waiters` list.
- `struct nt_addr_wait` - one entry per address being waited on.

## Invariants and locking (as implemented)

- Multi-lock paths must take object locks in ascending address order and release them in reverse. The header comment (`sync.c:4-16`) names this as the basis for the WaitAll atomicity argument. The sort is `sort_by_addr` (`sync.c:399`) in the shim and `kernel_sort_by_addr` (`sync.c:1367`) in the kernel build, applied when `wait_type == WaitAll` (`sync.c:581` and `sync.c:2020`).
- The same ordering is required in the acquisition loop, the undo-on-partial-failure path, waiter insertion and removal, and the zero-timeout poll. The kernel-side comment at `sync.c:1246` says to preserve it.
- Waiter insertion happens while holding all object locks, followed by blocking on a per-waiter wait context. Signalers wake that context. This prevents lost wakeups.
- Shim globals guard the wait lists: `g_keyed_lock` protects `g_keyed_waiters` and `g_keyed_releasers`, and `g_addr_lock` protects `g_addr_waiters` (`sync.c:50-53`). `pthread_once` runs the initialiser (`sync.c:54-62`).
- Timer signalling takes the timer lock and then the header lock (`timer_thread_func`, `sync.c:833`).
- Owned mutants are linked into the owning thread's `owned_mutants` list, and thread exit calls `nt_abandon_owned_mutants`.
- `sync.c` has 48 `spin_lock_irqsave` and 59 `spin_unlock_irqrestore` calls (kernel build) and 64 `pthread_mutex_lock` and 83 `pthread_mutex_unlock` calls (shim). The counts are for the whole file, not per build. I did not map each call to its field.

## Known gaps (evident in code)

- The shim timer thread (`timer_thread_func`, `sync.c:833`) waits on a condition variable with an absolute deadline (`pthread_cond_timedwait`). It no longer sleeps in one-second steps. Periodic timers skip missed periods rather than firing a burst. The kernel build uses `mod_timer` (`nt_set_timer`, `sync.c:1717`) and a different wait path.
- Timer APC routines are not implemented in either build. `NtSetTimer` returns `STATUS_NOT_IMPLEMENTED` when a routine is given (`sync.c:2554`).
- Two sort helpers exist: `sort_by_addr` (shim) and `kernel_sort_by_addr` (kernel), which the file-level comments cite. I did not trace which helper each call site uses in each build.
- Keyed events share global lists (`g_keyed_waiters`, `g_keyed_releasers`) rather than per-object state. The struct comment at `sync.h:230` still says the header is "not really used; global table holds waiters".
- The userspace build maps kernel primitives to pthreads. The kernel build's wait queues and TIF interaction are not exercised by the userspace tests (`sync_test.c`).
