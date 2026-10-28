/*
 * rtl_sync_pure.h - the user-mode synchronisation primitives of ntdll: a parking lot for waiting on
 * an address, slim reader/writer locks (SRWLOCK), condition variables, one-time initialisation
 * (INIT_ONCE, RtlRunOnce), RtlWaitOnAddress and interlocked singly linked lists.
 *
 * Nothing here calls the system. The code waits and wakes through a struct rs_os (the calling thread's
 * id, a blocking wait that a wake-up from another thread ends, the wake-up, and a yield), so ntdll.dll
 * runs it on NtWaitForAlertByThreadId and NtAlertThreadByThreadId while the host tests run it on
 * pthreads and check it under real concurrency (tests/ntdll-host/test_rtl_sync_pure.c).
 *
 * The wait primitive must behave like a per-thread binary semaphore: an alert sent before the thread
 * waits makes its next wait return at once, and one alert wakes one wait. Alerts can be spurious (a
 * stale one from an earlier park), so every user re-checks its own condition after a wake.
 *
 * Parking lot. A park puts the caller on a queue chosen by the address, but only if a validation
 * function, called with the queue locked, still says the caller should sleep. An unpark removes waiters
 * for an address under the same lock. Because validation and enqueue are one critical section, a wake
 * that runs after the caller's validation always finds the caller queued: no lost wake-ups.
 */
#ifndef NTDLL_RTL_SYNC_PURE_H
#define NTDLL_RTL_SYNC_PURE_H

#include <stddef.h>
#include <stdint.h>

#define RS_INFINITE 0xFFFFFFFFu

typedef struct rs_os {
    uint32_t (*self)(void);                 /* the calling thread's id, never 0 */
    int (*wait)(uint32_t timeout_ms);       /* 1 when alerted, 0 on timeout; RS_INFINITE waits for ever */
    void (*alert)(uint32_t tid);            /* wake that thread's wait, or its next one */
    void (*yield)(void);
} rs_os;

/* ---- Parking lot ----------------------------------------------------------------------- */

typedef struct rs_waiter {
    struct rs_waiter *next, *prev;
    const void *addr;
    uint32_t tid;
    volatile int woken;
} rs_waiter;

#define RS_BUCKETS 64u

typedef struct rs_bucket {
    volatile long lock;
    rs_waiter *head, *tail;
} rs_bucket;

typedef struct rs_lot {
    rs_bucket b[RS_BUCKETS];
} rs_lot;

enum { RS_TIMEOUT = 0, RS_WOKEN = 1, RS_NOT_ASLEEP = 2 };

static inline rs_bucket *rs_bucket_of(rs_lot *lot, const void *addr)
{
    uintptr_t a = (uintptr_t)addr;
    a ^= a >> 17;
    a *= 0x9E3779B97F4A7C15ull;
    return &lot->b[(a >> 40) % RS_BUCKETS];
}

static inline void rs_spin_lock(volatile long *l, const rs_os *os)
{
    unsigned spins = 0;
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {
        if (++spins > 100) {
            os->yield();
            spins = 0;
        }
    }
}

static inline void rs_spin_unlock(volatile long *l)
{
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

static inline void rs_queue_remove(rs_bucket *b, rs_waiter *w)
{
    if (w->prev)
        w->prev->next = w->next;
    else
        b->head = w->next;
    if (w->next)
        w->next->prev = w->prev;
    else
        b->tail = w->prev;
    w->next = w->prev = NULL;
}

static inline int rs_queued(const rs_bucket *b, const rs_waiter *w)
{
    return w->prev || b->head == w;
}

/* Waits on addr while validate(ctx) is true. Returns RS_NOT_ASLEEP when validate said no (nothing
 * was queued), RS_WOKEN when an unpark took the caller off the queue, RS_TIMEOUT when the time ran
 * out first. timeout_ms is RS_INFINITE or a number of milliseconds. */
static inline int rs_park(rs_lot *lot, const rs_os *os, const void *addr, int (*validate)(void *), void *ctx,
                          uint32_t timeout_ms, uint32_t (*now_ms)(void))
{
    rs_bucket *bk = rs_bucket_of(lot, addr);
    rs_waiter w;
    uint32_t start = 0;
    w.addr = addr;
    w.tid = os->self();
    w.woken = 0;
    w.next = w.prev = NULL;
    rs_spin_lock(&bk->lock, os);
    if (validate && !validate(ctx)) {
        rs_spin_unlock(&bk->lock);
        return RS_NOT_ASLEEP;
    }
    w.prev = bk->tail;
    if (bk->tail)
        bk->tail->next = &w;
    else
        bk->head = &w;
    bk->tail = &w;
    rs_spin_unlock(&bk->lock);
    if (timeout_ms != RS_INFINITE && now_ms)
        start = now_ms();
    while (!__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) {
        uint32_t left = RS_INFINITE;
        if (timeout_ms != RS_INFINITE) {
            uint32_t used = now_ms ? now_ms() - start : 0;
            if (used >= timeout_ms)
                break;
            left = timeout_ms - used;
        }
        os->wait(left);
    }
    if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE))
        return RS_WOKEN;
    /* Timed out: leave the queue unless an unpark took us off it in the meantime. */
    rs_spin_lock(&bk->lock, os);
    if (rs_queued(bk, &w)) {
        rs_queue_remove(bk, &w);
        rs_spin_unlock(&bk->lock);
        return RS_TIMEOUT;
    }
    rs_spin_unlock(&bk->lock);
    while (!__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE))
        os->yield(); /* the unparker has dequeued us and is about to mark us woken */
    return RS_WOKEN;
}

/* Wakes up to max waiters on addr, oldest first. Returns how many. */
static inline unsigned rs_unpark(rs_lot *lot, const rs_os *os, const void *addr, unsigned max)
{
    rs_bucket *bk = rs_bucket_of(lot, addr);
    uint32_t tids[32];
    unsigned total = 0;
    while (max) {
        unsigned n = 0, limit = max < 32 ? max : 32;
        rs_waiter *w;
        rs_spin_lock(&bk->lock, os);
        for (w = bk->head; w && n < limit;) {
            rs_waiter *next = w->next;
            if (w->addr == addr) {
                rs_queue_remove(bk, w);
                tids[n++] = w->tid;
                /* After this store the waiter may return and its frame go away: w is not touched again. */
                __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
            }
            w = next;
        }
        rs_spin_unlock(&bk->lock);
        for (unsigned i = 0; i < n; ++i)
            os->alert(tids[i]);
        total += n;
        if (n < limit)
            break;
        max -= n;
    }
    return total;
}

/* ---- Slim reader/writer lock ---------------------------------------------------------------- */

/* The lock is the low 32 bits of the SRWLOCK:
 *   bits 0..15   number of shared owners
 *   bits 16..29  number of threads in the slow path (about to park, parked, or just woken)
 *   bit 30       a writer is waiting: new readers queue behind it
 *   bit 31       a writer owns the lock */
#define RS_SRW_READERS 0x0000FFFFu
#define RS_SRW_WAITER_ONE 0x00010000u
#define RS_SRW_WAITERS 0x3FFF0000u
#define RS_SRW_WPENDING 0x40000000u
#define RS_SRW_WRITER 0x80000000u

typedef struct rs_ctx {
    rs_lot *lot;
    const rs_os *os;
    uint32_t (*now_ms)(void);
} rs_ctx;

typedef struct rs_srw_check {
    volatile uint32_t *state;
    uint32_t block_mask; /* sleep while any of these bits is set */
} rs_srw_check;

static inline int rs_srw_still_blocked(void *p)
{
    rs_srw_check *c = (rs_srw_check *)p;
    return (__atomic_load_n(c->state, __ATOMIC_SEQ_CST) & c->block_mask) != 0;
}

static inline void rs_srw_init(volatile uint32_t *s)
{
    __atomic_store_n(s, 0, __ATOMIC_RELEASE);
}

static inline int rs_srw_try_exclusive(volatile uint32_t *s)
{
    uint32_t old = __atomic_load_n(s, __ATOMIC_RELAXED);
    while (!(old & (RS_SRW_WRITER | RS_SRW_READERS))) {
        if (__atomic_compare_exchange_n(s, &old, (old & ~RS_SRW_WPENDING) | RS_SRW_WRITER, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

static inline int rs_srw_try_shared(volatile uint32_t *s)
{
    uint32_t old = __atomic_load_n(s, __ATOMIC_RELAXED);
    while (!(old & (RS_SRW_WRITER | RS_SRW_WPENDING)) && (old & RS_SRW_READERS) < RS_SRW_READERS) {
        if (__atomic_compare_exchange_n(s, &old, old + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

static inline void rs_srw_slow_wait(const rs_ctx *x, volatile uint32_t *s, uint32_t block_mask, int writer)
{
    rs_srw_check chk;
    uint32_t old;
    chk.state = s;
    chk.block_mask = block_mask;
    /* Count this thread as a waiter before validating, so a releaser that runs after our validation
     * sees it. A waiting writer also raises WPENDING so new readers do not starve it. */
    do {
        old = __atomic_load_n(s, __ATOMIC_RELAXED);
        if ((old & RS_SRW_WAITERS) == RS_SRW_WAITERS) { /* saturated: just yield and retry */
            x->os->yield();
            return;
        }
    } while (!__atomic_compare_exchange_n(s, &old, (old + RS_SRW_WAITER_ONE) | (writer ? RS_SRW_WPENDING : 0), 0,
                                          __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
    rs_park(x->lot, x->os, (const void *)s, rs_srw_still_blocked, &chk, RS_INFINITE, x->now_ms);
    __atomic_fetch_sub(s, RS_SRW_WAITER_ONE, __ATOMIC_SEQ_CST);
}

static inline void rs_srw_acquire_exclusive(const rs_ctx *x, volatile uint32_t *s)
{
    unsigned spins = 0;
    for (;;) {
        if (rs_srw_try_exclusive(s))
            return;
        if (++spins < 40) {
            x->os->yield();
            continue;
        }
        rs_srw_slow_wait(x, s, RS_SRW_WRITER | RS_SRW_READERS, 1);
    }
}

static inline void rs_srw_acquire_shared(const rs_ctx *x, volatile uint32_t *s)
{
    unsigned spins = 0;
    for (;;) {
        if (rs_srw_try_shared(s))
            return;
        if (++spins < 40) {
            x->os->yield();
            continue;
        }
        rs_srw_slow_wait(x, s, RS_SRW_WRITER | RS_SRW_WPENDING, 0);
    }
}

static inline void rs_srw_release_exclusive(const rs_ctx *x, volatile uint32_t *s)
{
    uint32_t old = __atomic_fetch_and(s, ~RS_SRW_WRITER, __ATOMIC_SEQ_CST);
    if (old & RS_SRW_WAITERS)
        rs_unpark(x->lot, x->os, (const void *)s, 0x7FFFFFFFu);
}

static inline void rs_srw_release_shared(const rs_ctx *x, volatile uint32_t *s)
{
    uint32_t old = __atomic_fetch_sub(s, 1, __ATOMIC_SEQ_CST);
    if ((old & RS_SRW_READERS) == 1 && (old & RS_SRW_WAITERS))
        rs_unpark(x->lot, x->os, (const void *)s, 0x7FFFFFFFu);
}

/* ---- Condition variable ------------------------------------------------------------------------ */

/* The condition variable is a 32-bit sequence number in the low half of the CONDITION_VARIABLE. A
 * sleeper reads it before releasing its lock and parks only while it is unchanged; a wake bumps it, so
 * a wake between "unlock" and "park" is not lost. A wake-one that finds no sleeper is harmless. */
typedef struct rs_cv_check {
    volatile uint32_t *seq;
    uint32_t seen;
} rs_cv_check;

static inline int rs_cv_unchanged(void *p)
{
    rs_cv_check *c = (rs_cv_check *)p;
    return __atomic_load_n(c->seq, __ATOMIC_SEQ_CST) == c->seen;
}

/* Sleeps until woken or timed out, calling release(arg) after noting the sequence and acquire(arg)
 * before returning, whatever the outcome. Returns 1 when woken (possibly spuriously) and 0 on timeout. */
static inline int rs_cv_sleep(const rs_ctx *x, volatile uint32_t *seq, void (*release)(void *), void (*acquire)(void *),
                              void *arg, uint32_t timeout_ms)
{
    rs_cv_check chk;
    int r;
    chk.seq = seq;
    chk.seen = __atomic_load_n(seq, __ATOMIC_SEQ_CST);
    release(arg);
    r = rs_park(x->lot, x->os, (const void *)seq, rs_cv_unchanged, &chk, timeout_ms, x->now_ms);
    acquire(arg);
    return r != RS_TIMEOUT;
}

static inline void rs_cv_wake(const rs_ctx *x, volatile uint32_t *seq, unsigned how_many)
{
    __atomic_fetch_add(seq, 1, __ATOMIC_SEQ_CST);
    rs_unpark(x->lot, x->os, (const void *)seq, how_many);
}

/* ---- Wait on address --------------------------------------------------------------------------- */

typedef struct rs_addr_check {
    const volatile void *addr;
    const void *expected;
    size_t size;
} rs_addr_check;

static inline int rs_addr_equals(void *p)
{
    rs_addr_check *c = (rs_addr_check *)p;
    switch (c->size) {
    case 1: return __atomic_load_n((const volatile uint8_t *)c->addr, __ATOMIC_SEQ_CST) == *(const uint8_t *)c->expected;
    case 2: return __atomic_load_n((const volatile uint16_t *)c->addr, __ATOMIC_SEQ_CST) == *(const uint16_t *)c->expected;
    case 4: return __atomic_load_n((const volatile uint32_t *)c->addr, __ATOMIC_SEQ_CST) == *(const uint32_t *)c->expected;
    case 8: return __atomic_load_n((const volatile uint64_t *)c->addr, __ATOMIC_SEQ_CST) == *(const uint64_t *)c->expected;
    default: return 0;
    }
}

/* WaitOnAddress. Returns 1 when the value differed at once or a wake came, 0 on timeout, -1 for a bad
 * size (not 1, 2, 4 or 8). */
static inline int rs_wait_on_address(const rs_ctx *x, const volatile void *addr, const void *expected, size_t size,
                                     uint32_t timeout_ms)
{
    rs_addr_check chk;
    int r;
    if (size != 1 && size != 2 && size != 4 && size != 8)
        return -1;
    chk.addr = addr;
    chk.expected = expected;
    chk.size = size;
    r = rs_park(x->lot, x->os, (const void *)addr, rs_addr_equals, &chk, timeout_ms, x->now_ms);
    return r != RS_TIMEOUT;
}

static inline void rs_wake_address(const rs_ctx *x, const volatile void *addr, int all)
{
    rs_unpark(x->lot, x->os, (const void *)addr, all ? 0x7FFFFFFFu : 1u);
}

/* ---- One-time initialisation ------------------------------------------------------------------- */

/* The INIT_ONCE word: 0 not started, 1 an initialiser is running, otherwise (context & ~3) | 2 when done. */
#define RS_ONCE_PENDING_STATE 1u
#define RS_ONCE_DONE_BIT 2u
#define RS_ONCE_CHECK_ONLY 0x1u
#define RS_ONCE_ASYNC 0x2u
#define RS_ONCE_INIT_FAILED 0x4u

enum { RS_ONCE_OK = 0, RS_ONCE_ERR_INVALID = 1, RS_ONCE_ERR_GEN_FAILURE = 2 };

static inline int rs_once_is_pending(void *p)
{
    return __atomic_load_n((volatile uintptr_t *)p, __ATOMIC_SEQ_CST) == RS_ONCE_PENDING_STATE;
}

/* InitOnceBeginInitialize. Returns RS_ONCE_OK with *pending set to 1 when the caller must run the
 * initialiser, to 0 when it already ran (and *context is set). RS_ONCE_ERR_GEN_FAILURE is returned
 * for CHECK_ONLY on an unfinished once. */
static inline int rs_once_begin(const rs_ctx *x, volatile uintptr_t *once, uint32_t flags, int *pending, void **context)
{
    if ((flags & ~(RS_ONCE_CHECK_ONLY | RS_ONCE_ASYNC)) || flags == (RS_ONCE_CHECK_ONLY | RS_ONCE_ASYNC))
        return RS_ONCE_ERR_INVALID;
    for (;;) {
        uintptr_t s = __atomic_load_n(once, __ATOMIC_ACQUIRE);
        if (s != 0 && s != RS_ONCE_PENDING_STATE) {
            *pending = 0;
            if (context)
                *context = (void *)(s & ~(uintptr_t)3);
            return RS_ONCE_OK;
        }
        if (flags & RS_ONCE_CHECK_ONLY) {
            *pending = 1;
            return RS_ONCE_ERR_GEN_FAILURE;
        }
        if (s == 0) {
            uintptr_t zero = 0;
            if (__atomic_compare_exchange_n(once, &zero, RS_ONCE_PENDING_STATE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                *pending = 1;
                return RS_ONCE_OK;
            }
            continue;
        }
        if (flags & RS_ONCE_ASYNC) { /* another thread is initialising; async callers may run too */
            *pending = 1;
            return RS_ONCE_OK;
        }
        rs_park(x->lot, x->os, (const void *)once, rs_once_is_pending, (void *)once, RS_INFINITE, x->now_ms);
    }
}

/* InitOnceComplete. flags may carry RS_ONCE_INIT_FAILED (the once goes back to "not started") and
 * RS_ONCE_ASYNC. The context's low two bits must be zero. */
static inline int rs_once_complete(const rs_ctx *x, volatile uintptr_t *once, uint32_t flags, void *context)
{
    uintptr_t expect = RS_ONCE_PENDING_STATE, fin;
    if (flags & ~(RS_ONCE_ASYNC | RS_ONCE_INIT_FAILED))
        return RS_ONCE_ERR_INVALID;
    if ((flags & RS_ONCE_INIT_FAILED) && context)
        return RS_ONCE_ERR_INVALID;
    if ((uintptr_t)context & 3)
        return RS_ONCE_ERR_INVALID;
    fin = (flags & RS_ONCE_INIT_FAILED) ? 0 : ((uintptr_t)context | RS_ONCE_DONE_BIT);
    if (!__atomic_compare_exchange_n(once, &expect, fin, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return RS_ONCE_ERR_GEN_FAILURE; /* not pending: another async thread finished first, or never begun */
    rs_unpark(x->lot, x->os, (const void *)once, 0x7FFFFFFFu);
    return RS_ONCE_OK;
}

/* ---- Interlocked singly linked list ------------------------------------------------------------- */

/* SLIST_ENTRY is one pointer. The header is two 64-bit words: the first entry, and the depth in the
 * low 16 bits with a lock bit in bit 63. The operations are short, so they take the lock bit and spin. */
typedef struct rs_slist_entry {
    struct rs_slist_entry *next;
} rs_slist_entry;

typedef struct rs_slist_header {
    volatile uintptr_t first;
    volatile uint64_t info;
} rs_slist_header;

#define RS_SLIST_LOCK ((uint64_t)1 << 63)
#define RS_SLIST_DEPTH 0xFFFFu

static inline void rs_slist_init(rs_slist_header *h)
{
    h->first = 0;
    h->info = 0;
}

static inline uint64_t rs_slist_lock(rs_slist_header *h, const rs_os *os)
{
    unsigned spins = 0;
    for (;;) {
        uint64_t v = __atomic_load_n(&h->info, __ATOMIC_RELAXED);
        if (!(v & RS_SLIST_LOCK) &&
            __atomic_compare_exchange_n(&h->info, &v, v | RS_SLIST_LOCK, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return v;
        if (++spins > 100) {
            os->yield();
            spins = 0;
        }
    }
}

static inline void rs_slist_unlock(rs_slist_header *h, uint64_t new_info)
{
    __atomic_store_n(&h->info, new_info & ~RS_SLIST_LOCK, __ATOMIC_RELEASE);
}

/* Pushes one entry; returns the previous first entry. */
static inline rs_slist_entry *rs_slist_push(rs_slist_header *h, const rs_os *os, rs_slist_entry *e)
{
    uint64_t info = rs_slist_lock(h, os);
    rs_slist_entry *prev = (rs_slist_entry *)h->first;
    e->next = prev;
    h->first = (uintptr_t)e;
    rs_slist_unlock(h, (info & ~(uint64_t)RS_SLIST_DEPTH) | (uint64_t)(((info & RS_SLIST_DEPTH) + 1) & RS_SLIST_DEPTH));
    return prev;
}

static inline rs_slist_entry *rs_slist_pop(rs_slist_header *h, const rs_os *os)
{
    uint64_t info = rs_slist_lock(h, os);
    rs_slist_entry *e = (rs_slist_entry *)h->first;
    if (e) {
        h->first = (uintptr_t)e->next;
        info = (info & ~(uint64_t)RS_SLIST_DEPTH) | (uint64_t)(((info & RS_SLIST_DEPTH) - 1) & RS_SLIST_DEPTH);
    }
    rs_slist_unlock(h, info);
    return e;
}

/* Pushes a chain of `count` entries whose first is `first` and whose last is `last`, keeping their
 * order; returns the previous first entry. */
static inline rs_slist_entry *rs_slist_push_list(rs_slist_header *h, const rs_os *os, rs_slist_entry *first,
                                                 rs_slist_entry *last, unsigned count)
{
    uint64_t info = rs_slist_lock(h, os);
    rs_slist_entry *prev = (rs_slist_entry *)h->first;
    last->next = prev;
    h->first = (uintptr_t)first;
    rs_slist_unlock(h, (info & ~(uint64_t)RS_SLIST_DEPTH) | (uint64_t)(((info & RS_SLIST_DEPTH) + count) & RS_SLIST_DEPTH));
    return prev;
}

/* Takes the whole list, oldest entry last, and empties the header. */
static inline rs_slist_entry *rs_slist_flush(rs_slist_header *h, const rs_os *os)
{
    uint64_t info = rs_slist_lock(h, os);
    rs_slist_entry *e = (rs_slist_entry *)h->first;
    h->first = 0;
    rs_slist_unlock(h, info & ~(uint64_t)RS_SLIST_DEPTH);
    return e;
}

static inline uint16_t rs_slist_depth(const rs_slist_header *h)
{
    return (uint16_t)(__atomic_load_n(&h->info, __ATOMIC_ACQUIRE) & RS_SLIST_DEPTH);
}

#endif /* NTDLL_RTL_SYNC_PURE_H */
