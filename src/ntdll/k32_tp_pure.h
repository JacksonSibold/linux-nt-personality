/*
 * k32_tp_pure.h - the thread pool engine behind QueueUserWorkItem, the Vista thread pool (work and timer objects) and the
 * timer queues: a pool of worker threads that grows on demand up to a limit and shrinks when idle, a FIFO of tasks, work
 * objects that can be waited for and cancelled, and timers served by one timer thread.
 *
 * The engine reaches the system only through ktp_os (a lock, condition variables, thread creation, a monotonic clock and an
 * allocator), so the same code runs on kernel32 in the DLL and on pthreads in the host tests, where it is exercised with
 * several threads at once. One lock protects all state; callbacks run outside it. Host tests: test_k32_tp_pure.c.
 */
#ifndef K32_TP_PURE_H
#define K32_TP_PURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct ktp_os {
    void *(*lock_new)(void);
    void (*lock)(void *lock);
    void (*unlock)(void *lock);
    void *(*cond_new)(void);
    /* Waits on the condition with the lock held; returns 0 when signalled (or spuriously), 1 on timeout. -1 waits forever. */
    int (*cond_wait)(void *cond, void *lock, uint64_t timeout_ms);
    void (*cond_signal)(void *cond);
    void (*cond_broadcast)(void *cond);
    int (*spawn)(void (*fn)(void *), void *arg); /* 0 on success */
    uint64_t (*now_ms)(void);
    void *(*alloc)(size_t n);
    void (*release)(void *p);
    /* Events for cancelling a handle wait: a manual-reset event, set to wake the waiting thread. */
    void *(*event_new)(void);
    void (*event_set)(void *event);
    void (*event_free)(void *event);
    /* Waits for the handle to be signalled, the cancel event to be set, or the timeout (-1: none).
     * Returns 0 for the handle, 1 for the timeout, 2 for the cancel event. */
    int (*wait2)(void *handle, void *cancel, uint64_t timeout_ms);
} ktp_os;

typedef struct ktp_task {
    struct ktp_task *next;
    void (*fn)(void *);
    void *ctx;
} ktp_task;

struct ktp;

typedef struct ktp_timer {
    struct ktp *tp;
    void (*fn)(void *ctx, struct ktp_timer *t);
    void *ctx;
    uint64_t due, period; /* absolute milliseconds on the clock of ktp_os, and the repeat interval (0: once) */
    int active;
    unsigned outstanding; /* callbacks queued or running */
    struct ktp_timer *next;
} ktp_timer;

typedef struct ktp_work {
    struct ktp *tp;
    void (*fn)(void *ctx, struct ktp_work *w);
    void *ctx;
    unsigned pending; /* submitted and not finished */
} ktp_work;

struct ktp_wait;

typedef struct ktp {
    const ktp_os *os;
    void *lock;
    void *work_cond;  /* idle workers wait here */
    void *done_cond;  /* broadcast whenever a callback finishes or is cancelled */
    void *timer_cond; /* the timer thread waits here */
    ktp_task *head, *tail;
    unsigned queued;
    unsigned workers, idle, min_workers, max_workers;
    uint64_t idle_ms;
    ktp_timer *timers;
    int timer_thread;
} ktp;

#define KTP_FOREVER ((uint64_t)-1)

static inline int ktp_init(ktp *tp, const ktp_os *os)
{
    memset(tp, 0, sizeof *tp);
    tp->os = os;
    tp->lock = os->lock_new();
    tp->work_cond = os->cond_new();
    tp->done_cond = os->cond_new();
    tp->timer_cond = os->cond_new();
    tp->min_workers = 0;
    tp->max_workers = 500;
    tp->idle_ms = 30000;
    return tp->lock && tp->work_cond && tp->done_cond && tp->timer_cond ? 0 : -1;
}

static inline void ktp_set_limits(ktp *tp, unsigned min_workers, unsigned max_workers)
{
    tp->os->lock(tp->lock);
    if (max_workers)
        tp->max_workers = max_workers;
    tp->min_workers = min_workers > tp->max_workers ? tp->max_workers : min_workers;
    tp->os->unlock(tp->lock);
}

/* ---- Workers and tasks --------------------------------------------------------------------------------------------------- */

static inline void ktp_worker(void *arg)
{
    ktp *tp = (ktp *)arg;
    const ktp_os *os = tp->os;
    os->lock(tp->lock);
    for (;;) {
        ktp_task *t;
        while (!tp->head) {
            int r;
            tp->idle++;
            r = os->cond_wait(tp->work_cond, tp->lock, tp->idle_ms);
            tp->idle--;
            if (r == 1 && !tp->head && tp->workers > tp->min_workers) {
                tp->workers--;
                os->unlock(tp->lock);
                return;
            }
        }
        t = tp->head;
        tp->head = t->next;
        if (!tp->head)
            tp->tail = NULL;
        tp->queued--;
        os->unlock(tp->lock);
        t->fn(t->ctx);
        os->release(t);
        os->lock(tp->lock);
    }
}

/* Queues a task with the lock held. Returns 0, or -1 when memory or a thread could not be had and nobody can run it. */
static inline int ktp_submit_locked(ktp *tp, void (*fn)(void *), void *ctx)
{
    const ktp_os *os = tp->os;
    ktp_task *t = (ktp_task *)os->alloc(sizeof *t);
    if (!t)
        return -1;
    t->next = NULL;
    t->fn = fn;
    t->ctx = ctx;
    if (tp->tail)
        tp->tail->next = t;
    else
        tp->head = t;
    tp->tail = t;
    tp->queued++;
    if (tp->queued > tp->idle && tp->workers < tp->max_workers) {
        tp->workers++;
        if (os->spawn(ktp_worker, tp) != 0) {
            tp->workers--;
            if (tp->workers == 0) { /* no thread at all: take the task back */
                tp->head = t->next;
                if (!tp->head)
                    tp->tail = NULL;
                tp->queued--;
                os->release(t);
                return -1;
            }
        }
    } else {
        os->cond_signal(tp->work_cond);
    }
    return 0;
}

static inline int ktp_submit(ktp *tp, void (*fn)(void *), void *ctx)
{
    int r;
    tp->os->lock(tp->lock);
    r = ktp_submit_locked(tp, fn, ctx);
    tp->os->unlock(tp->lock);
    return r;
}

/* Removes the queued tasks that are fn(ctx); returns how many. */
static inline unsigned ktp_remove_locked(ktp *tp, void (*fn)(void *), void *ctx)
{
    unsigned n = 0;
    ktp_task **pp = &tp->head, *prev = NULL;
    while (*pp) {
        ktp_task *t = *pp;
        if (t->fn == fn && t->ctx == ctx) {
            *pp = t->next;
            if (tp->tail == t)
                tp->tail = prev;
            tp->queued--;
            tp->os->release(t);
            ++n;
        } else {
            prev = t;
            pp = &t->next;
        }
    }
    return n;
}

/* ---- Work objects -------------------------------------------------------------------------------------------------------- */

static inline void ktp_run_work(void *arg)
{
    ktp_work *w = (ktp_work *)arg;
    ktp *tp = w->tp;
    w->fn(w->ctx, w);
    tp->os->lock(tp->lock);
    w->pending--;
    tp->os->cond_broadcast(tp->done_cond);
    tp->os->unlock(tp->lock);
}

static inline ktp_work *ktp_work_new(ktp *tp, void (*fn)(void *, ktp_work *), void *ctx)
{
    ktp_work *w = (ktp_work *)tp->os->alloc(sizeof *w);
    if (!w)
        return NULL;
    memset(w, 0, sizeof *w);
    w->tp = tp;
    w->fn = fn;
    w->ctx = ctx;
    return w;
}

static inline int ktp_work_submit(ktp_work *w)
{
    ktp *tp = w->tp;
    int r;
    tp->os->lock(tp->lock);
    w->pending++;
    r = ktp_submit_locked(tp, ktp_run_work, w);
    if (r)
        w->pending--;
    tp->os->unlock(tp->lock);
    return r;
}

/* Waits until every submitted callback has finished. With cancel_pending, callbacks that have not started never will. */
static inline void ktp_work_wait(ktp_work *w, int cancel_pending)
{
    ktp *tp = w->tp;
    tp->os->lock(tp->lock);
    if (cancel_pending) {
        w->pending -= ktp_remove_locked(tp, ktp_run_work, w);
        tp->os->cond_broadcast(tp->done_cond);
    }
    while (w->pending)
        tp->os->cond_wait(tp->done_cond, tp->lock, KTP_FOREVER);
    tp->os->unlock(tp->lock);
}

static inline void ktp_work_close(ktp_work *w)
{
    ktp *tp = w->tp;
    ktp_work_wait(w, 1);
    tp->os->release(w);
}

/* ---- Timers -------------------------------------------------------------------------------------------------------------- */

static inline void ktp_run_timer(void *arg)
{
    ktp_timer *t = (ktp_timer *)arg;
    ktp *tp = t->tp;
    t->fn(t->ctx, t);
    tp->os->lock(tp->lock);
    t->outstanding--;
    tp->os->cond_broadcast(tp->done_cond);
    tp->os->unlock(tp->lock);
}

/* The timer thread: sleeps until the earliest due time, queues the callbacks that are due, and exits when no timer is
 * armed (ktp_timer_set starts it again). */
static inline void ktp_timer_thread(void *arg)
{
    ktp *tp = (ktp *)arg;
    const ktp_os *os = tp->os;
    os->lock(tp->lock);
    for (;;) {
        uint64_t now = os->now_ms(), next = KTP_FOREVER;
        int armed = 0;
        for (ktp_timer *t = tp->timers; t; t = t->next) {
            if (!t->active)
                continue;
            if (t->due <= now) {
                t->outstanding++;
                if (ktp_submit_locked(tp, ktp_run_timer, t) != 0)
                    t->outstanding--;
                if (t->period) {
                    uint64_t missed = (now - t->due) / t->period;
                    t->due += (missed + 1) * t->period;
                } else {
                    t->active = 0;
                    continue;
                }
            }
            armed = 1;
            if (t->due < next)
                next = t->due;
        }
        if (!armed) {
            tp->timer_thread = 0;
            os->unlock(tp->lock);
            return;
        }
        now = os->now_ms();
        os->cond_wait(tp->timer_cond, tp->lock, next > now ? next - now : 0);
    }
}

static inline ktp_timer *ktp_timer_new(ktp *tp, void (*fn)(void *, ktp_timer *), void *ctx)
{
    ktp_timer *t = (ktp_timer *)tp->os->alloc(sizeof *t);
    if (!t)
        return NULL;
    memset(t, 0, sizeof *t);
    t->tp = tp;
    t->fn = fn;
    t->ctx = ctx;
    tp->os->lock(tp->lock);
    t->next = tp->timers;
    tp->timers = t;
    tp->os->unlock(tp->lock);
    return t;
}

/* Arms the timer to fire in due_ms and then every period_ms (0: once); a negative due cancels it. Returns 0, or -1 when the
 * timer thread could not be started. */
static inline int ktp_timer_set(ktp_timer *t, int64_t due_ms, uint64_t period_ms)
{
    ktp *tp = t->tp;
    int r = 0;
    tp->os->lock(tp->lock);
    if (due_ms < 0) {
        t->active = 0;
    } else {
        t->due = tp->os->now_ms() + (uint64_t)due_ms;
        t->period = period_ms;
        t->active = 1;
        if (!tp->timer_thread) {
            tp->timer_thread = 1;
            if (tp->os->spawn(ktp_timer_thread, tp) != 0) {
                tp->timer_thread = 0;
                t->active = 0;
                r = -1;
            }
        } else {
            tp->os->cond_signal(tp->timer_cond);
        }
    }
    tp->os->unlock(tp->lock);
    return r;
}

static inline int ktp_timer_is_set(ktp_timer *t)
{
    int a;
    t->tp->os->lock(t->tp->lock);
    a = t->active;
    t->tp->os->unlock(t->tp->lock);
    return a;
}

static inline void ktp_timer_wait(ktp_timer *t, int cancel_pending)
{
    ktp *tp = t->tp;
    tp->os->lock(tp->lock);
    if (cancel_pending) {
        t->outstanding -= ktp_remove_locked(tp, ktp_run_timer, t);
        tp->os->cond_broadcast(tp->done_cond);
    }
    while (t->outstanding)
        tp->os->cond_wait(tp->done_cond, tp->lock, KTP_FOREVER);
    tp->os->unlock(tp->lock);
}

static inline void ktp_timer_close(ktp_timer *t)
{
    ktp *tp = t->tp;
    ktp_timer **pp;
    tp->os->lock(tp->lock);
    t->active = 0;
    t->outstanding -= ktp_remove_locked(tp, ktp_run_timer, t);
    while (t->outstanding)
        tp->os->cond_wait(tp->done_cond, tp->lock, KTP_FOREVER);
    for (pp = &tp->timers; *pp; pp = &(*pp)->next)
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    tp->os->unlock(tp->lock);
    tp->os->release(t);
}

/* ---- Handle waits ---------------------------------------------------------------------------------------------------------- */

/* A registered wait: a thread blocks on the handle (or until the timeout) and queues the callback with the result 0 (handle
 * signalled) or 1 (timed out). A repeating wait waits again once the callback has finished; a single one disarms itself. */
typedef struct ktp_wait {
    ktp *tp;
    void (*fn)(void *ctx, struct ktp_wait *w, int result);
    void *ctx;
    int repeat;
    void *cancel;     /* the event of the current arming */
    unsigned gen;     /* identifies the current arming */
    unsigned pending; /* callbacks queued or running */
    unsigned waiters; /* waiter threads that may still look at this object */
    int result;
} ktp_wait;

typedef struct {
    ktp_wait *w;
    void *handle;
    void *cancel;
    uint64_t timeout_ms;
    unsigned gen;
} ktp_waiter;

static inline void ktp_run_wait(void *arg)
{
    ktp_wait *w = (ktp_wait *)arg;
    ktp *tp = w->tp;
    int result;
    tp->os->lock(tp->lock);
    result = w->result;
    tp->os->unlock(tp->lock);
    w->fn(w->ctx, w, result);
    tp->os->lock(tp->lock);
    w->pending--;
    tp->os->cond_broadcast(tp->done_cond);
    tp->os->unlock(tp->lock);
}

static inline void ktp_waiter_thread(void *arg)
{
    ktp_waiter *wt = (ktp_waiter *)arg;
    ktp_wait *w = wt->w;
    ktp *tp = w->tp;
    const ktp_os *os = tp->os;
    for (;;) {
        int r = os->wait2(wt->handle, wt->cancel, wt->timeout_ms);
        os->lock(tp->lock);
        if (r == 2 || w->gen != wt->gen)
            break; /* cancelled or re-armed: this arming is over */
        w->result = r;
        w->pending++;
        if (ktp_submit_locked(tp, ktp_run_wait, w) != 0)
            w->pending--;
        if (!w->repeat) {
            break;
        }
        /* a repeating wait waits again only after the callback has finished */
        while (w->pending && w->gen == wt->gen)
            os->cond_wait(tp->done_cond, tp->lock, KTP_FOREVER);
        if (w->gen != wt->gen)
            break;
        os->unlock(tp->lock);
    }
    if (w->cancel == wt->cancel)
        w->cancel = NULL; /* this arming's event is about to be freed: no later disarm may touch it */
    w->waiters--;
    os->cond_broadcast(tp->done_cond);
    os->unlock(tp->lock);
    os->event_free(wt->cancel);
    os->release(wt);
}

static inline ktp_wait *ktp_wait_new(ktp *tp, void (*fn)(void *, ktp_wait *, int), void *ctx, int repeat)
{
    ktp_wait *w = (ktp_wait *)tp->os->alloc(sizeof *w);
    if (!w)
        return NULL;
    memset(w, 0, sizeof *w);
    w->tp = tp;
    w->fn = fn;
    w->ctx = ctx;
    w->repeat = repeat;
    return w;
}

/* Disarms the current arming (its thread leaves) with the lock held. */
static inline void ktp_wait_disarm_locked(ktp_wait *w)
{
    w->gen++;
    if (w->cancel) {
        w->tp->os->event_set(w->cancel);
        w->cancel = NULL;
    }
    w->tp->os->cond_broadcast(w->tp->done_cond);
}

/* Arms the wait on handle with a timeout in milliseconds (-1: none); a NULL handle disarms it. Returns 0 or -1. */
static inline int ktp_wait_set(ktp_wait *w, void *handle, uint64_t timeout_ms)
{
    ktp *tp = w->tp;
    ktp_waiter *wt;
    int r = 0;
    tp->os->lock(tp->lock);
    ktp_wait_disarm_locked(w);
    if (handle) {
        wt = (ktp_waiter *)tp->os->alloc(sizeof *wt);
        if (!wt) {
            r = -1;
        } else {
            wt->w = w;
            wt->handle = handle;
            wt->cancel = tp->os->event_new();
            wt->timeout_ms = timeout_ms;
            wt->gen = w->gen;
            w->cancel = wt->cancel;
            w->waiters++;
            if (!wt->cancel || tp->os->spawn(ktp_waiter_thread, wt) != 0) {
                w->waiters--;
                w->cancel = NULL;
                if (wt->cancel)
                    tp->os->event_free(wt->cancel);
                tp->os->release(wt);
                r = -1;
            }
        }
    }
    tp->os->unlock(tp->lock);
    return r;
}

static inline void ktp_wait_wait(ktp_wait *w, int cancel_pending)
{
    ktp *tp = w->tp;
    tp->os->lock(tp->lock);
    if (cancel_pending) {
        w->pending -= ktp_remove_locked(tp, ktp_run_wait, w);
        tp->os->cond_broadcast(tp->done_cond);
    }
    while (w->pending)
        tp->os->cond_wait(tp->done_cond, tp->lock, KTP_FOREVER);
    tp->os->unlock(tp->lock);
}

/* Disarms the wait, drops queued callbacks, waits for a running one and for the waiter thread, then frees it. */
static inline void ktp_wait_close(ktp_wait *w)
{
    ktp *tp = w->tp;
    tp->os->lock(tp->lock);
    ktp_wait_disarm_locked(w);
    w->pending -= ktp_remove_locked(tp, ktp_run_wait, w);
    while (w->pending || w->waiters)
        tp->os->cond_wait(tp->done_cond, tp->lock, KTP_FOREVER);
    tp->os->unlock(tp->lock);
    tp->os->release(w);
}

#endif /* K32_TP_PURE_H */
