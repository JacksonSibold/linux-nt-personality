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
- `nt_set_timer(struct nt_timer *, const LARGE_INTEGER *due_time, LONG period_ms, void (*apc_routine)(void *), void *apc_ctx)` and `nt_cancel_timer(struct nt_timer *, BOOLEAN *current_state)`.
- `nt_wait_for_keyed_event(...)` and `nt_release_keyed_event(...)` - keyed-event rendezvous with `key`, `alertable`, `timeout`.

Waiting:

- `nt_wait_for_single_object(struct nt_dispatcher_header *obj, BOOLEAN alertable, LARGE_INTEGER *timeout)`.
- `nt_wait_for_multiple_objects(ULONG count, struct nt_dispatcher_header **objects, ULONG wait_type, BOOLEAN alertable, LARGE_INTEGER *timeout)` - `wait_type` is `WaitAll` (0) or `WaitAny` (1).
- `nt_wait_on_address(void *addr, void *cmp, size_t size, LARGE_INTEGER *timeout)`, `nt_wake_by_address_single(void *addr)`, `nt_wake_by_address_all(void *addr)`.
- `nt_abandon_owned_mutants(struct nt_thread *)` - thread-exit hook that abandons mutants the thread still owns.
- `nt_dispatcher_is_signaled(struct nt_dispatcher_header *h, struct nt_thread *for_thread)` - test helper.

Service entry point defined here: `nt_svc_NtWaitForMultipleObjects`. The single-object and event/mutant/semaphore service wrappers are in `nt_obj.c` and `nt_services.c`.

## Key data structures

- `enum nt_disp_type` - `NT_OBJ_EVENT`, `NT_OBJ_MUTANT`, `NT_OBJ_SEMAPHORE`, `NT_OBJ_TIMER`, `NT_OBJ_KEYED_EVENT`.
- `struct nt_dispatcher_header` - `type`, a per-object lock (`pthread_mutex_t` in the shim, `spinlock_t` in the kernel build), `waiters` list, `signal_state`, `manual_reset`, and an atomic refcount.
- `struct nt_event`, `struct nt_semaphore` (adds `limit`), `struct nt_mutant` (adds `owner`, `recursion`, `abandoned`, `owned_entry` linked into the owner thread's `owned_mutants`), `struct nt_timer`, `struct nt_keyed_event`.
- `struct nt_thread` - `tid`, `alerted`, `alert_pending`, `owned_mutants`, `dying`, and a lock. The shim version adds `pt` (pthread id). The kernel version adds `task` and a `global_entry`.
- `struct nt_wait_block` and `struct nt_wait_ctx` - per-waiter records queued on each object's `waiters` list.
- `struct nt_addr_wait` - one entry per address being waited on.

## Invariants and locking (as implemented)

- Multi-lock paths must take object locks in ascending address order and release them in reverse. The header comment (`sync.c:4-15`) names this as the basis for the WaitAll atomicity argument. The sort is `sort_by_addr` (`sync.c:394`), applied when `wait_type == WaitAll` (`sync.c:576`).
- The same ordering is required in the acquisition loop, the undo-on-partial-failure path, waiter insertion and removal, and the zero-timeout poll. The comment at `sync.c:1214-1221` says to preserve it.
- Waiter insertion happens while holding all object locks, followed by blocking on a per-waiter wait context. Signalers wake that context. This prevents lost wakeups.
- Shim globals `g_keyed_lock` and `g_addr_lock` guard the keyed-event and address-wait lists. `pthread_once` initialises them (`sync.c:59`).
- Owned mutants are linked into the owning thread's `owned_mutants` list, and thread exit calls `nt_abandon_owned_mutants`.
- `sync.c` uses `spin_lock_irqsave` and `spin_unlock_irqrestore` at about 46 and 57 sites respectively (kernel build), and `pthread_mutex_lock`/`unlock` at about 62 and 78 sites (shim). I did not map each site to its field.

## Known gaps (evident in code)

- The timer thread's sleep loop (`timer_thread_func`, `sync.c:813` onward) is commented as "simplified": it sleeps in one-second chunks and recomputes the remaining time approximately (`sync.c:849-852`).
- Two sort helpers exist: `sort_by_addr` (`sync.c:394`, used on the WaitAll path at line 576) and `kernel_sort_by_addr` (`sync.c:1339`), which the file-level comments cite. I did not trace which helper each call site uses in each build.
- `nt_svc_NtWaitForMultipleObjects` is the only wait service defined in this file. Other wait services are in `nt_obj.c`.
- `nt_keyed_event` carries a comment that its header is "not really used; global table holds waiters". Keyed events share a global list rather than per-object state.
- The userspace build maps kernel primitives to pthreads. The kernel build's wait queues and TIF interaction are not exercised by the userspace tests (`sync_test.c`).
