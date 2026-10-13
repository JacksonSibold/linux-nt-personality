/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sync.h - NT dispatcher objects (events, mutants, semaphores, timers,
 *          keyed events) and wait engine with true wait-all.
 *
 * Userspace torture harness: USERSPACE_KSHIM maps kernel primitives to pthreads.
 * Kernel build: __KERNEL__ uses real kernel headers (not needed for this harness).
 *
 * WaitAll atomicity:
 *  - Acquisition-with-undo protocol: attempt to acquire ALL objects atomically
 *    in a globally consistent order (sorted by address). On failure, release
 *    every already-acquired object before blocking. Then block on combined
 *    wait and retry.
 *  - Ordering discipline prevents lock-order inversion between two threads
 *    waiting on overlapping sets in different orders: we sort handles by
 *    pointer value before acquiring locks. All threads agree on same order.
 *  - Lost-wakeup prevention: insert wait blocks while holding all object locks,
 *    then release and block on per-waiter condvar. Wakers signal the waiter's
 *    condvar whenever an object becomes signaled.
 */

#ifndef NT_SYNC_H
#define NT_SYNC_H

#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"

#ifndef __KERNEL__
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#else
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/time.h>
#endif

#define MAXIMUM_WAIT_OBJECTS 64

/* EVENT_TYPE */
#define NotificationEvent       0
#define SynchronizationEvent    1

/* TIMER_TYPE */
#define NotificationTimer       0
#define SynchronizationTimer    1

/* WAIT_TYPE for NtWaitForMultipleObjects */
#define WaitAll   0
#define WaitAny   1
/* compat aliases */
#define WaitAllObject WaitAll
#define WaitAnyObject WaitAny

#ifndef INFINITE
#define INFINITE 0xFFFFFFFFUL
#endif

/* forward */
struct nt_dispatcher_header;
struct nt_thread;

/* ------------------------------------------------------------------ */
/* Thread abstraction for sync tests (distinct from object.h's nt_ethread) */
/* ------------------------------------------------------------------ */
#ifndef __KERNEL__
struct nt_thread {
    pthread_t         pt;
    unsigned long     tid; /* small integer id for mutant ownership checks */
    pthread_mutex_t   lock; /* protects alerted / apc_pending */
    bool              alerted;
    bool              alert_pending; /* APC pending -> STATUS_USER_APC */
    struct list_head  owned_mutants; /* list of nt_mutant.owned_entry */
    bool              dying; /* set when thread exiting */
};

/* TLS current thread */
extern __thread struct nt_thread *nt_current_thread_ptr;
static inline struct nt_thread *nt_current_thread(void) { return nt_current_thread_ptr; }
static inline void nt_set_current_thread(struct nt_thread *t) { nt_current_thread_ptr = t; }
#else
struct nt_thread {
    struct list_head    global_entry;
    struct task_struct *task;
    unsigned long     tid;
    spinlock_t        lock;
    bool              alerted;
    bool              alert_pending;
    struct list_head  owned_mutants;
    bool              dying;
};
extern struct nt_thread *nt_current_thread_ptr;
static inline struct nt_thread *nt_current_thread(void) { return nt_current_thread_ptr; }
static inline void nt_set_current_thread(struct nt_thread *t) { nt_current_thread_ptr = t; }
#endif

NTSTATUS nt_thread_create(struct nt_thread **out);
void     nt_thread_destroy(struct nt_thread *t);
void     nt_thread_alert(struct nt_thread *t);
void     nt_thread_queue_apc(struct nt_thread *t);
void     nt_thread_clear_alert(struct nt_thread *t);
/* Alert state as an alertable wait sees it. nt_thread_alert_pending only
 * reads; nt_thread_consume_alert reads and clears (NT: the alert is used up
 * by the wait or NtTestAlert that reports it). */
bool     nt_thread_alert_pending(struct nt_thread *t);
bool     nt_thread_consume_alert(struct nt_thread *t);

/* ------------------------------------------------------------------ */
/* Dispatcher object model                                              */
/* ------------------------------------------------------------------ */
enum nt_disp_type {
    NT_OBJ_EVENT = 1,
    NT_OBJ_MUTANT,
    NT_OBJ_SEMAPHORE,
    NT_OBJ_TIMER,
    NT_OBJ_KEYED_EVENT,
};

struct nt_wait_ctx;

struct nt_wait_block {
    struct list_head              entry; /* linked in header.waiters */
    struct nt_wait_ctx           *ctx;
    struct nt_dispatcher_header  *object;
    ULONG                         original_index;
};

#ifndef __KERNEL__
struct nt_wait_ctx {
    struct nt_thread *thread;
    ULONG             count;
    ULONG             wait_type; /* WaitAll / WaitAny */
    BOOLEAN           alertable;
    pthread_mutex_t   lock;
    pthread_cond_t    cond;
    bool              woken;
    NTSTATUS          status;
    ULONG             satisfied_index;
    bool              abandoned;
    _Atomic int       refcount;
    struct nt_wait_block blocks[MAXIMUM_WAIT_OBJECTS];
};

struct nt_dispatcher_header {
    enum nt_disp_type type;
    pthread_mutex_t     lock;    /* protects waiters + state */
    struct list_head    waiters; /* nt_wait_block.entry */
    LONG                signal_state; /* meaning per type */
    bool                manual_reset; /* event/timer */
    atomic_long         refcount;
};
#else
struct nt_wait_ctx {
    struct nt_thread *thread;
    ULONG             count;
    ULONG             wait_type;
    BOOLEAN           alertable;
    spinlock_t        lock;
    wait_queue_head_t wq;
    bool              woken;
    NTSTATUS          status;
    ULONG             satisfied_index;
    bool              abandoned;
    atomic_t          refcount;
    struct nt_wait_block blocks[MAXIMUM_WAIT_OBJECTS];
};

struct nt_dispatcher_header {
    enum nt_disp_type type;
    spinlock_t          lock;
    struct list_head    waiters;
    LONG                signal_state;
    bool                manual_reset;
    atomic_t            refcount;
    wait_queue_head_t   wq;
};
#endif

/* Concrete types - header must be first member */
struct nt_event {
    struct nt_dispatcher_header header;
};

struct nt_mutant {
    struct nt_dispatcher_header header;
    struct nt_thread           *owner;
    LONG                        recursion;
    bool                        abandoned;
    struct list_head            owned_entry; /* thread->owned_mutants */
};

struct nt_semaphore {
    struct nt_dispatcher_header header;
    LONG                        limit;
};

#ifndef __KERNEL__
struct nt_timer {
    struct nt_dispatcher_header header;
    pthread_mutex_t             timer_lock;
    pthread_cond_t              timer_cond; /* wakes the timer thread on cancel; signalled when it exits */
    pthread_t                   timer_thread;
    bool                        active;
    bool                        thread_running;
    bool                        cancel_requested;
    LARGE_INTEGER               due_time; /* copy */
    LONG                        period_ms; /* 0 = one-shot */
    void                      (*apc_routine)(void *);
    void                       *apc_ctx;
    struct nt_thread           *apc_thread;
    int64_t                     expiry_100ns; /* absolute NT time of the armed expiry, 0 if none */
};
#else
struct nt_timer {
    struct nt_dispatcher_header header;
    spinlock_t                  timer_lock;
    struct timer_list           timer;
    bool                        active;
    bool                        cancel_requested;
    LARGE_INTEGER               due_time;
    LONG                        period_ms;
    void                      (*apc_routine)(void *);
    void                       *apc_ctx;
    struct nt_thread           *apc_thread;
    int64_t                     expiry_100ns; /* absolute NT time of the armed expiry, 0 if none */
};
#endif

struct nt_keyed_event {
    struct nt_dispatcher_header header; /* not really used; global table holds waiters */
};

/* Address wait entry */
struct nt_addr_wait {
    struct list_head entry;
    struct nt_wait_ctx *ctx;
    void *addr;
};

/* ------------------------------------------------------------------ */
/* Creation / destruction                                               */
/* ------------------------------------------------------------------ */
NTSTATUS nt_create_event(struct nt_event **out, int type, BOOLEAN initial_state);
NTSTATUS nt_create_mutant(struct nt_mutant **out, BOOLEAN initial_owner);
NTSTATUS nt_create_semaphore(struct nt_semaphore **out, LONG initial_count, LONG maximum_count);
NTSTATUS nt_create_timer(struct nt_timer **out, int type);
NTSTATUS nt_create_keyed_event(struct nt_keyed_event **out);

void nt_put_event(struct nt_event *e);
void nt_put_mutant(struct nt_mutant *m);
void nt_put_semaphore(struct nt_semaphore *s);
void nt_put_timer(struct nt_timer *t);
void nt_put_keyed_event(struct nt_keyed_event *k);

/* internal header ref helpers */
void nt_header_addref(struct nt_dispatcher_header *h);
void nt_header_release(struct nt_dispatcher_header *h);

/* ------------------------------------------------------------------ */
/* Operations                                                           */
/* ------------------------------------------------------------------ */
NTSTATUS nt_set_event(struct nt_event *event, LONG *prev_state);
NTSTATUS nt_reset_event(struct nt_event *event, LONG *prev_state);
NTSTATUS nt_pulse_event(struct nt_event *event, LONG *prev_state);

NTSTATUS nt_release_mutant(struct nt_mutant *mutant, LONG *prev_count);

NTSTATUS nt_release_semaphore(struct nt_semaphore *sem, LONG release_count, LONG *prev_count);

NTSTATUS nt_set_timer(struct nt_timer *timer, const LARGE_INTEGER *due_time,
                      LONG period_ms, void (*apc_routine)(void *), void *apc_ctx);
NTSTATUS nt_cancel_timer(struct nt_timer *timer, BOOLEAN *current_state);

NTSTATUS nt_wait_for_keyed_event(struct nt_keyed_event *kev, void *key,
                                 BOOLEAN alertable, LARGE_INTEGER *timeout);
NTSTATUS nt_release_keyed_event(struct nt_keyed_event *kev, void *key,
                                BOOLEAN alertable, LARGE_INTEGER *timeout);

/* Wait engine */
NTSTATUS nt_wait_for_single_object(struct nt_dispatcher_header *obj,
                                   BOOLEAN alertable,
                                   LARGE_INTEGER *timeout);
NTSTATUS nt_wait_for_multiple_objects(ULONG count,
                                       struct nt_dispatcher_header **objects,
                                       ULONG wait_type,
                                       BOOLEAN alertable,
                                       LARGE_INTEGER *timeout);

/* Address waits */
NTSTATUS nt_wait_on_address(void *addr, void *cmp, size_t size, LARGE_INTEGER *timeout);
void nt_wake_by_address_single(void *addr);
void nt_wake_by_address_all(void *addr);

/* Thread exit hook - abandonment */
void nt_abandon_owned_mutants(struct nt_thread *thread);

/* Helpers for tests */
bool nt_dispatcher_is_signaled(struct nt_dispatcher_header *h, struct nt_thread *for_thread);

#endif /* NT_SYNC_H */
