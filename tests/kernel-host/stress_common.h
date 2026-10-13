/* stress_common.h - shared helpers for the concurrency stress programs
 * (stress_handle_test.c, stress_object_test.c, stress_sync_test.c).
 *
 * Provides a seeded PRNG (per-thread streams derived from one seed, so a
 * failing run is reproducible from its seed), a reusable barrier, a
 * progress-based watchdog, and argument parsing. Host (userspace kshim) only.
 */
#ifndef STRESS_COMMON_H
#define STRESS_COMMON_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STRESS_MIN_THREADS 8
#define STRESS_MAX_THREADS 16
#define STRESS_WATCHDOG_SECS 60

/* splitmix64: small, fast, and good enough for choosing operations. */
typedef struct { uint64_t s; } stress_rng;

static inline void stress_rng_seed(stress_rng *r, uint64_t seed, uint64_t stream)
{
    r->s = seed ^ ((stream + 1) * 0xD1B54A32D192ED03ULL);
}

static inline uint64_t stress_next(stress_rng *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform-ish value in [0, n). n must be non-zero. */
static inline uint32_t stress_rnd(stress_rng *r, uint32_t n)
{
    return (uint32_t)(stress_next(r) % n);
}

/* Reusable barrier (cyclic). pthread_barrier_t is not assumed on winpthreads. */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    unsigned        parties;
    unsigned        waiting;
    unsigned        generation;
} stress_barrier;

static inline void stress_barrier_init(stress_barrier *b, unsigned parties)
{
    pthread_mutex_init(&b->m, NULL);
    pthread_cond_init(&b->c, NULL);
    b->parties = parties;
    b->waiting = 0;
    b->generation = 0;
}

static inline void stress_barrier_wait(stress_barrier *b)
{
    pthread_mutex_lock(&b->m);
    unsigned gen = b->generation;
    if (++b->waiting == b->parties) {
        b->waiting = 0;
        b->generation++;
        pthread_cond_broadcast(&b->c);
    } else {
        while (gen == b->generation)
            pthread_cond_wait(&b->c, &b->m);
    }
    pthread_mutex_unlock(&b->m);
}

/* Progress watchdog. Workers call stress_progress() per operation. If the
 * counter does not move for STRESS_WATCHDOG_SECS, the watchdog prints a
 * report (test name, seed, last counter value) and aborts the process, so a
 * deadlock fails the run instead of hanging it. */
typedef struct {
    _Atomic unsigned long progress;
    _Atomic int           done;
    const char           *name;
    unsigned long         seed;
    pthread_t             thread;
} stress_watchdog;

static inline void stress_progress(stress_watchdog *wd)
{
    atomic_fetch_add_explicit(&wd->progress, 1, memory_order_relaxed);
}

static void *stress_watchdog_main(void *arg)
{
    stress_watchdog *wd = (stress_watchdog *)arg;
    unsigned long last = atomic_load(&wd->progress);
    time_t last_change = time(NULL);
    while (!atomic_load(&wd->done)) {
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        unsigned long now = atomic_load(&wd->progress);
        if (now != last) {
            last = now;
            last_change = time(NULL);
            continue;
        }
        if (time(NULL) - last_change >= STRESS_WATCHDOG_SECS) {
            fprintf(stderr,
                    "WATCHDOG: %s made no progress for %d s (progress=%lu seed=%lu)\n"
                    "WATCHDOG: probable deadlock; aborting\n",
                    wd->name, STRESS_WATCHDOG_SECS, now, wd->seed);
            fflush(stderr);
            abort();
        }
    }
    return NULL;
}

static inline void stress_watchdog_start(stress_watchdog *wd, const char *name, unsigned long seed)
{
    atomic_init(&wd->progress, 0);
    atomic_init(&wd->done, 0);
    wd->name = name;
    wd->seed = seed;
    pthread_create(&wd->thread, NULL, stress_watchdog_main, wd);
}

static inline void stress_watchdog_stop(stress_watchdog *wd)
{
    atomic_store(&wd->done, 1);
    pthread_join(wd->thread, NULL);
}

/* argv: [seed] [threads] [ops-per-thread]. Threads are clamped to 8..16. */
typedef struct {
    unsigned long seed;
    unsigned      threads;
    unsigned      ops;
} stress_args;

static inline void stress_parse_args(int argc, char **argv, stress_args *a,
                                     const char *prog, unsigned default_ops)
{
    a->seed = argc > 1 ? strtoul(argv[1], NULL, 0) : 1UL;
    a->threads = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 12U;
    a->ops = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : default_ops;
    if (a->threads < STRESS_MIN_THREADS) a->threads = STRESS_MIN_THREADS;
    if (a->threads > STRESS_MAX_THREADS) a->threads = STRESS_MAX_THREADS;
    if (a->ops < 64) a->ops = 64;
    printf("%s: seed=%lu threads=%u ops=%u (usage: %s [seed] [threads 8..16] [ops])\n",
           prog, a->seed, a->threads, a->ops, prog);
    fflush(stdout);
}

/* Violation counter shared by workers. Workers never return early on an
 * error (that would strand the barriers); they count and continue. */
static _Atomic long stress_errors;
static unsigned long stress_seed_for_report;

#define STRESS_CHECK(cond, ...) do { \
    if (!(cond)) { \
        long _n = atomic_fetch_add(&stress_errors, 1) + 1; \
        if (_n <= 20) { \
            fprintf(stderr, "VIOLATION seed=%lu %s:%d: %s: ", \
                    stress_seed_for_report, __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } \
} while (0)

#endif /* STRESS_COMMON_H */
