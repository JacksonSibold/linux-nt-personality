/*
 * sync.c - NT dispatcher objects and wait engine (userspace shim + kernel)
 *
 * LOCK ORDERING INVARIANT (WaitAll atomicity proof):
 *   Every path that acquires multiple dispatcher object locks ΓÇö WaitAll
 *   acquisition, undo on partial-failure, waiter block insertion,
 *   waiter block removal, and the zero-timeout poll abandoned scan ΓÇö
 *   MUST acquire locks in strictly increasing address order (sorted by
 *   pointer value via sort_by_addr / kernel_sort_by_addr) and release
 *   them in reverse order. Original handle order MUST NOT be used for
 *   any multi-lock acquisition. This discipline guarantees no circular
 *   wait when two threads wait on overlapping sets in opposite order
 *   (the sync_atomic test), and is the architectural justification for
 *   kernel-resident NT wait. Future agents must preserve this ordering
 *   for every new multi-lock path; use the sorted[] array for WaitAll
 *   and never lock objects[i] in original order when holding more than
 *   one lock at a time.
 */
#include "sync.h"
#include "../../ntdll/ntstatus.h"
#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/uaccess.h>

#else
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#endif

#ifndef __KERNEL__

/* TLS */
__thread struct nt_thread *nt_current_thread_ptr = NULL;

/* globals for keyed events and address waits */
static pthread_mutex_t g_keyed_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list_head g_keyed_waiters;
static pthread_mutex_t g_addr_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list_head g_addr_waiters;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void g_init(void) {
    INIT_LIST_HEAD(&g_keyed_waiters);
    INIT_LIST_HEAD(&g_addr_waiters);
}
static inline void ensure_global_init(void) {
    pthread_once(&g_init_once, g_init);
}

/* helper: current time */
static void now_realtime(struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
}
static int64_t timespec_to_100ns(const struct timespec *ts) {
    return (int64_t)ts->tv_sec * 10000000LL + ts->tv_nsec / 100;
}

/* timeout -> deadline conversion */
static bool timeout_to_deadline(LARGE_INTEGER *to, struct timespec *out, bool *is_zero_poll) {
    if (is_zero_poll) *is_zero_poll = false;
    if (!to) return false; /* infinite */
    if (to->QuadPart == 0) {
        if (is_zero_poll) *is_zero_poll = true;
        return false;
    }
    int64_t rel100 = 0;
    struct timespec now;
    if (to->QuadPart < 0) {
        rel100 = -to->QuadPart;
    } else {
        /* absolute */
        now_realtime(&now);
        int64_t now100 = timespec_to_100ns(&now) + 116444736000000000LL;
        int64_t due100 = to->QuadPart;
        if (due100 <= now100) {
            if (is_zero_poll) *is_zero_poll = true; /* already expired => poll */
            return false;
        }
        rel100 = due100 - now100;
    }
    now_realtime(&now);
    int64_t sec = rel100 / 10000000LL;
    int64_t rem100 = rel100 % 10000000LL;
    out->tv_sec = now.tv_sec + sec;
    out->tv_nsec = now.tv_nsec + (long)(rem100 * 100);
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec += out->tv_nsec / 1000000000L;
        out->tv_nsec %= 1000000000L;
    }
    return true;
}
static bool deadline_expired(const struct timespec *dl) {
    if (!dl) return false;
    struct timespec now;
    now_realtime(&now);
    if (now.tv_sec > dl->tv_sec) return true;
    if (now.tv_sec == dl->tv_sec && now.tv_nsec >= dl->tv_nsec) return true;
    return false;
}

/* header helpers */
static void header_init(struct nt_dispatcher_header *h, enum nt_disp_type t, LONG init_state, bool manual) {
    h->type = t;
    pthread_mutex_init(&h->lock, NULL);
    INIT_LIST_HEAD(&h->waiters);
    h->signal_state = init_state;
    h->manual_reset = manual;
    atomic_init(&h->refcount, 1);
}
void nt_header_addref(struct nt_dispatcher_header *h) { atomic_fetch_add(&h->refcount, 1); }
void nt_header_release(struct nt_dispatcher_header *h) {
    if (atomic_fetch_sub(&h->refcount, 1) == 1) {
        pthread_mutex_destroy(&h->lock);
        kfree(h);
    }
}

/* thread helpers */
static atomic_ulong g_tid_seq = 1;
NTSTATUS nt_thread_create(struct nt_thread **out) {
    if (!out) return STATUS_INVALID_PARAMETER;
    struct nt_thread *t = (struct nt_thread*)kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t) return STATUS_NO_MEMORY;
    t->tid = atomic_fetch_add(&g_tid_seq, 1);
    pthread_mutex_init(&t->lock, NULL);
    INIT_LIST_HEAD(&t->owned_mutants);
    t->alerted = false;
    t->alert_pending = false;
    *out = t;
    return STATUS_SUCCESS;
}
void nt_thread_destroy(struct nt_thread *t) {
    if (!t) return;
    nt_abandon_owned_mutants(t);
    pthread_mutex_destroy(&t->lock);
    kfree(t);
}
void nt_thread_alert(struct nt_thread *t) {
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    t->alerted = true;
    pthread_mutex_unlock(&t->lock);
}
void nt_thread_queue_apc(struct nt_thread *t) {
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    t->alert_pending = true;
    pthread_mutex_unlock(&t->lock);
}
void nt_thread_clear_alert(struct nt_thread *t) {
    if (!t) return;
    pthread_mutex_lock(&t->lock);
    t->alerted = false;
    t->alert_pending = false;
    pthread_mutex_unlock(&t->lock);
}

/* --------------------------------------------------------------- */
/* Creation */
NTSTATUS nt_create_event(struct nt_event **out, int type, BOOLEAN initial_state) {
    if (!out) return STATUS_INVALID_PARAMETER;
    if (type != NotificationEvent && type != SynchronizationEvent) return STATUS_INVALID_PARAMETER;
    struct nt_event *e = (struct nt_event*)kzalloc(sizeof(*e), GFP_KERNEL);
    if (!e) return STATUS_NO_MEMORY;
    header_init(&e->header, NT_OBJ_EVENT, initial_state ? 1 : 0, type == NotificationEvent);
    *out = e;
    ensure_global_init();
    return STATUS_SUCCESS;
}
NTSTATUS nt_create_mutant(struct nt_mutant **out, BOOLEAN initial_owner) {
    if (!out) return STATUS_INVALID_PARAMETER;
    struct nt_mutant *m = (struct nt_mutant*)kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m) return STATUS_NO_MEMORY;
    header_init(&m->header, NT_OBJ_MUTANT, initial_owner ? 0 : 1, false);
    m->owner = NULL;
    m->recursion = 0;
    m->abandoned = false;
    INIT_LIST_HEAD(&m->owned_entry);
    if (initial_owner) {
        struct nt_thread *cur = nt_current_thread();
        if (!cur) { free(m); return STATUS_INVALID_PARAMETER; }
        m->owner = cur;
        m->recursion = 1;
        m->header.signal_state = 0;
        pthread_mutex_lock(&cur->lock);
        list_add_tail(&m->owned_entry, &cur->owned_mutants);
        pthread_mutex_unlock(&cur->lock);
    }
    *out = m;
    ensure_global_init();
    return STATUS_SUCCESS;
}
NTSTATUS nt_create_semaphore(struct nt_semaphore **out, LONG initial_count, LONG maximum_count) {
    if (!out) return STATUS_INVALID_PARAMETER;
    if (maximum_count <= 0 || initial_count < 0 || initial_count > maximum_count) return STATUS_INVALID_PARAMETER;
    struct nt_semaphore *s = (struct nt_semaphore*)kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s) return STATUS_NO_MEMORY;
    header_init(&s->header, NT_OBJ_SEMAPHORE, initial_count, false);
    s->limit = maximum_count;
    *out = s;
    ensure_global_init();
    return STATUS_SUCCESS;
}
NTSTATUS nt_create_timer(struct nt_timer **out, int type) {
    if (!out) return STATUS_INVALID_PARAMETER;
    if (type != NotificationTimer && type != SynchronizationTimer) return STATUS_INVALID_PARAMETER;
    struct nt_timer *t = (struct nt_timer*)kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t) return STATUS_NO_MEMORY;
    header_init(&t->header, NT_OBJ_TIMER, 0, type == NotificationTimer);
    pthread_mutex_init(&t->timer_lock, NULL);
    t->active = false;
    t->thread_running = false;
    t->period_ms = 0;
    *out = t;
    ensure_global_init();
    return STATUS_SUCCESS;
}
NTSTATUS nt_create_keyed_event(struct nt_keyed_event **out) {
    if (!out) return STATUS_INVALID_PARAMETER;
    struct nt_keyed_event *k = (struct nt_keyed_event*)kzalloc(sizeof(*k), GFP_KERNEL);
    if (!k) return STATUS_NO_MEMORY;
    header_init(&k->header, NT_OBJ_KEYED_EVENT, 0, false);
    *out = k;
    ensure_global_init();
    return STATUS_SUCCESS;
}

void nt_put_event(struct nt_event *e) { if (!e) return; nt_header_release(&e->header); }
void nt_put_mutant(struct nt_mutant *m) {
    if (!m) return;
    /* if still owned, remove from owner's list under owner's lock? best effort */
    if (m->owner) {
        /* caller should have released; but if leaked we detach */
        // Don't touch owner's list without lock - skip
    }
    nt_header_release(&m->header);
}
void nt_put_semaphore(struct nt_semaphore *s) { if (!s) return; nt_header_release(&s->header); }
void nt_put_timer(struct nt_timer *t) {
    if (!t) return;
    BOOLEAN dummy;
    nt_cancel_timer(t, &dummy);
    pthread_mutex_destroy(&t->timer_lock);
    nt_header_release(&t->header);
}
void nt_put_keyed_event(struct nt_keyed_event *k) { if (!k) return; nt_header_release(&k->header); }

/* --------------------------------------------------------------- */
/* Helpers: is_signaled, try consume, undo */
bool nt_dispatcher_is_signaled(struct nt_dispatcher_header *h, struct nt_thread *thr) {
    switch (h->type) {
        case NT_OBJ_EVENT: return h->signal_state != 0;
        case NT_OBJ_SEMAPHORE: return h->signal_state > 0;
        case NT_OBJ_MUTANT: {
            struct nt_mutant *m = (struct nt_mutant*)h;
            if (m->abandoned) return true;
            if (m->owner == NULL) return true;
            if (m->owner == thr) return true;
            return false;
        }
        case NT_OBJ_TIMER: return h->signal_state != 0;
        case NT_OBJ_KEYED_EVENT: return false;
        default: return false;
    }
}

/* forward for wake */
static void wake_ctx(struct nt_wait_ctx *ctx) {
    pthread_mutex_lock(&ctx->lock);
    ctx->woken = true;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}
static void wake_waiters_for_header(struct nt_dispatcher_header *h) {
    /* snapshot with refcount bump to keep heap ctx alive */
    struct nt_wait_ctx *to_wake[64];
    int n = 0;
    pthread_mutex_lock(&h->lock);
    struct list_head *pos;
    for (pos = h->waiters.next; pos != &h->waiters && n < 64; pos = pos->next) {
        struct nt_wait_block *blk = list_entry(pos, struct nt_wait_block, entry);
        struct nt_wait_ctx *ctx = blk->ctx;
        atomic_fetch_add(&ctx->refcount, 1);
        to_wake[n++] = ctx;
    }
    pthread_mutex_unlock(&h->lock);
    for (int i=0;i<n;i++) {
        wake_ctx(to_wake[i]);
        if (atomic_fetch_sub(&to_wake[i]->refcount, 1) == 1) {
            pthread_mutex_destroy(&to_wake[i]->lock);
            pthread_cond_destroy(&to_wake[i]->cond);
            kfree(to_wake[i]);
        }
    }
}

/* consume helpers - caller holds h->lock */
static bool consume_object_locked(struct nt_dispatcher_header *h, struct nt_thread *thr, bool *was_abandoned) {
    if (was_abandoned) *was_abandoned = false;
    switch (h->type) {
        case NT_OBJ_EVENT: {
            if (h->signal_state == 0) return false;
            if (!h->manual_reset) h->signal_state = 0; /* auto */
            return true;
        }
        case NT_OBJ_SEMAPHORE: {
            if (h->signal_state <= 0) return false;
            h->signal_state--;
            return true;
        }
        case NT_OBJ_MUTANT: {
            struct nt_mutant *m = (struct nt_mutant*)h;
            if (m->abandoned) {
                if (was_abandoned) *was_abandoned = true;
                m->abandoned = false;
                m->owner = thr;
                m->recursion = 1;
                h->signal_state = 0;
                /* add to owned */
                pthread_mutex_lock(&thr->lock);
                list_add_tail(&m->owned_entry, &thr->owned_mutants);
                pthread_mutex_unlock(&thr->lock);
                return true;
            }
            if (m->owner == NULL) {
                m->owner = thr;
                m->recursion = 1;
                h->signal_state = 0;
                pthread_mutex_lock(&thr->lock);
                list_add_tail(&m->owned_entry, &thr->owned_mutants);
                pthread_mutex_unlock(&thr->lock);
                return true;
            }
            if (m->owner == thr) {
                m->recursion++;
                return true;
            }
            return false;
        }
        case NT_OBJ_TIMER: {
            if (h->signal_state == 0) return false;
            if (!h->manual_reset) h->signal_state = 0;
            return true;
        }
        default: return false;
    }
}
static void undo_consume_locked(struct nt_dispatcher_header *h, struct nt_thread *thr) {
    switch (h->type) {
        case NT_OBJ_EVENT:
            if (!h->manual_reset) h->signal_state = 1;
            break;
        case NT_OBJ_SEMAPHORE:
            h->signal_state++;
            break;
        case NT_OBJ_MUTANT: {
            struct nt_mutant *m = (struct nt_mutant*)h;
            if (m->owner != thr) break; /* shouldn't happen */
            if (m->recursion > 1) {
                m->recursion--;
            } else {
                /* was 1 -> release */
                pthread_mutex_lock(&thr->lock);
                // remove from owned if present
                if (m->owned_entry.next && m->owned_entry.prev) list_del(&m->owned_entry);
                pthread_mutex_unlock(&thr->lock);
                m->owner = NULL;
                m->recursion = 0;
                h->signal_state = 1;
                INIT_LIST_HEAD(&m->owned_entry);
            }
            break;
        }
        case NT_OBJ_TIMER:
            if (!h->manual_reset) h->signal_state = 1;
            break;
        default: break;
    }
}

/* sorting for WaitAll */
static void sort_by_addr(ULONG count, struct nt_dispatcher_header **objs, ULONG *orig_idx, struct nt_dispatcher_header **sorted, ULONG *sorted_orig) {
    for (ULONG i=0;i<count;i++) { sorted[i]=objs[i]; sorted_orig[i]=orig_idx?orig_idx[i]:i; }
    for (ULONG i=0;i<count;i++) for (ULONG j=i+1;j<count;j++) if (sorted[j] < sorted[i]) {
        struct nt_dispatcher_header *tmp = sorted[i]; sorted[i]=sorted[j]; sorted[j]=tmp;
        ULONG ti = sorted_orig[i]; sorted_orig[i]=sorted_orig[j]; sorted_orig[j]=ti;
    }
}

/* try acquire WaitAny : caller holds no locks, will lock per object */
static bool try_acquire_wait_any(ULONG count, struct nt_dispatcher_header **objs, struct nt_thread *thr, ULONG *out_idx, bool *out_abandoned) {
    for (ULONG i=0;i<count;i++) {
        struct nt_dispatcher_header *h = objs[i];
        bool aband=false;
        pthread_mutex_lock(&h->lock);
        bool sig = nt_dispatcher_is_signaled(h, thr);
        bool ok = false;
        if (sig) ok = consume_object_locked(h, thr, &aband);
        pthread_mutex_unlock(&h->lock);
        if (ok) {
            *out_idx = i;
            *out_abandoned = aband;
            return true;
        }
    }
    return false;
}
/* try acquire WaitAll atomic with sorted locks held
 * INVARIANT: sorted[] is ascending address order; all locks taken ascending, released reverse.
 */
static bool try_acquire_wait_all_sorted(ULONG count, struct nt_dispatcher_header **sorted, ULONG *sorted_orig __attribute__((unused)), struct nt_thread *thr, bool *out_abandoned) {
    bool abandoned=false;
    bool was_aband[MAXIMUM_WAIT_OBJECTS] = {0};
    /* lock all in sorted order (ascending address) */
    for (ULONG i=0;i<count;i++) pthread_mutex_lock(&sorted[i]->lock);
    /* check signaled */
    for (ULONG i=0;i<count;i++) {
        if (!nt_dispatcher_is_signaled(sorted[i], thr)) {
            for (LONG j=(LONG)count-1;j>=0;j--) pthread_mutex_unlock(&sorted[j]->lock);
            return false;
        }
    }
    /* consume all - must be atomic, undo on failure in reverse order */
    for (ULONG i=0;i<count;i++) {
        bool a=false;
        bool ok = consume_object_locked(sorted[i], thr, &a);
        was_aband[i]=a;
        if (!ok) {
            /* undo already-consumed prefix in reverse order, restoring prior state */
            for (LONG k=(LONG)i-1;k>=0;k--) {
                undo_consume_locked(sorted[k], thr);
                if (was_aband[k] && sorted[k]->type == NT_OBJ_MUTANT) {
                    struct nt_mutant *m = (struct nt_mutant*)sorted[k];
                    m->abandoned = true; /* restore abandoned which undo cleared */
                    /* signal_state already 1 from undo */
                }
            }
            for (LONG j=(LONG)count-1;j>=0;j--) pthread_mutex_unlock(&sorted[j]->lock);
            return false;
        }
        if (a) abandoned = true;
    }
    for (LONG i=(LONG)count-1;i>=0;i--) pthread_mutex_unlock(&sorted[i]->lock);
    if (out_abandoned) *out_abandoned = abandoned;
    return true;
}

/* insertion / removal of wait blocks */
static void insert_blocks(ULONG count, struct nt_dispatcher_header **objs, struct nt_wait_ctx *ctx) {
    for (ULONG i=0;i<count;i++) {
        struct nt_dispatcher_header *h = objs[i];
        struct nt_wait_block *blk = &ctx->blocks[i];
        blk->ctx = ctx;
        blk->object = h;
        blk->original_index = i;
        pthread_mutex_lock(&h->lock);
        list_add_tail(&blk->entry, &h->waiters);
        pthread_mutex_unlock(&h->lock);
    }
}
static void insert_blocks_sorted(ULONG count, struct nt_dispatcher_header **sorted, struct nt_wait_ctx *ctx, ULONG *sorted_orig __attribute__((unused)), struct nt_dispatcher_header **orig_objs) {
    /* INVARIANT: lock all sorted (ascending address), release reverse */
    for (ULONG i=0;i<count;i++) pthread_mutex_lock(&sorted[i]->lock);
    for (ULONG i=0;i<count;i++) {
        struct nt_dispatcher_header *h = sorted[i];
        /* find which ctx block corresponds to this h (by matching pointer) */
        for (ULONG k=0;k<count;k++) if (orig_objs[k]==h) {
            struct nt_wait_block *blk = &ctx->blocks[k];
            blk->ctx = ctx;
            blk->object = h;
            blk->original_index = k;
            list_add_tail(&blk->entry, &h->waiters);
            break;
        }
    }
    for (LONG i=(LONG)count-1;i>=0;i--) pthread_mutex_unlock(&sorted[i]->lock);
}
static void remove_blocks(ULONG count, struct nt_dispatcher_header **objs, struct nt_wait_ctx *ctx) {
    for (ULONG i=0;i<count;i++) {
        struct nt_dispatcher_header *h = objs[i];
        struct nt_wait_block *blk = &ctx->blocks[i];
        pthread_mutex_lock(&h->lock);
        if (blk->entry.next && blk->entry.prev) {
            // check if still linked (next != NULL after init? kshim list_del sets NULL)
            // we can test by seeing if entry is in list: brute check if list contains blk
            // simpler: if entry.next != NULL, try remove; but after first insert next is set, after remove set NULL.
            if (blk->entry.next != NULL) {
                // ensure it is still linked by scanning? just del if not empty
                // Use list_del; if not linked its next is NULL we skip
                // To avoid double del, we check if not NULL
                list_del(&blk->entry);
                blk->entry.next = NULL;
                blk->entry.prev = NULL;
            }
        }
        pthread_mutex_unlock(&h->lock);
    }
}
static void remove_blocks_sorted(ULONG count, struct nt_dispatcher_header **sorted, struct nt_wait_ctx *ctx, struct nt_dispatcher_header **orig_objs) {
    /* INVARIANT: lock ascending, unlock reverse */
    for (ULONG i=0;i<count;i++) pthread_mutex_lock(&sorted[i]->lock);
    for (ULONG i=0;i<count;i++) {
        struct nt_dispatcher_header *h = sorted[i];
        for (ULONG k=0;k<count;k++) if (orig_objs[k]==h) {
            struct nt_wait_block *blk = &ctx->blocks[k];
            if (blk->entry.next) {
                list_del(&blk->entry);
                blk->entry.next=NULL; blk->entry.prev=NULL;
            }
            break;
        }
    }
    for (LONG i=(LONG)count-1;i>=0;i--) pthread_mutex_unlock(&sorted[i]->lock);
}

/* --------------------------------------------------------------- */
/* Wait engine core */
NTSTATUS nt_wait_for_multiple_objects(ULONG count,
                                      struct nt_dispatcher_header **objects,
                                      ULONG wait_type,
                                      BOOLEAN alertable,
                                      LARGE_INTEGER *timeout) {
    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS) return STATUS_INVALID_PARAMETER;
    if (!objects) return STATUS_INVALID_PARAMETER;
    for (ULONG i=0;i<count;i++) if (!objects[i]) return STATUS_INVALID_PARAMETER;
    if (wait_type != WaitAll && wait_type != WaitAny) return STATUS_INVALID_PARAMETER;

    ensure_global_init();
    struct nt_thread *thr = nt_current_thread();
    if (!thr) return STATUS_INVALID_PARAMETER;

    struct timespec deadline_ts;
    bool has_deadline = timeout_to_deadline(timeout, &deadline_ts, NULL);
    bool is_zero = false;
    if (timeout && timeout->QuadPart==0) is_zero=true;
    /* if zero poll, don't block */
    bool zero_poll = is_zero;
    /* also check has_deadline zero case via helper */
    bool poll_is_zero=false;
    timeout_to_deadline(timeout, &deadline_ts, &poll_is_zero);
    if (poll_is_zero) zero_poll=true;
    /* check has_deadline false + poll => infinite? need distinguish */
    /* if timeout == NULL -> infinite */
    /* reuse has_deadline flag; if timeout==0 poll_is_zero true */

    /* fast check for alert/APC before any wait */
    if (alertable) {
        pthread_mutex_lock(&thr->lock);
        bool al = thr->alerted;
        bool apc = thr->alert_pending;
        pthread_mutex_unlock(&thr->lock);
        if (apc) { pthread_mutex_lock(&thr->lock); thr->alert_pending=false; pthread_mutex_unlock(&thr->lock); return STATUS_USER_APC; }
        if (al) { /* alerted waits return STATUS_ALERTED, but we clear? Windows Alert stays? We'll clear */
            pthread_mutex_lock(&thr->lock); thr->alerted=false; pthread_mutex_unlock(&thr->lock);
            return STATUS_ALERTED;
        }
    }

    /* prepare sorted for WaitAll */
    struct nt_dispatcher_header *sorted[MAXIMUM_WAIT_OBJECTS];
    ULONG sorted_orig[MAXIMUM_WAIT_OBJECTS];
    ULONG orig_idx[MAXIMUM_WAIT_OBJECTS];
    for (ULONG i=0;i<count;i++) orig_idx[i]=i;
    if (wait_type == WaitAll) sort_by_addr(count, objects, orig_idx, sorted, sorted_orig);

    struct nt_wait_ctx *ctx = (struct nt_wait_ctx*)kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx) return STATUS_NO_MEMORY;
    ctx->thread = thr;
    ctx->count = count;
    ctx->wait_type = wait_type;
    ctx->alertable = alertable;
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->woken = false;
    atomic_init(&ctx->refcount, 1);
    for (ULONG i=0;i<count;i++) { ctx->blocks[i].entry.next=NULL; ctx->blocks[i].entry.prev=NULL; }

    NTSTATUS final_status = STATUS_TIMEOUT;
    bool abandoned = false;
    ULONG satisfied_idx = 0;

    while (1) {
        /* try acquire */
        bool got=false;
        abandoned=false;
        if (wait_type == WaitAny) {
            got = try_acquire_wait_any(count, objects, thr, &satisfied_idx, &abandoned);
        } else {
            got = try_acquire_wait_all_sorted(count, sorted, sorted_orig, thr, &abandoned);
            if (got) {
                /* for WaitAll, satisfied_idx not meaningful; set 0 */
                satisfied_idx = 0;
            }
        }
        if (got) {
            if (abandoned) final_status = (NTSTATUS)(STATUS_ABANDONED_WAIT_0 + (wait_type==WaitAny? satisfied_idx:0));
            else final_status = (NTSTATUS)(STATUS_WAIT_0 + satisfied_idx);
            break;
        }
        if (zero_poll) {
            /* INVARIANT: WaitAll abandoned scan and all_sig check use sorted order */
            bool found_abandoned = false;
            ULONG aband_idx = 0;
            if (wait_type == WaitAll) {
                for (ULONG i = 0; i < count; i++) {
                    struct nt_dispatcher_header *h = sorted[i];
                    if (h->type == NT_OBJ_MUTANT) {
                        struct nt_mutant *m = (struct nt_mutant *)h;
                        pthread_mutex_lock(&h->lock);
                        bool ab = m->abandoned;
                        pthread_mutex_unlock(&h->lock);
                        if (ab) { found_abandoned = true; }
                    }
                }
            } else {
                for (ULONG i = 0; i < count; i++) {
                    struct nt_dispatcher_header *h = objects[i];
                    if (h->type == NT_OBJ_MUTANT) {
                        struct nt_mutant *m = (struct nt_mutant *)h;
                        pthread_mutex_lock(&h->lock);
                        bool ab = m->abandoned;
                        pthread_mutex_unlock(&h->lock);
                        if (ab) { found_abandoned = true; aband_idx = i; break; }
                    }
                }
            }
            if (found_abandoned) {
                if (wait_type == WaitAll) {
                    bool all_sig = true;
                    for (ULONG i=0;i<count;i++) pthread_mutex_lock(&sorted[i]->lock);
                    for (ULONG i=0;i<count;i++) if (!nt_dispatcher_is_signaled(sorted[i], thr)) { all_sig = false; break; }
                    for (LONG i=(LONG)count-1;i>=0;i--) pthread_mutex_unlock(&sorted[i]->lock);
                    if (all_sig) { final_status = (NTSTATUS)(STATUS_ABANDONED_WAIT_0); break; }
                    else { final_status = STATUS_TIMEOUT; break; }
                } else {
                    final_status = (NTSTATUS)(STATUS_ABANDONED_WAIT_0 + aband_idx);
                    break;
                }
            }
            final_status = STATUS_TIMEOUT; break;
        }
        if (has_deadline && deadline_expired(&deadline_ts)) { final_status = STATUS_TIMEOUT; break; }

        /* check alert/APC before blocking */
        if (alertable) {
            pthread_mutex_lock(&thr->lock);
            bool apc = thr->alert_pending;
            bool al = thr->alerted;
            if (apc) { thr->alert_pending=false; pthread_mutex_unlock(&thr->lock); final_status = STATUS_USER_APC; break; }
            if (al) { thr->alerted=false; pthread_mutex_unlock(&thr->lock); final_status = STATUS_ALERTED; break; }
            pthread_mutex_unlock(&thr->lock);
        }

        /* insert waiters */
        if (wait_type == WaitAny) insert_blocks(count, objects, ctx);
        else insert_blocks_sorted(count, sorted, ctx, sorted_orig, objects);

        /* block */
        pthread_mutex_lock(&ctx->lock);
        ctx->woken = false;
        int rc=0;
        if (has_deadline) {
            rc = pthread_cond_timedwait(&ctx->cond, &ctx->lock, &deadline_ts);
        } else {
            rc = pthread_cond_wait(&ctx->cond, &ctx->lock);
        }
        bool w = ctx->woken;
        pthread_mutex_unlock(&ctx->lock);
        (void)rc; (void)w;

        /* remove waiters (always) */
        if (wait_type == WaitAny) remove_blocks(count, objects, ctx);
        else remove_blocks_sorted(count, sorted, ctx, objects);

        /* if alertable check again after wake */
        if (alertable) {
            pthread_mutex_lock(&thr->lock);
            bool apc = thr->alert_pending;
            bool al = thr->alerted;
            if (apc) { thr->alert_pending=false; pthread_mutex_unlock(&thr->lock); final_status = STATUS_USER_APC; break; }
            if (al) { thr->alerted=false; pthread_mutex_unlock(&thr->lock); final_status = STATUS_ALERTED; break; }
            pthread_mutex_unlock(&thr->lock);
        }
        if (rc == ETIMEDOUT && has_deadline && deadline_expired(&deadline_ts)) {
            /* loop will try once more then timeout; but to avoid extra iteration, check try again next loop top will timeout.
               Continue loop to re-try acquire once before declaring timeout - handles race where signal arrived at deadline */
            continue;
        }
        /* loop retries try_acquire */
        if (has_deadline && deadline_expired(&deadline_ts)) {
            /* will be caught at top as timeout if not acquired */
        }
    }

    if (atomic_fetch_sub(&ctx->refcount, 1) == 1) {
        pthread_mutex_destroy(&ctx->lock);
        pthread_cond_destroy(&ctx->cond);
        kfree(ctx);
    }
    return final_status;
}


NTSTATUS nt_wait_for_single_object(struct nt_dispatcher_header *obj, BOOLEAN alertable, LARGE_INTEGER *timeout) {
    return nt_wait_for_multiple_objects(1, &obj, WaitAny, alertable, timeout);
}

/* --------------------------------------------------------------- */
/* Operations */
NTSTATUS nt_set_event(struct nt_event *event, LONG *prev) {
    if (!event) return STATUS_INVALID_PARAMETER;
    struct nt_dispatcher_header *h=&event->header;
    pthread_mutex_lock(&h->lock);
    if (prev) *prev = h->signal_state;
    h->signal_state = 1;
    pthread_mutex_unlock(&h->lock);
    wake_waiters_for_header(h);
    return STATUS_SUCCESS;
}
NTSTATUS nt_reset_event(struct nt_event *event, LONG *prev) {
    if (!event) return STATUS_INVALID_PARAMETER;
    struct nt_dispatcher_header *h=&event->header;
    pthread_mutex_lock(&h->lock);
    if (prev) *prev = h->signal_state;
    h->signal_state = 0;
    pthread_mutex_unlock(&h->lock);
    return STATUS_SUCCESS;
}
NTSTATUS nt_pulse_event(struct nt_event *event, LONG *prev) {
    if (!event) return STATUS_INVALID_PARAMETER;
    struct nt_dispatcher_header *h=&event->header;
    pthread_mutex_lock(&h->lock);
    if (prev) *prev = h->signal_state;
    struct nt_wait_ctx *to_wake[64]; int n=0;
    struct list_head *pos;
    for (pos = h->waiters.next; pos != &h->waiters && n<64; pos = pos->next) {
        struct nt_wait_block *blk = list_entry(pos, struct nt_wait_block, entry);
        atomic_fetch_add(&blk->ctx->refcount, 1);
        to_wake[n++]=blk->ctx;
    }
    h->signal_state = 1;
    pthread_mutex_unlock(&h->lock);
    for (int i=0;i<n;i++) wake_ctx(to_wake[i]);
    for (int i=0;i<n;i++) {
        if (atomic_fetch_sub(&to_wake[i]->refcount, 1) == 1) {
            pthread_mutex_destroy(&to_wake[i]->lock);
            pthread_cond_destroy(&to_wake[i]->cond);
            kfree(to_wake[i]);
        }
    }
    usleep(1000);
    pthread_mutex_lock(&h->lock);
    h->signal_state = 0;
    pthread_mutex_unlock(&h->lock);
    return STATUS_SUCCESS;
}

NTSTATUS nt_release_mutant(struct nt_mutant *mutant, LONG *prev) {
    if (!mutant) return STATUS_INVALID_PARAMETER;
    struct nt_thread *thr = nt_current_thread();
    struct nt_dispatcher_header *h=&mutant->header;
    pthread_mutex_lock(&h->lock);
    if (mutant->owner != thr) { pthread_mutex_unlock(&h->lock); return STATUS_MUTANT_NOT_OWNED; }
    if (prev) *prev = mutant->recursion;
    mutant->recursion--;
    if (mutant->recursion > 0) {
        pthread_mutex_unlock(&h->lock);
        return STATUS_SUCCESS;
    }
    /* last release */
    pthread_mutex_lock(&thr->lock);
    if (mutant->owned_entry.next) list_del(&mutant->owned_entry);
    mutant->owned_entry.next=NULL; mutant->owned_entry.prev=NULL;
    pthread_mutex_unlock(&thr->lock);
    mutant->owner = NULL;
    h->signal_state = 1;
    mutant->abandoned = false;
    pthread_mutex_unlock(&h->lock);
    wake_waiters_for_header(h);
    return STATUS_SUCCESS;
}

NTSTATUS nt_release_semaphore(struct nt_semaphore *sem, LONG release_count, LONG *prev) {
    if (!sem || release_count <=0) return STATUS_INVALID_PARAMETER;
    struct nt_dispatcher_header *h=&sem->header;
    pthread_mutex_lock(&h->lock);
    if (h->signal_state + release_count > sem->limit || h->signal_state + release_count < 0) {
        pthread_mutex_unlock(&h->lock);
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
    if (prev) *prev = h->signal_state;
    h->signal_state += release_count;
    pthread_mutex_unlock(&h->lock);
    wake_waiters_for_header(h);
    return STATUS_SUCCESS;
}

/* timer thread */
struct timer_arg { struct nt_timer *t; };

static void *timer_thread_func(void *arg) {
    struct nt_timer *t = (struct nt_timer*)arg;
    // compute due relative
    LARGE_INTEGER due;
    pthread_mutex_lock(&t->timer_lock);
    due = t->due_time;
    pthread_mutex_unlock(&t->timer_lock);

    int64_t rel100;
    if (due.QuadPart < 0) rel100 = -due.QuadPart;
    else {
        struct timespec now;
        now_realtime(&now);
        int64_t now100 = timespec_to_100ns(&now) + 116444736000000000LL;
        int64_t due100 = due.QuadPart;
        if (due100 <= now100) rel100 = 0;
        else rel100 = due100 - now100;
    }
    long ms = (long)(rel100 / 10000LL);
    long ns_rem = (long)((rel100 % 10000LL)*100);
    struct timespec sleep_ts;
    sleep_ts.tv_sec = ms/1000;
    sleep_ts.tv_nsec = (ms%1000)*1000000L + ns_rem;
    if (sleep_ts.tv_nsec >= 1000000000L) { sleep_ts.tv_sec++; sleep_ts.tv_nsec-=1000000000L; }

    /* sleep with cancellation check using nanosleep loop + check cancel flag */
    struct timespec rem = sleep_ts;
    while (rem.tv_sec>0 || rem.tv_nsec>0) {
        pthread_mutex_lock(&t->timer_lock);
        bool cancel = t->cancel_requested;
        pthread_mutex_unlock(&t->timer_lock);
        if (cancel) break;
        struct timespec chunk = rem;
        if (chunk.tv_sec > 1) chunk.tv_sec = 1;
        int rc = nanosleep(&chunk, &rem);
        if (rc==0) {
            if (chunk.tv_sec==1) { /* need to compute remaining total? simplified: use loop with 1 sec chunks */
                // recalc remaining?
                // Instead just continue using rem leftover from nanosleep interrupted? But we sliced.
                // Simplify: if we did 1 sec chunk, rem now is sleep_ts - chunk? We approximated.
                // For simplicity, just sleep full then check.
                // To handle period correctly for small sleeps it's fine.
                // We'll break after full sleep if not periodic
            }
            break;
        } else {
            if (errno==EINTR) continue;
            else break;
        }
    }

    pthread_mutex_lock(&t->timer_lock);
    bool cancelled = t->cancel_requested;
    pthread_mutex_unlock(&t->timer_lock);
    if (cancelled) {
        pthread_mutex_lock(&t->timer_lock);
        t->active = false;
        t->thread_running = false;
        pthread_mutex_unlock(&t->timer_lock);
        return NULL;
    }

    /* fire */
    struct nt_dispatcher_header *h=&t->header;
    pthread_mutex_lock(&h->lock);
    h->signal_state = 1;
    pthread_mutex_unlock(&h->lock);
    wake_waiters_for_header(h);
    /* APC */
    if (t->apc_routine) {
        if (t->apc_thread) {
            nt_thread_queue_apc(t->apc_thread);
            // wake apc thread if it is in alertable wait - caller will handle via wake
        } else {
            t->apc_routine(t->apc_ctx);
        }
    }

    pthread_mutex_lock(&t->timer_lock);
    if (t->period_ms > 0 && !t->cancel_requested) {
        // periodic: re-arm by restarting thread? For periodic we loop.
        // To implement periodic, we will loop in this thread.
        LONG period_ms = t->period_ms;
        // re-read cancel
        if (!t->cancel_requested) {
            // schedule next: keep active, spawn next? Instead loop
            pthread_mutex_unlock(&t->timer_lock);
            // loop for periodic: sleep period then fire again
            while (1) {
                struct timespec p;
                p.tv_sec = period_ms/1000;
                p.tv_nsec = (period_ms%1000)*1000000L;
                nanosleep(&p, NULL);
                pthread_mutex_lock(&t->timer_lock);
                bool canc = t->cancel_requested;
                pthread_mutex_unlock(&t->timer_lock);
                if (canc) break;
                pthread_mutex_lock(&h->lock);
                h->signal_state = 1;
                pthread_mutex_unlock(&h->lock);
                wake_waiters_for_header(h);
                if (t->apc_routine) {
                    if (t->apc_thread) nt_thread_queue_apc(t->apc_thread);
                    else t->apc_routine(t->apc_ctx);
                }
                if (h->manual_reset) { /* notification stays signaled; but periodic notification also stays */
                } else {
                    // sync timer will be reset by waiter
                }
            }
            pthread_mutex_lock(&t->timer_lock);
            t->active = false;
            t->thread_running = false;
            pthread_mutex_unlock(&t->timer_lock);
            return NULL;
        } else {
            t->active = false;
            t->thread_running = false;
            pthread_mutex_unlock(&t->timer_lock);
            return NULL;
        }
    } else {
        t->active = false;
        t->thread_running = false;
        pthread_mutex_unlock(&t->timer_lock);
        return NULL;
    }
}

/* Absolute NT expiry (100 ns since 1601) for a due time; negative is relative. */
static int64_t timer_expiry_100ns(const LARGE_INTEGER *due) {
    if (due->QuadPart >= 0) return due->QuadPart;
    struct timespec now;
    now_realtime(&now);
    return timespec_to_100ns(&now) + 116444736000000000LL - due->QuadPart;
}
NTSTATUS nt_set_timer(struct nt_timer *timer, const LARGE_INTEGER *due_time, LONG period_ms, void (*apc_routine)(void*), void *apc_ctx) {
    if (!timer || !due_time) return STATUS_INVALID_PARAMETER;
    BOOLEAN dummy;
    nt_cancel_timer(timer, &dummy);
    pthread_mutex_lock(&timer->timer_lock);
    timer->due_time = *due_time;
    timer->expiry_100ns = timer_expiry_100ns(due_time);
    timer->period_ms = period_ms;
    timer->apc_routine = apc_routine;
    timer->apc_ctx = apc_ctx;
    timer->apc_thread = nt_current_thread();
    timer->cancel_requested = false;
    timer->active = true;
    timer->thread_running = true;
    pthread_mutex_unlock(&timer->timer_lock);
    /* reset signal */
    pthread_mutex_lock(&timer->header.lock);
    timer->header.signal_state = 0;
    pthread_mutex_unlock(&timer->header.lock);
    int rc = pthread_create(&timer->timer_thread, NULL, timer_thread_func, timer);
    if (rc!=0) {
        pthread_mutex_lock(&timer->timer_lock);
        timer->active=false; timer->thread_running=false;
        pthread_mutex_unlock(&timer->timer_lock);
        return STATUS_NO_MEMORY;
    }
    pthread_detach(timer->timer_thread);
    return STATUS_SUCCESS;
}
NTSTATUS nt_cancel_timer(struct nt_timer *timer, BOOLEAN *current_state) {
    if (!timer) return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&timer->timer_lock);
    if (current_state) *current_state = timer->header.signal_state ? 1 : 0;
    if (!timer->active) { pthread_mutex_unlock(&timer->timer_lock); return STATUS_SUCCESS; }
    timer->cancel_requested = true;
    pthread_mutex_unlock(&timer->timer_lock);
    /* give thread time to exit */
    usleep(2000);
    /* reset signal if needed? Windows cancel doesn't reset signal? Keep as is */
    return STATUS_SUCCESS;
}

/* keyed events */
struct keyed_waiter {
    struct list_head entry;
    struct nt_keyed_event *kev;
    void *key;
    struct nt_wait_ctx *ctx;
    struct nt_wait_block blk;
};

NTSTATUS nt_wait_for_keyed_event(struct nt_keyed_event *kev, void *key, BOOLEAN alertable, LARGE_INTEGER *timeout) {
    ensure_global_init();
    struct nt_thread *thr = nt_current_thread();
    if (!thr || !kev) return STATUS_INVALID_PARAMETER;
    if (alertable) {
        pthread_mutex_lock(&thr->lock);
        bool apc=thr->alert_pending; bool al=thr->alerted;
        pthread_mutex_unlock(&thr->lock);
        if (apc) { pthread_mutex_lock(&thr->lock); thr->alert_pending=false; pthread_mutex_unlock(&thr->lock); return STATUS_USER_APC; }
        if (al) { pthread_mutex_lock(&thr->lock); thr->alerted=false; pthread_mutex_unlock(&thr->lock); return STATUS_ALERTED; }
    }
    struct timespec dl; bool has_dl=false; bool zero=false;
    has_dl = timeout_to_deadline(timeout, &dl, &zero);
    if (zero) return STATUS_TIMEOUT;
    if (timeout && timeout->QuadPart==0) return STATUS_TIMEOUT;

    struct nt_wait_ctx ctx;
    memset(&ctx,0,sizeof(ctx));
    pthread_mutex_init(&ctx.lock,NULL); pthread_cond_init(&ctx.cond,NULL);
    ctx.woken=false;
    struct keyed_waiter w;
    w.kev = kev; w.key = key; w.ctx=&ctx;
    w.blk.ctx=&ctx; w.blk.object=&kev->header;
    INIT_LIST_HEAD(&w.entry);

    pthread_mutex_lock(&g_keyed_lock);
    list_add_tail(&w.entry, &g_keyed_waiters);
    pthread_mutex_unlock(&g_keyed_lock);

    pthread_mutex_lock(&ctx.lock);
    while (!ctx.woken) {
        if (has_dl) {
            int rc=pthread_cond_timedwait(&ctx.cond, &ctx.lock, &dl);
            if (rc==ETIMEDOUT) break;
        } else {
            pthread_cond_wait(&ctx.cond, &ctx.lock);
        }
        if (alertable) {
            pthread_mutex_lock(&thr->lock);
            bool apc=thr->alert_pending; bool al=thr->alerted;
            pthread_mutex_unlock(&thr->lock);
            if (apc||al) break;
        }
    }
    bool got = ctx.woken;
    pthread_mutex_unlock(&ctx.lock);

    pthread_mutex_lock(&g_keyed_lock);
    if (w.entry.next) list_del(&w.entry);
    pthread_mutex_unlock(&g_keyed_lock);
    pthread_mutex_destroy(&ctx.lock); pthread_cond_destroy(&ctx.cond);

    if (!got) {
        if (alertable) {
            pthread_mutex_lock(&thr->lock);
            bool apc=thr->alert_pending; bool al=thr->alerted;
            if (apc) { thr->alert_pending=false; pthread_mutex_unlock(&thr->lock); return STATUS_USER_APC; }
            if (al) { thr->alerted=false; pthread_mutex_unlock(&thr->lock); return STATUS_ALERTED; }
            pthread_mutex_unlock(&thr->lock);
        }
        return STATUS_TIMEOUT;
    }
    if (alertable) {
        pthread_mutex_lock(&thr->lock);
        bool apc=thr->alert_pending;
        if (apc) { thr->alert_pending=false; pthread_mutex_unlock(&thr->lock); return STATUS_USER_APC; }
        bool al=thr->alerted;
        if (al) { thr->alerted=false; pthread_mutex_unlock(&thr->lock); return STATUS_ALERTED; }
        pthread_mutex_unlock(&thr->lock);
    }
    return STATUS_SUCCESS;
}
NTSTATUS nt_release_keyed_event(struct nt_keyed_event *kev, void *key, BOOLEAN alertable, LARGE_INTEGER *timeout) {
    (void)alertable; (void)timeout;
    ensure_global_init();
    if (!kev) return STATUS_INVALID_PARAMETER;
    struct nt_wait_ctx *target=NULL;
    pthread_mutex_lock(&g_keyed_lock);
    struct list_head *pos;
    for (pos=g_keyed_waiters.next; pos!=&g_keyed_waiters; pos=pos->next) {
        struct keyed_waiter *w=list_entry(pos, struct keyed_waiter, entry);
        if (w->kev==kev && w->key==key) { target=w->ctx; list_del(&w->entry); w->entry.next=NULL; break; }
    }
    pthread_mutex_unlock(&g_keyed_lock);
    if (target) wake_ctx(target);
    return STATUS_SUCCESS;
}

/* address waits */
struct addr_waiter {
    struct list_head entry;
    void *addr;
    struct nt_wait_ctx *ctx;
    size_t size;
};

NTSTATUS nt_wait_on_address(void *addr, void *cmp, size_t size, LARGE_INTEGER *timeout) {
    ensure_global_init();
    if (!addr || !cmp || (size!=1 && size!=2 && size!=4 && size!=8)) return STATUS_INVALID_PARAMETER;
    /* quick compare */
    bool equal=false;
    if (size==1) equal = (*(volatile uint8_t*)addr == *(uint8_t*)cmp);
    else if (size==2) equal = (*(volatile uint16_t*)addr == *(uint16_t*)cmp);
    else if (size==4) equal = (*(volatile uint32_t*)addr == *(uint32_t*)cmp);
    else if (size==8) equal = (*(volatile uint64_t*)addr == *(uint64_t*)cmp);
    if (!equal) return STATUS_INVALID_PARAMETER; /* Windows returns error if not equal */

    struct timespec dl; bool has_dl=false; bool zero=false;
    has_dl = timeout_to_deadline(timeout, &dl, &zero);
    if (zero) return STATUS_TIMEOUT;

    struct nt_wait_ctx ctx;
    memset(&ctx,0,sizeof(ctx));
    pthread_mutex_init(&ctx.lock,NULL); pthread_cond_init(&ctx.cond,NULL);
    struct addr_waiter w; w.addr=addr; w.ctx=&ctx; w.size=size;
    INIT_LIST_HEAD(&w.entry);

    pthread_mutex_lock(&g_addr_lock);
    list_add_tail(&w.entry, &g_addr_waiters);
    pthread_mutex_unlock(&g_addr_lock);

    /* recheck after insertion to avoid lost wake between check and insert */
    if (size==1) equal = (*(volatile uint8_t*)addr == *(uint8_t*)cmp);
    else if (size==2) equal = (*(volatile uint16_t*)addr == *(uint16_t*)cmp);
    else if (size==4) equal = (*(volatile uint32_t*)addr == *(uint32_t*)cmp);
    else equal = (*(volatile uint64_t*)addr == *(uint64_t*)cmp);
    if (!equal) {
        pthread_mutex_lock(&g_addr_lock);
        if (w.entry.next) list_del(&w.entry);
        pthread_mutex_unlock(&g_addr_lock);
        pthread_mutex_destroy(&ctx.lock); pthread_cond_destroy(&ctx.cond);
        return STATUS_INVALID_PARAMETER;
    }

    pthread_mutex_lock(&ctx.lock);
    while (!ctx.woken) {
        if (has_dl) {
            int rc=pthread_cond_timedwait(&ctx.cond,&ctx.lock,&dl);
            if (rc==ETIMEDOUT) break;
        } else {
            pthread_cond_wait(&ctx.cond,&ctx.lock);
        }
    }
    bool woken=ctx.woken;
    pthread_mutex_unlock(&ctx.lock);

    pthread_mutex_lock(&g_addr_lock);
    if (w.entry.next) list_del(&w.entry);
    w.entry.next=NULL;
    pthread_mutex_unlock(&g_addr_lock);
    pthread_mutex_destroy(&ctx.lock); pthread_cond_destroy(&ctx.cond);
    return woken? STATUS_SUCCESS : STATUS_TIMEOUT;
}
void nt_wake_by_address_single(void *addr) {
    ensure_global_init();
    struct nt_wait_ctx *t=NULL;
    pthread_mutex_lock(&g_addr_lock);
    struct list_head *pos;
    for (pos=g_addr_waiters.next; pos!=&g_addr_waiters; pos=pos->next) {
        struct addr_waiter *w=list_entry(pos, struct addr_waiter, entry);
        if (w->addr==addr) { t=w->ctx; list_del(&w->entry); w->entry.next=NULL; break; }
    }
    pthread_mutex_unlock(&g_addr_lock);
    if (t) wake_ctx(t);
}
void nt_wake_by_address_all(void *addr) {
    ensure_global_init();
    struct nt_wait_ctx *targets[64]; int n=0;
    pthread_mutex_lock(&g_addr_lock);
    struct list_head *pos = g_addr_waiters.next;
    while (pos != &g_addr_waiters && n<64) {
        struct addr_waiter *w=list_entry(pos, struct addr_waiter, entry);
        pos = pos->next;
        if (w->addr==addr) { targets[n++]=w->ctx; list_del(&w->entry); w->entry.next=NULL; }
    }
    pthread_mutex_unlock(&g_addr_lock);
    for (int i=0;i<n;i++) wake_ctx(targets[i]);
}

/* abandonment */
void nt_abandon_owned_mutants(struct nt_thread *thread) {
    if (!thread) return;
    // snapshot list to avoid holding thread lock while locking mutant
    struct nt_mutant *arr[64]; int n=0;
    pthread_mutex_lock(&thread->lock);
    struct list_head *pos;
    for (pos=thread->owned_mutants.next; pos!=&thread->owned_mutants && n<64; pos=pos->next) {
        struct nt_mutant *m=list_entry(pos, struct nt_mutant, owned_entry);
        arr[n++]=m;
    }
    // clear list
    INIT_LIST_HEAD(&thread->owned_mutants);
    pthread_mutex_unlock(&thread->lock);
    for (int i=0;i<n;i++) {
        struct nt_mutant *m=arr[i];
        struct nt_dispatcher_header *h=&m->header;
        pthread_mutex_lock(&h->lock);
        m->abandoned = true;
        m->owner = NULL;
        m->recursion = 0;
        h->signal_state = 1;
        INIT_LIST_HEAD(&m->owned_entry);
        pthread_mutex_unlock(&h->lock);
        wake_waiters_for_header(h);
    }
}
#else /* __KERNEL__ : real kernel wait engine
 * Implements the same acquisition-with-undo WaitAll protocol as the
 * userspace shim, but with kernel primitives:
 *  - spinlock_t for object state + waiters list
 *  - wait_queue_head_t per wait ctx + prepare_to_wait/finish_wait
 *  - sorted-by-address locking to avoid deadlock
 *  - wake_up_all() on signal
 *
 * Deadlock-freedom argument (see sync.h):
 *  WaitAll acquires objects in strictly increasing address order.
 *  All threads sort the same way, so no circular wait can form.
 *  Partial acquisition is undone before blocking, giving atomic
 *  all-or-nothing semantics.  Lost wakeups are prevented by
 *  inserting wait blocks while holding all locks, then releasing
 *  and sleeping on the ctx wq signaled by every waker.
 *
 * LOCK ORDERING INVARIANT (MUST PRESERVE - see top of file):
 *   Every path that takes multiple object locks ΓÇö acquisition,
 *   undo on partial failure, waiter insertion, waiter removal, and
 *   the zero-timeout poll abandoned scan ΓÇö MUST take locks in
 *   ascending address order (kernel_sort_by_addr) and release in
 *   reverse order. Never use original handle order for multi-lock
 *   operations. Violation causes deadlock under overlapping opposite-order
 *   WaitAll (sync_atomic).
 */

#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/ktime.h>


/* Per-CPU current thread pointer for kernel tests (mirrors shim TLS) */
struct nt_thread *nt_current_thread_ptr = NULL;
static LIST_HEAD(g_nt_thread_list);
static DEFINE_SPINLOCK(g_nt_thread_list_lock);

/* ---- helpers ---- */

static bool kernel_is_signaled(struct nt_dispatcher_header *h, struct nt_thread *thr)
{
    switch (h->type) {
        case NT_OBJ_EVENT: return h->signal_state != 0;
        case NT_OBJ_SEMAPHORE: return h->signal_state > 0;
        case NT_OBJ_MUTANT: {
            struct nt_mutant *m = (struct nt_mutant *)h;
            if (m->abandoned) return true;
            if (m->owner == NULL) return true;
            if (m->owner == thr) return true;
            return false;
        }
        case NT_OBJ_TIMER: return h->signal_state != 0;
        case NT_OBJ_KEYED_EVENT: return false;
        default: return false;
    }
}

static bool kernel_consume_locked(struct nt_dispatcher_header *h, struct nt_thread *thr, bool *was_abandoned)
{
    if (was_abandoned) *was_abandoned = false;
    switch (h->type) {
        case NT_OBJ_EVENT: {
            if (h->signal_state == 0) return false;
            if (!h->manual_reset) h->signal_state = 0;
            return true;
        }
        case NT_OBJ_SEMAPHORE: {
            if (h->signal_state <= 0) return false;
            h->signal_state--;
            return true;
        }
        case NT_OBJ_MUTANT: {
            struct nt_mutant *m = (struct nt_mutant *)h;
            if (m->abandoned) {
                if (was_abandoned) *was_abandoned = true;
                m->abandoned = false;
                m->owner = thr;
                m->recursion = 1;
                h->signal_state = 0;
                spin_lock(&thr->lock);
                list_add_tail(&m->owned_entry, &thr->owned_mutants);
                spin_unlock(&thr->lock);
                return true;
            }
            if (m->owner == NULL) {
                m->owner = thr;
                m->recursion = 1;
                h->signal_state = 0;
                spin_lock(&thr->lock);
                list_add_tail(&m->owned_entry, &thr->owned_mutants);
                spin_unlock(&thr->lock);
                return true;
            }
            if (m->owner == thr) {
                m->recursion++;
                return true;
            }
            return false;
        }
        case NT_OBJ_TIMER: {
            if (h->signal_state == 0) return false;
            if (!h->manual_reset) h->signal_state = 0;
            return true;
        }
        default: return false;
    }
}

static void kernel_undo_locked(struct nt_dispatcher_header *h, struct nt_thread *thr)
{
    switch (h->type) {
        case NT_OBJ_EVENT:
            if (!h->manual_reset) h->signal_state = 1;
            break;
        case NT_OBJ_SEMAPHORE:
            h->signal_state++;
            break;
        case NT_OBJ_MUTANT: {
            struct nt_mutant *m = (struct nt_mutant *)h;
            if (m->owner != thr) break;
            if (m->recursion > 1) {
                m->recursion--;
            } else {
                spin_lock(&thr->lock);
                if (m->owned_entry.next && m->owned_entry.prev)
                    list_del(&m->owned_entry);
                m->owned_entry.next = NULL; m->owned_entry.prev = NULL;
                spin_unlock(&thr->lock);
                m->owner = NULL;
                m->recursion = 0;
                h->signal_state = 1;
                INIT_LIST_HEAD(&m->owned_entry);
            }
            break;
        }
        case NT_OBJ_TIMER:
            if (!h->manual_reset) h->signal_state = 1;
            break;
        default: break;
    }
}

static void kernel_sort_by_addr(ULONG count, struct nt_dispatcher_header **objs,
                                ULONG *orig_idx, struct nt_dispatcher_header **sorted,
                                ULONG *sorted_orig)
{
    for (ULONG i = 0; i < count; i++) { sorted[i] = objs[i]; sorted_orig[i] = orig_idx ? orig_idx[i] : i; }
    for (ULONG i = 0; i < count; i++) for (ULONG j = i + 1; j < count; j++) if (sorted[j] < sorted[i]) {
        struct nt_dispatcher_header *tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
        ULONG ti = sorted_orig[i]; sorted_orig[i] = sorted_orig[j]; sorted_orig[j] = ti;
    }
}

static void header_init_kernel(struct nt_dispatcher_header *h, enum nt_disp_type t, LONG init_state, bool manual)
{
    h->type = t;
    spin_lock_init(&h->lock);
    INIT_LIST_HEAD(&h->waiters);
    h->signal_state = init_state;
    h->manual_reset = manual;
    atomic_set(&h->refcount, 1);
    init_waitqueue_head(&h->wq);
}

void nt_header_addref(struct nt_dispatcher_header *h) { atomic_inc(&h->refcount); }
void nt_header_release(struct nt_dispatcher_header *h) {
    if (atomic_dec_and_test(&h->refcount)) {
        kfree(h);
    }
}

/* TLS / thread helpers (kernel version) */
NTSTATUS nt_thread_create(struct nt_thread **out)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    struct nt_thread *t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t) return STATUS_NO_MEMORY;
    t->task = current;
    t->tid = (unsigned long)current->pid;
    spin_lock_init(&t->lock);
    INIT_LIST_HEAD(&t->owned_mutants);
    INIT_LIST_HEAD(&t->global_entry);
    spin_lock(&g_nt_thread_list_lock);
    list_add(&t->global_entry, &g_nt_thread_list);
    spin_unlock(&g_nt_thread_list_lock);
    t->alerted = false;
    t->alert_pending = false;
    *out = t;
    return STATUS_SUCCESS;
}
void nt_thread_destroy(struct nt_thread *t)
{
    if (!t) return;
    nt_abandon_owned_mutants(t);
    kfree(t);
}
void nt_thread_alert(struct nt_thread *t)
{
    if (!t) return;
    unsigned long flags;
    spin_lock_irqsave(&t->lock, flags);
    t->alerted = true;
    spin_unlock_irqrestore(&t->lock, flags);
    /* wake any wait ctx that thread is sleeping on – caller must wake via ctx */
}
void nt_thread_queue_apc(struct nt_thread *t)
{
    if (!t) return;
    unsigned long flags;
    spin_lock_irqsave(&t->lock, flags);
    t->alert_pending = true;
    spin_unlock_irqrestore(&t->lock, flags);
}
void nt_thread_clear_alert(struct nt_thread *t)
{
    if (!t) return;
    unsigned long flags;
    spin_lock_irqsave(&t->lock, flags);
    t->alerted = false;
    t->alert_pending = false;
    spin_unlock_irqrestore(&t->lock, flags);
}

/* timeout conversion: return jiffies timeout, -1 for infinite, 0 for poll */
static long timeout_to_jiffies(LARGE_INTEGER *to, bool *is_poll)
{
    if (is_poll) *is_poll = false;
    if (!to) return MAX_SCHEDULE_TIMEOUT; /* infinite */
    LARGE_INTEGER t;
    if (!access_ok(to, sizeof(t))) {
        t = *to;
    } else {
        if (copy_from_user(&t, to, sizeof(t))) return -1;
    }
    if (t.QuadPart == 0) {
        if (is_poll) *is_poll = true;
        return 0;
    }
    int64_t rel100;
    if (t.QuadPart < 0) {
        rel100 = -t.QuadPart;
    } else {
        int64_t now100 = (ktime_get_real_ns() / 100) + 116444736000000000LL;
        int64_t due100 = t.QuadPart;
        if (due100 <= now100) {
            if (is_poll) *is_poll = true;
            return 0;
        }
        rel100 = due100 - now100;
    }
    /* 100ns -> ns -> jiffies */
    unsigned long long ns = (unsigned long long)rel100 * 100ULL;
    return nsecs_to_jiffies(ns);
}

bool nt_dispatcher_is_signaled(struct nt_dispatcher_header *h, struct nt_thread *thr)
{
    return kernel_is_signaled(h, thr);
}

static void kernel_wake_ctx(struct nt_wait_ctx *ctx)
{
    unsigned long flags;
    spin_lock_irqsave(&ctx->lock, flags);
    ctx->woken = true;
    spin_unlock_irqrestore(&ctx->lock, flags);
    wake_up_all(&ctx->wq);
}

static void kernel_wake_waiters(struct nt_dispatcher_header *h)
{
    struct nt_wait_ctx *to_wake[64];
    int n = 0;
    unsigned long flags;
    spin_lock_irqsave(&h->lock, flags);
    struct list_head *pos;
    for (pos = h->waiters.next; pos != &h->waiters && n < 64; pos = pos->next) {
        struct nt_wait_block *blk = list_entry(pos, struct nt_wait_block, entry);
        struct nt_wait_ctx *ctx = blk->ctx;
        atomic_inc(&ctx->refcount);
        to_wake[n++] = ctx;
    }
    spin_unlock_irqrestore(&h->lock, flags);
    for (int i = 0; i < n; i++) {
        kernel_wake_ctx(to_wake[i]);
        if (atomic_dec_and_test(&to_wake[i]->refcount)) {
            kfree(to_wake[i]);
        }
    }
}

/* creation wrappers (kernel) */
NTSTATUS nt_create_event(struct nt_event **out, int type, BOOLEAN initial_state)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    if (type != NotificationEvent && type != SynchronizationEvent) return STATUS_INVALID_PARAMETER;
    struct nt_event *e = kzalloc(sizeof(*e), GFP_KERNEL);
    if (!e) return STATUS_NO_MEMORY;
    header_init_kernel(&e->header, NT_OBJ_EVENT, initial_state ? 1 : 0, type == NotificationEvent);
    *out = e;
    return STATUS_SUCCESS;
}
NTSTATUS nt_create_mutant(struct nt_mutant **out, BOOLEAN initial_owner)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    struct nt_mutant *m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m) return STATUS_NO_MEMORY;
    header_init_kernel(&m->header, NT_OBJ_MUTANT, initial_owner ? 0 : 1, false);
    m->owner = NULL; m->recursion = 0; m->abandoned = false;
    INIT_LIST_HEAD(&m->owned_entry);
    if (initial_owner) {
        struct nt_thread *cur = nt_current_thread();
        if (!cur) { kfree(m); return STATUS_INVALID_PARAMETER; }
        m->owner = cur;
        m->recursion = 1;
        m->header.signal_state = 0;
        unsigned long flags;
        spin_lock_irqsave(&cur->lock, flags);
        list_add_tail(&m->owned_entry, &cur->owned_mutants);
        spin_unlock_irqrestore(&cur->lock, flags);
    }
    *out = m;
    return STATUS_SUCCESS;
}
NTSTATUS nt_create_semaphore(struct nt_semaphore **out, LONG initial_count, LONG maximum_count)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    if (maximum_count <= 0 || initial_count < 0 || initial_count > maximum_count) return STATUS_INVALID_PARAMETER;
    struct nt_semaphore *s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s) return STATUS_NO_MEMORY;
    header_init_kernel(&s->header, NT_OBJ_SEMAPHORE, initial_count, false);
    s->limit = maximum_count;
    *out = s;
    return STATUS_SUCCESS;
}
static void timer_callback(struct timer_list *tl);
NTSTATUS nt_create_timer(struct nt_timer **out, int type)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    if (type != NotificationTimer && type != SynchronizationTimer) return STATUS_INVALID_PARAMETER;
    struct nt_timer *t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t) return STATUS_NO_MEMORY;
    header_init_kernel(&t->header, NT_OBJ_TIMER, 0, type == NotificationTimer);
    spin_lock_init(&t->timer_lock);
    t->active = false;
    t->cancel_requested = false;
    timer_setup(&t->timer, timer_callback, 0);
    *out = t;
    return STATUS_SUCCESS;
}
NTSTATUS nt_create_keyed_event(struct nt_keyed_event **out)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    struct nt_keyed_event *k = kzalloc(sizeof(*k), GFP_KERNEL);
    if (!k) return STATUS_NO_MEMORY;
    header_init_kernel(&k->header, NT_OBJ_KEYED_EVENT, 0, false);
    *out = k;
    return STATUS_SUCCESS;
}

void nt_put_event(struct nt_event *e) { if (!e) return; nt_header_release(&e->header); }
void nt_put_mutant(struct nt_mutant *m) { if (!m) return; nt_header_release(&m->header); }
void nt_put_semaphore(struct nt_semaphore *s) { if (!s) return; nt_header_release(&s->header); }
void nt_put_timer(struct nt_timer *t) {
    if (!t) return;
    BOOLEAN dummy;
    nt_cancel_timer(t, &dummy);
    nt_header_release(&t->header);
}
void nt_put_keyed_event(struct nt_keyed_event *k) { if (!k) return; nt_header_release(&k->header); }

/* operations */
NTSTATUS nt_set_event(struct nt_event *event, LONG *prev)
{
    if (!event) return STATUS_INVALID_PARAMETER;
    struct nt_dispatcher_header *h = &event->header;
    unsigned long flags;
    spin_lock_irqsave(&h->lock, flags);
    if (prev) *prev = h->signal_state;
    h->signal_state = 1;
    spin_unlock_irqrestore(&h->lock, flags);
    kernel_wake_waiters(h);
    return STATUS_SUCCESS;
}
NTSTATUS nt_reset_event(struct nt_event *event, LONG *prev)
{
    if (!event) return STATUS_INVALID_PARAMETER;
    struct nt_dispatcher_header *h = &event->header;
    unsigned long flags;
    spin_lock_irqsave(&h->lock, flags);
    if (prev) *prev = h->signal_state;
    h->signal_state = 0;
    spin_unlock_irqrestore(&h->lock, flags);
    return STATUS_SUCCESS;
}
NTSTATUS nt_pulse_event(struct nt_event *event, LONG *prev)
{
    if (!event) return STATUS_INVALID_PARAMETER;
    struct nt_dispatcher_header *h = &event->header;
    unsigned long flags;
    spin_lock_irqsave(&h->lock, flags);
    if (prev) *prev = h->signal_state;
    h->signal_state = 1;
    /* snapshot waiters while holding lock */
    struct nt_wait_ctx *to_wake[64]; int n = 0;
    struct list_head *pos;
    for (pos = h->waiters.next; pos != &h->waiters && n < 64; pos = pos->next) {
        struct nt_wait_block *blk = list_entry(pos, struct nt_wait_block, entry);
        atomic_inc(&blk->ctx->refcount);
        to_wake[n++] = blk->ctx;
    }
    spin_unlock_irqrestore(&h->lock, flags);
    for (int i = 0; i < n; i++) kernel_wake_ctx(to_wake[i]);
    for (int i = 0; i < n; i++) if (atomic_dec_and_test(&to_wake[i]->refcount)) kfree(to_wake[i]);
    /* pulse resets after a short delay – emulate with immediate reset after wakers have been queued */
    msleep(1);
    spin_lock_irqsave(&h->lock, flags);
    h->signal_state = 0;
    spin_unlock_irqrestore(&h->lock, flags);
    return STATUS_SUCCESS;
}
NTSTATUS nt_release_mutant(struct nt_mutant *mutant, LONG *prev)
{
    if (!mutant) return STATUS_INVALID_PARAMETER;
    struct nt_thread *thr = nt_current_thread();
    struct nt_dispatcher_header *h = &mutant->header;
    unsigned long flags;
    spin_lock_irqsave(&h->lock, flags);
    if (!mutant->owner || mutant->owner->tid != (unsigned long)current->pid) { spin_unlock_irqrestore(&h->lock, flags); return STATUS_MUTANT_NOT_OWNED; }
    if (prev) {
        *prev = mutant->recursion;
    }
    mutant->recursion--;
    if (mutant->recursion > 0) { spin_unlock_irqrestore(&h->lock, flags); return STATUS_SUCCESS; }
    spin_lock(&thr->lock);
    if (mutant->owned_entry.next) list_del(&mutant->owned_entry);
    mutant->owned_entry.next = NULL; mutant->owned_entry.prev = NULL;
    spin_unlock(&thr->lock);
    mutant->owner = NULL;
    h->signal_state = 1;
    mutant->abandoned = false;
    spin_unlock_irqrestore(&h->lock, flags);
    kernel_wake_waiters(h);
    return STATUS_SUCCESS;
}
NTSTATUS nt_release_semaphore(struct nt_semaphore *sem, LONG release_count, LONG *prev)
{
    if (!sem || release_count <= 0) return STATUS_INVALID_PARAMETER;
    struct nt_dispatcher_header *h = &sem->header;
    unsigned long flags;
    spin_lock_irqsave(&h->lock, flags);
    if (h->signal_state + release_count > sem->limit || h->signal_state + release_count < 0) {
        spin_unlock_irqrestore(&h->lock, flags);
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
    if (prev) {
        *prev = h->signal_state;
    }
    h->signal_state += release_count;
    spin_unlock_irqrestore(&h->lock, flags);
    kernel_wake_waiters(h);
    return STATUS_SUCCESS;
}
/* timer ops – kernel timer_list */
static void timer_callback(struct timer_list *tl)
{
    struct nt_timer *t = timer_container_of(t, tl, timer);
    unsigned long flags;
    spin_lock_irqsave(&t->header.lock, flags);
    t->header.signal_state = 1;
    spin_unlock_irqrestore(&t->header.lock, flags);
    kernel_wake_waiters(&t->header);
    if (t->apc_routine) {
        if (t->apc_thread) nt_thread_queue_apc(t->apc_thread);
        else t->apc_routine(t->apc_ctx);
    }
    spin_lock_irqsave(&t->timer_lock, flags);
    if (t->period_ms > 0 && !t->cancel_requested) {
        spin_unlock_irqrestore(&t->timer_lock, flags);
        mod_timer(&t->timer, jiffies + msecs_to_jiffies(t->period_ms));
    } else {
        t->active = false;
        spin_unlock_irqrestore(&t->timer_lock, flags);
    }
}
/* Absolute NT expiry (100 ns since 1601) for a due time; negative is relative. */
static int64_t timer_expiry_100ns(const LARGE_INTEGER *due)
{
    struct timespec64 ts;
    if (due->QuadPart >= 0) return due->QuadPart;
    ktime_get_real_ts64(&ts);
    return (int64_t)ts.tv_sec * 10000000LL + ts.tv_nsec / 100 + 116444736000000000LL - due->QuadPart;
}

NTSTATUS nt_set_timer(struct nt_timer *timer, const LARGE_INTEGER *due_time, LONG period_ms,
                      void (*apc_routine)(void*), void *apc_ctx)
{
    if (!timer || !due_time) return STATUS_INVALID_PARAMETER;
    BOOLEAN dummy;
    nt_cancel_timer(timer, &dummy);
    unsigned long flags;
    spin_lock_irqsave(&timer->timer_lock, flags);
    timer->due_time = *due_time;
    timer->expiry_100ns = timer_expiry_100ns(due_time);
    timer->period_ms = period_ms;
    timer->apc_routine = apc_routine;
    timer->apc_ctx = apc_ctx;
    timer->apc_thread = nt_current_thread();
    timer->cancel_requested = false;
    timer->active = true;
    spin_unlock_irqrestore(&timer->timer_lock, flags);
    spin_lock_irqsave(&timer->header.lock, flags);
    timer->header.signal_state = 0;
    spin_unlock_irqrestore(&timer->header.lock, flags);
    long j = timeout_to_jiffies((LARGE_INTEGER *)due_time, NULL);
    if (j == 0) j = 1;
    if (j == MAX_SCHEDULE_TIMEOUT) j = 1;
    mod_timer(&timer->timer, jiffies + j);
    return STATUS_SUCCESS;
}
NTSTATUS nt_cancel_timer(struct nt_timer *timer, BOOLEAN *current_state)
{
    if (!timer) return STATUS_INVALID_PARAMETER;
    unsigned long flags;
    spin_lock_irqsave(&timer->timer_lock, flags);
    if (current_state) *current_state = timer->header.signal_state ? 1 : 0;
    if (!timer->active) { spin_unlock_irqrestore(&timer->timer_lock, flags); return STATUS_SUCCESS; }
    timer->cancel_requested = true;
    spin_unlock_irqrestore(&timer->timer_lock, flags);
    timer_shutdown_sync(&timer->timer);
    spin_lock_irqsave(&timer->timer_lock, flags);
    timer->active = false;
    spin_unlock_irqrestore(&timer->timer_lock, flags);
    return STATUS_SUCCESS;
}

/* keyed events – global registry */
static LIST_HEAD(g_keyed_waiters_kernel);
static spinlock_t g_keyed_lock_kernel;
static bool g_keyed_init = false;
static inline void ensure_keyed_init(void) {
    if (!g_keyed_init) { spin_lock_init(&g_keyed_lock_kernel); INIT_LIST_HEAD(&g_keyed_waiters_kernel); g_keyed_init = true; }
}
struct keyed_waiter_k {
    struct list_head entry;
    struct nt_keyed_event *kev;
    void *key;
    struct nt_wait_ctx *ctx;
    struct nt_wait_block blk;
};
NTSTATUS nt_wait_for_keyed_event(struct nt_keyed_event *kev, void *key, BOOLEAN alertable, LARGE_INTEGER *timeout)
{
    ensure_keyed_init();
    struct nt_thread *thr = nt_current_thread();
    if (!thr || !kev) return STATUS_INVALID_PARAMETER;
    /* alert/APC fast check */
    {
        unsigned long flags; spin_lock_irqsave(&thr->lock, flags);
        bool apc = thr->alert_pending, al = thr->alerted;
        spin_unlock_irqrestore(&thr->lock, flags);
        if (apc) { spin_lock_irqsave(&thr->lock, flags); thr->alert_pending = false; spin_unlock_irqrestore(&thr->lock, flags); return STATUS_USER_APC; }
        if (al && alertable) { spin_lock_irqsave(&thr->lock, flags); thr->alerted = false; spin_unlock_irqrestore(&thr->lock, flags); return STATUS_ALERTED; }
    }
    bool is_poll = false;
    long j = timeout_to_jiffies(timeout, &is_poll);
    if (is_poll) return STATUS_TIMEOUT;

    struct nt_wait_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx) return STATUS_NO_MEMORY;
    ctx->thread = thr; ctx->woken = false;
    spin_lock_init(&ctx->lock);
    init_waitqueue_head(&ctx->wq);
    atomic_set(&ctx->refcount, 1);

    struct keyed_waiter_k w;
    w.kev = kev; w.key = key; w.ctx = ctx;
    w.blk.ctx = ctx; w.blk.object = &kev->header;
    INIT_LIST_HEAD(&w.entry);

    unsigned long flags;
    spin_lock_irqsave(&g_keyed_lock_kernel, flags);
    list_add_tail(&w.entry, &g_keyed_waiters_kernel);
    spin_unlock_irqrestore(&g_keyed_lock_kernel, flags);

    DEFINE_WAIT(wait);
    prepare_to_wait(&ctx->wq, &wait, TASK_INTERRUPTIBLE);
    if (j == MAX_SCHEDULE_TIMEOUT) schedule();
    else schedule_timeout(j);
    finish_wait(&ctx->wq, &wait);

    bool got;
    spin_lock_irqsave(&ctx->lock, flags);
    got = ctx->woken;
    spin_unlock_irqrestore(&ctx->lock, flags);

    spin_lock_irqsave(&g_keyed_lock_kernel, flags);
    if (w.entry.next) list_del(&w.entry);
    spin_unlock_irqrestore(&g_keyed_lock_kernel, flags);

    if (atomic_dec_and_test(&ctx->refcount)) kfree(ctx);

    if (!got) {
        unsigned long f2; spin_lock_irqsave(&thr->lock, f2);
        bool apc = thr->alert_pending, al = thr->alerted;
        if (apc) { thr->alert_pending = false; spin_unlock_irqrestore(&thr->lock, f2); return STATUS_USER_APC; }
        if (al && alertable) { thr->alerted = false; spin_unlock_irqrestore(&thr->lock, f2); return STATUS_ALERTED; }
        spin_unlock_irqrestore(&thr->lock, f2);
        return STATUS_TIMEOUT;
    }
    return STATUS_SUCCESS;
}
NTSTATUS nt_release_keyed_event(struct nt_keyed_event *kev, void *key, BOOLEAN alertable, LARGE_INTEGER *timeout)
{
    (void)alertable; (void)timeout;
    ensure_keyed_init();
    if (!kev) return STATUS_INVALID_PARAMETER;
    struct nt_wait_ctx *target = NULL;
    unsigned long flags;
    spin_lock_irqsave(&g_keyed_lock_kernel, flags);
    struct list_head *pos;
    for (pos = g_keyed_waiters_kernel.next; pos != &g_keyed_waiters_kernel; pos = pos->next) {
        struct keyed_waiter_k *w = list_entry(pos, struct keyed_waiter_k, entry);
        if (w->kev == kev && w->key == key) { target = w->ctx; list_del(&w->entry); w->entry.next = NULL; break; }
    }
    spin_unlock_irqrestore(&g_keyed_lock_kernel, flags);
    if (target) kernel_wake_ctx(target);
    return STATUS_SUCCESS;
}

/* address waits – kernel version uses wait queue per address */
static LIST_HEAD(g_addr_waiters_kernel);
static spinlock_t g_addr_lock_kernel;
static bool g_addr_init = false;
static inline void ensure_addr_init(void) {
    if (!g_addr_init) { spin_lock_init(&g_addr_lock_kernel); INIT_LIST_HEAD(&g_addr_waiters_kernel); g_addr_init = true; }
}
struct addr_waiter_k {
    struct list_head entry;
    void *addr;
    struct nt_wait_ctx *ctx;
};
NTSTATUS nt_wait_on_address(void *addr, void *cmp, size_t size, LARGE_INTEGER *timeout)
{
    ensure_addr_init();
    if (!addr || !cmp || (size!=1 && size!=2 && size!=4 && size!=8)) return STATUS_INVALID_PARAMETER;
    /* addr and cmp may point into user space: copy both values in (zero-extended,
     * so the compare is the same for every width) instead of dereferencing them. */
    uint64_t cur = 0, want = 0;
    if (copy_from_user(&cur, addr, size) || copy_from_user(&want, cmp, size))
        return STATUS_ACCESS_VIOLATION;
    if (cur != want) return STATUS_INVALID_PARAMETER;
    bool is_poll = false;
    long j = timeout_to_jiffies(timeout, &is_poll);
    if (is_poll) return STATUS_TIMEOUT;

    struct nt_wait_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx) return STATUS_NO_MEMORY;
    spin_lock_init(&ctx->lock);
    init_waitqueue_head(&ctx->wq);
    atomic_set(&ctx->refcount, 1);
    ctx->woken = false;

    struct addr_waiter_k w; w.addr = addr; w.ctx = ctx; INIT_LIST_HEAD(&w.entry);
    unsigned long flags;
    spin_lock_irqsave(&g_addr_lock_kernel, flags);
    list_add_tail(&w.entry, &g_addr_waiters_kernel);
    spin_unlock_irqrestore(&g_addr_lock_kernel, flags);

    /* recheck after insertion to avoid lost wake */
    if (size==1) equal = (*(volatile uint8_t*)addr == *(uint8_t*)cmp);
    else if (size==2) equal = (*(volatile uint16_t*)addr == *(uint16_t*)cmp);
    else if (size==4) equal = (*(volatile uint32_t*)addr == *(uint32_t*)cmp);
    else equal = (*(volatile uint64_t*)addr == *(uint64_t*)cmp);
    if (!equal) {
        spin_lock_irqsave(&g_addr_lock_kernel, flags);
        if (w.entry.next) list_del(&w.entry);
        spin_unlock_irqrestore(&g_addr_lock_kernel, flags);
        kfree(ctx);
        return STATUS_INVALID_PARAMETER;
    }

    DEFINE_WAIT(wait);
    prepare_to_wait(&ctx->wq, &wait, TASK_INTERRUPTIBLE);
    if (j == MAX_SCHEDULE_TIMEOUT) schedule();
    else schedule_timeout(j);
    finish_wait(&ctx->wq, &wait);

    spin_lock_irqsave(&ctx->lock, flags);
    bool woken = ctx->woken;
    spin_unlock_irqrestore(&ctx->lock, flags);

    spin_lock_irqsave(&g_addr_lock_kernel, flags);
    if (w.entry.next) list_del(&w.entry);
    w.entry.next = NULL;
    spin_unlock_irqrestore(&g_addr_lock_kernel, flags);
    kfree(ctx);
    return woken ? STATUS_SUCCESS : STATUS_TIMEOUT;
}
void nt_wake_by_address_single(void *addr)
{
    ensure_addr_init();
    struct nt_wait_ctx *t = NULL;
    unsigned long flags;
    spin_lock_irqsave(&g_addr_lock_kernel, flags);
    struct list_head *pos;
    for (pos = g_addr_waiters_kernel.next; pos != &g_addr_waiters_kernel; pos = pos->next) {
        struct addr_waiter_k *w = list_entry(pos, struct addr_waiter_k, entry);
        if (w->addr == addr) { t = w->ctx; list_del(&w->entry); w->entry.next = NULL; break; }
    }
    spin_unlock_irqrestore(&g_addr_lock_kernel, flags);
    if (t) kernel_wake_ctx(t);
}
void nt_wake_by_address_all(void *addr)
{
    ensure_addr_init();
    struct nt_wait_ctx *targets[64]; int n = 0;
    unsigned long flags;
    spin_lock_irqsave(&g_addr_lock_kernel, flags);
    struct list_head *pos = g_addr_waiters_kernel.next;
    while (pos != &g_addr_waiters_kernel && n < 64) {
        struct addr_waiter_k *w = list_entry(pos, struct addr_waiter_k, entry);
        pos = pos->next;
        if (w->addr == addr) { targets[n++] = w->ctx; list_del(&w->entry); w->entry.next = NULL; }
    }
    spin_unlock_irqrestore(&g_addr_lock_kernel, flags);
    for (int i = 0; i < n; i++) kernel_wake_ctx(targets[i]);
}

void nt_abandon_owned_mutants(struct nt_thread *thread)
{
    if (!thread) return;
    struct nt_mutant *arr[64]; int n = 0;
    unsigned long flags;
    spin_lock_irqsave(&thread->lock, flags);
    struct list_head *pos;
    for (pos = thread->owned_mutants.next; pos != &thread->owned_mutants && n < 64; pos = pos->next) {
        struct nt_mutant *m = list_entry(pos, struct nt_mutant, owned_entry);
        arr[n++] = m;
    }
    INIT_LIST_HEAD(&thread->owned_mutants);
    spin_unlock_irqrestore(&thread->lock, flags);
    for (int i = 0; i < n; i++) {
        struct nt_mutant *m = arr[i];
        struct nt_dispatcher_header *h = &m->header;
        spin_lock_irqsave(&h->lock, flags);
        m->abandoned = true;
        m->owner = NULL;
        m->recursion = 0;
        h->signal_state = 1;
        INIT_LIST_HEAD(&m->owned_entry);
        spin_unlock_irqrestore(&h->lock, flags);
        kernel_wake_waiters(h);
    }
}

/* ---- wait engine core ---- */
NTSTATUS nt_wait_for_multiple_objects(ULONG count,
                                      struct nt_dispatcher_header **objects,
                                      ULONG wait_type,
                                      BOOLEAN alertable,
                                      LARGE_INTEGER *timeout)
{
    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS) return STATUS_INVALID_PARAMETER;
    if (!objects) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < count; i++) if (!objects[i]) return STATUS_INVALID_PARAMETER;
    if (wait_type != WaitAll && wait_type != WaitAny) return STATUS_INVALID_PARAMETER;

    struct nt_thread *thr = nt_current_thread();
    if (!thr) {
        /* Main thread may not have sync_thread set yet (e.g., after nt_enter_personality
         * before ethread creation). Create a temporary sync thread for this task.
         */
        if (NT_SUCCESS(nt_thread_create(&thr))) {
            nt_set_current_thread(thr);
        } else {
            return STATUS_INVALID_PARAMETER;
        }
    }

    bool is_poll = false;
    long jiffies_timeout = timeout_to_jiffies(timeout, &is_poll);
    if (jiffies_timeout == -1) return STATUS_ACCESS_VIOLATION;

    /* fast alert/APC check */
    if (alertable) {
        unsigned long flags; spin_lock_irqsave(&thr->lock, flags);
        bool apc = thr->alert_pending, al = thr->alerted;
        spin_unlock_irqrestore(&thr->lock, flags);
        if (apc) { spin_lock_irqsave(&thr->lock, flags); thr->alert_pending = false; spin_unlock_irqrestore(&thr->lock, flags); return STATUS_USER_APC; }
        if (al) { spin_lock_irqsave(&thr->lock, flags); thr->alerted = false; spin_unlock_irqrestore(&thr->lock, flags); return STATUS_ALERTED; }
    }

    struct nt_dispatcher_header *sorted[MAXIMUM_WAIT_OBJECTS];
    ULONG sorted_orig[MAXIMUM_WAIT_OBJECTS];
    ULONG orig_idx[MAXIMUM_WAIT_OBJECTS];
    for (ULONG i = 0; i < count; i++) orig_idx[i] = i;
    if (wait_type == WaitAll) kernel_sort_by_addr(count, objects, orig_idx, sorted, sorted_orig);

    struct nt_wait_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx) return STATUS_NO_MEMORY;
    ctx->thread = thr;
    ctx->count = count;
    ctx->wait_type = wait_type;
    ctx->alertable = alertable;
    spin_lock_init(&ctx->lock);
    init_waitqueue_head(&ctx->wq);
    ctx->woken = false;
    atomic_set(&ctx->refcount, 1);
    for (ULONG i = 0; i < count; i++) { ctx->blocks[i].entry.next = NULL; ctx->blocks[i].entry.prev = NULL; }

    NTSTATUS final_status = STATUS_TIMEOUT;
    bool abandoned = false;
    ULONG satisfied_idx = 0;
    long remaining_jiffies = jiffies_timeout;

    while (1) {
        /* try acquire */
        bool got = false;
        abandoned = false;
        if (wait_type == WaitAny) {
            for (ULONG i = 0; i < count; i++) {
                struct nt_dispatcher_header *h = objects[i];
                bool aband = false;
                unsigned long flags; spin_lock_irqsave(&h->lock, flags);
                bool sig = kernel_is_signaled(h, thr);
                bool ok = false;
                if (sig) ok = kernel_consume_locked(h, thr, &aband);
                spin_unlock_irqrestore(&h->lock, flags);
                if (ok) { got = true; satisfied_idx = i; abandoned = aband; break; }
            }
        } else {
            /* WaitAll: sorted locking */
            for (ULONG i = 0; i < count; i++) spin_lock(&sorted[i]->lock);
            bool all_sig = true;
            for (ULONG i = 0; i < count; i++) if (!kernel_is_signaled(sorted[i], thr)) { all_sig = false; break; }
            if (all_sig) {
                bool ok_all = true;
                bool any_aband = false;
                bool was_aband[MAXIMUM_WAIT_OBJECTS] = {0};
                ULONG consumed = 0;
                for (ULONG i = 0; i < count; i++) {
                    bool a = false;
                    if (!kernel_consume_locked(sorted[i], thr, &a)) { ok_all = false; break; }
                    was_aband[i] = a;
                    consumed++;
                    if (a) any_aband = true;
                }
                if (ok_all) { got = true; abandoned = any_aband; satisfied_idx = 0; }
                else {
                    /* undo consumed prefix in reverse order, restoring prior state */
                    for (LONG k = (LONG)consumed - 1; k >= 0; k--) {
                        kernel_undo_locked(sorted[k], thr);
                        if (was_aband[k] && sorted[k]->type == NT_OBJ_MUTANT) {
                            struct nt_mutant *m = (struct nt_mutant *)sorted[k];
                            m->abandoned = true;
                        }
                    }
                }
            }
            for (LONG i = (LONG)count - 1; i >= 0; i--) spin_unlock(&sorted[i]->lock);
            if (got) { /* success */ }
        }
        if (got) {
            if (abandoned) final_status = (NTSTATUS)(STATUS_ABANDONED_WAIT_0 + (wait_type==WaitAny ? satisfied_idx : 0));
            else final_status = (NTSTATUS)(STATUS_WAIT_0 + satisfied_idx);
            break;
        }
        if (is_poll) {
            /* INVARIANT: WaitAll abandoned scan and all_sig check use sorted order */
            bool found_abandoned = false;
            ULONG aband_idx = 0;
            if (wait_type == WaitAll) {
                for (ULONG i = 0; i < count; i++) {
                    struct nt_dispatcher_header *h = sorted[i];
                    if (h->type == NT_OBJ_MUTANT) {
                        struct nt_mutant *m = (struct nt_mutant *)h;
                        unsigned long flags;
                        spin_lock_irqsave(&h->lock, flags);
                        bool ab = m->abandoned;
                        spin_unlock_irqrestore(&h->lock, flags);
                        if (ab) found_abandoned = true;
                    }
                }
            } else {
                for (ULONG i = 0; i < count; i++) {
                    struct nt_dispatcher_header *h = objects[i];
                    if (h->type == NT_OBJ_MUTANT) {
                        struct nt_mutant *m = (struct nt_mutant *)h;
                        unsigned long flags;
                        spin_lock_irqsave(&h->lock, flags);
                        bool ab = m->abandoned;
                        spin_unlock_irqrestore(&h->lock, flags);
                        if (ab) { found_abandoned = true; aband_idx = i; break; }
                    }
                }
            }
            if (found_abandoned) {
                if (wait_type == WaitAll) {
                    bool all_sig = true;
                    for (ULONG i = 0; i < count; i++) spin_lock(&sorted[i]->lock);
                    for (ULONG i = 0; i < count; i++) if (!kernel_is_signaled(sorted[i], thr)) { all_sig = false; break; }
                    for (LONG i = (LONG)count - 1; i >= 0; i--) spin_unlock(&sorted[i]->lock);
                    if (all_sig) { final_status = (NTSTATUS)(STATUS_ABANDONED_WAIT_0); break; }
                    else { final_status = STATUS_TIMEOUT; break; }
                } else {
                    final_status = (NTSTATUS)(STATUS_ABANDONED_WAIT_0 + aband_idx);
                    break;
                }
            }
            final_status = STATUS_TIMEOUT; break;
        }
        if (jiffies_timeout != MAX_SCHEDULE_TIMEOUT && remaining_jiffies <= 0) { final_status = STATUS_TIMEOUT; break; }

        /* alert check before blocking */
        if (alertable) {
            unsigned long flags; spin_lock_irqsave(&thr->lock, flags);
            bool apc = thr->alert_pending, al = thr->alerted;
            if (apc) { thr->alert_pending = false; spin_unlock_irqrestore(&thr->lock, flags); final_status = STATUS_USER_APC; break; }
            if (al) { thr->alerted = false; spin_unlock_irqrestore(&thr->lock, flags); final_status = STATUS_ALERTED; break; }
            spin_unlock_irqrestore(&thr->lock, flags);
        }

        /* insert wait blocks */
        if (wait_type == WaitAny) {
            for (ULONG i = 0; i < count; i++) {
                struct nt_dispatcher_header *h = objects[i];
                struct nt_wait_block *blk = &ctx->blocks[i];
                blk->ctx = ctx; blk->object = h; blk->original_index = i;
                unsigned long flags; spin_lock_irqsave(&h->lock, flags);
                list_add_tail(&blk->entry, &h->waiters);
                spin_unlock_irqrestore(&h->lock, flags);
            }
        } else {
            for (ULONG i = 0; i < count; i++) spin_lock(&sorted[i]->lock);
            for (ULONG i = 0; i < count; i++) {
                struct nt_dispatcher_header *h = sorted[i];
                for (ULONG k = 0; k < count; k++) if (objects[k] == h) {
                    struct nt_wait_block *blk = &ctx->blocks[k];
                    blk->ctx = ctx; blk->object = h; blk->original_index = k;
                    list_add_tail(&blk->entry, &h->waiters);
                    break;
                }
            }
            for (LONG i = (LONG)count - 1; i >= 0; i--) spin_unlock(&sorted[i]->lock);
        }

        /* block */
        DEFINE_WAIT(wait);
        prepare_to_wait(&ctx->wq, &wait, alertable ? TASK_INTERRUPTIBLE : TASK_UNINTERRUPTIBLE);
        /* Recheck woken before sleeping to avoid lost wakeup between insert and prepare */
        {
            unsigned long flags; spin_lock_irqsave(&ctx->lock, flags);
            bool w = ctx->woken;
            spin_unlock_irqrestore(&ctx->lock, flags);
            if (!w) {
                if (jiffies_timeout == MAX_SCHEDULE_TIMEOUT) {
                    schedule();
                } else {
                    remaining_jiffies = schedule_timeout(remaining_jiffies);
                }
            }
        }
        finish_wait(&ctx->wq, &wait);

        /* remove blocks */
        if (wait_type == WaitAny) {
            for (ULONG i = 0; i < count; i++) {
                struct nt_dispatcher_header *h = objects[i];
                struct nt_wait_block *blk = &ctx->blocks[i];
                unsigned long flags; spin_lock_irqsave(&h->lock, flags);
                if (blk->entry.next) { list_del(&blk->entry); blk->entry.next = NULL; blk->entry.prev = NULL; }
                spin_unlock_irqrestore(&h->lock, flags);
            }
        } else {
            for (ULONG i = 0; i < count; i++) spin_lock(&sorted[i]->lock);
            for (ULONG i = 0; i < count; i++) {
                struct nt_dispatcher_header *h = sorted[i];
                for (ULONG k = 0; k < count; k++) if (objects[k] == h) {
                    struct nt_wait_block *blk = &ctx->blocks[k];
                    if (blk->entry.next) { list_del(&blk->entry); blk->entry.next = NULL; blk->entry.prev = NULL; }
                    break;
                }
            }
            for (LONG i = (LONG)count - 1; i >= 0; i--) spin_unlock(&sorted[i]->lock);
        }

        /* reset woken for next iteration */
        {
            unsigned long flags; spin_lock_irqsave(&ctx->lock, flags);
            ctx->woken = false;
            spin_unlock_irqrestore(&ctx->lock, flags);
        }

        if (alertable) {
            unsigned long flags; spin_lock_irqsave(&thr->lock, flags);
            bool apc = thr->alert_pending, al = thr->alerted;
            if (apc) { thr->alert_pending = false; spin_unlock_irqrestore(&thr->lock, flags); final_status = STATUS_USER_APC; break; }
            if (al) { thr->alerted = false; spin_unlock_irqrestore(&thr->lock, flags); final_status = STATUS_ALERTED; break; }
            spin_unlock_irqrestore(&thr->lock, flags);
        }
        if (jiffies_timeout != MAX_SCHEDULE_TIMEOUT && remaining_jiffies <= 0) {
            /* loop will retry try_acquire once more before declaring timeout – handles race at deadline */
            continue;
        }
    }

    if (atomic_dec_and_test(&ctx->refcount)) kfree(ctx);
    return final_status;
}


#include "object.h"
#include "handle.h"
NTSTATUS nt_svc_NtWaitForMultipleObjects(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    ULONG count = (ULONG)a0;
    HANDLE __user *handles = (HANDLE __user *)a1;
    ULONG wait_type = (ULONG)a2;
    BOOLEAN alertable = (BOOLEAN)a3;
    PLARGE_INTEGER timeout = (PLARGE_INTEGER)a4;
    (void)a5;(void)a6;(void)a7;(void)a8;(void)a9;(void)a10;(void)a11;
    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS) return STATUS_INVALID_PARAMETER;
    if (wait_type != WaitAny && wait_type != WaitAll) return STATUS_INVALID_PARAMETER;
    if (!handles) return STATUS_INVALID_PARAMETER;
    if (!access_ok(handles, count * sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION;
    if (timeout && !access_ok(timeout, sizeof(LARGE_INTEGER))) return STATUS_ACCESS_VIOLATION;
    HANDLE k_handles[MAXIMUM_WAIT_OBJECTS];
    if (copy_from_user(k_handles, handles, count * sizeof(HANDLE))) return STATUS_ACCESS_VIOLATION;
    LARGE_INTEGER k_timeout;
    PLARGE_INTEGER p_timeout = NULL;
    if (timeout) {
        if (copy_from_user(&k_timeout, timeout, sizeof(k_timeout))) return STATUS_ACCESS_VIOLATION;
        p_timeout = &k_timeout;
    }
    struct nt_dispatcher_header *hdrs[MAXIMUM_WAIT_OBJECTS];
    struct nt_object_header *obj_hdrs[MAXIMUM_WAIT_OBJECTS];
    NTSTATUS st = STATUS_SUCCESS;
    extern struct nt_eprocess *nt_get_current_eprocess(void);
    extern struct nt_object_type nt_event_type, nt_mutant_type, nt_semaphore_type, nt_timer_type;
    struct nt_eprocess *proc = nt_get_current_eprocess();
    if (!proc) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < count; i++) {
        struct nt_object_header *hdr = NULL;
        st = ObReferenceObjectByHandle(proc, k_handles[i], NULL, UserMode, 0, &hdr, NULL);
        if (!NT_SUCCESS(st)) {
            for (ULONG j = 0; j < i; j++) ObDereferenceObject(obj_hdrs[j]);
            return st;
        }
        /* Only event, mutant, semaphore and timer bodies begin with a dispatcher
         * header. Any other object type would be read as one. */
        if (hdr->type != &nt_event_type && hdr->type != &nt_mutant_type &&
            hdr->type != &nt_semaphore_type && hdr->type != &nt_timer_type) {
            ObDereferenceObject(hdr);
            for (ULONG j = 0; j < i; j++) ObDereferenceObject(obj_hdrs[j]);
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
        obj_hdrs[i] = hdr;
        hdrs[i] = (struct nt_dispatcher_header *)nt_object_body(hdr);
    }
    st = nt_wait_for_multiple_objects(count, hdrs, wait_type, alertable, p_timeout);
    for (ULONG i = 0; i < count; i++) ObDereferenceObject(obj_hdrs[i]);
    return st;
}

NTSTATUS nt_wait_for_single_object(struct nt_dispatcher_header *obj, BOOLEAN alertable, LARGE_INTEGER *timeout)
{
    return nt_wait_for_multiple_objects(1, &obj, WaitAny, alertable, timeout);
}

#endif /* __KERNEL__ real */

/* ------------------------------------------------------------------ */
/* Timer and keyed-event objects: syscall wrappers (both modes)        */
/* One body serves the userspace shim and the kernel. User pointers    */
/* go through access_ok / copy_*_user, which kshim.h maps to plain     */
/* memory in the userspace build.                                      */
/* ------------------------------------------------------------------ */
#include "object.h"
#include "handle.h"

#ifdef __KERNEL__
extern struct nt_eprocess *nt_get_current_eprocess(void);
#define SYNC_CURRENT_PROCESS() nt_get_current_eprocess()
#else
#define SYNC_CURRENT_PROCESS() kshim_current_process()
#endif

static void timer_obj_delete(struct nt_object_header *hdr);
static void keyed_event_obj_delete(struct nt_object_header *hdr);

struct nt_object_type nt_timer_type = {
    .name = "Timer",
    .ops = { .delete_obj = timer_obj_delete, .close = NULL, },
    .pool_tag = 0,
};

struct nt_object_type nt_keyed_event_type = {
    .name = "KeyedEvent",
    .ops = { .delete_obj = keyed_event_obj_delete, .close = NULL, },
    .pool_tag = 0,
};

/* Initialise a timer body that lives inside an object-manager allocation. */
static void timer_object_init(struct nt_timer *t, bool manual)
{
#ifndef __KERNEL__
    t->header.type = NT_OBJ_TIMER;
    pthread_mutex_init(&t->header.lock, NULL);
    INIT_LIST_HEAD(&t->header.waiters);
    t->header.signal_state = 0;
    t->header.manual_reset = manual;
    atomic_init(&t->header.refcount, 1);
    pthread_mutex_init(&t->timer_lock, NULL);
    t->thread_running = false;
#else
    t->header.type = NT_OBJ_TIMER;
    spin_lock_init(&t->header.lock);
    INIT_LIST_HEAD(&t->header.waiters);
    t->header.signal_state = 0;
    t->header.manual_reset = manual;
    atomic_set(&t->header.refcount, 1);
    init_waitqueue_head(&t->header.wq);
    spin_lock_init(&t->timer_lock);
    timer_setup(&t->timer, timer_callback, 0);
#endif
    t->active = false;
    t->cancel_requested = false;
    t->period_ms = 0;
    t->expiry_100ns = 0;
    t->apc_routine = NULL;
    t->apc_ctx = NULL;
    t->apc_thread = NULL;
}

/* Last reference dropped: stop the timer, then let its thread leave. */
static void timer_obj_delete(struct nt_object_header *hdr)
{
    struct nt_timer *t = (struct nt_timer *)nt_object_body(hdr);
    BOOLEAN dummy = 0;
    nt_cancel_timer(t, &dummy);
#ifndef __KERNEL__
    /* The timer thread is detached and still reads t, so wait for it to exit. */
    for (;;) {
        pthread_mutex_lock(&t->timer_lock);
        bool running = t->thread_running;
        pthread_mutex_unlock(&t->timer_lock);
        if (!running) break;
        usleep(1000);
    }
    pthread_mutex_destroy(&t->timer_lock);
    pthread_mutex_destroy(&t->header.lock);
#endif
}

static void keyed_event_obj_delete(struct nt_object_header *hdr)
{
#ifndef __KERNEL__
    struct nt_keyed_event *k = (struct nt_keyed_event *)nt_object_body(hdr);
    pthread_mutex_destroy(&k->header.lock);
#else
    (void)hdr;
#endif
}

/* Copy an OBJECT_ATTRIBUTES and its name from user memory (as nt_obj.c does). */
static NTSTATUS sync_copy_oa(POBJECT_ATTRIBUTES uptr, OBJECT_ATTRIBUTES *k_oa, UNICODE_STRING *k_name,
                             WCHAR *k_buf, size_t k_buf_bytes, POBJECT_ATTRIBUTES *out_oa)
{
    if (!uptr) { memset(k_oa, 0, sizeof(*k_oa)); k_oa->Length = sizeof(OBJECT_ATTRIBUTES); *out_oa = NULL; return STATUS_SUCCESS; }
    if (!access_ok(uptr, sizeof(OBJECT_ATTRIBUTES))) return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(k_oa, uptr, sizeof(OBJECT_ATTRIBUTES))) return STATUS_ACCESS_VIOLATION;
    if (k_oa->Length != sizeof(OBJECT_ATTRIBUTES)) return STATUS_INVALID_PARAMETER;
    if (k_oa->ObjectName) {
        if (!access_ok(k_oa->ObjectName, sizeof(UNICODE_STRING))) return STATUS_ACCESS_VIOLATION;
        UNICODE_STRING tmp;
        if (copy_from_user(&tmp, k_oa->ObjectName, sizeof(tmp))) return STATUS_ACCESS_VIOLATION;
        if (tmp.Length) {
            if (!access_ok(tmp.Buffer, tmp.Length)) return STATUS_ACCESS_VIOLATION;
            if ((size_t)tmp.Length > k_buf_bytes) return STATUS_NAME_TOO_LONG;
            if (copy_from_user(k_buf, tmp.Buffer, tmp.Length)) return STATUS_ACCESS_VIOLATION;
            k_name->Buffer = k_buf; k_name->Length = tmp.Length; k_name->MaximumLength = tmp.Length;
            k_oa->ObjectName = k_name;
        } else {
            k_oa->ObjectName = NULL;
        }
    }
    *out_oa = k_oa;
    return STATUS_SUCCESS;
}

static NTSTATUS sync_ensure_handle_table(struct nt_eprocess *proc)
{
    if (!proc) return STATUS_INVALID_PARAMETER;
    if (proc->handle_table) return STATUS_SUCCESS;
    return ObCreateHandleTable(proc, &proc->handle_table);
}

/* Create a timer, or open the existing one when the name is taken (as NtCreateEvent). */
static NTSTATUS timer_create_internal(struct nt_eprocess *proc, ACCESS_MASK access,
                                      POBJECT_ATTRIBUTES oa, ULONG timer_type, PHANDLE out)
{
    if (timer_type != NotificationTimer && timer_type != SynchronizationTimer)
        return STATUS_INVALID_PARAMETER;
    if (oa && oa->ObjectName && oa->ObjectName->Length) {
        struct nt_object_header *existing = NULL;
        NTSTATUS st = ObReferenceObjectByName(oa->ObjectName, &nt_timer_type, UserMode, access, &existing);
        if (NT_SUCCESS(st)) {
            ULONG hattr = 0;
            if (oa->Attributes & OBJ_INHERIT) hattr |= OBJ_INHERIT;
            if (oa->Attributes & OBJ_PROTECT_FROM_CLOSE) hattr |= OBJ_PROTECT_FROM_CLOSE;
            HANDLE h = NULL;
            st = ObCreateHandle(proc, existing, access, hattr, &h);
            ObDereferenceObject(existing);
            if (NT_SUCCESS(st)) { *out = h; return STATUS_OBJECT_NAME_COLLISION; }
            return st;
        }
    }
    struct nt_object_header *hdr = NULL;
    struct nt_timer *t = NULL;
    OBJECT_ATTRIBUTES tmp = {0};
    if (oa) tmp = *oa; else tmp.Length = sizeof(tmp);
    NTSTATUS st = ObCreateObject(&nt_timer_type, &tmp, sizeof(*t), &hdr, (void **)&t);
    if (!NT_SUCCESS(st)) return st;
    timer_object_init(t, timer_type == NotificationTimer);
    st = ObInsertObject(hdr, oa, access, proc, out);
    ObDereferenceObject(hdr);
    return st;
}

/* Open an existing named object of the given type. */
static NTSTATUS sync_open_named(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES oa,
                                struct nt_object_type *type, struct nt_eprocess *proc)
{
    if (!oa || !oa->ObjectName) return STATUS_INVALID_PARAMETER;
    HANDLE h = NULL;
    NTSTATUS st = ObOpenObjectByName(oa, type, UserMode, access, proc, &h);
    if (NT_SUCCESS(st)) *out = h;
    return st;
}

NTSTATUS nt_svc_NtCreateTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                              ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                              ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    PHANDLE TimerHandle = (PHANDLE)a0;
    ACCESS_MASK DesiredAccess = (ACCESS_MASK)a1;
    POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)a2;
    ULONG TimerType = (ULONG)a3;
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!TimerHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(TimerHandle, sizeof(*TimerHandle))) return STATUS_ACCESS_VIOLATION;
    struct nt_eprocess *proc = SYNC_CURRENT_PROCESS();
    if (!proc) return STATUS_INVALID_PARAMETER;
    NTSTATUS st = sync_ensure_handle_table(proc);
    if (!NT_SUCCESS(st)) return st;
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[256];
    POBJECT_ATTRIBUTES oa = NULL;
    st = sync_copy_oa(ObjectAttributes, &k_oa, &k_name, k_buf, sizeof(k_buf), &oa);
    if (!NT_SUCCESS(st)) return st;
    HANDLE h = NULL;
    st = timer_create_internal(proc, DesiredAccess, oa, TimerType, &h);
    if (NT_SUCCESS(st) || st == STATUS_OBJECT_NAME_COLLISION) {
        if (copy_to_user(TimerHandle, &h, sizeof(h))) return STATUS_ACCESS_VIOLATION;
    }
    return st;
}

NTSTATUS nt_svc_NtOpenTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                            ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                            ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    PHANDLE TimerHandle = (PHANDLE)a0;
    ACCESS_MASK DesiredAccess = (ACCESS_MASK)a1;
    POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)a2;
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!TimerHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(TimerHandle, sizeof(*TimerHandle))) return STATUS_ACCESS_VIOLATION;
    struct nt_eprocess *proc = SYNC_CURRENT_PROCESS();
    if (!proc) return STATUS_INVALID_PARAMETER;
    NTSTATUS st = sync_ensure_handle_table(proc);
    if (!NT_SUCCESS(st)) return st;
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[256];
    POBJECT_ATTRIBUTES oa = NULL;
    st = sync_copy_oa(ObjectAttributes, &k_oa, &k_name, k_buf, sizeof(k_buf), &oa);
    if (!NT_SUCCESS(st)) return st;
    HANDLE h = NULL;
    st = sync_open_named(&h, DesiredAccess, oa, &nt_timer_type, proc);
    if (NT_SUCCESS(st) && copy_to_user(TimerHandle, &h, sizeof(h))) return STATUS_ACCESS_VIOLATION;
    return st;
}

/* Resolve a timer handle to its body; the caller drops *hdr with ObDereferenceObject. */
static NTSTATUS timer_ref(struct nt_eprocess *proc, HANDLE h, struct nt_timer **out_t,
                          struct nt_object_header **out_hdr)
{
    struct nt_object_header *hdr = NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, h, &nt_timer_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    *out_t = (struct nt_timer *)nt_object_body(hdr);
    *out_hdr = hdr;
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtSetTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                           ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                           ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    HANDLE TimerHandle = (HANDLE)a0;
    PLARGE_INTEGER DueTime = (PLARGE_INTEGER)a1;
    void (*TimerApcRoutine)(void *) = (void (*)(void *))a2;
    void *TimerContext = (void *)a3;
    /* ResumeTimer: timers here are never suspended, so there is nothing to resume. */
    LONG Period = (LONG)a5;
    PBOOLEAN PreviousState = (PBOOLEAN)a6;
    (void)a4; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!DueTime) return STATUS_INVALID_PARAMETER;
    if (!access_ok(DueTime, sizeof(LARGE_INTEGER))) return STATUS_ACCESS_VIOLATION;
    LARGE_INTEGER due;
    if (copy_from_user(&due, DueTime, sizeof(due))) return STATUS_ACCESS_VIOLATION;
    if (Period < 0) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *proc = SYNC_CURRENT_PROCESS();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_timer *t = NULL;
    struct nt_object_header *hdr = NULL;
    NTSTATUS st = timer_ref(proc, TimerHandle, &t, &hdr);
    if (!NT_SUCCESS(st)) return st;
    BOOLEAN prev = 0;
    st = nt_cancel_timer(t, &prev);   /* stop any earlier arming and read the old state */
    if (NT_SUCCESS(st)) st = nt_set_timer(t, &due, Period, TimerApcRoutine, TimerContext);
    if (NT_SUCCESS(st) && PreviousState) {
        if (!access_ok(PreviousState, sizeof(BOOLEAN)) || copy_to_user(PreviousState, &prev, sizeof(prev)))
            st = STATUS_ACCESS_VIOLATION;
    }
    ObDereferenceObject(hdr);
    return st;
}

NTSTATUS nt_svc_NtCancelTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                              ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                              ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    HANDLE TimerHandle = (HANDLE)a0;
    PBOOLEAN CurrentState = (PBOOLEAN)a1;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    struct nt_eprocess *proc = SYNC_CURRENT_PROCESS();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_timer *t = NULL;
    struct nt_object_header *hdr = NULL;
    NTSTATUS st = timer_ref(proc, TimerHandle, &t, &hdr);
    if (!NT_SUCCESS(st)) return st;
    BOOLEAN cur = 0;
    st = nt_cancel_timer(t, &cur);
    if (NT_SUCCESS(st) && CurrentState) {
        if (!access_ok(CurrentState, sizeof(BOOLEAN)) || copy_to_user(CurrentState, &cur, sizeof(cur)))
            st = STATUS_ACCESS_VIOLATION;
    }
    ObDereferenceObject(hdr);
    return st;
}

/* Current time in NT units (100 ns since 1601-01-01). */
static int64_t timer_now_100ns(void)
{
#ifndef __KERNEL__
    struct timespec now;
    now_realtime(&now);
    return timespec_to_100ns(&now) + 116444736000000000LL;
#else
    struct timespec64 ts;
    ktime_get_real_ts64(&ts);
    return (int64_t)ts.tv_sec * 10000000LL + ts.tv_nsec / 100 + 116444736000000000LL;
#endif
}

/* Read the fields NtQueryTimer reports, each under its own lock. */
static void timer_snapshot(struct nt_timer *t, bool *active, int64_t *expiry, bool *signalled)
{
#ifndef __KERNEL__
    pthread_mutex_lock(&t->timer_lock);
    *active = t->active;
    *expiry = t->expiry_100ns;
    pthread_mutex_unlock(&t->timer_lock);
    pthread_mutex_lock(&t->header.lock);
    *signalled = t->header.signal_state != 0;
    pthread_mutex_unlock(&t->header.lock);
#else
    unsigned long flags;
    spin_lock_irqsave(&t->timer_lock, flags);
    *active = t->active;
    *expiry = t->expiry_100ns;
    spin_unlock_irqrestore(&t->timer_lock, flags);
    spin_lock_irqsave(&t->header.lock, flags);
    *signalled = t->header.signal_state != 0;
    spin_unlock_irqrestore(&t->header.lock, flags);
#endif
}

/* TIMER_BASIC_INFORMATION, the only class NtQueryTimer supports. */
struct sync_timer_basic_info {
    LARGE_INTEGER RemainingTime;
    BOOLEAN       TimerState;
};

NTSTATUS nt_svc_NtQueryTimer(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                             ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                             ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    HANDLE TimerHandle = (HANDLE)a0;
    ULONG TimerInformationClass = (ULONG)a1;
    PVOID TimerInformation = (PVOID)a2;
    ULONG TimerInformationLength = (ULONG)a3;
    PULONG ReturnLength = (PULONG)a4;
    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (TimerInformationClass != 0) return STATUS_INVALID_INFO_CLASS;
    const ULONG needed = sizeof(struct sync_timer_basic_info);
    if (ReturnLength) {
        if (!access_ok(ReturnLength, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
        ULONG n = needed;
        if (copy_to_user(ReturnLength, &n, sizeof(n))) return STATUS_ACCESS_VIOLATION;
    }
    if (TimerInformationLength != needed) return STATUS_INFO_LENGTH_MISMATCH;
    if (!TimerInformation || !access_ok(TimerInformation, needed)) return STATUS_ACCESS_VIOLATION;
    struct nt_eprocess *proc = SYNC_CURRENT_PROCESS();
    if (!proc) return STATUS_INVALID_PARAMETER;
    struct nt_timer *t = NULL;
    struct nt_object_header *hdr = NULL;
    NTSTATUS st = timer_ref(proc, TimerHandle, &t, &hdr);
    if (!NT_SUCCESS(st)) return st;
    bool active = false, signalled = false;
    int64_t expiry = 0;
    timer_snapshot(t, &active, &expiry, &signalled);
    struct sync_timer_basic_info info;
    int64_t remaining = 0;
    if (active) {
        int64_t now = timer_now_100ns();
        if (expiry > now) remaining = expiry - now;
    }
    info.RemainingTime.QuadPart = remaining;
    info.TimerState = signalled ? 1 : 0;
    st = STATUS_SUCCESS;
    if (copy_to_user(TimerInformation, &info, needed)) st = STATUS_ACCESS_VIOLATION;
    ObDereferenceObject(hdr);
    return st;
}

extern struct nt_object_type nt_event_type, nt_mutant_type, nt_semaphore_type;

/* Signal one object, then wait on another. Both handles are resolved first so that a
 * bad handle fails before anything is signalled. */
NTSTATUS nt_svc_NtSignalAndWaitForSingleObject(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                               ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                               ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    HANDLE SignalHandle = (HANDLE)a0;
    HANDLE WaitHandle = (HANDLE)a1;
    BOOLEAN Alertable = (BOOLEAN)a2;
    PLARGE_INTEGER Timeout = (PLARGE_INTEGER)a3;
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    struct nt_eprocess *proc = SYNC_CURRENT_PROCESS();
    if (!proc) return STATUS_INVALID_PARAMETER;
    LARGE_INTEGER k_timeout;
    PLARGE_INTEGER p_timeout = NULL;
    if (Timeout) {
        if (!access_ok(Timeout, sizeof(LARGE_INTEGER))) return STATUS_ACCESS_VIOLATION;
        if (copy_from_user(&k_timeout, Timeout, sizeof(k_timeout))) return STATUS_ACCESS_VIOLATION;
        p_timeout = &k_timeout;
    }
    struct nt_object_header *sig_hdr = NULL, *wait_hdr = NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, SignalHandle, NULL, UserMode, 0, &sig_hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = ObReferenceObjectByHandle(proc, WaitHandle, NULL, UserMode, 0, &wait_hdr, NULL);
    if (!NT_SUCCESS(st)) { ObDereferenceObject(sig_hdr); return st; }
    if (sig_hdr->type != &nt_event_type && sig_hdr->type != &nt_mutant_type &&
        sig_hdr->type != &nt_semaphore_type) {
        st = STATUS_OBJECT_TYPE_MISMATCH;
    } else if (wait_hdr->type != &nt_event_type && wait_hdr->type != &nt_mutant_type &&
               wait_hdr->type != &nt_semaphore_type && wait_hdr->type != &nt_timer_type) {
        st = STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (NT_SUCCESS(st)) {
        LONG prev = 0;
        void *sbody = nt_object_body(sig_hdr);
        if (sig_hdr->type == &nt_event_type) st = nt_set_event((struct nt_event *)sbody, &prev);
        else if (sig_hdr->type == &nt_mutant_type) st = nt_release_mutant((struct nt_mutant *)sbody, &prev);
        else st = nt_release_semaphore((struct nt_semaphore *)sbody, 1, &prev);
    }
    if (NT_SUCCESS(st)) {
        st = nt_wait_for_single_object((struct nt_dispatcher_header *)nt_object_body(wait_hdr), Alertable, p_timeout);
    }
    ObDereferenceObject(wait_hdr);
    ObDereferenceObject(sig_hdr);
    return st;
}

/* Keyed event body: the wait list lives in the global keyed table, so the header only needs init. */
static void keyed_object_init(struct nt_keyed_event *k)
{
#ifndef __KERNEL__
    k->header.type = NT_OBJ_KEYED_EVENT;
    pthread_mutex_init(&k->header.lock, NULL);
    INIT_LIST_HEAD(&k->header.waiters);
    k->header.signal_state = 0;
    k->header.manual_reset = false;
    atomic_init(&k->header.refcount, 1);
#else
    k->header.type = NT_OBJ_KEYED_EVENT;
    spin_lock_init(&k->header.lock);
    INIT_LIST_HEAD(&k->header.waiters);
    k->header.signal_state = 0;
    k->header.manual_reset = false;
    atomic_set(&k->header.refcount, 1);
    init_waitqueue_head(&k->header.wq);
#endif
}

static NTSTATUS keyed_create_internal(struct nt_eprocess *proc, ACCESS_MASK access,
                                      POBJECT_ATTRIBUTES oa, PHANDLE out)
{
    if (oa && oa->ObjectName && oa->ObjectName->Length) {
        struct nt_object_header *existing = NULL;
        NTSTATUS st = ObReferenceObjectByName(oa->ObjectName, &nt_keyed_event_type, UserMode, access, &existing);
        if (NT_SUCCESS(st)) {
            ULONG hattr = 0;
            if (oa->Attributes & OBJ_INHERIT) hattr |= OBJ_INHERIT;
            if (oa->Attributes & OBJ_PROTECT_FROM_CLOSE) hattr |= OBJ_PROTECT_FROM_CLOSE;
            HANDLE h = NULL;
            st = ObCreateHandle(proc, existing, access, hattr, &h);
            ObDereferenceObject(existing);
            if (NT_SUCCESS(st)) { *out = h; return STATUS_OBJECT_NAME_COLLISION; }
            return st;
        }
    }
    struct nt_object_header *hdr = NULL;
    struct nt_keyed_event *k = NULL;
    OBJECT_ATTRIBUTES tmp = {0};
    if (oa) tmp = *oa; else tmp.Length = sizeof(tmp);
    NTSTATUS st = ObCreateObject(&nt_keyed_event_type, &tmp, sizeof(*k), &hdr, (void **)&k);
    if (!NT_SUCCESS(st)) return st;
    keyed_object_init(k);
    st = ObInsertObject(hdr, oa, access, proc, out);
    ObDereferenceObject(hdr);
    return st;
}

NTSTATUS nt_svc_NtCreateKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                   ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                   ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    PHANDLE KeyedEventHandle = (PHANDLE)a0;
    ACCESS_MASK DesiredAccess = (ACCESS_MASK)a1;
    POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)a2;
    ULONG Flags = (ULONG)a3;
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!KeyedEventHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(KeyedEventHandle, sizeof(*KeyedEventHandle))) return STATUS_ACCESS_VIOLATION;
    if (Flags != 0) return STATUS_INVALID_PARAMETER;
    struct nt_eprocess *proc = SYNC_CURRENT_PROCESS();
    if (!proc) return STATUS_INVALID_PARAMETER;
    NTSTATUS st = sync_ensure_handle_table(proc);
    if (!NT_SUCCESS(st)) return st;
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[256];
    POBJECT_ATTRIBUTES oa = NULL;
    st = sync_copy_oa(ObjectAttributes, &k_oa, &k_name, k_buf, sizeof(k_buf), &oa);
    if (!NT_SUCCESS(st)) return st;
    HANDLE h = NULL;
    st = keyed_create_internal(proc, DesiredAccess, oa, &h);
    if (NT_SUCCESS(st) || st == STATUS_OBJECT_NAME_COLLISION) {
        if (copy_to_user(KeyedEventHandle, &h, sizeof(h))) return STATUS_ACCESS_VIOLATION;
    }
    return st;
}

NTSTATUS nt_svc_NtOpenKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                 ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                 ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    PHANDLE KeyedEventHandle = (PHANDLE)a0;
    ACCESS_MASK DesiredAccess = (ACCESS_MASK)a1;
    POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)a2;
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!KeyedEventHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(KeyedEventHandle, sizeof(*KeyedEventHandle))) return STATUS_ACCESS_VIOLATION;
    struct nt_eprocess *proc = SYNC_CURRENT_PROCESS();
    if (!proc) return STATUS_INVALID_PARAMETER;
    NTSTATUS st = sync_ensure_handle_table(proc);
    if (!NT_SUCCESS(st)) return st;
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[256];
    POBJECT_ATTRIBUTES oa = NULL;
    st = sync_copy_oa(ObjectAttributes, &k_oa, &k_name, k_buf, sizeof(k_buf), &oa);
    if (!NT_SUCCESS(st)) return st;
    HANDLE h = NULL;
    st = sync_open_named(&h, DesiredAccess, oa, &nt_keyed_event_type, proc);
    if (NT_SUCCESS(st) && copy_to_user(KeyedEventHandle, &h, sizeof(h))) return STATUS_ACCESS_VIOLATION;
    return st;
}

/* Shared body of NtWaitForKeyedEvent and NtReleaseKeyedEvent. */
static NTSTATUS keyed_event_op(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3, bool wait)
{
    HANDLE KeyedEventHandle = (HANDLE)a0;
    void *Key = (void *)a1;
    BOOLEAN Alertable = (BOOLEAN)a2;
    PLARGE_INTEGER Timeout = (PLARGE_INTEGER)a3;
    struct nt_eprocess *proc = SYNC_CURRENT_PROCESS();
    if (!proc) return STATUS_INVALID_PARAMETER;
    LARGE_INTEGER k_timeout;
    PLARGE_INTEGER p_timeout = NULL;
    if (Timeout) {
        if (!access_ok(Timeout, sizeof(LARGE_INTEGER))) return STATUS_ACCESS_VIOLATION;
        if (copy_from_user(&k_timeout, Timeout, sizeof(k_timeout))) return STATUS_ACCESS_VIOLATION;
        p_timeout = &k_timeout;
    }
    struct nt_object_header *hdr = NULL;
    NTSTATUS st = ObReferenceObjectByHandle(proc, KeyedEventHandle, &nt_keyed_event_type, UserMode, 0, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    struct nt_keyed_event *k = (struct nt_keyed_event *)nt_object_body(hdr);
    if (wait) st = nt_wait_for_keyed_event(k, Key, Alertable, p_timeout);
    else st = nt_release_keyed_event(k, Key, Alertable, p_timeout);
    ObDereferenceObject(hdr);
    return st;
}

NTSTATUS nt_svc_NtWaitForKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                    ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                    ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    return keyed_event_op(a0, a1, a2, a3, true);
}

NTSTATUS nt_svc_NtReleaseKeyedEvent(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                    ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                    ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    return keyed_event_op(a0, a1, a2, a3, false);
}

/* Give up the processor to another ready thread. Always reports success: sched_yield and
 * cond_resched do not say whether a switch happened, so STATUS_NO_YIELD_PERFORMED is never returned. */
NTSTATUS nt_svc_NtYieldExecution(ULONG_PTR a0, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                                 ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6, ULONG_PTR a7,
                                 ULONG_PTR a8, ULONG_PTR a9, ULONG_PTR a10, ULONG_PTR a11)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
#ifndef __KERNEL__
    sched_yield();
#else
    cond_resched();
#endif
    return STATUS_SUCCESS;
}
