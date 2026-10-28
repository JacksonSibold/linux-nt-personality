/*
 * test_k32_tp_pure.c - the thread pool engine of src/ntdll/k32_tp_pure.h on pthreads: tasks from several threads, the
 * worker limit, work objects (wait and cancel), timers (one-shot, periodic, cancel, re-arm, close while running), and
 * workers leaving when idle.
 */
#include "minitest.h"
#include "k32_tp_pure.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* ---- pthread flavour of ktp_os -------------------------------------------------------------------------------------------- */

static void *p_lock_new(void)
{
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutex_init(m, NULL);
    return m;
}
static void p_lock(void *m) { pthread_mutex_lock(m); }
static void p_unlock(void *m) { pthread_mutex_unlock(m); }
static void *p_cond_new(void)
{
    pthread_cond_t *c = malloc(sizeof *c);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    return c;
}
static uint64_t p_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static int p_cond_wait(void *c, void *m, uint64_t ms)
{
    if (ms == KTP_FOREVER) {
        pthread_cond_wait(c, m);
        return 0;
    }
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += (time_t)(ms / 1000);
        ts.tv_nsec += (long)(ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        return pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT ? 1 : 0;
    }
}
static void p_signal(void *c) { pthread_cond_signal(c); }
static void p_broadcast(void *c) { pthread_cond_broadcast(c); }

typedef struct {
    void (*fn)(void *);
    void *arg;
} spawn_arg;

static void *thread_main(void *p)
{
    spawn_arg a = *(spawn_arg *)p;
    free(p);
    a.fn(a.arg);
    return NULL;
}

static int p_spawn(void (*fn)(void *), void *arg)
{
    pthread_t t;
    spawn_arg *a = malloc(sizeof *a);
    a->fn = fn;
    a->arg = arg;
    if (pthread_create(&t, NULL, thread_main, a) != 0) {
        free(a);
        return -1;
    }
    pthread_detach(t);
    return 0;
}

static void *p_alloc(size_t n) { return malloc(n); }
static void p_release(void *p) { free(p); }

/* Events: a mutex, a condition and a flag. auto_reset ones (the test's "handles") give the signal to one waiter. */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int set, auto_reset;
} pevent;

static pevent *pevent_new(int auto_reset)
{
    pevent *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->auto_reset = auto_reset;
    return e;
}
static void *p_event_new(void) { return pevent_new(0); }
static void pevent_signal(pevent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = 1;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}
static void p_event_set(void *e) { pevent_signal(e); }
static void p_event_free(void *e) { free(e); }

/* Waits on two events by polling: simple, and good enough for a test. */
static int p_wait2(void *h, void *cancel, uint64_t ms)
{
    pevent *hh = h, *cc = cancel;
    uint64_t start = p_now();
    for (;;) {
        pthread_mutex_lock(&cc->m);
        int cancelled = cc->set;
        pthread_mutex_unlock(&cc->m);
        if (cancelled)
            return 2;
        pthread_mutex_lock(&hh->m);
        if (hh->set) {
            if (hh->auto_reset)
                hh->set = 0;
            pthread_mutex_unlock(&hh->m);
            return 0;
        }
        pthread_mutex_unlock(&hh->m);
        if (ms != KTP_FOREVER && p_now() - start >= ms)
            return 1;
        usleep(1000);
    }
}

static const ktp_os os = {p_lock_new, p_lock, p_unlock, p_cond_new, p_cond_wait, p_signal, p_broadcast, p_spawn, p_now, p_alloc, p_release,
                          p_event_new, p_event_set, p_event_free, p_wait2};

static void sleep_ms(unsigned ms) { usleep(ms * 1000); }

/* The pools are kept for the life of the process: idle workers may still look at one after its test returns. */
static ktp *g_pools[32];
static int g_npools;

static ktp *new_tp(void)
{
    ktp *tp = calloc(1, sizeof *tp);
    g_pools[g_npools++] = tp;
    return tp;
}

/* ---- tests --------------------------------------------------------------------------------------------------------------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_count;
static int g_max_concurrent, g_concurrent;

static void count_task(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&g_mu);
    ++g_count;
    pthread_mutex_unlock(&g_mu);
}

typedef struct {
    ktp *tp;
    int n;
} submitter;

static void *submit_thread(void *arg)
{
    submitter *s = arg;
    for (int i = 0; i < s->n; ++i)
        ktp_submit(s->tp, count_task, NULL);
    return NULL;
}

static int count_now(void)
{
    int c;
    pthread_mutex_lock(&g_mu);
    c = g_count;
    pthread_mutex_unlock(&g_mu);
    return c;
}

static int wait_for_count(int want, unsigned timeout_ms)
{
    for (unsigned t = 0; t < timeout_ms; t += 2) {
        if (count_now() >= want)
            return 1;
        sleep_ms(2);
    }
    return count_now() >= want;
}

static void every_task_runs_exactly_once(void)
{
    ktp *tp = new_tp();
    pthread_t th[4];
    submitter s[4];
    MT_ASSERT(ktp_init(tp, &os) == 0);
    ktp_set_limits(tp, 0, 8);
    g_count = 0;
    for (int i = 0; i < 4; ++i) {
        s[i].tp = tp;
        s[i].n = 500;
        pthread_create(&th[i], NULL, submit_thread, &s[i]);
    }
    for (int i = 0; i < 4; ++i)
        pthread_join(th[i], NULL);
    MT_EXPECT(wait_for_count(2000, 5000));
    sleep_ms(30);
    MT_EXPECT_EQ(count_now(), 2000);
    MT_EXPECT(tp->workers <= 8);
}

static void slow_task(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&g_mu);
    if (++g_concurrent > g_max_concurrent)
        g_max_concurrent = g_concurrent;
    pthread_mutex_unlock(&g_mu);
    sleep_ms(20);
    pthread_mutex_lock(&g_mu);
    --g_concurrent;
    ++g_count;
    pthread_mutex_unlock(&g_mu);
}

static void worker_limit_bounds_concurrency(void)
{
    ktp *tp = new_tp();
    MT_ASSERT(ktp_init(tp, &os) == 0);
    ktp_set_limits(tp, 0, 3);
    g_count = g_concurrent = g_max_concurrent = 0;
    for (int i = 0; i < 12; ++i)
        ktp_submit(tp, slow_task, NULL);
    MT_EXPECT(wait_for_count(12, 5000));
    MT_EXPECT(g_max_concurrent <= 3 && g_max_concurrent >= 2);
}

static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate_open, g_started;

static void gated_work(void *ctx, ktp_work *w)
{
    (void)ctx;
    (void)w;
    pthread_mutex_lock(&g_gate_mu);
    ++g_started;
    while (!g_gate_open)
        pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    pthread_mutex_unlock(&g_gate_mu);
    pthread_mutex_lock(&g_mu);
    ++g_count;
    pthread_mutex_unlock(&g_mu);
}

static void open_gate(void)
{
    pthread_mutex_lock(&g_gate_mu);
    g_gate_open = 1;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}

static void work_wait_and_cancel(void)
{
    ktp *tp = new_tp();
    ktp_work *w;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    ktp_set_limits(tp, 0, 1); /* one worker: the first callback blocks, the rest queue behind it */
    g_count = g_gate_open = g_started = 0;
    w = ktp_work_new(tp, gated_work, NULL);
    MT_ASSERT(w != NULL);
    for (int i = 0; i < 5; ++i)
        MT_EXPECT(ktp_work_submit(w) == 0);
    while (1) { /* wait for the first to start */
        pthread_mutex_lock(&g_gate_mu);
        int s = g_started;
        pthread_mutex_unlock(&g_gate_mu);
        if (s)
            break;
        sleep_ms(1);
    }
    open_gate();
    ktp_work_wait(w, 1);
    MT_EXPECT_EQ(count_now(), 1);
    MT_EXPECT_EQ((int)w->pending, 0);
    /* waiting without cancel runs all of them */
    g_count = g_started = 0;
    for (int i = 0; i < 5; ++i)
        ktp_work_submit(w);
    ktp_work_wait(w, 0);
    MT_EXPECT_EQ(count_now(), 5);
    ktp_work_close(w);
}

static void cancel_drops_queued_callbacks(void)
{
    ktp *tp = new_tp();
    ktp_work *w;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    ktp_set_limits(tp, 0, 1);
    g_count = g_gate_open = g_started = 0;
    w = ktp_work_new(tp, gated_work, NULL);
    for (int i = 0; i < 5; ++i)
        ktp_work_submit(w);
    while (1) {
        pthread_mutex_lock(&g_gate_mu);
        int s = g_started;
        pthread_mutex_unlock(&g_gate_mu);
        if (s)
            break;
        sleep_ms(1);
    }
    /* the first callback is running (blocked); cancel while it is blocked, from a helper that opens the gate later */
    tp->os->lock(tp->lock);
    {
        unsigned removed = ktp_remove_locked(tp, ktp_run_work, w);
        w->pending -= removed;
        MT_EXPECT_EQ((int)removed, 4);
    }
    tp->os->unlock(tp->lock);
    open_gate();
    ktp_work_wait(w, 0);
    MT_EXPECT_EQ(count_now(), 1);
    ktp_work_close(w);
}

static int g_timer_fires;
static uint64_t g_fire_at[16];

static void timer_cb(void *ctx, ktp_timer *t)
{
    (void)ctx;
    (void)t;
    pthread_mutex_lock(&g_mu);
    if (g_timer_fires < 16)
        g_fire_at[g_timer_fires] = p_now();
    ++g_timer_fires;
    pthread_mutex_unlock(&g_mu);
}

static int fires(void)
{
    int c;
    pthread_mutex_lock(&g_mu);
    c = g_timer_fires;
    pthread_mutex_unlock(&g_mu);
    return c;
}

static void one_shot_timer_fires_once_after_its_delay(void)
{
    ktp *tp = new_tp();
    ktp_timer *t;
    uint64_t start;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    g_timer_fires = 0;
    t = ktp_timer_new(tp, timer_cb, NULL);
    start = p_now();
    MT_EXPECT(ktp_timer_set(t, 40, 0) == 0);
    sleep_ms(10);
    MT_EXPECT_EQ(fires(), 0);
    sleep_ms(120);
    MT_EXPECT_EQ(fires(), 1);
    MT_EXPECT(g_fire_at[0] >= start + 40);
    MT_EXPECT(!ktp_timer_is_set(t));
    ktp_timer_close(t);
}

static void periodic_timer_repeats_until_cancelled(void)
{
    ktp *tp = new_tp();
    ktp_timer *t;
    int at_cancel;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    g_timer_fires = 0;
    t = ktp_timer_new(tp, timer_cb, NULL);
    MT_EXPECT(ktp_timer_set(t, 10, 20) == 0);
    sleep_ms(150);
    MT_EXPECT(fires() >= 4 && fires() <= 9);
    ktp_timer_set(t, -1, 0); /* cancel */
    ktp_timer_wait(t, 0);
    at_cancel = fires();
    sleep_ms(80);
    MT_EXPECT_EQ(fires(), at_cancel);
    ktp_timer_close(t);
}

static void re_arming_replaces_the_due_time(void)
{
    ktp *tp = new_tp();
    ktp_timer *t;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    g_timer_fires = 0;
    t = ktp_timer_new(tp, timer_cb, NULL);
    ktp_timer_set(t, 500, 0);
    ktp_timer_set(t, 20, 0); /* earlier than before: the timer thread must wake for it */
    sleep_ms(100);
    MT_EXPECT_EQ(fires(), 1);
    ktp_timer_close(t);
}

static void two_timers_fire_in_due_order(void)
{
    ktp *tp = new_tp();
    ktp_timer *a, *b;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    g_timer_fires = 0;
    a = ktp_timer_new(tp, timer_cb, (void *)1);
    b = ktp_timer_new(tp, timer_cb, (void *)2);
    ktp_timer_set(a, 80, 0);
    ktp_timer_set(b, 20, 0);
    sleep_ms(160);
    MT_EXPECT_EQ(fires(), 2);
    MT_EXPECT(g_fire_at[0] <= g_fire_at[1]);
    ktp_timer_close(a);
    ktp_timer_close(b);
}

static int g_slow_cb_done;

static void slow_timer_cb(void *ctx, ktp_timer *t)
{
    (void)ctx;
    (void)t;
    sleep_ms(80);
    pthread_mutex_lock(&g_mu);
    g_slow_cb_done = 1;
    pthread_mutex_unlock(&g_mu);
}

static void close_waits_for_a_running_callback(void)
{
    ktp *tp = new_tp();
    ktp_timer *t;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    g_slow_cb_done = 0;
    t = ktp_timer_new(tp, slow_timer_cb, NULL);
    ktp_timer_set(t, 5, 0);
    sleep_ms(30); /* the callback is running now */
    ktp_timer_close(t);
    MT_EXPECT_EQ(g_slow_cb_done, 1);
}

static void idle_workers_leave(void)
{
    ktp *tp = new_tp();
    MT_ASSERT(ktp_init(tp, &os) == 0);
    tp->idle_ms = 40;
    ktp_set_limits(tp, 0, 6);
    g_count = 0;
    for (int i = 0; i < 20; ++i)
        ktp_submit(tp, slow_task, NULL);
    MT_EXPECT(wait_for_count(20, 5000));
    sleep_ms(300);
    tp->os->lock(tp->lock);
    MT_EXPECT_EQ((int)tp->workers, 0);
    tp->os->unlock(tp->lock);
    /* and a later task brings a worker back */
    ktp_submit(tp, count_task, NULL);
    MT_EXPECT(wait_for_count(21, 2000));
}

static void minimum_workers_stay(void)
{
    ktp *tp = new_tp();
    MT_ASSERT(ktp_init(tp, &os) == 0);
    tp->idle_ms = 30;
    ktp_set_limits(tp, 2, 6);
    g_count = 0;
    for (int i = 0; i < 12; ++i)
        ktp_submit(tp, slow_task, NULL);
    MT_EXPECT(wait_for_count(12, 5000));
    sleep_ms(250);
    tp->os->lock(tp->lock);
    MT_EXPECT_EQ((int)tp->workers, 2);
    tp->os->unlock(tp->lock);
}

static int g_wait_calls, g_wait_results[8];

static void wait_cb(void *ctx, ktp_wait *w, int result)
{
    (void)ctx;
    (void)w;
    pthread_mutex_lock(&g_mu);
    if (g_wait_calls < 8)
        g_wait_results[g_wait_calls] = result;
    ++g_wait_calls;
    pthread_mutex_unlock(&g_mu);
}

static int wait_calls(void)
{
    int c;
    pthread_mutex_lock(&g_mu);
    c = g_wait_calls;
    pthread_mutex_unlock(&g_mu);
    return c;
}

static void a_signalled_handle_queues_the_callback_once(void)
{
    ktp *tp = new_tp();
    pevent *h = pevent_new(1);
    ktp_wait *w;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    g_wait_calls = 0;
    w = ktp_wait_new(tp, wait_cb, NULL, 0);
    MT_EXPECT(ktp_wait_set(w, h, KTP_FOREVER) == 0);
    sleep_ms(20);
    MT_EXPECT_EQ(wait_calls(), 0);
    pevent_signal(h);
    sleep_ms(60);
    MT_EXPECT_EQ(wait_calls(), 1);
    MT_EXPECT_EQ(g_wait_results[0], 0);
    pevent_signal(h); /* a single wait is spent */
    sleep_ms(40);
    MT_EXPECT_EQ(wait_calls(), 1);
    ktp_wait_close(w);
    free(h);
}

static void a_wait_times_out(void)
{
    ktp *tp = new_tp();
    pevent *h = pevent_new(1);
    ktp_wait *w;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    g_wait_calls = 0;
    w = ktp_wait_new(tp, wait_cb, NULL, 0);
    ktp_wait_set(w, h, 30);
    sleep_ms(120);
    MT_EXPECT_EQ(wait_calls(), 1);
    MT_EXPECT_EQ(g_wait_results[0], 1);
    ktp_wait_close(w);
    free(h);
}

static void a_repeating_wait_rearms_after_each_callback(void)
{
    ktp *tp = new_tp();
    pevent *h = pevent_new(1);
    ktp_wait *w;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    g_wait_calls = 0;
    w = ktp_wait_new(tp, wait_cb, NULL, 1);
    ktp_wait_set(w, h, KTP_FOREVER);
    for (int i = 0; i < 3; ++i) {
        pevent_signal(h);
        sleep_ms(50);
    }
    MT_EXPECT_EQ(wait_calls(), 3);
    ktp_wait_close(w);
    pevent_signal(h);
    sleep_ms(40);
    MT_EXPECT_EQ(wait_calls(), 3); /* closed: no more */
    free(h);
}

static void disarming_stops_a_wait(void)
{
    ktp *tp = new_tp();
    pevent *h = pevent_new(1);
    ktp_wait *w;
    MT_ASSERT(ktp_init(tp, &os) == 0);
    g_wait_calls = 0;
    w = ktp_wait_new(tp, wait_cb, NULL, 0);
    ktp_wait_set(w, h, KTP_FOREVER);
    sleep_ms(10);
    ktp_wait_set(w, NULL, 0);
    pevent_signal(h);
    sleep_ms(60);
    MT_EXPECT_EQ(wait_calls(), 0);
    /* arming again works */
    ktp_wait_set(w, h, KTP_FOREVER);
    sleep_ms(60);
    MT_EXPECT_EQ(wait_calls(), 1);
    ktp_wait_close(w);
    free(h);
}

static const mt_case_t cases[] = {
    MT_CASE(a_signalled_handle_queues_the_callback_once), MT_CASE(a_wait_times_out),
    MT_CASE(a_repeating_wait_rearms_after_each_callback), MT_CASE(disarming_stops_a_wait),
    MT_CASE(every_task_runs_exactly_once), MT_CASE(worker_limit_bounds_concurrency), MT_CASE(work_wait_and_cancel),
    MT_CASE(cancel_drops_queued_callbacks), MT_CASE(one_shot_timer_fires_once_after_its_delay),
    MT_CASE(periodic_timer_repeats_until_cancelled), MT_CASE(re_arming_replaces_the_due_time),
    MT_CASE(two_timers_fire_in_due_order), MT_CASE(close_waits_for_a_running_callback), MT_CASE(idle_workers_leave),
    MT_CASE(minimum_workers_stay),
};

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) /* run one case by index, for debugging */
        return mt_run("k32_tp_pure", &cases[atoi(argv[1])], 1);
    return MT_RUN("k32_tp_pure", cases);
}
