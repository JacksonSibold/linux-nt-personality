/* drvhost_plat.c - platform layer for the driver host (POSIX userspace model).
 *
 * Everything that differs between a hosted model and the eventual in-kernel
 * build goes through this file: memory, image mapping, the executive monitor,
 * clocks and threads.  The rest of the driver host calls only these functions.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "drvhost_int.h"

static pthread_mutex_t g_mon = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv;
static pthread_once_t  g_cv_once = PTHREAD_ONCE_INIT;

static void cv_init(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cv, &a);
    pthread_condattr_destroy(&a);
}

void *dh_alloc(size_t n)
{
    if (n == 0) n = 1;
    return calloc(1, n);
}

void dh_free(void *p)
{
    free(p);
}

void *dh_map_image(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int dh_protect_rx(void *p, size_t n, int exec, int write)
{
    int prot = PROT_READ;
    if (write) prot |= PROT_WRITE;
    if (exec)  prot |= PROT_EXEC;
    return mprotect(p, n, prot);
}

void dh_unmap_image(void *p, size_t n)
{
    if (p) munmap(p, n);
}

int dh_read_file(const char *path, unsigned char **out, size_t *len)
{
    enum { DH_MAX_FILE = 64u * 1024u * 1024u };
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long end = ftell(f);
    if (end <= 0 || (unsigned long)end > DH_MAX_FILE) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    unsigned char *buf = malloc((size_t)end);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)end, f);
    fclose(f);
    if (got != (size_t)end) { free(buf); return -1; }
    *out = buf;
    *len = (size_t)end;
    return 0;
}

void dh_free_file(unsigned char *p)
{
    free(p);
}

uint64_t dh_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t dh_system_time_100ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t unix100 = (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100u;
    /* Offset between 1601-01-01 and 1970-01-01 in 100ns units. */
    return unix100 + 116444736000000000ull;
}

void dh_sleep_ns(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

void dh_lock(void)
{
    pthread_once(&g_cv_once, cv_init);
    pthread_mutex_lock(&g_mon);
}

void dh_unlock(void)
{
    pthread_mutex_unlock(&g_mon);
}

void dh_wait(void)
{
    pthread_once(&g_cv_once, cv_init);
    pthread_cond_wait(&g_cv, &g_mon);
}

int dh_timedwait_until(uint64_t deadline_ns)
{
    pthread_once(&g_cv_once, cv_init);
    struct timespec ts;
    ts.tv_sec = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    int rc = pthread_cond_timedwait(&g_cv, &g_mon, &ts);
    return rc == ETIMEDOUT ? 1 : 0;
}

void dh_notify_all(void)
{
    pthread_once(&g_cv_once, cv_init);
    pthread_cond_broadcast(&g_cv);
}

typedef struct dh_native {
    pthread_t tid;
    dh_thread_fn fn;
    void *arg;
} dh_native_t;

static void *native_entry(void *p)
{
    dh_native_t *n = p;
    return n->fn(n->arg);
}

void *dh_thread_start(dh_thread_fn fn, void *arg, int *err)
{
    dh_native_t *n = dh_alloc(sizeof *n);
    if (!n) { if (err) *err = ENOMEM; return NULL; }
    n->fn = fn;
    n->arg = arg;
    int rc = pthread_create(&n->tid, NULL, native_entry, n);
    if (rc != 0) { free(n); if (err) *err = rc; return NULL; }
    if (err) *err = 0;
    return n;
}

void dh_thread_join(void *handle)
{
    dh_native_t *n = handle;
    if (!n) return;
    pthread_join(n->tid, NULL);
    free(n);
}
