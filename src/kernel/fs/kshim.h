/* kshim.h - kernel API shim for userspace unit-testing of binfmt_pe.c.
 *
 * Included by binfmt_pe.c when compiled outside the kernel
 * (PE_BINFMT_TEST is defined).  Provides stub types and functions that
 * mirror the kernel APIs the loader calls, so the *logic* of base selection,
 * section mapping, relocation, PEB/TEB construction and stack layout can be
 * exercised without booting a kernel.
 */

#ifndef KSHIM_H
#define KSHIM_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <stdint.h>
#include <sys/syscall.h>

/* pe_format.h defines u8/u16/u32/u64 in userspace.  Include it first so the
 * shim can use those names without redeclaring them. */
#include "../../loader/pe_format.h"

/* -------------------------------------------------------------------------- */
/* Minimal kernel types                                                       */
/* -------------------------------------------------------------------------- */
struct task_struct {
    int pid;
    void *gsbase;
    int nt_abi;
};

struct mm_struct {
    int pe_image;
    u64 mmap_base_hint;
};

struct file {
    int fd;
    u64 size;
};

struct pt_regs {
    unsigned long cx;
    unsigned long sp;
    unsigned long ip;
};

struct linux_binprm {
    const char *filename;
    const u8 *buf;
    size_t buf_len;
    struct file *file;
    struct mm_struct *mm;
    struct pt_regs *regs;
    /* test-only: argv/env passed via bprm for command-line tests */
    int argc;
    int envc;
    const char *const *argv;
    const char *const *envp;
};

#define current ((struct task_struct *)kshim_current_task())

static inline struct task_struct *kshim_current_task(void)
{
    static struct task_struct t = { .pid = 42, .gsbase = NULL, .nt_abi = 0 };
    return &t;
}

/* -------------------------------------------------------------------------- */
/* Constants                                                                  */
/* -------------------------------------------------------------------------- */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096U
#define PAGE_SHIFT 12U
#endif

#ifndef PAGE_MASK
#define PAGE_MASK (~(PAGE_SIZE - 1))
#endif

#ifndef PAGE_ALIGN
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & ~((unsigned long)PAGE_SIZE - 1))
#endif
#ifndef PAGE_ALIGN_DOWN
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~((unsigned long)PAGE_SIZE - 1))
#endif

#ifndef PROT_READ
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define PROT_NONE  0x0
#endif

#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANONYMOUS 0x20
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MAP_DENYWRITE
#define MAP_DENYWRITE       0x000800
#endif

#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define BINPRM_BUF_SIZE 128

/* errno already from <errno.h>; ensure ENOEXEC etc available */
#ifndef ENOEXEC
#define ENOEXEC 8
#endif

#ifndef IS_ERR_VALUE
#define IS_ERR_VALUE(x) ((unsigned long)(void *)(long)(x) >= (unsigned long)-4095)
#endif

/* -------------------------------------------------------------------------- */
/* Logging / printing                                                         */
/* -------------------------------------------------------------------------- */
#define KERN_ERR ""
#define KERN_WARNING ""
#define KERN_INFO ""

static inline int printk(const char *fmt, ...)
{
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return n;
}

#define pr_err(fmt, ...) printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO fmt, ##__VA_ARGS__)

/* -------------------------------------------------------------------------- */
/* Memory allocation                                                          */
/* -------------------------------------------------------------------------- */
static inline void *kmalloc(size_t size, int flags)
{
    (void)flags;
    return malloc(size);
}

static inline void *kzalloc(size_t size, int flags)
{
    (void)flags;
    void *p = calloc(1, size);
    return p;
}

static inline void kfree(const void *p)
{
    free((void *)p);
}

/* -------------------------------------------------------------------------- */
/* Access helpers                                                             */
/* -------------------------------------------------------------------------- */
static inline int access_ok(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
    return 1;
}

static inline unsigned long copy_to_user(void *to, const void *from, unsigned long n)
{
    memcpy(to, from, n);
    return 0;
}

__attribute__((no_sanitize_address))
static inline unsigned long clear_user(void *addr, unsigned long n)
{
    volatile char *p = (volatile char *)addr;
    for (unsigned long i = 0; i < n; ++i) p[i] = 0;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* File / mapping stubs                                                       */
/* -------------------------------------------------------------------------- */
struct kshim_mapping {
    void *addr;
    size_t len;
    int prot;
    unsigned long flags;
    u64 file_off;
    struct file *file;
    struct kshim_mapping *next;
};

extern struct kshim_mapping *kshim_mappings;
extern u64 kshim_preferred_base_ok;
extern u64 kshim_forced_random_base;

static inline struct kshim_mapping *kshim_mapping_alloc(void *addr, size_t len,
                                                        int prot, unsigned long flags,
                                                        u64 file_off,
                                                        struct file *f)
{
    struct kshim_mapping *m = malloc(sizeof(*m));
    if (!m) return NULL;
    m->addr = addr;
    m->len = len;
    m->prot = prot;
    m->flags = flags;
    m->file_off = file_off;
    m->file = f;
    m->next = kshim_mappings;
    kshim_mappings = m;
    return m;
}

static inline void kshim_mapping_reset(void)
{
    struct kshim_mapping *m = kshim_mappings;
    while (m) {
        struct kshim_mapping *next = m->next;
        if (m->addr && m->addr != MAP_FAILED)
            munmap(m->addr, m->len);
        free(m);
        m = next;
    }
    kshim_mappings = NULL;
    kshim_preferred_base_ok = 1;
    kshim_forced_random_base = 0;
}

__attribute__((no_sanitize_address))
static inline long kernel_read(struct file *f, u64 offset, void *buf, unsigned long count)
{
    if (!f || f->fd < 0) return -1;
    if (lseek(f->fd, (off_t)offset, SEEK_SET) < 0) return -errno;
    ssize_t n = (ssize_t)syscall(SYS_read, f->fd, buf, count);
    if (n < 0) return -errno;
    return n;
}

__attribute__((no_sanitize_address))
static inline unsigned long vm_mmap(struct file *f, unsigned long addr,
                                    unsigned long len, unsigned long prot,
                                    unsigned long flags, unsigned long offset)
{
    int mmap_flags = 0;
    int mmap_prot = (int)(prot & 0x7);
    void *p;

    if ((flags & MAP_FIXED) || (flags & MAP_FIXED_NOREPLACE))
        mmap_flags |= MAP_FIXED;
    if (flags & MAP_PRIVATE)
        mmap_flags |= MAP_PRIVATE;
    if (flags & MAP_ANONYMOUS)
        mmap_flags |= MAP_ANONYMOUS;

    if ((flags & MAP_FIXED_NOREPLACE) && kshim_preferred_base_ok == 0) {
        return (unsigned long)-ENOMEM;
    }

    int need_write = (f && !(flags & MAP_ANONYMOUS));
    int effective_prot = need_write ? (mmap_prot | PROT_WRITE) : mmap_prot;

    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        int anon_flags = mmap_flags | MAP_FIXED | MAP_ANONYMOUS;
        p = mmap((void *)addr, len, effective_prot, anon_flags, -1, 0);
        if (p == MAP_FAILED) {
            pr_info("kshim vm_mmap FIXED_NOREPLACE failed addr=0x%lx errno=%d\n", addr, errno);
            if (kshim_forced_random_base) {
                addr = (unsigned long)kshim_forced_random_base;
                p = mmap((void *)addr, len, effective_prot, anon_flags, -1, 0);
            }
            if (p == MAP_FAILED)
                return (unsigned long)-ENOMEM;
        }
    } else if (flags & MAP_FIXED) {
        int anon_flags = mmap_flags | MAP_FIXED | MAP_ANONYMOUS;
        p = mmap((void *)addr, len, effective_prot, anon_flags, -1, 0);
        if (p == MAP_FAILED) {
            pr_info("kshim vm_mmap MAP_FIXED failed addr=0x%lx len=0x%lx errno=%d\n", addr, len, errno);
            return (unsigned long)-ENOMEM;
        }
    } else {
        int anon = MAP_PRIVATE | MAP_ANONYMOUS;
        p = mmap(NULL, len, effective_prot, anon, -1, 0);
        if (p == MAP_FAILED)
            return (unsigned long)-ENOMEM;
    }

    if (need_write) {
        if (lseek(f->fd, (off_t)offset, SEEK_SET) < 0) {
            pr_info("kshim vm_mmap lseek failed offset=0x%lx errno=%d\n", offset, errno);
            munmap(p, len);
            return (unsigned long)-ENOMEM;
        }
        ssize_t n = (ssize_t)syscall(SYS_read, f->fd, p, len);
        if (n < 0) {
            pr_info("kshim vm_mmap read failed addr=0x%lx len=0x%lx offset=0x%lx errno=%d\n", (unsigned long)p, len, offset, errno);
            munmap(p, len);
            return (unsigned long)-ENOMEM;
        }
        if ((size_t)n < len) {
            char *cp = (char *)p + n;
            size_t rem = len - (size_t)n;
            for (size_t i = 0; i < rem; ++i) cp[i] = 0;
        }
        if (effective_prot != mmap_prot) {
            if (mprotect(p, len, mmap_prot) != 0) {
                pr_info("kshim vm_mmap mprotect failed addr=0x%lx len=0x%lx prot=0x%x errno=%d\n", (unsigned long)p, len, mmap_prot, errno);
                munmap(p, len);
                return (unsigned long)-ENOMEM;
            }
        }
    }

    kshim_mapping_alloc(p, len, mmap_prot, flags, offset, f);
    return (unsigned long)p;
}

static inline int vm_munmap(unsigned long addr, size_t len)
{
    struct kshim_mapping **pp = &kshim_mappings;
    while (*pp) {
        struct kshim_mapping *m = *pp;
        if ((unsigned long)m->addr == addr && m->len == len) {
            *pp = m->next;
            munmap(m->addr, m->len);
            free(m);
            return 0;
        }
        pp = &m->next;
    }
    munmap((void *)addr, len);
    return 0;
}

static inline unsigned long get_unmapped_area(struct file *f,
                                              unsigned long addr_hint,
                                              unsigned long len,
                                              unsigned long pgoff,
                                              unsigned long flags)
{
    (void)f;
    (void)len;
    (void)pgoff;
    (void)flags;
    if (addr_hint == 0)
        addr_hint = 0x140000000ULL;
    if (kshim_preferred_base_ok) {
        return addr_hint;
    }
    if (kshim_forced_random_base)
        return (unsigned long)kshim_forced_random_base;
    return addr_hint + 0x10000ULL;
}

/* -------------------------------------------------------------------------- */
/* Personality / GS stubs                                                     */
/* -------------------------------------------------------------------------- */
static inline int set_user_gsbase(struct task_struct *task, unsigned long base)
{
    task->gsbase = (void *)base;
    return 0;
}

static inline void nt_enter_personality(struct task_struct *task)
{
    task->nt_abi = 1;
}

/* Misc */
#ifndef _Static_assert
#define _Static_assert(x, y) static_assert(x, y)
#endif

/* Provide memset/memcpy for freestanding kernel code in test mode */
#include <string.h>

#endif /* KSHIM_H */
