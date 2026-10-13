/* kshim.h - kernel API shim for userspace unit-testing */
#ifndef NTOBJ_KSHIM_H
#define NTOBJ_KSHIM_H

#ifndef __KERNEL__
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/types.h>
#endif

#define u8  uint8_t
#define u16 uint16_t
#define u32 uint32_t
#define u64 uint64_t
#define s8  int8_t
#define s16 int16_t
#define s32 int32_t
#define s64 int64_t

#define GFP_KERNEL 0
#define GFP_ATOMIC 0

/* Provide NTSTATUS forward decl for userspace helpers */
#ifndef NTSTATUS_DEFINED
#define NTSTATUS_DEFINED
#ifdef __KERNEL__
#include <linux/types.h>
typedef s32 NTSTATUS;
#else
typedef int32_t NTSTATUS;
#endif
#endif
struct _UNICODE_STRING;
typedef struct _UNICODE_STRING UNICODE_STRING;
typedef UNICODE_STRING *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;



#ifndef __user
#define __user
#endif
#ifndef __kernel
#define __kernel
#endif

#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/bug.h>
#define KASSERT(x) BUG_ON(!(x))
#else /* userspace */

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#ifndef access_ok
#define access_ok(p,s) (1)
#endif
#ifndef copy_from_user
#define copy_from_user(dst,src,size) (memcpy((dst),(src),(size)),0)
#endif
#ifndef copy_to_user
#define copy_to_user(dst,src,size) (memcpy((dst),(src),(size)),0)
#endif

/* atomic64 */
typedef struct { _Atomic long long counter; } atomic64_t;
static inline void atomic64_set(atomic64_t *v, long long i) { atomic_store(&v->counter, i); }
static inline long long atomic64_read(atomic64_t *v) { return atomic_load(&v->counter); }
static inline void atomic64_inc(atomic64_t *v) { atomic_fetch_add(&v->counter, 1); }
static inline void atomic64_dec(atomic64_t *v) { atomic_fetch_sub(&v->counter, 1); }
static inline long long atomic64_inc_return(atomic64_t *v) { return atomic_fetch_add(&v->counter, 1) + 1; }
static inline long long atomic64_dec_return(atomic64_t *v) { return atomic_fetch_sub(&v->counter, 1) - 1; }
static inline long long atomic64_add_return(atomic64_t *v, long long a) { return atomic_fetch_add(&v->counter, a) + a; }

/* atomic_t */
typedef struct { _Atomic int counter; } atomic_t;
static inline void atomic_set(atomic_t *v, int i) { atomic_store(&v->counter, i); }
static inline int atomic_read(atomic_t *v) { return atomic_load(&v->counter); }
static inline void atomic_inc(atomic_t *v) { atomic_fetch_add(&v->counter, 1); }
static inline void atomic_dec(atomic_t *v) { atomic_fetch_sub(&v->counter, 1); }
static inline int atomic_inc_return(atomic_t *v) { return atomic_fetch_add(&v->counter, 1) + 1; }
static inline int atomic_dec_return(atomic_t *v) { return atomic_fetch_sub(&v->counter, 1) - 1; }
static inline int atomic_dec_and_test(atomic_t *v) { return atomic_fetch_sub(&v->counter, 1) == 1; }
static inline int atomic_add_return(atomic_t *v, int a) { return atomic_fetch_add(&v->counter, a) + a; }

struct kref { atomic_t refcount; };
static inline void kref_init(struct kref *kref) { atomic_set(&kref->refcount, 1); }
static inline void kref_get(struct kref *kref) { atomic_inc(&kref->refcount); }
static inline int kref_read(struct kref *kref) { return atomic_read(&kref->refcount); }
static inline int kref_put(struct kref *kref, void (*release)(struct kref *kref)) {
    if (atomic_dec_and_test(&kref->refcount)) { release(kref); return 1; }
    return 0;
}

typedef struct { pthread_mutex_t mutex; pthread_cond_t cond; } wait_queue_head_t;
static inline void init_waitqueue_head(wait_queue_head_t *q) { pthread_mutex_init(&q->mutex, NULL); pthread_cond_init(&q->cond, NULL); }
#define wake_up(q) do { pthread_mutex_lock(&(q)->mutex); pthread_cond_broadcast(&(q)->cond); pthread_mutex_unlock(&(q)->mutex); } while(0)
#define wake_up_all(q) wake_up(q)
#define wait_event_interruptible_timeout(wq, condition, timeout_ms) \
    ({ long __ret = 0; struct timespec __ts; int __has_timeout = ((timeout_ms) >= 0); \
       if (__has_timeout && (timeout_ms) > 0) { clock_gettime(CLOCK_REALTIME, &__ts); __ts.tv_sec += (timeout_ms)/1000; __ts.tv_nsec += ((timeout_ms)%1000)*1000000L; if (__ts.tv_nsec >= 1000000000L) { __ts.tv_sec++; __ts.tv_nsec-=1000000000L; } } \
       pthread_mutex_lock(&(wq)->mutex); \
       while (!(condition)) { \
           if ((timeout_ms)==0) { __ret=0; break; } \
           if (!__has_timeout || (timeout_ms) < 0) { pthread_cond_wait(&(wq)->cond, &(wq)->mutex); __ret=1; } \
           else { int __rc = pthread_cond_timedwait(&(wq)->cond, &(wq)->mutex, &__ts); if (__rc==ETIMEDOUT) { __ret=0; break; } __ret=1; } \
           if (condition) { __ret=1; break; } \
       } \
       if (condition) __ret=1; \
       pthread_mutex_unlock(&(wq)->mutex); __ret; })

#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef EISDIR
#define EISDIR 21
#endif
#ifndef EFBIG
#define EFBIG 27
#endif
#ifndef ENOTDIR
#define ENOTDIR 20
#endif

struct file { int fd; bool is_dir; char *path_copy; };
struct kstat {
    dev_t dev; ino_t ino; mode_t mode; off_t size;
    struct timespec atime; struct timespec mtime; struct timespec ctime;
    unsigned long nlink;
};
struct dentry { int dummy; };

struct file *kshim_filp_open(const char *path, int flags, mode_t mode);
void kshim_filp_close(struct file *f);
int kshim_vfs_getattr(struct file *f, struct kstat *st);
ssize_t kshim_kernel_read(struct file *f, void *buf, size_t len, loff_t *pos);
ssize_t kshim_kernel_write(struct file *f, const void *buf, size_t len, loff_t *pos);
int kshim_vfs_truncate(struct file *f, loff_t size);
int kshim_vfs_rename(const char *oldpath, const char *newpath);
int kshim_vfs_unlink(const char *path);
int kshim_vfs_getattr_path(const char *path, struct kstat *st);
void kshim_set_root(const char *root);
NTSTATUS kshim_ntpath_to_linux(const struct _UNICODE_STRING *us, char *out, size_t outlen);
extern char g_kshim_root[PATH_MAX];

/* spinlock / rwlock */
typedef pthread_mutex_t spinlock_t;
static inline void spin_lock_init(spinlock_t *l) { pthread_mutex_init(l, NULL); }
static inline void spin_lock(spinlock_t *l) { pthread_mutex_lock(l); }
static inline void spin_unlock(spinlock_t *l) { pthread_mutex_unlock(l); }
typedef pthread_mutex_t raw_spinlock_t;
static inline void raw_spin_lock_init(raw_spinlock_t *l) { pthread_mutex_init(l, NULL); }
static inline void raw_spin_lock(raw_spinlock_t *l) { pthread_mutex_lock(l); }
static inline void raw_spin_unlock(raw_spinlock_t *l) { pthread_mutex_unlock(l); }
typedef pthread_rwlock_t rwlock_t;
static inline void rwlock_init(rwlock_t *l) { pthread_rwlock_init(l, NULL); }
static inline void read_lock(rwlock_t *l) { pthread_rwlock_rdlock(l); }
static inline void read_unlock(rwlock_t *l) { pthread_rwlock_unlock(l); }
static inline void write_lock(rwlock_t *l) { pthread_rwlock_wrlock(l); }
static inline void write_unlock(rwlock_t *l) { pthread_rwlock_unlock(l); }

/* rbtree shim */
struct rb_node { struct rb_node *rb_left; struct rb_node *rb_right; unsigned long __rb_parent_color; };
struct rb_root { struct rb_node *rb_node; };
#define RB_ROOT (struct rb_root) { NULL }
static inline struct rb_node *rb_parent(const struct rb_node *rb) { return (struct rb_node *)(rb->__rb_parent_color & ~3UL); }
static inline int rb_color(const struct rb_node *rb) { return (int)(rb->__rb_parent_color & 1); }
static inline int rb_is_red(const struct rb_node *rb) { return rb && !rb_color(rb); }
static inline int rb_is_black(const struct rb_node *rb) { return !rb_is_red(rb); }
static inline void rb_set_parent(struct rb_node *rb, struct rb_node *p) { rb->__rb_parent_color = (rb->__rb_parent_color & 3UL) | (unsigned long)p; }
static inline void rb_set_parent_color(struct rb_node *rb, struct rb_node *p, int color) { rb->__rb_parent_color = (unsigned long)p | (unsigned long)color; }
static inline void rb_set_color(struct rb_node *rb, int color) { rb->__rb_parent_color = (rb->__rb_parent_color & ~1UL) | (unsigned long)color; }
static inline void rb_set_red(struct rb_node *rb) { rb_set_color(rb, 0); }
static inline void rb_set_black(struct rb_node *rb) { rb_set_color(rb, 1); }
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **link) { node->__rb_parent_color = (unsigned long)parent; node->rb_left = node->rb_right = NULL; *link = node; }
#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
#define rb_entry(ptr, type, member) container_of(ptr, type, member)

/* ntmm helpers */
#ifndef KSHIM_SECTION_BACKING_DEFINED
#define KSHIM_SECTION_BACKING_DEFINED
struct kshim_section_backing { int fd; size_t size; void *mem; };
#endif
struct kshim_guard_state { int active; void *guard_base; size_t guard_size; unsigned long base_protect; };
extern struct kshim_guard_state kshim_guard;
void *kshim_mmap_reserve(void *addr, size_t size);
int kshim_mmap_commit(void *addr, size_t size, int prot);
int kshim_munmap(void *addr, size_t size);
int kshim_mprotect(void *addr, size_t size, int prot);
void kshim_madvise_free(void *addr, size_t size);
int kshim_section_create(struct kshim_section_backing *b, size_t size);
int kshim_section_create_file(struct kshim_section_backing *b, const char *path, size_t *size);
void *kshim_section_map(struct kshim_section_backing *b, void *addr, size_t offset, size_t size, int prot, int cow);
/* File-backed view: copies the file bytes at offset into the view. Shared writes
 * are written back by mm.c (flush and unmap), so the view is not a live mapping. */
void *kshim_section_map_file(struct kshim_section_backing *b, void *addr, size_t offset, size_t size, int prot, int cow);
void kshim_install_guard_handler(void);
int kshim_handle_guard_fault(void *addr);

/* list_head */
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(name) struct list_head name = { &(name), &(name) }
#define INIT_LIST_HEAD(ptr) do { (ptr)->next = (ptr); (ptr)->prev = (ptr); } while(0)
static inline void __list_add(struct list_head *newEntry, struct list_head *prev, struct list_head *next) {
    next->prev = newEntry; newEntry->next = next; newEntry->prev = prev; prev->next = newEntry;
}
static inline void list_add(struct list_head *newEntry, struct list_head *head) { __list_add(newEntry, head, head->next); }
static inline void list_add_tail(struct list_head *newEntry, struct list_head *head) { __list_add(newEntry, head->prev, head); }
static inline void __list_del(struct list_head *prev, struct list_head *next) { next->prev = prev; prev->next = next; }
static inline void list_del(struct list_head *entry) { __list_del(entry->prev, entry->next); entry->next = NULL; entry->prev = NULL; }
static inline int list_empty(const struct list_head *head) { return head->next == head; }
#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_entry(pos, head, member) \
    for (pos = list_entry((head)->next, __typeof__(*pos), member); &pos->member != (head); pos = list_entry(pos->member.next, __typeof__(*pos), member))
#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_entry((head)->next, __typeof__(*pos), member), n = list_entry(pos->member.next, __typeof__(*pos), member); &pos->member != (head); pos = n, n = list_entry(n->member.next, __typeof__(*n), member))

#ifndef offsetof
#define offsetof(type, member) ((size_t)&((type *)0)->member)
#endif

#define BUG_ON(x) assert(!(x))
#define KASSERT(x) assert((x))

static inline int _kshim_printk(const char *fmt, ...) {
    va_list ap; int n; va_start(ap, fmt); n = vfprintf(stderr, fmt, ap); va_end(ap); return n;
}
#define KERN_ERR ""
#define KERN_WARNING ""
#define KERN_INFO ""
#define pr_err(fmt, ...)   _kshim_printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)  _kshim_printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)  _kshim_printk(KERN_INFO fmt, ##__VA_ARGS__)

#ifdef USERSPACE_KSHIM
extern _Atomic long kshim_alloc_count;
/* Allocation-site tagging: every kmalloc/kzalloc records __FILE__/__LINE__ in
 * a table in kshim.c, and kfree removes it. kshim_leak_report() prints the
 * sites of whatever is still live. */
void *kshim_alloc_tagged(size_t size, int zero, const char *file, int line);
void kshim_free_tagged(const void *p);
void kshim_leak_report(void);
/* Live tagged allocations whose __FILE__ ends with file_suffix (e.g. "sync.c"). */
long kshim_live_site_count(const char *file_suffix);
#define kmalloc(size, flags) kshim_alloc_tagged((size), 0, __FILE__, __LINE__)
#define kzalloc(size, flags) kshim_alloc_tagged((size), 1, __FILE__, __LINE__)
#define kfree(p)             kshim_free_tagged((p))
#else
static inline void *kmalloc(size_t size, int flags) { (void)flags; return malloc(size); }
static inline void *kzalloc(size_t size, int flags) { (void)flags; return calloc(1, size); }
static inline void kfree(const void *p) { free((void*)p); }
#endif

#endif /* __KERNEL__ else */

/* kstrdup allocates through kmalloc, so the tagged leak counter sees both the
 * allocation and the kfree that releases it. A strdup here was never counted,
 * and every kfree of it drove the counter negative. */
static inline char *kstrdup(const char *s, int flags) {
    size_t n = strlen(s) + 1;
    char *d;
    (void)flags;
    d = kmalloc(n, flags);
    if (d) memcpy(d, s, n);
    return d;
}

/* Per-call diagnostic tracing, off by default. Build with -DNT_DEBUG_TRACE to
 * restore the old unconditional prints: same message text, same log levels.
 * NT_TRACE is KERN_INFO (pr_info); NT_TRACE_ERR is KERN_ERR (pr_err). With the
 * macro off the arguments are not evaluated, so a trace call must not carry
 * side effects. */
#ifdef NT_DEBUG_TRACE
#define NT_TRACE(...)     pr_info(__VA_ARGS__)
#define NT_TRACE_ERR(...) pr_err(__VA_ARGS__)
#else
#define NT_TRACE(...)     do { } while (0)
#define NT_TRACE_ERR(...) do { } while (0)
#endif

typedef enum { KernelMode = 0, UserMode = 1, MaximumMode = 2 } KPROCESSOR_MODE;
struct nt_eprocess;
struct nt_ethread;
#ifndef __KERNEL__
extern __thread struct nt_eprocess *kshim_current_proc_ptr;
extern __thread struct nt_ethread *kshim_current_thread_ptr;
static inline struct nt_eprocess *kshim_current_process(void) { return kshim_current_proc_ptr; }
static inline struct nt_ethread *kshim_current_thread(void) { return kshim_current_thread_ptr; }
static inline void kshim_set_current_process(struct nt_eprocess *p) { kshim_current_proc_ptr = p; }
static inline void kshim_set_current_thread(struct nt_ethread *t) { kshim_current_thread_ptr = t; }
#else
/* Kernel: current process/thread via eprocess/ethread registry */
struct nt_eprocess *kshim_current_process(void);
struct nt_ethread *kshim_current_thread(void);
static inline void kshim_set_current_process(struct nt_eprocess *p) { (void)p; }
static inline void kshim_set_current_thread(struct nt_ethread *t) { (void)t; }
#endif

#endif /* NTOBJ_KSHIM_H */
