/* drvhost_plat.c - platform layer for the driver host (POSIX userspace model).
 *
 * Everything that differs between a hosted model and the eventual in-kernel
 * build goes through this file: memory, image mapping, the executive monitor,
 * clocks and threads.  The rest of the driver host calls only these functions.
 */
#include "drvhost_int.h"

#ifndef __KERNEL__
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

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

#else /* __KERNEL__: the personality kernel build. Kernel primitives only. */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/ktime.h>
#include <asm/set_memory.h>

/* dh_alloc hands out 16-byte aligned blocks (the host contract). kmalloc only
 * guarantees its minimum alignment, so the block is over-allocated and the
 * original pointer is stored in the word just before the aligned address. */
void *dh_alloc(size_t n)
{
    unsigned char *raw;
    uintptr_t aligned;
    if (n == 0) n = 1;
    raw = kzalloc(n + 16 + sizeof(void *), GFP_KERNEL);
    if (!raw) return NULL;
    aligned = ((uintptr_t)raw + sizeof(void *) + 15u) & ~(uintptr_t)15u;
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

void dh_free(void *p)
{
    if (!p) return;
    kfree(((void **)p)[-1]);
}

/* Images are page-aligned, zero-filled and RW+NX until dh_protect_rx sets
 * their final permissions. vmalloc areas can have their permissions changed. */
void *dh_map_image(size_t n)
{
    return vzalloc(n);
}

int dh_protect_rx(void *p, size_t n, int exec, int write)
{
    unsigned long addr = (unsigned long)p;
    int pages = (int)DIV_ROUND_UP(n, PAGE_SIZE);
    int rc;
    if (!p || (addr & (PAGE_SIZE - 1)) || pages <= 0) return -1;
    rc = write ? set_memory_rw(addr, pages) : set_memory_ro(addr, pages);
    if (rc) return -1;
    rc = exec ? set_memory_x(addr, pages) : set_memory_nx(addr, pages);
    return rc ? -1 : 0;
}

void dh_unmap_image(void *p, size_t n)
{
    (void)n;
    if (p) vfree(p);
}

/* Reads a whole file from the kernel's root filesystem (the initramfs) with
 * kernel_read. Same limits and result contract as the host branch. */
int dh_read_file(const char *path, unsigned char **out, size_t *len)
{
    enum { DH_MAX_FILE = 64u * 1024u * 1024u };
    struct file *f;
    loff_t size, pos = 0;
    unsigned char *buf;
    ssize_t got;
    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f)) return -1;
    size = i_size_read(file_inode(f));
    if (size <= 0 || size > DH_MAX_FILE) { filp_close(f, NULL); return -1; }
    buf = kvmalloc((size_t)size, GFP_KERNEL);
    if (!buf) { filp_close(f, NULL); return -1; }
    while (pos < size) {
        got = kernel_read(f, buf + pos, (size_t)(size - pos), &pos);
        if (got <= 0) { kvfree(buf); filp_close(f, NULL); return -1; }
    }
    filp_close(f, NULL);
    *out = buf;
    *len = (size_t)size;
    return 0;
}

void dh_free_file(unsigned char *p)
{
    kvfree(p);
}

uint64_t dh_now_ns(void)
{
    return (uint64_t)ktime_get_ns();
}

uint64_t dh_system_time_100ns(void)
{
    /* ktime_get_real_ns counts from 1970; the offset is to 1601. */
    return (uint64_t)ktime_get_real_ns() / 100u + 116444736000000000ull;
}

void dh_sleep_ns(uint64_t ns)
{
    ktime_t kt = ns_to_ktime((s64)ns);
    set_current_state(TASK_UNINTERRUPTIBLE);
    schedule_hrtimeout(&kt, HRTIMER_MODE_REL);
}

/* The monitor is a mutex. dh_wait and dh_timedwait_until sleep on g_wq and
 * wake when g_gen changes, which dh_notify_all does. A waiter reads g_gen
 * before it drops the monitor, so a notify in the gap is never lost. */
static DEFINE_MUTEX(g_mon);
static DECLARE_WAIT_QUEUE_HEAD(g_wq);
static atomic_long_t g_gen = ATOMIC_LONG_INIT(0);

void dh_lock(void)
{
    mutex_lock(&g_mon);
}

void dh_unlock(void)
{
    mutex_unlock(&g_mon);
}

void dh_wait(void)
{
    long gen = atomic_long_read(&g_gen);
    mutex_unlock(&g_mon);
    wait_event(g_wq, atomic_long_read(&g_gen) != gen);
    mutex_lock(&g_mon);
}

int dh_timedwait_until(uint64_t deadline_ns)
{
    long gen = atomic_long_read(&g_gen);
    uint64_t now = dh_now_ns();
    mutex_unlock(&g_mon);
    if (deadline_ns > now)
        wait_event_hrtimeout(g_wq, atomic_long_read(&g_gen) != gen,
                             ns_to_ktime((s64)(deadline_ns - now)));
    mutex_lock(&g_mon);
    return atomic_long_read(&g_gen) == gen ? 1 : 0;
}

void dh_notify_all(void)
{
    atomic_long_inc(&g_gen);
    wake_up_all(&g_wq);
}

/* A host thread is a kthread. The completion is signalled after fn returns,
 * and join waits for it, then drops the task reference with kthread_stop. */
typedef struct dh_native {
    struct task_struct *task;
    struct completion done;
    dh_thread_fn fn;
    void *arg;
} dh_native_t;

static int native_entry(void *p)
{
    dh_native_t *n = p;
    n->fn(n->arg);
    complete(&n->done);
    return 0;
}

void *dh_thread_start(dh_thread_fn fn, void *arg, int *err)
{
    dh_native_t *n = dh_alloc(sizeof *n);
    if (!n) { if (err) *err = ENOMEM; return NULL; }
    n->fn = fn;
    n->arg = arg;
    init_completion(&n->done);
    n->task = kthread_run(native_entry, n, "drvhost");
    if (IS_ERR(n->task)) {
        int rc = -(int)PTR_ERR(n->task);
        dh_free(n);
        if (err) *err = rc;
        return NULL;
    }
    if (err) *err = 0;
    return n;
}

void dh_thread_join(void *handle)
{
    dh_native_t *n = handle;
    if (!n) return;
    wait_for_completion(&n->done);
    kthread_stop(n->task);
    dh_free(n);
}

#endif /* __KERNEL__ */
