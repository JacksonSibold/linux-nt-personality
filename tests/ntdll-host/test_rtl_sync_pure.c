/*
 * test_rtl_sync_pure.c - host tests for src/ntdll/rtl_sync_pure.h on pthreads: the parking lot
 * (wake, timeout, validation, ordering, the timeout and wake race), slim reader/writer locks
 * (mutual exclusion, reader consistency, writer preference), condition variables, WaitOnAddress,
 * one-time initialisation and the interlocked list.
 */
#include "minitest.h"
#include "rtl_sync_pure.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- A pthread implementation of rs_os ---------------------------------------------------- */

typedef struct tslot {
    pthread_mutex_t m;
    pthread_cond_t c;
    int alerted;
    int used;
} tslot;

#define MAX_THREADS 64
static tslot slots[MAX_THREADS];
static pthread_mutex_t slots_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint32_t my_tid;

static uint32_t os_self(void)
{
    if (!my_tid) {
        uint32_t i;
        pthread_mutex_lock(&slots_lock);
        for (i = 1; i < MAX_THREADS; ++i) {
            if (!slots[i].used) {
                slots[i].used = 1;
                pthread_mutex_init(&slots[i].m, NULL);
                pthread_cond_init(&slots[i].c, NULL);
                slots[i].alerted = 0;
                my_tid = i;
                break;
            }
        }
        pthread_mutex_unlock(&slots_lock);
    }
    return my_tid;
}

static void release_self(void)
{
    pthread_mutex_lock(&slots_lock);
    if (my_tid) {
        pthread_mutex_destroy(&slots[my_tid].m);
        pthread_cond_destroy(&slots[my_tid].c);
        slots[my_tid].used = 0;
        my_tid = 0;
    }
    pthread_mutex_unlock(&slots_lock);
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static int os_wait(uint32_t timeout_ms)
{
    tslot *s = &slots[os_self()];
    int was;
    pthread_mutex_lock(&s->m);
    if (!s->alerted) {
        if (timeout_ms == RS_INFINITE) {
            while (!s->alerted)
                pthread_cond_wait(&s->c, &s->m);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            while (!s->alerted && pthread_cond_timedwait(&s->c, &s->m, &ts) == 0)
                ;
        }
    }
    was = s->alerted;
    s->alerted = 0;
    pthread_mutex_unlock(&s->m);
    return was;
}

static void os_alert(uint32_t tid)
{
    tslot *s;
    if (tid == 0 || tid >= MAX_THREADS)
        return;
    s = &slots[tid];
    pthread_mutex_lock(&slots_lock);
    if (!s->used) { /* the thread is gone: nothing to alert */
        pthread_mutex_unlock(&slots_lock);
        return;
    }
    pthread_mutex_lock(&s->m);
    pthread_mutex_unlock(&slots_lock);
    s->alerted = 1;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}

static void os_yield(void)
{
    sched_yield();
}

static const rs_os OS = {os_self, os_wait, os_alert, os_yield};
static rs_lot LOT;
static const rs_ctx CTX = {&LOT, &OS, now_ms};

/* ---- Helpers --------------------------------------------------------------------------------- */

static void *run_thread_fn(void *p)
{
    void (**fn)(void *) = (void (**)(void *))p;
    (*fn)(((void **)p)[1]);
    release_self();
    return NULL;
}

typedef struct thr {
    pthread_t t;
    void (*fn)(void *);
    void *arg;
} thr;

static void *thr_main(void *p)
{
    thr *t = (thr *)p;
    t->fn(t->arg);
    release_self();
    return NULL;
}

static void spawn(thr *t, void (*fn)(void *), void *arg)
{
    t->fn = fn;
    t->arg = arg;
    pthread_create(&t->t, NULL, thr_main, t);
}

static void join(thr *t)
{
    pthread_join(t->t, NULL);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int always(void *p)
{
    (void)p;
    return 1;
}

static int never(void *p)
{
    (void)p;
    return 0;
}

/* ---- Parking lot -------------------------------------------------------------------------------- */

typedef struct park_arg {
    int key;
    int result;
    volatile int started;
    uint32_t timeout;
} park_arg;

static void parker(void *p)
{
    park_arg *a = (park_arg *)p;
    a->started = 1;
    a->result = rs_park(&LOT, &OS, &a->key, always, NULL, a->timeout, now_ms);
}

static void park_and_unpark(void)
{
    park_arg a;
    thr t;
    unsigned n;
    memset(&a, 0, sizeof(a));
    a.timeout = RS_INFINITE;
    spawn(&t, parker, &a);
    while (!a.started)
        sched_yield();
    sleep_ms(20); /* let it block */
    n = rs_unpark(&LOT, &OS, &a.key, 10);
    join(&t);
    MT_EXPECT_EQ(n, 1);
    MT_EXPECT_EQ(a.result, RS_WOKEN);
    /* An unpark with nobody parked is a no-op. */
    MT_EXPECT_EQ(rs_unpark(&LOT, &OS, &a.key, 10), 0);
    MT_EXPECT_EQ(rs_unpark(&LOT, &OS, &a.key, 0), 0);
}

static void park_validation_and_timeout(void)
{
    int key = 0;
    uint32_t t0, t1;
    MT_EXPECT_EQ(rs_park(&LOT, &OS, &key, never, NULL, RS_INFINITE, now_ms), RS_NOT_ASLEEP);
    t0 = now_ms();
    MT_EXPECT_EQ(rs_park(&LOT, &OS, &key, always, NULL, 50, now_ms), RS_TIMEOUT);
    t1 = now_ms();
    MT_EXPECT(t1 - t0 >= 50);
    MT_EXPECT(t1 - t0 < 1000);
    /* A timed-out waiter has left the queue: an unpark finds nobody. */
    MT_EXPECT_EQ(rs_unpark(&LOT, &OS, &key, 10), 0);
    /* A zero timeout returns at once. */
    t0 = now_ms();
    MT_EXPECT_EQ(rs_park(&LOT, &OS, &key, always, NULL, 0, now_ms), RS_TIMEOUT);
    MT_EXPECT(now_ms() - t0 < 500);
}

typedef struct order_arg {
    int key;
    volatile int order;
    int my_order[4];
    volatile int parked;
} order_arg;

static void order_parker(void *p)
{
    order_arg *a = (order_arg *)p;
    int idx = __sync_fetch_and_add(&a->parked, 1);
    rs_park(&LOT, &OS, &a->key, always, NULL, RS_INFINITE, now_ms);
    a->my_order[idx] = __sync_fetch_and_add(&a->order, 1);
}

static void unpark_is_fifo_and_counts(void)
{
    order_arg a;
    thr t[4];
    int i;
    memset(&a, 0, sizeof(a));
    /* Park four threads one after the other so the queue order is known. */
    for (i = 0; i < 4; ++i) {
        spawn(&t[i], order_parker, &a);
        while (a.parked < i + 1)
            sched_yield();
        sleep_ms(30);
    }
    MT_EXPECT_EQ(rs_unpark(&LOT, &OS, &a.key, 1), 1);
    sleep_ms(30);
    MT_EXPECT_EQ(a.order, 1);
    MT_EXPECT_EQ(a.my_order[0], 0); /* the first parked thread woke first */
    MT_EXPECT_EQ(rs_unpark(&LOT, &OS, &a.key, 2), 2);
    sleep_ms(30);
    MT_EXPECT_EQ(a.order, 3);
    MT_EXPECT_EQ(rs_unpark(&LOT, &OS, &a.key, 1000), 1);
    for (i = 0; i < 4; ++i)
        join(&t[i]);
    MT_EXPECT_EQ(a.order, 4);
}

/* Threads that time out race with unparks. Every park must end as woken or timed out, never hang, and
 * each unpark's count matches the number of parks that report being woken. */
typedef struct race_arg {
    int key;
    volatile int stop;
    volatile int woken, timed_out;
} race_arg;

static void race_parker(void *p)
{
    race_arg *a = (race_arg *)p;
    while (!a->stop) {
        int r = rs_park(&LOT, &OS, &a->key, always, NULL, 1, now_ms);
        if (r == RS_WOKEN)
            __sync_fetch_and_add(&a->woken, 1);
        else if (r == RS_TIMEOUT)
            __sync_fetch_and_add(&a->timed_out, 1);
    }
}

static void timeout_versus_unpark_race(void)
{
    race_arg a;
    thr t[3];
    int i, unparked = 0;
    memset(&a, 0, sizeof(a));
    for (i = 0; i < 3; ++i)
        spawn(&t[i], race_parker, &a);
    for (i = 0; i < 3000; ++i) {
        unparked += (int)rs_unpark(&LOT, &OS, &a.key, 1);
        if (i % 50 == 0)
            sleep_ms(1);
    }
    a.stop = 1;
    for (i = 0; i < 3; ++i)
        join(&t[i]);
    /* Late unparks may have woken a thread whose count the loop has not seen yet; drain. */
    unparked += (int)rs_unpark(&LOT, &OS, &a.key, 100);
    MT_EXPECT_EQ(a.woken, unparked);
    MT_EXPECT(a.woken + a.timed_out > 0);
}

/* ---- SRW lock ------------------------------------------------------------------------------------ */

static volatile uint32_t srw;
static volatile long guarded_counter;

static void srw_incrementer(void *p)
{
    int i, n = *(int *)p;
    for (i = 0; i < n; ++i) {
        rs_srw_acquire_exclusive(&CTX, &srw);
        guarded_counter = guarded_counter + 1; /* not atomic: only the lock keeps it right */
        rs_srw_release_exclusive(&CTX, &srw);
    }
}

static void srw_mutual_exclusion(void)
{
    enum { T = 4 };
    thr t[T];
    int n = 20000, i;
    rs_srw_init(&srw);
    guarded_counter = 0;
    for (i = 0; i < T; ++i)
        spawn(&t[i], srw_incrementer, &n);
    for (i = 0; i < T; ++i)
        join(&t[i]);
    MT_EXPECT_EQ(guarded_counter, (long)T * n);
    MT_EXPECT_EQ(srw, 0);
}

static volatile long pair_a, pair_b;
static volatile int rw_stop;
static volatile long torn_reads;

static void rw_writer(void *p)
{
    long i;
    (void)p;
    for (i = 1; i <= 3000; ++i) {
        rs_srw_acquire_exclusive(&CTX, &srw);
        pair_a = i;
        sched_yield(); /* widen the window in which a reader could see the pair torn */
        pair_b = i;
        rs_srw_release_exclusive(&CTX, &srw);
    }
}

static void rw_reader(void *p)
{
    (void)p;
    while (!rw_stop) {
        long a, b;
        rs_srw_acquire_shared(&CTX, &srw);
        a = pair_a;
        b = pair_b;
        rs_srw_release_shared(&CTX, &srw);
        if (a != b)
            __sync_fetch_and_add(&torn_reads, 1);
    }
}

static void srw_readers_see_consistent_state(void)
{
    thr w[2], r[3];
    int i;
    rs_srw_init(&srw);
    pair_a = pair_b = 0;
    rw_stop = 0;
    torn_reads = 0;
    for (i = 0; i < 3; ++i)
        spawn(&r[i], rw_reader, NULL);
    for (i = 0; i < 2; ++i)
        spawn(&w[i], rw_writer, NULL);
    for (i = 0; i < 2; ++i)
        join(&w[i]);
    rw_stop = 1;
    for (i = 0; i < 3; ++i)
        join(&r[i]);
    MT_EXPECT_EQ(torn_reads, 0);
    MT_EXPECT_EQ(srw, 0);
}

static void srw_try_and_sharing(void)
{
    rs_srw_init(&srw);
    MT_EXPECT(rs_srw_try_shared(&srw));
    MT_EXPECT(rs_srw_try_shared(&srw)); /* readers share */
    MT_EXPECT(!rs_srw_try_exclusive(&srw));
    rs_srw_release_shared(&CTX, &srw);
    MT_EXPECT(!rs_srw_try_exclusive(&srw)); /* one reader left */
    rs_srw_release_shared(&CTX, &srw);
    MT_EXPECT(rs_srw_try_exclusive(&srw));
    MT_EXPECT(!rs_srw_try_exclusive(&srw));
    MT_EXPECT(!rs_srw_try_shared(&srw));
    rs_srw_release_exclusive(&CTX, &srw);
    MT_EXPECT_EQ(srw, 0);
}

static volatile int writer_got_it;

static void pending_writer(void *p)
{
    (void)p;
    rs_srw_acquire_exclusive(&CTX, &srw);
    writer_got_it = 1;
    rs_srw_release_exclusive(&CTX, &srw);
}

/* A waiting writer keeps new readers out, so it cannot be starved by a stream of readers. */
static void srw_waiting_writer_blocks_new_readers(void)
{
    thr w;
    int i;
    rs_srw_init(&srw);
    writer_got_it = 0;
    rs_srw_acquire_shared(&CTX, &srw);
    spawn(&w, pending_writer, NULL);
    for (i = 0; i < 400 && !(srw & RS_SRW_WPENDING); ++i)
        sleep_ms(2);
    MT_EXPECT(srw & RS_SRW_WPENDING);
    MT_EXPECT(!rs_srw_try_shared(&srw)); /* queued behind the writer */
    MT_EXPECT(!writer_got_it);
    rs_srw_release_shared(&CTX, &srw);
    join(&w);
    MT_EXPECT(writer_got_it);
    MT_EXPECT_EQ(srw, 0);
}

/* ---- Condition variable ------------------------------------------------------------------------- */

static volatile uint32_t cv_lock, cv_seq;
static int queue_items[1024];
static int queue_n, queue_taken_sum, queue_done;

static void cv_release(void *p)
{
    (void)p;
    rs_srw_release_exclusive(&CTX, &cv_lock);
}

static void cv_acquire(void *p)
{
    (void)p;
    rs_srw_acquire_exclusive(&CTX, &cv_lock);
}

static void cv_consumer(void *p)
{
    (void)p;
    for (;;) {
        rs_srw_acquire_exclusive(&CTX, &cv_lock);
        while (queue_n == 0 && !queue_done)
            rs_cv_sleep(&CTX, &cv_seq, cv_release, cv_acquire, NULL, RS_INFINITE);
        if (queue_n == 0 && queue_done) {
            rs_srw_release_exclusive(&CTX, &cv_lock);
            return;
        }
        queue_taken_sum += queue_items[--queue_n];
        rs_srw_release_exclusive(&CTX, &cv_lock);
    }
}

static void cv_producer_consumer(void)
{
    enum { C = 3, ITEMS = 2000 };
    thr t[C];
    int i, want = 0;
    rs_srw_init(&cv_lock);
    cv_seq = 0;
    queue_n = queue_taken_sum = queue_done = 0;
    for (i = 0; i < C; ++i)
        spawn(&t[i], cv_consumer, NULL);
    for (i = 1; i <= ITEMS; ++i) {
        rs_srw_acquire_exclusive(&CTX, &cv_lock);
        while (queue_n == 1024) {
            rs_srw_release_exclusive(&CTX, &cv_lock);
            sched_yield();
            rs_srw_acquire_exclusive(&CTX, &cv_lock);
        }
        queue_items[queue_n++] = i;
        rs_srw_release_exclusive(&CTX, &cv_lock);
        rs_cv_wake(&CTX, &cv_seq, 1);
        want += i;
    }
    rs_srw_acquire_exclusive(&CTX, &cv_lock);
    queue_done = 1;
    rs_srw_release_exclusive(&CTX, &cv_lock);
    rs_cv_wake(&CTX, &cv_seq, 1000); /* wake everyone */
    for (i = 0; i < C; ++i)
        join(&t[i]);
    MT_EXPECT_EQ(queue_taken_sum, want);
    MT_EXPECT_EQ(queue_n, 0);
}

static void cv_timeout_reacquires_the_lock(void)
{
    uint32_t t0;
    int r;
    rs_srw_init(&cv_lock);
    cv_seq = 0;
    rs_srw_acquire_exclusive(&CTX, &cv_lock);
    t0 = now_ms();
    r = rs_cv_sleep(&CTX, &cv_seq, cv_release, cv_acquire, NULL, 40);
    MT_EXPECT_EQ(r, 0);
    MT_EXPECT(now_ms() - t0 >= 40);
    MT_EXPECT(!rs_srw_try_exclusive(&cv_lock)); /* we hold it again after the timeout */
    rs_srw_release_exclusive(&CTX, &cv_lock);
}

static volatile int cv_woke;

static void cv_sleeper(void *p)
{
    (void)p;
    rs_srw_acquire_exclusive(&CTX, &cv_lock);
    rs_cv_sleep(&CTX, &cv_seq, cv_release, cv_acquire, NULL, RS_INFINITE);
    __sync_fetch_and_add(&cv_woke, 1);
    rs_srw_release_exclusive(&CTX, &cv_lock);
}

static void cv_wake_all(void)
{
    thr t[4];
    int i;
    rs_srw_init(&cv_lock);
    cv_seq = 0;
    cv_woke = 0;
    for (i = 0; i < 4; ++i)
        spawn(&t[i], cv_sleeper, NULL);
    sleep_ms(100);
    MT_EXPECT_EQ(cv_woke, 0);
    rs_cv_wake(&CTX, &cv_seq, 1);
    sleep_ms(60);
    MT_EXPECT_EQ(cv_woke, 1); /* wake one */
    rs_cv_wake(&CTX, &cv_seq, 100);
    for (i = 0; i < 4; ++i)
        join(&t[i]);
    MT_EXPECT_EQ(cv_woke, 4);
}

/* A wake between the sleeper noting the sequence and parking is not lost. */
static void cv_wake_before_park_is_not_lost(void)
{
    volatile uint32_t seq = 0;
    rs_cv_check chk;
    chk.seq = &seq;
    chk.seen = seq;
    rs_cv_wake(&CTX, &seq, 1); /* nobody parked yet: only the sequence moves */
    MT_EXPECT_EQ(rs_park(&LOT, &OS, (const void *)&seq, rs_cv_unchanged, &chk, RS_INFINITE, now_ms), RS_NOT_ASLEEP);
}

/* ---- Wait on address ------------------------------------------------------------------------------- */

static volatile uint32_t word;

static void address_setter(void *p)
{
    (void)p;
    sleep_ms(40);
    __atomic_store_n(&word, 1, __ATOMIC_SEQ_CST);
    rs_wake_address(&CTX, &word, 1);
}

static void wait_on_address_basics(void)
{
    uint32_t expect = 0, other = 5;
    thr t;
    uint16_t small = 3, small_expect = 4;
    word = 0;
    /* A different value returns at once. */
    MT_EXPECT_EQ(rs_wait_on_address(&CTX, &word, &other, 4, 1000), 1);
    /* An equal value with a timeout times out. */
    MT_EXPECT_EQ(rs_wait_on_address(&CTX, &word, &expect, 4, 30), 0);
    /* A wake ends the wait. */
    spawn(&t, address_setter, NULL);
    MT_EXPECT_EQ(rs_wait_on_address(&CTX, &word, &expect, 4, RS_INFINITE), 1);
    join(&t);
    MT_EXPECT_EQ(word, 1);
    /* Only sizes 1, 2, 4 and 8 are accepted. */
    MT_EXPECT_EQ(rs_wait_on_address(&CTX, &word, &expect, 3, 10), -1);
    MT_EXPECT_EQ(rs_wait_on_address(&CTX, &word, &expect, 16, 10), -1);
    MT_EXPECT_EQ(rs_wait_on_address(&CTX, &small, &small_expect, 2, 1000), 1);
}

/* ---- One-time initialisation -------------------------------------------------------------------------- */

static volatile uintptr_t once;
static volatile int init_runs;
static void *seen_ctx[8];

static void once_runner(void *p)
{
    int idx = (int)(intptr_t)p, pending = 0;
    void *ctx = NULL;
    int r = rs_once_begin(&CTX, &once, 0, &pending, &ctx);
    if (r == RS_ONCE_OK && pending) {
        __sync_fetch_and_add(&init_runs, 1);
        sleep_ms(30); /* the others must wait for this */
        rs_once_complete(&CTX, &once, 0, (void *)(uintptr_t)0x1000);
        ctx = (void *)(uintptr_t)0x1000;
    }
    seen_ctx[idx] = ctx;
}

static void once_runs_one_initialiser(void)
{
    thr t[8];
    int i;
    once = 0;
    init_runs = 0;
    memset(seen_ctx, 0, sizeof(seen_ctx));
    for (i = 0; i < 8; ++i)
        spawn(&t[i], once_runner, (void *)(intptr_t)i);
    for (i = 0; i < 8; ++i)
        join(&t[i]);
    MT_EXPECT_EQ(init_runs, 1);
    for (i = 0; i < 8; ++i)
        MT_EXPECT(seen_ctx[i] == (void *)(uintptr_t)0x1000);
    MT_EXPECT_EQ(once, 0x1002);
}

static void once_states_and_flags(void)
{
    uintptr_t o = 0;
    int pending = 9;
    void *ctx = (void *)0x55;
    /* CHECK_ONLY on an unstarted once fails and leaves it alone. */
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, RS_ONCE_CHECK_ONLY, &pending, &ctx), RS_ONCE_ERR_GEN_FAILURE);
    MT_EXPECT_EQ(o, 0);
    /* CHECK_ONLY and ASYNC together are invalid, as are unknown flags. */
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, RS_ONCE_CHECK_ONLY | RS_ONCE_ASYNC, &pending, &ctx), RS_ONCE_ERR_INVALID);
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, 0x80, &pending, &ctx), RS_ONCE_ERR_INVALID);
    /* Begin and fail: the once goes back to not started. */
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, 0, &pending, &ctx), RS_ONCE_OK);
    MT_EXPECT(pending);
    MT_EXPECT_EQ(o, RS_ONCE_PENDING_STATE);
    MT_EXPECT_EQ(rs_once_complete(&CTX, &o, RS_ONCE_INIT_FAILED, NULL), RS_ONCE_OK);
    MT_EXPECT_EQ(o, 0);
    /* A failed completion may not carry a context; a context may not use the low two bits. */
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, 0, &pending, &ctx), RS_ONCE_OK);
    MT_EXPECT_EQ(rs_once_complete(&CTX, &o, RS_ONCE_INIT_FAILED, (void *)0x10), RS_ONCE_ERR_INVALID);
    MT_EXPECT_EQ(rs_once_complete(&CTX, &o, 0, (void *)0x11), RS_ONCE_ERR_INVALID);
    MT_EXPECT_EQ(o, RS_ONCE_PENDING_STATE);
    /* Async: a second caller may also run while the first is pending. */
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, RS_ONCE_ASYNC, &pending, &ctx), RS_ONCE_OK);
    MT_EXPECT(pending);
    /* The first to complete wins; the second finds it already done. */
    MT_EXPECT_EQ(rs_once_complete(&CTX, &o, RS_ONCE_ASYNC, (void *)0x2000), RS_ONCE_OK);
    MT_EXPECT_EQ(rs_once_complete(&CTX, &o, RS_ONCE_ASYNC, (void *)0x3000), RS_ONCE_ERR_GEN_FAILURE);
    MT_EXPECT_EQ(o, 0x2002);
    /* Once done: begin reports not pending and the context; CHECK_ONLY succeeds. */
    ctx = NULL;
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, 0, &pending, &ctx), RS_ONCE_OK);
    MT_EXPECT(!pending);
    MT_EXPECT(ctx == (void *)0x2000);
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, RS_ONCE_CHECK_ONLY, &pending, &ctx), RS_ONCE_OK);
    MT_EXPECT(!pending);
    /* A null context is valid and distinct from not started. */
    o = 0;
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, 0, &pending, &ctx), RS_ONCE_OK);
    MT_EXPECT_EQ(rs_once_complete(&CTX, &o, 0, NULL), RS_ONCE_OK);
    MT_EXPECT_EQ(o, 2);
    ctx = (void *)1;
    MT_EXPECT_EQ(rs_once_begin(&CTX, &o, 0, &pending, &ctx), RS_ONCE_OK);
    MT_EXPECT(!pending);
    MT_EXPECT(ctx == NULL);
}

static volatile uintptr_t retry_once;
static volatile int second_ran;

static void retry_second(void *p)
{
    int pend = 0;
    void *c = NULL;
    (void)p;
    if (rs_once_begin(&CTX, &retry_once, 0, &pend, &c) == RS_ONCE_OK && pend) {
        second_ran = 1;
        rs_once_complete(&CTX, &retry_once, 0, (void *)0x40);
    }
}

/* A second thread blocked behind a failing initialiser runs its own initialiser. */
static void once_waiter_retries_after_failure(void)
{
    thr t;
    retry_once = RS_ONCE_PENDING_STATE; /* the first initialiser is running */
    second_ran = 0;
    spawn(&t, retry_second, NULL);
    sleep_ms(50);
    MT_EXPECT(!second_ran); /* still waiting for the first */
    MT_EXPECT_EQ(rs_once_complete(&CTX, &retry_once, RS_ONCE_INIT_FAILED, NULL), RS_ONCE_OK);
    join(&t);
    MT_EXPECT(second_ran);
    MT_EXPECT_EQ(retry_once, 0x42);
}

/* ---- Interlocked list -------------------------------------------------------------------------------- */

typedef struct item {
    rs_slist_entry link;
    int value;
} item;

static rs_slist_header sl;
static item pool[4][2000];
static volatile long popped_sum, popped_count;

static void sl_worker(void *p)
{
    int id = (int)(intptr_t)p, i;
    for (i = 0; i < 2000; ++i) {
        pool[id][i].value = id * 2000 + i + 1;
        rs_slist_push(&sl, &OS, &pool[id][i].link);
        if (i % 3 == 0) {
            rs_slist_entry *e = rs_slist_pop(&sl, &OS);
            if (e) {
                __sync_fetch_and_add(&popped_sum, ((item *)e)->value);
                __sync_fetch_and_add(&popped_count, 1);
            }
        }
    }
}

static void slist_push_pop_flush(void)
{
    thr t[4];
    int i;
    long total = 0;
    rs_slist_entry *e;
    long remaining_sum = 0, remaining_count = 0;
    rs_slist_init(&sl);
    MT_EXPECT_EQ(rs_slist_depth(&sl), 0);
    MT_EXPECT(rs_slist_pop(&sl, &OS) == NULL);
    MT_EXPECT(rs_slist_flush(&sl, &OS) == NULL);
    popped_sum = popped_count = 0;
    for (i = 0; i < 4; ++i)
        spawn(&t[i], sl_worker, (void *)(intptr_t)i);
    for (i = 0; i < 4; ++i)
        join(&t[i]);
    for (i = 0; i < 4 * 2000; ++i)
        total += i + 1;
    e = rs_slist_flush(&sl, &OS);
    for (; e; e = e->next) {
        remaining_sum += ((item *)e)->value;
        ++remaining_count;
    }
    /* Every item was popped exactly once or is still in the list. */
    MT_EXPECT_EQ(popped_sum + remaining_sum, total);
    MT_EXPECT_EQ(popped_count + remaining_count, 4 * 2000);
    MT_EXPECT_EQ(rs_slist_depth(&sl), 0);
}

static void slist_order_and_depth(void)
{
    item a, b, c;
    rs_slist_entry *prev, *f;
    rs_slist_init(&sl);
    prev = rs_slist_push(&sl, &OS, &a.link);
    MT_EXPECT(prev == NULL);
    prev = rs_slist_push(&sl, &OS, &b.link);
    MT_EXPECT(prev == &a.link); /* the previous first entry */
    prev = rs_slist_push(&sl, &OS, &c.link);
    MT_EXPECT(prev == &b.link);
    MT_EXPECT_EQ(rs_slist_depth(&sl), 3);
    MT_EXPECT(rs_slist_pop(&sl, &OS) == &c.link); /* last in, first out */
    MT_EXPECT_EQ(rs_slist_depth(&sl), 2);
    f = rs_slist_flush(&sl, &OS);
    MT_EXPECT(f == &b.link);
    MT_EXPECT(f->next == &a.link);
    MT_EXPECT(a.link.next == NULL);
    MT_EXPECT_EQ(rs_slist_depth(&sl), 0);
    MT_EXPECT(rs_slist_pop(&sl, &OS) == NULL);
    MT_EXPECT_EQ(rs_slist_depth(&sl), 0); /* popping an empty list leaves the depth at zero */
}

static void slist_push_list(void)
{
    item a, b, c, d;
    rs_slist_entry *prev;
    rs_slist_init(&sl);
    rs_slist_push(&sl, &OS, &d.link);
    b.link.next = &c.link; /* the chain a -> b -> c */
    a.link.next = &b.link;
    prev = rs_slist_push_list(&sl, &OS, &a.link, &c.link, 3);
    MT_EXPECT(prev == &d.link);
    MT_EXPECT_EQ(rs_slist_depth(&sl), 4);
    MT_EXPECT(rs_slist_pop(&sl, &OS) == &a.link);
    MT_EXPECT(rs_slist_pop(&sl, &OS) == &b.link);
    MT_EXPECT(rs_slist_pop(&sl, &OS) == &c.link);
    MT_EXPECT(rs_slist_pop(&sl, &OS) == &d.link);
    MT_EXPECT(rs_slist_pop(&sl, &OS) == NULL);
    MT_EXPECT_EQ(rs_slist_depth(&sl), 0);
}

static const mt_case_t cases[] = {
    MT_CASE(park_and_unpark),
    MT_CASE(park_validation_and_timeout),
    MT_CASE(unpark_is_fifo_and_counts),
    MT_CASE(timeout_versus_unpark_race),
    MT_CASE(srw_mutual_exclusion),
    MT_CASE(srw_readers_see_consistent_state),
    MT_CASE(srw_try_and_sharing),
    MT_CASE(srw_waiting_writer_blocks_new_readers),
    MT_CASE(cv_producer_consumer),
    MT_CASE(cv_timeout_reacquires_the_lock),
    MT_CASE(cv_wake_all),
    MT_CASE(cv_wake_before_park_is_not_lost),
    MT_CASE(wait_on_address_basics),
    MT_CASE(once_runs_one_initialiser),
    MT_CASE(once_states_and_flags),
    MT_CASE(once_waiter_retries_after_failure),
    MT_CASE(slist_push_pop_flush),
    MT_CASE(slist_order_and_depth),
    MT_CASE(slist_push_list),
};

int main(void)
{
    (void)run_thread_fn;
    return MT_RUN("rtl_sync_pure", cases);
}
