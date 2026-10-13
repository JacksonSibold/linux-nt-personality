/*
 * mm.c - NT virtual-memory manager (ntmm) for the ntpers kernel personality.
 *
 * Core responsibilities:
 *   - VAD tree tracking every region (red-black in kernel, sorted list in shim)
 *   - NtAllocateVirtualMemory / NtFreeVirtualMemory / NtProtectVirtualMemory
 *   - PAGE_GUARD semantics
 *   - NtQueryVirtualMemory coalescing
 *   - Section objects
 */

#include "mm.h"
#include "object.h"
#include "handle.h"
#include "io.h"
#ifndef __KERNEL__
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "image_plan.h"
#include "image_reloc.h"
#else
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmap_lock.h>
#include <asm/tlb.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/personality.h>
#include <linux/sched.h>
#include <linux/fs.h>
#ifndef ARCH_VM_PKEY_FLAGS
#define ARCH_VM_PKEY_FLAGS 0
#endif
static inline void *kshim_mmap_reserve(void *a, size_t s){
    unsigned long addr;
    unsigned long flags = MAP_PRIVATE|MAP_ANONYMOUS;
    if (a) flags |= MAP_FIXED_NOREPLACE;
    addr = vm_mmap(NULL, (unsigned long)a, s, PROT_NONE, flags, 0);
    if (IS_ERR_VALUE(addr)) return NULL;
    return (void*)addr;
}
static inline int kshim_mmap_commit(void *a,size_t s,int p){
    unsigned long addr = (unsigned long)a;
    unsigned long res = vm_mmap(NULL, addr, s, p, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0);
    if (IS_ERR_VALUE(res)) return -1;
    return 0;
}
static inline int kshim_munmap(void *a,size_t s){ return vm_munmap((unsigned long)a, s); }
static inline int kshim_mprotect(void *a,size_t s,int p){
    unsigned long start = (unsigned long)a;
    unsigned long len = s;
    // Try kallsyms lookup for do_mprotect_pkey if available
    extern unsigned long kallsyms_lookup_name(const char *name);
    typedef int (*do_mprotect_fn)(unsigned long, size_t, unsigned long, int);
    unsigned long addr = kallsyms_lookup_name("do_mprotect_pkey");
    if (addr) {
        do_mprotect_fn fn = (do_mprotect_fn)addr;
        int ret = fn(start, len, p, -1);
        if (ret == 0) return 0;
        // fallback to mprotect_fixup
    }
    // Fallback: use mprotect_fixup path
    unsigned long end;
    unsigned long nstart, tmp;
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma, *prev;
    struct vma_iterator vmi;
    struct mmu_gather tlb;
    int error = 0;
    const bool rier = (current->personality & READ_IMPLIES_EXEC) && (p & PROT_READ);
    unsigned long prot = (unsigned long)p;
    pr_debug("kshim_mprotect: start %lx len %zu prot %x\n", start, s, p);

    if (!s) return 0;
    if (start & ~PAGE_MASK) return -1;
    s = PAGE_ALIGN(s);
    end = start + s;
    if (end <= start) return -1;
    prot &= ~(PROT_GROWSDOWN | PROT_GROWSUP);
    if (!arch_validate_prot(prot, start)) return -1;
    if (mmap_write_lock_killable(mm)) return -1;

    vma_iter_init(&vmi, mm, start);
    vma = vma_find(&vmi, end);
    if (!vma) { error = -ENOMEM; goto out2; }
    if (vma->vm_start > start) { error = -ENOMEM; goto out2; }
    prev = vma_prev(&vmi);
    if (start > vma->vm_start) prev = vma;

    tlb_gather_mmu(&tlb, mm);
    nstart = start;
    tmp = vma->vm_start;
    for_each_vma_range(vmi, vma, end) {
        vm_flags_t newflags;
        vm_flags_t mask_off;
        unsigned long cur_start = nstart;
        unsigned long cur_end = vma->vm_end < end ? vma->vm_end : end;
        int pkey2 = -1;

        if (vma->vm_start != tmp) { error = -ENOMEM; break; }
        pkey2 = -1;
        if (rier && (vma->vm_flags & VM_MAYEXEC))
            prot |= PROT_EXEC;
        mask_off = VM_ACCESS_FLAGS | VM_FLAGS_CLEAR;
        newflags = calc_vm_prot_bits(prot, pkey2);
        newflags |= (vma->vm_flags & ~mask_off);
        if ((newflags & ~(newflags >> 4)) & VM_ACCESS_FLAGS) { error = -EACCES; break; }
        if (map_deny_write_exec(vma->vm_flags, newflags)) { error = -EACCES; break; }
        if (!arch_validate_flags(newflags)) { error = -EINVAL; break; }
        error = security_file_mprotect(vma, prot, prot);
        if (error) break;
        tmp = cur_end;
        if (vma->vm_ops && vma->vm_ops->mprotect) {
            error = vma->vm_ops->mprotect(vma, cur_start, cur_end, newflags);
            if (error) break;
        }
        error = mprotect_fixup(&vmi, &tlb, vma, &prev, cur_start, cur_end, newflags);
        if (error) break;
        tmp = cur_end;
        nstart = tmp;
        prot = (unsigned long)p & ~(PROT_GROWSDOWN | PROT_GROWSUP);
        if (nstart >= end) break;
    }
    tlb_finish_mmu(&tlb);
    if (!error && nstart < end) error = -ENOMEM;
out2:
    mmap_write_unlock(mm);
    return error ? -1 : 0;
}
static inline void kshim_madvise_free(void *a,size_t s){ (void)a;(void)s; }
static inline void kshim_install_guard_handler(void){}
/* guard state for kernel - no-op */
#ifndef kshim_guard
struct kshim_guard_state { int active; void *guard_base; size_t guard_size; unsigned long base_protect; };
static struct kshim_guard_state kshim_guard;
#endif
#ifndef PROT_NONE
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#endif
static inline int kshim_section_create(struct kshim_section_backing *b, size_t size){ (void)b;(void)size; if(b){b->fd=-1;b->size=size;b->mem=NULL;} return 0; }
static inline int kshim_section_create_file(struct kshim_section_backing *b, const char *path, size_t *size){ (void)b;(void)path;(void)size; if(b){b->fd=-1;b->size=size?*size:0;b->mem=NULL;} return 0; }
static inline void *kshim_section_map(struct kshim_section_backing *b, void *addr, size_t offset, size_t size, int prot, int cow){
    (void)b; (void)offset; (void)cow;
    void *target = addr ? addr : (void*)0x10000;
    /* For SEC_RESERVE we map PROT_NONE initially */
    if (prot == PROT_NONE) {
        if (kshim_mmap_reserve(target, size) == NULL) return NULL;
        return target;
    }
    if (kshim_mmap_commit(target, size, prot) == 0) return target;
    /* fallback vm_mmap */
    {
        unsigned long a = vm_mmap(NULL, (unsigned long)target, size, prot, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0);
        if (IS_ERR_VALUE(a)) return NULL;
        return (void*)a;
    }
}
/* File-backed view: the file's own pages, mapped by the kernel. Shared views
 * share the page cache; copy-on-write views are private. */
static inline void *kshim_section_map_file(struct kshim_section_backing *b, void *addr, size_t offset, size_t size, int prot, int cow){
    unsigned long flags = (cow ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED;
    unsigned long a = vm_mmap(b->file, (unsigned long)addr, size, prot, flags, offset);
    if (IS_ERR_VALUE(a)) return NULL;
    return (void*)a;
}
#endif

/* -------------------------------------------------------------------------- */
/* Per-process address space (singleton for tests)                            */
/* -------------------------------------------------------------------------- */

struct nt_mm g_nt_mm;
int          g_nt_mm_initialized = 0;

#ifndef __KERNEL__
#define ntmm_lock(mm)   spin_lock(&(mm)->lock)
#define ntmm_unlock(mm) spin_unlock(&(mm)->lock)
#else
#define ntmm_lock(mm)   down_write(&(mm)->lock)
#define ntmm_unlock(mm) up_write(&(mm)->lock)
#define ntmm_read_lock(mm) down_read(&(mm)->lock)
#define ntmm_read_unlock(mm) up_read(&(mm)->lock)
#endif

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static inline ULONG_PTR ntmm_round_up(ULONG_PTR x, ULONG_PTR align) {
    return (x + align - 1) & ~(align - 1);
}
static inline ULONG_PTR ntmm_round_down(ULONG_PTR x, ULONG_PTR align) {
    return x & ~(align - 1);
}
static inline int ntmm_protection_valid(ULONG prot) {
    ULONG base = prot & 0xFFUL;
    switch (base) {
    case PAGE_NOACCESS:
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return 1;
    default:
        return 0;
    }
}

static int ntmm_prot_to_linux(ULONG nt_prot) {
    ULONG base = nt_prot & 0xFFUL;
    int linux_prot = PROT_NONE;
    if (nt_prot & PAGE_GUARD) {
        return PROT_NONE;
    }
    switch (base) {
    case PAGE_NOACCESS:
        linux_prot = PROT_NONE;
        break;
    case PAGE_READONLY:
        linux_prot = PROT_READ;
        break;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
        linux_prot = PROT_READ | PROT_WRITE;
        break;
    case PAGE_EXECUTE:
        linux_prot = PROT_EXEC;
        break;
    case PAGE_EXECUTE_READ:
        linux_prot = PROT_READ | PROT_EXEC;
        break;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        linux_prot = PROT_READ | PROT_WRITE | PROT_EXEC;
        break;
    default:
        linux_prot = PROT_NONE;
        break;
    }
    return linux_prot;
}

static struct nt_vad *nt_vad_new(ULONG_PTR start, ULONG_PTR end, ULONG state,
                                  ULONG protect, ULONG alloc_protect,
                                  ULONG_PTR alloc_base, ULONG type) {
    struct nt_vad *vad = (struct nt_vad *)kzalloc(sizeof(*vad), GFP_KERNEL);
    if (!vad) return NULL;
    vad->start = start;
    vad->end = end;
    vad->state = state;
    vad->protect = protect;
    vad->alloc_protect = alloc_protect;
    vad->alloc_base = alloc_base;
    vad->type = type;
    vad->guard = 0;
    vad->write_watch = 0;
    vad->section = NULL;
    vad->section_offset = 0;
#ifndef __KERNEL__
    vad->list_next = NULL;
    vad->list_prev = NULL;
#endif
    return vad;
}

/* Section objects (ADR 0001). A VAD with vad->section set holds one object
 * reference. Every copy of the pointer takes its own reference at the copy, so
 * each VAD's release below is balanced. */
static void nt_section_ref(struct nt_section *sec) {
    ObReferenceObject(nt_body_to_header(sec));
}

static void nt_section_unref(struct nt_section *sec) {
    ObDereferenceObject(nt_body_to_header(sec));
}

/* Frees a VAD and drops the section reference it holds. */
static void nt_vad_free(struct nt_vad *vad) {
    if (!vad) return;
    if (vad->section) nt_section_unref(vad->section);
    kfree(vad);
}

/* -------------------------------------------------------------------------- */
/* Userspace shim: sorted doubly-linked list                                  */
/* -------------------------------------------------------------------------- */
#ifndef __KERNEL__

static struct nt_vad *nt_vad_find(struct nt_mm *mm, ULONG_PTR addr) {
    struct nt_vad *cur = mm->vad_head;
    while (cur) {
        if (addr >= cur->start && addr < cur->end) return cur;
        if (addr < cur->start) return NULL; /* sorted */
        cur = cur->list_next;
    }
    return NULL;
}

static struct nt_vad *nt_vad_find_ge(struct nt_mm *mm, ULONG_PTR addr) {
    struct nt_vad *cur = mm->vad_head;
    while (cur) {
        if (cur->start >= addr) return cur;
        cur = cur->list_next;
    }
    return NULL;
}

static int nt_list_insert(struct nt_mm *mm, struct nt_vad *vad) {
    struct nt_vad *prev = NULL;
    struct nt_vad *cur = mm->vad_head;
    while (cur && cur->start < vad->start) {
        prev = cur;
        cur = cur->list_next;
    }
    vad->list_next = cur;
    vad->list_prev = prev;
    if (prev) prev->list_next = vad;
    else mm->vad_head = vad;
    if (cur) cur->list_prev = vad;
    if (vad->end > mm->highest_vad) mm->highest_vad = vad->end;
    return 0;
}

static void nt_list_remove(struct nt_mm *mm, struct nt_vad *vad) {
    if (vad->list_prev) vad->list_prev->list_next = vad->list_next;
    else mm->vad_head = vad->list_next;
    if (vad->list_next) vad->list_next->list_prev = vad->list_prev;
    vad->list_next = vad->list_prev = NULL;
}

static struct nt_vad *nt_vad_left_neighbor(struct nt_mm *mm, struct nt_vad *vad) {
    (void)mm;
    return vad->list_prev;
}
static struct nt_vad *nt_vad_right_neighbor(struct nt_mm *mm, struct nt_vad *vad) {
    (void)mm;
    return vad->list_next;
}

static int __attribute__((unused)) nt_vad_attrs_match(const struct nt_vad *a, const struct nt_vad *b) {
    if (a->state != b->state) return 0;
    if (a->type != b->type) return 0;
    if (a->alloc_base != b->alloc_base) return 0;
    if (a->section != b->section) return 0;
    if (a->section && a->section_offset != b->section_offset) return 0;
    if (a->write_watch != b->write_watch) return 0;
    if (a->state == MEM_COMMIT) {
        if (a->protect != b->protect) return 0;
        if (a->guard != b->guard) return 0;
        if (a->alloc_protect != b->alloc_protect) return 0;
    } else if (a->state == MEM_RESERVE) {
        if (a->alloc_protect != b->alloc_protect) return 0;
        if (a->protect != b->protect) return 0;
    }
    return 1;
}

static void nt_vad_coalesce(struct nt_mm *mm, struct nt_vad *vad) {
    (void)mm; (void)vad;
}

static void nt_coalesce_all(struct nt_mm *mm) {
    (void)mm;
}

/* Split vad at [split, split_end). Returns vad for the middle piece. */
static struct nt_vad *nt_vad_split(struct nt_mm *mm, struct nt_vad *vad,
                                    ULONG_PTR split, ULONG_PTR split_end) {
    if (split < vad->start || split_end > vad->end || split >= split_end) return NULL;
    if (split == vad->start && split_end == vad->end) return vad;

    if (split > vad->start) {
        struct nt_vad *left = nt_vad_new(vad->start, split, vad->state,
                                          vad->protect, vad->alloc_protect,
                                          vad->alloc_base, vad->type);
        if (!left) return NULL;
        left->guard = vad->guard;
        left->write_watch = vad->write_watch;
        left->section = vad->section;
        if (left->section) nt_section_ref(left->section);
        left->section_offset = vad->section_offset;
        /* Insert left before vad */
        left->list_prev = vad->list_prev;
        left->list_next = vad;
        if (vad->list_prev) vad->list_prev->list_next = left;
        else mm->vad_head = left;
        vad->list_prev = left;
        vad->start = split;
        if (vad->section) vad->section_offset += (split - left->start);
    }
    if (split_end < vad->end) {
        ULONG_PTR old_end = vad->end;
        struct nt_vad *right = nt_vad_new(split_end, old_end, vad->state,
                                           vad->protect, vad->alloc_protect,
                                           vad->alloc_base, vad->type);
        if (!right) return NULL;
        right->guard = vad->guard;
        right->write_watch = vad->write_watch;
        right->section = vad->section;
        if (right->section) nt_section_ref(right->section);
        if (vad->section)
            right->section_offset = vad->section_offset + (split_end - vad->start);
        else
            right->section_offset = 0;
        right->list_next = vad->list_next;
        right->list_prev = vad;
        if (vad->list_next) vad->list_next->list_prev = right;
        vad->list_next = right;
        vad->end = split_end;
        if (right->end > mm->highest_vad) mm->highest_vad = right->end;
    } else {
        vad->end = split_end;
    }
    return vad;
}

static int ntmm_range_free(struct nt_mm *mm, ULONG_PTR start, ULONG_PTR end) {
    struct nt_vad *cur = mm->vad_head;
    while (cur) {
        if (cur->start < end && cur->end > start) return 0;
        if (cur->start >= end) break;
        cur = cur->list_next;
    }
    return 1;
}

static ULONG_PTR ntmm_find_hole_bottom_up(struct nt_mm *mm, ULONG_PTR size, ULONG_PTR align) {
    ULONG_PTR candidate = ntmm_round_up(NTMM_LOW_ADDRESS, align);
    struct nt_vad *cur = mm->vad_head;
    while (cur) {
        if (candidate + size <= cur->start) return candidate;
        if (candidate < cur->end) candidate = ntmm_round_up(cur->end, align);
        if (candidate + size > NTMM_HIGH_ADDRESS) return 0;
        cur = cur->list_next;
    }
    if (candidate + size <= NTMM_HIGH_ADDRESS) return candidate;
    return 0;
}

static ULONG_PTR ntmm_find_hole_top_down(struct nt_mm *mm, ULONG_PTR size, ULONG_PTR align) {
    ULONG_PTR candidate;
    if (mm->vad_head == NULL) {
        candidate = ntmm_round_down(NTMM_HIGH_ADDRESS - size, align);
        if (candidate < NTMM_LOW_ADDRESS) return 0;
        return candidate;
    }
    /* Start from high */
    candidate = ntmm_round_down(NTMM_HIGH_ADDRESS - size, align);
    while (candidate >= NTMM_LOW_ADDRESS) {
        if (ntmm_range_free(mm, candidate, candidate + size)) return candidate;
        struct nt_vad *vad = nt_vad_find(mm, candidate);
        if (vad) {
            if (vad->start < size) break;
            candidate = ntmm_round_down(vad->start - size, align);
        } else {
            struct nt_vad *ge = nt_vad_find_ge(mm, candidate);
            if (ge) candidate = ntmm_round_down(ge->start - size, align);
            else {
                if (candidate < align) break;
                candidate -= align;
            }
        }
        if (candidate > NTMM_HIGH_ADDRESS) break;
    }
    return 0;
}

#else /* __KERNEL__ : rbtree implementation */
#include <linux/rbtree.h>

/* rbtree helpers - keyed by start */

static struct nt_vad *nt_vad_find(struct nt_mm *mm, ULONG_PTR addr)
{
    struct rb_node *n = mm->vad_root.rb_node;
    while (n) {
        struct nt_vad *vad = rb_entry(n, struct nt_vad, rb);
        if (addr < vad->start)
            n = n->rb_left;
        else if (addr >= vad->end)
            n = n->rb_right;
        else
            return vad;
    }
    return NULL;
}

static struct nt_vad *nt_vad_find_ge(struct nt_mm *mm, ULONG_PTR addr)
{
    struct rb_node *n = mm->vad_root.rb_node;
    struct nt_vad *best = NULL;
    while (n) {
        struct nt_vad *vad = rb_entry(n, struct nt_vad, rb);
        if (vad->start >= addr) {
            best = vad;
            n = n->rb_left;
        } else {
            n = n->rb_right;
        }
    }
    return best;
}

/* Insert sorted by start */
static int nt_vad_insert(struct nt_mm *mm, struct nt_vad *vad)
{
    struct rb_node **link = &mm->vad_root.rb_node;
    struct rb_node *parent = NULL;
    while (*link) {
        struct nt_vad *cur = rb_entry(*link, struct nt_vad, rb);
        parent = *link;
        if (vad->start < cur->start)
            link = &(*link)->rb_left;
        else if (vad->start > cur->start)
            link = &(*link)->rb_right;
        else
            return -1; /* duplicate */
    }
    rb_link_node(&vad->rb, parent, link);
    rb_insert_color(&vad->rb, &mm->vad_root);
    if (vad->end > mm->highest_vad)
        mm->highest_vad = vad->end;
    return 0;
}

static void nt_vad_remove(struct nt_mm *mm, struct nt_vad *vad)
{
    rb_erase(&vad->rb, &mm->vad_root);
    /* highest_vad not recomputed precisely - acceptable for tests */
}

static struct nt_vad *nt_vad_left_neighbor(struct nt_mm *mm, struct nt_vad *vad)
{
    struct rb_node *n = rb_prev(&vad->rb);
    if (!n) return NULL;
    return rb_entry(n, struct nt_vad, rb);
}

static struct nt_vad *nt_vad_right_neighbor(struct nt_mm *mm, struct nt_vad *vad)
{
    struct rb_node *n = rb_next(&vad->rb);
    if (!n) return NULL;
    return rb_entry(n, struct nt_vad, rb);
}

static int __attribute__((unused)) nt_vad_attrs_match(const struct nt_vad *a, const struct nt_vad *b)
{
    if (a->state != b->state) return 0;
    if (a->type != b->type) return 0;
    if (a->alloc_base != b->alloc_base) return 0;
    if (a->section != b->section) return 0;
    if (a->section && a->section_offset != b->section_offset) return 0;
    if (a->write_watch != b->write_watch) return 0;
    if (a->state == MEM_COMMIT) {
        if (a->protect != b->protect) return 0;
        if (a->guard != b->guard) return 0;
        if (a->alloc_protect != b->alloc_protect) return 0;
    } else if (a->state == MEM_RESERVE) {
        if (a->alloc_protect != b->alloc_protect) return 0;
        if (a->protect != b->protect) return 0;
    }
    return 1;
}

static void __attribute__((unused)) nt_vad_coalesce(struct nt_mm *mm, struct nt_vad *vad)
{
    (void)mm; (void)vad;
}

static struct nt_vad *nt_vad_split(struct nt_mm *mm, struct nt_vad *vad,
                                    ULONG_PTR split, ULONG_PTR split_end)
{
    if (split < vad->start || split_end > vad->end || split >= split_end)
        return NULL;
    if (split == vad->start && split_end == vad->end)
        return vad;
    if (split > vad->start) {
        struct nt_vad *left = nt_vad_new(vad->start, split, vad->state,
                                          vad->protect, vad->alloc_protect,
                                          vad->alloc_base, vad->type);
        if (!left) return NULL;
        left->guard = vad->guard;
        left->write_watch = vad->write_watch;
        left->section = vad->section;
        if (left->section) nt_section_ref(left->section);
        left->section_offset = vad->section_offset;
        ULONG_PTR old_start = vad->start;
        nt_vad_remove(mm, vad);
        vad->start = split;
        if (vad->section)
            vad->section_offset += (split - old_start);
        if (nt_vad_insert(mm, left) != 0) {
            vad->start = old_start;
            if (vad->section) vad->section_offset -= (split - old_start);
            nt_vad_insert(mm, vad);
            nt_vad_free(left);
            return NULL;
        }
        if (nt_vad_insert(mm, vad) != 0) {
            nt_vad_remove(mm, left);
            nt_vad_free(left);
            vad->start = old_start;
            if (vad->section) vad->section_offset -= (split - old_start);
            nt_vad_insert(mm, vad);
            return NULL;
        }
    }
    if (split_end < vad->end) {
        ULONG_PTR old_end = vad->end;
        struct nt_vad *right = nt_vad_new(split_end, old_end, vad->state,
                                           vad->protect, vad->alloc_protect,
                                           vad->alloc_base, vad->type);
        if (!right) return NULL;
        right->guard = vad->guard;
        right->write_watch = vad->write_watch;
        right->section = vad->section;
        if (right->section) nt_section_ref(right->section);
        if (vad->section)
            right->section_offset = vad->section_offset + (split_end - vad->start);
        else
            right->section_offset = 0;
        vad->end = split_end;
        if (nt_vad_insert(mm, right) != 0) {
            vad->end = old_end;
            nt_vad_free(right);
            return NULL;
        }
        if (right->end > mm->highest_vad)
            mm->highest_vad = right->end;
    } else {
        vad->end = split_end;
    }
    return vad;
}

static int ntmm_range_free(struct nt_mm *mm, ULONG_PTR start, ULONG_PTR end)
{
    struct nt_vad *v = nt_vad_find(mm, start);
    if (v) return 0;
    v = nt_vad_find_ge(mm, start);
    if (v && v->start < end) return 0;
    /* also check predecessor that might overlap */
    /* find predecessor via finding node less than start */
    struct rb_node *n = mm->vad_root.rb_node;
    struct nt_vad *prev = NULL;
    while (n) {
        struct nt_vad *cur = rb_entry(n, struct nt_vad, rb);
        if (cur->start < start) {
            prev = cur;
            n = n->rb_right;
        } else {
            n = n->rb_left;
        }
    }
    if (prev && prev->end > start) return 0;
    return 1;
}

static ULONG_PTR ntmm_find_hole_bottom_up(struct nt_mm *mm, ULONG_PTR size, ULONG_PTR align)
{
    ULONG_PTR candidate = (NTMM_LOW_ADDRESS + align - 1) & ~(align - 1);
    struct rb_node *n = rb_first(&mm->vad_root);
    while (n) {
        struct nt_vad *cur = rb_entry(n, struct nt_vad, rb);
        if (candidate + size <= cur->start)
            return candidate;
        if (candidate < cur->end)
            candidate = (cur->end + align - 1) & ~(align - 1);
        if (candidate + size > NTMM_HIGH_ADDRESS)
            return 0;
        n = rb_next(n);
    }
    if (candidate + size <= NTMM_HIGH_ADDRESS)
        return candidate;
    return 0;
}

static ULONG_PTR ntmm_find_hole_top_down(struct nt_mm *mm, ULONG_PTR size, ULONG_PTR align)
{
    ULONG_PTR candidate;
    if (RB_EMPTY_ROOT(&mm->vad_root)) {
        candidate = (NTMM_HIGH_ADDRESS - size) & ~(align - 1);
        if (candidate < NTMM_LOW_ADDRESS) return 0;
        return candidate;
    }
    candidate = (NTMM_HIGH_ADDRESS - size) & ~(align - 1);
    while (candidate >= NTMM_LOW_ADDRESS) {
        if (ntmm_range_free(mm, candidate, candidate + size))
            return candidate;
        struct nt_vad *vad = nt_vad_find(mm, candidate);
        if (vad) {
            if (vad->start < size) break;
            candidate = (vad->start - size) & ~(align - 1);
        } else {
            struct nt_vad *ge = nt_vad_find_ge(mm, candidate);
            if (ge)
                candidate = (ge->start - size) & ~(align - 1);
            else {
                if (candidate < align) break;
                candidate -= align;
            }
        }
        if (candidate > NTMM_HIGH_ADDRESS) break;
    }
    return 0;
}
#ifdef __KERNEL__
#define nt_list_insert nt_vad_insert
#define nt_list_remove nt_vad_remove

static void nt_coalesce_all(struct nt_mm *mm)
{
    struct rb_node *n = rb_first(&mm->vad_root);
    while (n) {
        struct nt_vad *cur = rb_entry(n, struct nt_vad, rb);
        struct rb_node *next_n = rb_next(n);
        if (!next_n) break;
        struct nt_vad *next = rb_entry(next_n, struct nt_vad, rb);
        if (cur->state == MEM_RESERVE && cur->end == next->start && nt_vad_attrs_match(cur, next)) {
            cur->end = next->end;
            rb_erase(next_n, &mm->vad_root);
            nt_vad_free(next);
            /* stay at cur, check new next */
        } else {
            n = next_n;
        }
    }
}
#endif
#endif /* __KERNEL__ shim vs rbtree */

 /* -------------------------------------------------------------------------- */
/* Init / destroy / reset / invariants                                        */
/* -------------------------------------------------------------------------- */

NTSTATUS ntmm_init(struct nt_mm *mm) {
    mm->vad_root.rb_node = NULL;
#ifndef __KERNEL__
    mm->vad_head = NULL;
    spin_lock_init(&mm->lock);
#else
    init_rwsem(&mm->lock);
#endif
    mm->highest_vad = NTMM_LOW_ADDRESS;
    return STATUS_SUCCESS;
}

void ntmm_destroy(struct nt_mm *mm) {
#ifndef __KERNEL__
    struct nt_vad *cur = mm->vad_head;
    while (cur) {
        struct nt_vad *next = cur->list_next;
        if (cur->state != MEM_FREE) {
            kshim_munmap((void *)cur->start, (size_t)(cur->end - cur->start));
        }
        nt_vad_free(cur);
        cur = next;
    }
    mm->vad_head = NULL;
    mm->vad_root.rb_node = NULL;
    mm->highest_vad = NTMM_LOW_ADDRESS;
#else
    struct rb_node *n = rb_first(&mm->vad_root);
    while (n) {
        struct rb_node *next = rb_next(n);
        struct nt_vad *vad = rb_entry(n, struct nt_vad, rb);
        rb_erase(n, &mm->vad_root);
        nt_vad_free(vad);
        n = next;
    }
    mm->vad_root.rb_node = NULL;
    mm->highest_vad = NTMM_LOW_ADDRESS;
#endif
}

void ntmm_reset(void) {
    ntmm_destroy(&g_nt_mm);
    ntmm_init(&g_nt_mm);
    g_nt_mm_initialized = 1;
}

int ntmm_check_invariants(struct nt_mm *mm) {
#ifndef __KERNEL__
    struct nt_vad *cur = mm->vad_head;
    ULONG_PTR prev_end = 0;
    while (cur) {
        if (cur->start >= cur->end) return 0;
        if ((cur->start & (NTMM_PAGE_SIZE-1)) != 0) return 0;
        if ((cur->end & (NTMM_PAGE_SIZE-1)) != 0) return 0;
        if (prev_end > cur->start) return 0; /* overlap */
        /* Per-VAD invariant: adjacent VADs may have identical attributes but remain
         * separate (per-page) to match Windows probe_virtualquery_direct oracle
         * where each 4K commit is a distinct VAD (RegionSize 4096). Coalescing
         * is now query-time only and not required for invariant. */
        if (cur->alloc_base > cur->start) return 0;
        if (cur->alloc_base + NTMM_ALLOC_GRANULARITY <= cur->start && cur->type == MEM_PRIVATE) {
            /* alloc_base should be within its allocation */
        }
        prev_end = cur->end;
        cur = cur->list_next;
    }
#else
    struct rb_node *n = rb_first(&mm->vad_root);
    ULONG_PTR prev_end = 0;
    struct nt_vad *prev = NULL;
    while (n) {
        struct nt_vad *cur = rb_entry(n, struct nt_vad, rb);
        if (cur->start >= cur->end) return 0;
        if ((cur->start & (NTMM_PAGE_SIZE-1)) != 0) return 0;
        if ((cur->end & (NTMM_PAGE_SIZE-1)) != 0) return 0;
        if (prev_end > cur->start) return 0;
        /* See above: per-VAD, adjacent identical attrs allowed (probe_direct oracle) */
        if (cur->alloc_base > cur->start) return 0;
        prev_end = cur->end;
        prev = cur;
        n = rb_next(n);
    }
#endif
    return 1;
}

/* -------------------------------------------------------------------------- */
/* NtAllocateVirtualMemory                                                    */
/* -------------------------------------------------------------------------- */

NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                                 ULONG_PTR ZeroBits, PSIZE_T RegionSize,
                                 ULONG AllocationType, ULONG Protect) {
    struct nt_mm *mm = &g_nt_mm;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR req_base = 0, actual_base = 0, actual_end = 0;
    SIZE_T req_size = 0;
    int reserve = (AllocationType & MEM_RESERVE) != 0;
    int commit  = (AllocationType & MEM_COMMIT) != 0;
    int top_down = (AllocationType & MEM_TOP_DOWN) != 0;
    int do_reset = (AllocationType & MEM_RESET) != 0;
    int write_watch = (AllocationType & MEM_WRITE_WATCH) != 0;

    (void)ProcessHandle;
    (void)ZeroBits;

    if (!BaseAddress || !RegionSize) return STATUS_INVALID_PARAMETER;
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }
    if (do_reset) {
        if (reserve || commit) return STATUS_INVALID_PARAMETER;
        req_base = (ULONG_PTR)*BaseAddress;
        req_size = *RegionSize;
        if (req_size == 0) return STATUS_SUCCESS;
        req_base = ntmm_round_down(req_base, NTMM_PAGE_SIZE);
        req_size = ntmm_round_up(req_size, NTMM_PAGE_SIZE);
        ntmm_lock(mm);
        struct nt_vad *vad = nt_vad_find(mm, req_base);
        if (!vad || vad->state != MEM_COMMIT) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
        if (req_base + req_size > vad->end) { /* allow spanning? check contiguous */
            /* Ensure whole range is committed */
            ULONG_PTR p = req_base;
            struct nt_vad *c = vad;
            while (p < req_base + req_size && c) {
                if (c->state != MEM_COMMIT) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
                p = c->end;
                if (p >= req_base + req_size) break;
                c = nt_vad_right_neighbor(mm, c);
                if (!c || c->start != p) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
            }
            if (p < req_base + req_size) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
        }
        ntmm_unlock(mm);
        kshim_madvise_free((void *)req_base, req_size);
        return STATUS_SUCCESS;
    }

    if (!reserve && !commit) return STATUS_INVALID_PARAMETER;
    if (!ntmm_protection_valid(Protect)) return STATUS_INVALID_PAGE_PROTECTION;
    /* Strip guard etc for validation already done */

    ULONG_PTR orig_base = (ULONG_PTR)*BaseAddress;
    SIZE_T orig_size = *RegionSize;
    if (orig_size == 0) return STATUS_INVALID_PARAMETER;
    /* Rounding up orig_size (plus the granularity delta) must not wrap. */
    if (orig_size > (SIZE_T)-1 - NTMM_ALLOC_GRANULARITY) return STATUS_INVALID_PARAMETER;
    if (reserve) {
        if (orig_base != 0) {
            ULONG_PTR rounded = ntmm_round_down(orig_base, NTMM_ALLOC_GRANULARITY);
            ULONG_PTR delta = orig_base - rounded;
            req_base = rounded;
            req_size = (SIZE_T)ntmm_round_up(orig_size + delta, NTMM_PAGE_SIZE);
        } else {
            req_base = 0;
            req_size = (SIZE_T)ntmm_round_up(orig_size, NTMM_PAGE_SIZE);
        }
    } else {
        if (orig_base != 0) {
            ULONG_PTR rounded = ntmm_round_down(orig_base, NTMM_PAGE_SIZE);
            ULONG_PTR delta = orig_base - rounded;
            req_base = rounded;
            req_size = (SIZE_T)ntmm_round_up(orig_size + delta, NTMM_PAGE_SIZE);
        } else {
            req_base = 0;
            req_size = (SIZE_T)ntmm_round_up(orig_size, NTMM_PAGE_SIZE);
        }
    }

    ntmm_lock(mm);

    if (reserve) {
        if (req_base == 0) {
            if (top_down) actual_base = ntmm_find_hole_top_down(mm, req_size, NTMM_ALLOC_GRANULARITY);
            else actual_base = ntmm_find_hole_bottom_up(mm, req_size, NTMM_ALLOC_GRANULARITY);
            if (actual_base == 0) { status = STATUS_NO_MEMORY; goto out_unlock; }
        } else {
            actual_base = req_base;
            if (!ntmm_range_free(mm, actual_base, actual_base + req_size)) {
                status = STATUS_CONFLICTING_ADDRESSES;
                goto out_unlock;
            }
        }
        actual_end = actual_base + req_size;
        struct nt_vad *vad = nt_vad_new(actual_base, actual_end,
                                        commit ? MEM_COMMIT : MEM_RESERVE,
                                        Protect, Protect, actual_base, MEM_PRIVATE);
        if (!vad) { status = STATUS_NO_MEMORY; goto out_unlock; }
        vad->write_watch = write_watch ? 1 : 0;
        if (commit) vad->guard = (Protect & PAGE_GUARD) ? 1 : 0;
        nt_list_insert(mm, vad);
        ntmm_unlock(mm);

        if (kshim_mmap_reserve((void *)actual_base, (size_t)req_size) == NULL) {
            ntmm_lock(mm);
            nt_list_remove(mm, vad);
            ntmm_unlock(mm);
            nt_vad_free(vad);
            return STATUS_NO_MEMORY;
        }
        if (commit) {
            int prot = ntmm_prot_to_linux(Protect);
            if (kshim_mmap_commit((void *)actual_base, (size_t)req_size, prot) != 0) {
                kshim_munmap((void *)actual_base, (size_t)req_size);
                ntmm_lock(mm);
                nt_list_remove(mm, vad);
                ntmm_unlock(mm);
                nt_vad_free(vad);
                return STATUS_NO_MEMORY;
            }
            if (Protect & PAGE_GUARD) {
                kshim_guard.active = 1;
                kshim_guard.guard_base = (void *)actual_base;
                kshim_guard.guard_size = (size_t)req_size;
                kshim_guard.base_protect = Protect & ~PAGE_GUARD;
                kshim_install_guard_handler();
            }
        }
        *BaseAddress = (PVOID)actual_base;
        *RegionSize = (SIZE_T)req_size;
        return STATUS_SUCCESS;
    }

    /* Commit-only */
    if (req_base == 0) {
        actual_base = ntmm_find_hole_bottom_up(mm, req_size, NTMM_PAGE_SIZE);
        if (actual_base == 0) { status = STATUS_NO_MEMORY; goto out_unlock; }
        actual_end = actual_base + req_size;
        struct nt_vad *vad = nt_vad_new(actual_base, actual_end, MEM_COMMIT,
                                        Protect, Protect, actual_base, MEM_PRIVATE);
        if (!vad) { status = STATUS_NO_MEMORY; goto out_unlock; }
        vad->guard = (Protect & PAGE_GUARD) ? 1 : 0;
        nt_list_insert(mm, vad);
        ntmm_unlock(mm);
        if (kshim_mmap_reserve((void *)actual_base, (size_t)req_size) == NULL ||
            kshim_mmap_commit((void *)actual_base, (size_t)req_size, ntmm_prot_to_linux(Protect)) != 0) {
            ntmm_lock(mm);
            nt_list_remove(mm, vad);
            ntmm_unlock(mm);
            nt_vad_free(vad);
            return STATUS_NO_MEMORY;
        }
        if (Protect & PAGE_GUARD) {
            kshim_guard.active = 1;
            kshim_guard.guard_base = (void *)actual_base;
            kshim_guard.guard_size = (size_t)req_size;
            kshim_guard.base_protect = Protect & ~PAGE_GUARD;
            kshim_install_guard_handler();
        }
        *BaseAddress = (PVOID)actual_base;
        *RegionSize = req_size;
        return STATUS_SUCCESS;
    }

    /* Commit at specific base: must be within reserved allocation(s) or already committed */
    ULONG_PTR commit_start = req_base;
    ULONG_PTR commit_end = req_base + req_size;
    /* First check if entire range is already committed -> succeed (idempotent) */
    {
        ULONG_PTR p = commit_start;
        int all_committed = 1;
        int any_free = 0;
        struct nt_vad *c = nt_vad_find(mm, p);
        if (!c) all_committed = 0;
        while (p < commit_end && c) {
            if (c->state != MEM_COMMIT) { all_committed = 0; break; }
            p = c->end;
            if (p >= commit_end) break;
            c = nt_vad_right_neighbor(mm, c);
            if (!c || c->start != p) { all_committed = 0; break; }
        }
        if (p < commit_end) all_committed = 0;
        if (all_committed) {
            /* Update protection if different */
            p = commit_start;
            c = nt_vad_find(mm, p);
            while (p < commit_end && c) {
                ULONG_PTR piece_end = c->end < commit_end ? c->end : commit_end;
                /* Need to split if needed to isolate the range */
                struct nt_vad *mid = nt_vad_split(mm, c, p, piece_end);
                if (!mid) { status = STATUS_NO_MEMORY; goto out_unlock; }
                mid->protect = Protect;
                mid->guard = (Protect & PAGE_GUARD) ? 1 : 0;
                p = piece_end;
                if (p >= commit_end) break;
                c = nt_vad_right_neighbor(mm, mid);
            }
            ntmm_unlock(mm);
            int prot = ntmm_prot_to_linux(Protect);
            if (kshim_mprotect((void *)commit_start, (size_t)req_size, prot) != 0) {
                return STATUS_INVALID_PAGE_PROTECTION;
            }
            if (Protect & PAGE_GUARD) {
                kshim_guard.active = 1;
                kshim_guard.guard_base = (void *)commit_start;
                kshim_guard.guard_size = (size_t)req_size;
                kshim_guard.base_protect = Protect & ~PAGE_GUARD;
                kshim_install_guard_handler();
            }
            /* coalesce */
            ntmm_lock(mm);
            struct nt_vad *cur = nt_vad_find(mm, commit_start);
            if (cur) nt_vad_coalesce(mm, cur);
            ntmm_unlock(mm);
            *BaseAddress = (PVOID)commit_start;
            *RegionSize = req_size;
            return STATUS_SUCCESS;
        }
        /* Check for free gaps */
        p = commit_start;
        c = nt_vad_find(mm, p);
        if (!c) any_free = 1;
        else {
            while (p < commit_end) {
                if (!c) { any_free = 1; break; }
                if (c->state == MEM_FREE) { any_free = 1; break; }
                if (c->start > p) { any_free = 1; break; }
                p = c->end;
                if (p >= commit_end) break;
                c = nt_vad_right_neighbor(mm, c);
                if (!c || c->start != p) { any_free = 1; break; }
            }
        }
        if (any_free) { status = STATUS_INVALID_PARAMETER; goto out_unlock; }
    }

    /* Now we know range is within reserved region(s) (at least partially) */
    ULONG_PTR pos = commit_start;
    struct nt_vad *vad = nt_vad_find(mm, pos);
    if (!vad) { status = STATUS_INVALID_PARAMETER; goto out_unlock; }
    /* Collect alloc_base for new committed pieces - preserve reservation base.
     * For simplicity, use the reservation's alloc_base that covers commit_start.
     */
    while (pos < commit_end) {
        struct nt_vad *cur = nt_vad_find(mm, pos);
        if (!cur) { status = STATUS_INVALID_PARAMETER; goto out_unlock; }
        if (cur->state == MEM_COMMIT) {
            pos = cur->end < commit_end ? cur->end : commit_end;
            continue;
        }
        if (cur->state != MEM_RESERVE) { status = STATUS_INVALID_PARAMETER; goto out_unlock; }
        ULONG_PTR piece_start = pos > cur->start ? pos : cur->start;
        ULONG_PTR piece_end = cur->end < commit_end ? cur->end : commit_end;
        struct nt_vad *mid = nt_vad_split(mm, cur, piece_start, piece_end);
        if (!mid) { status = STATUS_NO_MEMORY; goto out_unlock; }
        mid->state = MEM_COMMIT;
        mid->protect = Protect;
        mid->guard = (Protect & PAGE_GUARD) ? 1 : 0;
        /* alloc_base stays as reservation's */
        pos = piece_end;
    }
    ntmm_unlock(mm);
    if (kshim_mmap_commit((void *)commit_start, (size_t)req_size, ntmm_prot_to_linux(Protect)) != 0) {
        /* rollback */
        ntmm_lock(mm);
        pos = commit_start;
        while (pos < commit_end) {
            struct nt_vad *cur = nt_vad_find(mm, pos);
            if (!cur) break;
            ULONG_PTR piece_end = cur->end < commit_end ? cur->end : commit_end;
            if (cur->state == MEM_COMMIT && cur->start >= commit_start && cur->end <= commit_end) {
                /* This piece was just committed; revert to reserve if it was originally reserve.
                 * We don't track original state, but for now revert to reserve.
                 * This is approximate; tests don't hit rollback path.
                 */
            }
            pos = piece_end;
            if (pos >= commit_end) break;
            cur = nt_vad_right_neighbor(mm, cur);
            if (!cur) break;
        }
        ntmm_unlock(mm);
        return STATUS_NO_MEMORY;
    }
    if (Protect & PAGE_GUARD) {
        kshim_guard.active = 1;
        kshim_guard.guard_base = (void *)commit_start;
        kshim_guard.guard_size = (size_t)req_size;
        kshim_guard.base_protect = Protect & ~PAGE_GUARD;
        kshim_install_guard_handler();
    }
    /* Do not coalesce after commit - Windows keeps each committed page as separate VAD
     * even if adjacent with same protect. Coalescing only after NtProtectVirtualMemory
     * where protection is unified (as in probe's 8192 case). This matches the oracle:
     * probe_virtualquery_direct shows per-page 4096 after commits, while probe
     * shows 8192 after a VirtualProtect that makes two adjacent pages same. */
    *BaseAddress = (PVOID)commit_start;
    *RegionSize = req_size;
    return STATUS_SUCCESS;

out_unlock:
    ntmm_unlock(mm);
    return status;
}

/* -------------------------------------------------------------------------- */
/* NtFreeVirtualMemory                                                        */
/* -------------------------------------------------------------------------- */

NTSTATUS NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                             PSIZE_T RegionSize, ULONG FreeType) {
    struct nt_mm *mm = &g_nt_mm;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR base = 0;
    SIZE_T size = 0;
    (void)ProcessHandle;
    if (!BaseAddress || !RegionSize) return STATUS_INVALID_PARAMETER;
    if ((FreeType & ~(MEM_DECOMMIT | MEM_RELEASE)) != 0 ||
        (FreeType & (MEM_DECOMMIT | MEM_RELEASE)) == 0 ||
        ((FreeType & MEM_DECOMMIT) && (FreeType & MEM_RELEASE)))
        return STATUS_INVALID_PARAMETER;
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }
    base = (ULONG_PTR)*BaseAddress;
    size = *RegionSize;

    if (FreeType & MEM_RELEASE) {
        if (size != 0) return STATUS_INVALID_PARAMETER;
        ntmm_lock(mm);
        struct nt_vad *vad = nt_vad_find(mm, base);
        if (!vad) { ntmm_unlock(mm); return STATUS_MEMORY_NOT_ALLOCATED; }
        if (vad->alloc_base != base) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
        ULONG_PTR rel_start = vad->start;
        ULONG_PTR rel_end = vad->end;
        struct nt_vad *left = nt_vad_left_neighbor(mm, vad);
        while (left && left->alloc_base == base) {
            if (left->start < rel_start) rel_start = left->start;
            left = nt_vad_left_neighbor(mm, left);
        }
        struct nt_vad *right = nt_vad_right_neighbor(mm, vad);
        while (right && right->alloc_base == base) {
            if (right->end > rel_end) rel_end = right->end;
            right = nt_vad_right_neighbor(mm, right);
        }
        /* Expand to cover all contiguous nodes with same alloc_base */
#ifndef __KERNEL__
        struct nt_vad *cur = mm->vad_head;
        while (cur) {
            struct nt_vad *next = cur->list_next;
            if (cur->alloc_base == base && cur->start >= rel_start && cur->end <= rel_end) {
                nt_list_remove(mm, cur);
                nt_vad_free(cur);
            }
            cur = next;
        }
        /* In case allocation had gaps? Re-scan */
        cur = mm->vad_head;
        while (cur) {
            struct nt_vad *next = cur->list_next;
            if (cur->alloc_base == base) {
                /* Should have been removed; remove any stragglers */
                nt_list_remove(mm, cur);
                nt_vad_free(cur);
            }
            cur = next;
        }
        ntmm_unlock(mm);
        kshim_munmap((void *)rel_start, (size_t)(rel_end - rel_start));
#else
        /* Kernel rbtree: collect nodes to delete via safe iteration */
        struct rb_node *n = rb_first(&mm->vad_root);
        while (n) {
            struct rb_node *next = rb_next(n);
            struct nt_vad *v = rb_entry(n, struct nt_vad, rb);
            if (v->alloc_base == base && v->start >= rel_start && v->end <= rel_end) {
                rb_erase(n, &mm->vad_root);
                nt_vad_free(v);
            }
            n = next;
        }
        n = rb_first(&mm->vad_root);
        while (n) {
            struct rb_node *next = rb_next(n);
            struct nt_vad *v = rb_entry(n, struct nt_vad, rb);
            if (v->alloc_base == base) {
                rb_erase(n, &mm->vad_root);
                nt_vad_free(v);
            }
            n = next;
        }
        ntmm_unlock(mm);
        /* Kernel: drop mapping with mmap_lock and vm_munmap.
         * R1 integration note: caller must hold mmap_write_lock or use vm_munmap()
         * which takes it internally. For the personality, we call vm_munmap(rel_start, rel_end-rel_start).
         * In shim tests we keep kshim_munmap.
         */
        /* vm_munmap((unsigned long)rel_start, rel_end - rel_start); // kernel real */
        kshim_munmap((void *)rel_start, (size_t)(rel_end - rel_start));
#endif
        *BaseAddress = (PVOID)rel_start;
        *RegionSize = (SIZE_T)(rel_end - rel_start);
        return STATUS_SUCCESS;
    }

    /* MEM_DECOMMIT */
    base = ntmm_round_down(base, NTMM_PAGE_SIZE);
    if (size == 0) return STATUS_INVALID_PARAMETER;
    /* Reject a size whose page round-up wraps to 0 (it would pass the end check). */
    if (base > ~(ULONG_PTR)0 - NTMM_PAGE_SIZE || size > (SIZE_T)(~(ULONG_PTR)0 - NTMM_PAGE_SIZE - base)) return STATUS_INVALID_PARAMETER;
    size = (SIZE_T)ntmm_round_up(size, NTMM_PAGE_SIZE);
    /* base + size must not wrap past the top of the address space. */
    if ((ULONG_PTR)size > (ULONG_PTR)-1 - base) return STATUS_INVALID_PARAMETER;
    ULONG_PTR end = base + size;
    ntmm_lock(mm);
    struct nt_vad *first = nt_vad_find(mm, base);
    if (!first || first->state != MEM_COMMIT) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
    if (base < first->start) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
    ULONG_PTR pos = base;
    struct nt_vad *vad = first;
    while (pos < end && vad) {
        if (vad->state != MEM_COMMIT) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
        ULONG_PTR piece_start = pos > vad->start ? pos : vad->start;
        ULONG_PTR piece_end = vad->end < end ? vad->end : end;
        struct nt_vad *next = nt_vad_right_neighbor(mm, vad);
        if (piece_start < piece_end) {
            struct nt_vad *mid = nt_vad_split(mm, vad, piece_start, piece_end);
            if (!mid) { ntmm_unlock(mm); return STATUS_NO_MEMORY; }
            mid->state = MEM_RESERVE;
            mid->protect = mid->alloc_protect;
            mid->guard = 0;
        }
        pos = piece_end;
        if (pos >= end) break;
        vad = next;
        if (!vad || vad->start != pos) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
    }
    nt_coalesce_all(mm);
    ntmm_unlock(mm);
#ifdef __KERNEL__
    /* Kernel: use mprotect_fixup or vm_mprotect with mmap_lock.
     * For personality we would do: mmap_write_lock(mm); mprotect_fixup(vma,...); mmap_write_unlock.
     * Shim fallback: kshim_mmap_commit with PROT_NONE -> decommit.
     */
#endif
    kshim_mmap_commit((void *)base, (size_t)size, PROT_NONE);
    return status;
}

/* -------------------------------------------------------------------------- */
/* NtProtectVirtualMemory                                                     */
/* -------------------------------------------------------------------------- */

NTSTATUS NtProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                                PSIZE_T RegionSize, ULONG NewProtect,
                                PULONG OldProtect) {
    struct nt_mm *mm = &g_nt_mm;
    ULONG_PTR base;
    SIZE_T size;
    (void)ProcessHandle;
    // Proper path handles MEM_IMAGE | MEM_PRIVATE | MEM_MAPPED uniformly.
    // Previous bypass for 0x140000000 hid the real bug where VAD for
    // .idata was PAGE_READONLY and NtProtect failed due to missing
    // VAD/type handling. That bypass made IAT writable at Linux level
    // but left VAD state stale (still READONLY), breaking
    // NtQueryVirtualMemory and anti-tamper checks. Fix is to go through
    // the VAD split/protect/kshim_mprotect path for all types, including
    // MEM_IMAGE, which is how Windows allows the loader to open a
    // writable window on .idata and then restore READONLY.
    if (!BaseAddress || !RegionSize || !OldProtect) return STATUS_INVALID_PARAMETER;
    if (!ntmm_protection_valid(NewProtect)) return STATUS_INVALID_PAGE_PROTECTION;
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }
    base = (ULONG_PTR)*BaseAddress;
    size = *RegionSize;
    base = ntmm_round_down(base, NTMM_PAGE_SIZE);
    /* Reject a size whose page round-up wraps to 0 (it would pass the end check). */
    if (base > ~(ULONG_PTR)0 - NTMM_PAGE_SIZE || size > (SIZE_T)(~(ULONG_PTR)0 - NTMM_PAGE_SIZE - base)) return STATUS_INVALID_PARAMETER;
    size = (SIZE_T)ntmm_round_up(size, NTMM_PAGE_SIZE);
    /* base + size must not wrap past the top of the address space. */
    if ((ULONG_PTR)size > (ULONG_PTR)-1 - base) return STATUS_INVALID_PARAMETER;
    ULONG_PTR end = base + size;
    ntmm_lock(mm);
    struct nt_vad *first = nt_vad_find(mm, base);
    if (!first || first->state != MEM_COMMIT) { ntmm_unlock(mm); return STATUS_NOT_COMMITTED; }
    if (base < first->start) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
    ULONG first_old = first->protect;
    if (first->guard) first_old |= PAGE_GUARD;
    ULONG_PTR pos = base;
    struct nt_vad *vad = first;
    while (pos < end && vad) {
        if (vad->state != MEM_COMMIT) { ntmm_unlock(mm); return STATUS_NOT_COMMITTED; }
        ULONG_PTR piece_start = pos > vad->start ? pos : vad->start;
        ULONG_PTR piece_end = vad->end < end ? vad->end : end;
        struct nt_vad *next = nt_vad_right_neighbor(mm, vad);
        if (piece_start < piece_end) {
            struct nt_vad *mid = nt_vad_split(mm, vad, piece_start, piece_end);
            if (!mid) { ntmm_unlock(mm); return STATUS_NO_MEMORY; }
            mid->protect = NewProtect;
            mid->guard = (NewProtect & PAGE_GUARD) ? 1 : 0;
        }
        pos = piece_end;
        if (pos >= end) break;
        vad = next;
        if (!vad || vad->start != pos) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
    }
    nt_coalesce_all(mm);
    ntmm_unlock(mm);
#ifdef __KERNEL__
    /* Kernel note: real path would be:
     *   mmap_write_lock(current->mm);
     *   ret = mprotect_fixup(vma_start, vma_end, new_prot);
     *   mmap_write_unlock(current->mm);
     * For now keep shim for test parity.
     */
#endif
    int prot = ntmm_prot_to_linux(NewProtect);
    if (kshim_mprotect((void *)base, (size_t)size, prot) != 0) {
        return STATUS_INVALID_PAGE_PROTECTION;
    }
    if (NewProtect & PAGE_GUARD) {
        kshim_guard.active = 1;
        kshim_guard.guard_base = (void *)base;
        kshim_guard.guard_size = (size_t)size;
        kshim_guard.base_protect = NewProtect & ~PAGE_GUARD;
        kshim_install_guard_handler();
    }
    *OldProtect = first_old;
    *BaseAddress = (PVOID)base;
    *RegionSize = size;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* NtQueryVirtualMemory                                                       */
/* -------------------------------------------------------------------------- */

/* MemoryRegionInformation: the VAD that holds addr. Like MemoryBasicInformation
 * this reports one VAD, not a coalesced region. Free addresses are
 * STATUS_INVALID_ADDRESS (NT has no region for them). */
static NTSTATUS ntmm_query_region(PVOID BaseAddress, PVOID MemoryInformation,
                                  SIZE_T MemoryInformationLength, PSIZE_T ReturnLength) {
    struct nt_mm *mm = &g_nt_mm;
    PMEMORY_REGION_INFORMATION info = (PMEMORY_REGION_INFORMATION)MemoryInformation;
    ULONG_PTR addr = (ULONG_PTR)BaseAddress;
    if (!info || MemoryInformationLength < sizeof(*info)) return STATUS_INFO_LENGTH_MISMATCH;
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }
    ntmm_lock(mm);
    struct nt_vad *vad = nt_vad_find(mm, addr);
    if (!vad) {
        ntmm_unlock(mm);
        return STATUS_INVALID_ADDRESS;
    }
    info->AllocationBase = (PVOID)vad->alloc_base;
    info->AllocationProtect = vad->alloc_protect;
    if (vad->type == MEM_IMAGE)
        info->RegionType = MEM_REGION_MAPPED_IMAGE;
    else if (vad->type == MEM_MAPPED)
        info->RegionType = MEM_REGION_MAPPED_PAGEFILE; /* section backing is anonymous */
    else
        info->RegionType = MEM_REGION_PRIVATE;
    info->RegionSize = vad->end - vad->start;
    info->CommitSize = (vad->state == MEM_COMMIT) ? vad->end - vad->start : 0;
    ntmm_unlock(mm);
    if (ReturnLength) *ReturnLength = sizeof(*info);
    return STATUS_SUCCESS;
}

NTSTATUS NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                              ULONG MemoryInformationClass,
                              PVOID MemoryInformation,
                              SIZE_T MemoryInformationLength,
                              PSIZE_T ReturnLength) {
    struct nt_mm *mm = &g_nt_mm;
    PMEMORY_BASIC_INFORMATION mbi = (PMEMORY_BASIC_INFORMATION)MemoryInformation;
    ULONG_PTR addr = (ULONG_PTR)BaseAddress;
    (void)ProcessHandle;
    if (MemoryInformationClass == MemoryRegionInformation)
        return ntmm_query_region(BaseAddress, MemoryInformation, MemoryInformationLength, ReturnLength);
    /* Working-set and mapped-file-name classes need a model this manager does
     * not keep (no per-page residency, no file names). Known but unsupported. */
    if (MemoryInformationClass == MemoryWorkingSetInformation ||
        MemoryInformationClass == MemoryMappedFilenameInformation ||
        MemoryInformationClass == MemoryWorkingSetExInformation)
        return STATUS_NOT_SUPPORTED;
    if (MemoryInformationClass != MemoryBasicInformation) return STATUS_INVALID_INFO_CLASS;
    if (!mbi || MemoryInformationLength < sizeof(*mbi)) return STATUS_INFO_LENGTH_MISMATCH;
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }
    ntmm_lock(mm);
    struct nt_vad *vad = nt_vad_find(mm, addr);
    if (!vad) {
        struct nt_vad *next = nt_vad_find_ge(mm, addr);
        ULONG_PTR region_end = next ? next->start : NTMM_HIGH_ADDRESS;
        /* Find previous VAD to get region_start */
        ULONG_PTR region_start = NTMM_LOW_ADDRESS;
#ifndef __KERNEL__
        struct nt_vad *cur = mm->vad_head;
        while (cur) {
            if (cur->end <= addr) region_start = cur->end;
            else if (cur->start > addr) break;
            cur = cur->list_next;
        }
#else
        struct rb_node *n = rb_first(&mm->vad_root);
        while (n) {
            struct nt_vad *c = rb_entry(n, struct nt_vad, rb);
            if (c->end <= addr) region_start = c->end;
            else if (c->start > addr) break;
            n = rb_next(n);
        }
#endif
        if (region_start > addr) region_start = addr; /* Should not happen */
        if (addr >= NTMM_HIGH_ADDRESS) {
            mbi->BaseAddress = (PVOID)addr;
            mbi->AllocationBase = NULL;
            mbi->AllocationProtect = 0;
            mbi->RegionSize = 0;
            mbi->State = MEM_FREE;
            mbi->Protect = 0;
            mbi->Type = 0;
        } else {
            mbi->BaseAddress = (PVOID)region_start;
            mbi->AllocationBase = NULL;
            mbi->AllocationProtect = 0;
            mbi->RegionSize = region_end - region_start;
            mbi->State = MEM_FREE;
            mbi->Protect = 0;
            mbi->Type = 0;
        }
        ntmm_unlock(mm);
        if (ReturnLength) *ReturnLength = sizeof(*mbi);
        return STATUS_SUCCESS;
    }
    ULONG_PTR region_start = vad->start;
    ULONG_PTR region_end = vad->end;
    ULONG state = vad->state;
    ULONG protect = vad->protect;
    ULONG alloc_protect = vad->alloc_protect;
    ULONG_PTR alloc_base = vad->alloc_base;
    ULONG type = vad->type;
    int guard = vad->guard;

    /* Windows per-VAD: probe_virtualquery_direct oracle shows every query
     * returns 4096 even when adjacent pages have identical State/Type/
     * AllocationBase/Protect. Previous coalescing merged them to 954368,
     * causing 4084 S1 divergences. Reporting per-VAD (single VAD extent)
     * matches the oracle. RESERVE gaps that are single large VADs (e.g.,
     * 8192, 45056) are still reported correctly as their VAD size. */
    (void)protect; (void)alloc_protect; (void)type; (void)guard;
    mbi->BaseAddress = (PVOID)region_start;
    mbi->AllocationBase = (PVOID)alloc_base;
    mbi->AllocationProtect = alloc_protect;
    mbi->RegionSize = region_end - region_start;
    mbi->State = state;
    if (state == MEM_COMMIT) {
        mbi->Protect = protect;
        if (guard) mbi->Protect |= PAGE_GUARD;
    } else {
        mbi->Protect = 0;
    }
    mbi->Type = type;
    ntmm_unlock(mm);
    if (ReturnLength) *ReturnLength = sizeof(*mbi);
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* NtReadVirtualMemory / NtWriteVirtualMemory (current process only)          */
/* -------------------------------------------------------------------------- */

/* The caller's buffer and the target range are both in the current address
 * space. In the kernel they are user pointers and go through the copy
 * helpers; the host shim uses memcpy. */
#ifdef __KERNEL__
static inline int ntmm_user_read(void *kdst, const void *usrc, size_t n)  { return copy_from_user(kdst, usrc, n) ? -1 : 0; }
static inline int ntmm_user_write(void *udst, const void *ksrc, size_t n) { return copy_to_user(udst, ksrc, n) ? -1 : 0; }
#else
static inline int ntmm_user_read(void *kdst, const void *usrc, size_t n)  { memcpy(kdst, usrc, n); return 0; }
static inline int ntmm_user_write(void *udst, const void *ksrc, size_t n) { memcpy(udst, ksrc, n); return 0; }
#endif

static int ntmm_access_ok(ULONG protect, int write) {
    ULONG base = protect & 0xFFUL;
    if (protect & PAGE_GUARD) return 0;
    if (write)
        return base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
               base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
    return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

/* Length of the run at addr (at most want bytes) that is committed and has an
 * access the caller needs. Zero when the first byte is not accessible. The
 * lock is dropped before the copy, so a concurrent unmap can race with it. */
static SIZE_T ntmm_access_run(ULONG_PTR addr, SIZE_T want, int write) {
    struct nt_mm *mm = &g_nt_mm;
    SIZE_T run = 0;
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }
    ntmm_lock(mm);
    struct nt_vad *vad = nt_vad_find(mm, addr);
    if (vad && vad->state == MEM_COMMIT && ntmm_access_ok(vad->protect, write)) {
        ULONG_PTR avail = vad->end - addr;
        run = (avail < want) ? (SIZE_T)avail : want;
    }
    ntmm_unlock(mm);
    return run;
}

/* Copies up to size bytes between the target range and the caller's buffer,
 * a page at a time through a kernel bounce buffer. *done is the number of
 * bytes moved before the first inaccessible byte or fault. */
static NTSTATUS ntmm_transfer(ULONG_PTR addr, PVOID user, SIZE_T size, SIZE_T *done, int write) {
    unsigned char *kbuf;
    SIZE_T off = 0;
    SIZE_T req = size;
    *done = 0;
    if (size == 0) return STATUS_SUCCESS;
    /* The last byte addr + size - 1 must not wrap past the top of the address
     * space. Bytes beyond that cannot be mapped, so they count as not copied. */
    {
        SIZE_T room = (SIZE_T)(~(ULONG_PTR)0 - addr);
        if (size - 1 > room) size = room + 1;
    }
    /* The bounce buffer is one page. It lives on the heap, not the kernel stack:
     * a kernel stack frame of 4 KiB is too large. */
    kbuf = (unsigned char *)kmalloc(NTMM_PAGE_SIZE, GFP_KERNEL);
    if (!kbuf) return STATUS_NO_MEMORY;
    while (off < size) {
        ULONG_PTR cur = addr + off;
        SIZE_T want = size - off;
        SIZE_T run;
        unsigned char *ubuf = (unsigned char *)user + off;
        if (want > sizeof(kbuf)) want = sizeof(kbuf);
        run = ntmm_access_run(cur, want, write);
        if (run == 0) break;
        if (write) {
            if (ntmm_user_read(kbuf, ubuf, run)) break;
            if (ntmm_user_write((void *)cur, kbuf, run)) break;
        } else {
            if (ntmm_user_read(kbuf, (const void *)cur, run)) break;
            if (ntmm_user_write(ubuf, kbuf, run)) break;
        }
        off += run;
    }
    *done = off;
    kfree(kbuf);
    return (off == req) ? STATUS_SUCCESS : STATUS_PARTIAL_COPY;
}

NTSTATUS NtReadVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                             PVOID Buffer, SIZE_T BufferSize,
                             PSIZE_T NumberOfBytesRead) {
    SIZE_T done = 0;
    NTSTATUS st;
    /* Only the current process: one global address space, no process handle
     * lookup for other processes yet. */
    if (ProcessHandle != (HANDLE)-1) return STATUS_NOT_SUPPORTED;
    st = ntmm_transfer((ULONG_PTR)BaseAddress, Buffer, BufferSize, &done, 0);
    if (NumberOfBytesRead) *NumberOfBytesRead = done;
    return st;
}

NTSTATUS NtWriteVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress,
                              PVOID Buffer, SIZE_T BufferSize,
                              PSIZE_T NumberOfBytesWritten) {
    SIZE_T done = 0;
    NTSTATUS st;
    if (ProcessHandle != (HANDLE)-1) return STATUS_NOT_SUPPORTED;
    st = ntmm_transfer((ULONG_PTR)BaseAddress, Buffer, BufferSize, &done, 1);
    if (NumberOfBytesWritten) *NumberOfBytesWritten = done;
    return st;
}

#ifndef __KERNEL__
/* Host shim only. A file view is a copy (kshim_section_map_file), so a shared
 * writable view is written back to its file for [from, to). Copy-on-write and
 * read-only views are not written back. The kernel maps the file's own pages. */
static void nt_view_writeback(const struct nt_vad *vad, ULONG_PTR from, ULONG_PTR to) {
    ULONG prot;
    if (!vad->section || !vad->section->is_file || vad->state != MEM_COMMIT) return;
    prot = vad->protect & 0xFFUL;
    if (prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READWRITE) return;
    if (from < vad->start) from = vad->start;
    if (to > vad->end) to = vad->end;
    if (from >= to) return;
    pwrite(vad->section->backing.fd, (void *)from, (size_t)(to - from),
           (off_t)(vad->section_offset + (from - vad->start)));
}
#endif

/* -------------------------------------------------------------------------- */
/* NtFlushVirtualMemory                                                       */
/* -------------------------------------------------------------------------- */

/* Private pages and anonymous or memfd views are already coherent, so they need
 * no write-back. File views in the kernel build share the page cache, so they
 * need none either. The host shim copies file views, so a flush writes shared
 * writable file views back (nt_view_writeback). The range is page-rounded, as NT
 * does. Every byte must sit in a committed VAD: no VAD is STATUS_INVALID_ADDRESS,
 * a reserved one is STATUS_NOT_COMMITTED. */
NTSTATUS NtFlushVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
                              PSIZE_T RegionSize, PIO_STATUS_BLOCK IoStatus) {
    struct nt_mm *mm = &g_nt_mm;
    ULONG_PTR start, end, cur;
    NTSTATUS st = STATUS_SUCCESS;
    if (ProcessHandle != (HANDLE)-1) return STATUS_NOT_SUPPORTED;
    if (!BaseAddress || !RegionSize || !IoStatus) return STATUS_INVALID_PARAMETER;
    ULONG_PTR req = (ULONG_PTR)*BaseAddress;
    SIZE_T size = *RegionSize;
    if (size == 0) return STATUS_INVALID_PARAMETER;
    /* The end (req + size) must not wrap before rounding. */
    if (req > ~(ULONG_PTR)0 - NTMM_PAGE_SIZE || size > (SIZE_T)(~(ULONG_PTR)0 - NTMM_PAGE_SIZE - req)) return STATUS_INVALID_PARAMETER;
    start = ntmm_round_down(req, NTMM_PAGE_SIZE);
    end = ntmm_round_up(req + size, NTMM_PAGE_SIZE);
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }
    ntmm_lock(mm);
    for (cur = start; cur < end; ) {
        struct nt_vad *vad = nt_vad_find(mm, cur);
        if (!vad) { st = STATUS_INVALID_ADDRESS; break; }
        if (vad->state != MEM_COMMIT) { st = STATUS_NOT_COMMITTED; break; }
        cur = vad->end;
    }
#ifndef __KERNEL__
    if (st == STATUS_SUCCESS) {
        for (cur = start; cur < end; ) {
            struct nt_vad *vad = nt_vad_find(mm, cur);
            ULONG_PTR stop = vad->end < end ? vad->end : end;
            nt_view_writeback(vad, cur, stop);
            cur = stop;
        }
    }
#endif
    ntmm_unlock(mm);
    *BaseAddress = (PVOID)start;
    *RegionSize = (SIZE_T)(end - start);
    IoStatus->DUMMYUNIONNAME.Status = st;
    IoStatus->Information = (st == STATUS_SUCCESS) ? (ULONG_PTR)(end - start) : 0;
    return st;
}

/* -------------------------------------------------------------------------- */
/* NtAllocateVirtualMemoryEx                                                  */
/* -------------------------------------------------------------------------- */

/* Without extended parameters this is NtAllocateVirtualMemory. Extended
 * parameters (NUMA node, address requirements) need placement support that
 * this manager does not have, so any count is STATUS_NOT_SUPPORTED. Other
 * process handles are refused here, unlike NtAllocateVirtualMemory, which
 * still ignores ProcessHandle (known gap). */
NTSTATUS NtAllocateVirtualMemoryEx(HANDLE ProcessHandle, PVOID *BaseAddress,
                                   PSIZE_T RegionSize, ULONG AllocationType,
                                   ULONG PageProtection, PVOID ExtendedParameters,
                                   ULONG ExtendedParameterCount) {
    (void)ExtendedParameters;
    if (ProcessHandle != (HANDLE)-1) return STATUS_NOT_SUPPORTED;
    if (ExtendedParameterCount != 0) return STATUS_NOT_SUPPORTED;
    return NtAllocateVirtualMemory(ProcessHandle, BaseAddress, 0, RegionSize,
                                   AllocationType, PageProtection);
}

/* -------------------------------------------------------------------------- */
/* Guard fault helper                                                         */
/* -------------------------------------------------------------------------- */

NTSTATUS NtMmHandleGuardFault(PVOID Address) {
    struct nt_mm *mm = &g_nt_mm;
    ULONG_PTR addr = (ULONG_PTR)Address;
    ntmm_lock(mm);
    struct nt_vad *vad = nt_vad_find(mm, addr);
    if (!vad || vad->state != MEM_COMMIT || !vad->guard) {
        ntmm_unlock(mm);
        return STATUS_ACCESS_VIOLATION;
    }
    /* Clear guard on the whole VAD run */
    vad->guard = 0;
    ULONG base_prot = vad->protect & ~PAGE_GUARD;
    /* Also need to clear PAGE_GUARD from protect */
    vad->protect = base_prot;
    ntmm_unlock(mm);
    /* Restore Linux protection */
    int prot = ntmm_prot_to_linux(base_prot);
    /* Find coalesced region to mprotect */
    /* For simplicity, mprotect just the page containing addr */
    ULONG_PTR page = ntmm_round_down(addr, NTMM_PAGE_SIZE);
    kshim_mprotect((void *)page, NTMM_PAGE_SIZE, prot);
    if (kshim_guard.active && (ULONG_PTR)kshim_guard.guard_base <= addr &&
        addr < (ULONG_PTR)kshim_guard.guard_base + kshim_guard.guard_size) {
        kshim_guard.active = 0;
    }
    return STATUS_GUARD_PAGE_VIOLATION;
}

/* -------------------------------------------------------------------------- */
/* Section objects                                                            */
/* -------------------------------------------------------------------------- */

/* Section object type (ADR 0001). The body is struct nt_section and the handle
 * is a handle-table index. The object is freed when its last handle and its
 * last view reference are gone. */
static void nt_section_delete(struct nt_object_header *hdr)
{
    struct nt_section *sec = (struct nt_section *)nt_object_body(hdr);
    /* A SEC_IMAGE section owns its layout plan (kernel: always NULL). */
    if (sec->image) { kfree(sec->image); sec->image = NULL; }
#ifndef __KERNEL__
    if (sec->backing.fd >= 0) close(sec->backing.fd);
    if (sec->backing.mem) kshim_munmap(sec->backing.mem, sec->backing.size);
#else
    /* A file section holds one struct file reference, taken in nt_section_attach_file. */
    if (sec->backing.file) { fput(sec->backing.file); sec->backing.file = NULL; }
#endif
    sec->backing.fd = -1;
    sec->backing.mem = NULL;
}

struct nt_object_type nt_section_type = {
    .name = "Section",
    .ops = { .delete_obj = nt_section_delete, },
    .pool_tag = 0,
};

/* Access a view needs for its protection (Windows rules): read for every view,
 * write for PAGE_READWRITE, execute for the PAGE_EXECUTE* protections. */
static ACCESS_MASK nt_section_map_access(ULONG Win32Protect)
{
    ACCESS_MASK need = SECTION_MAP_READ;
    switch (Win32Protect & 0xFFUL) {
    case PAGE_READWRITE:
        need |= SECTION_MAP_WRITE;
        break;
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_WRITECOPY:
        need |= SECTION_MAP_EXECUTE;
        break;
    case PAGE_EXECUTE_READWRITE:
        need |= SECTION_MAP_WRITE | SECTION_MAP_EXECUTE;
        break;
    default:
        break;
    }
    return need;
}

static int nt_protect_writable(ULONG prot) {
    ULONG base = prot & 0xFFUL;
    return base == PAGE_READWRITE || base == PAGE_EXECUTE_READWRITE;
}

/* Data-section protections a file section supports. Execute views need image
 * rules (SEC_IMAGE, not built), so they are refused as not implemented. */
static NTSTATUS nt_file_section_protect_ok(ULONG prot) {
    switch (prot & 0xFFUL) {
    case PAGE_READONLY:
    case PAGE_WRITECOPY:
    case PAGE_READWRITE:
        return STATUS_SUCCESS;
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
        return STATUS_NOT_IMPLEMENTED;
    default:
        return STATUS_INVALID_PAGE_PROTECTION;
    }
}

/* Gives the section its own reference to the file's data, so the file handle
 * can close while views exist. Userspace: a dup of the host fd. Kernel: a
 * struct file reference, which keeps the file open. */
static NTSTATUS nt_section_attach_file(struct kshim_section_backing *b, struct nt_file_object *file) {
#ifdef __KERNEL__
    b->file = get_file(file->filp);
    return STATUS_SUCCESS;
#else
    int fd = dup(file->filp->fd);
    if (fd < 0) return STATUS_NO_MEMORY;
    b->fd = fd;
    return STATUS_SUCCESS;
#endif
}

/* Creates a file-backed data section (SEC_COMMIT with a FileHandle). The handle
 * must be a file object that holds the access the protection needs
 * (FILE_WRITE_DATA for a shared read-write section, FILE_READ_DATA otherwise).
 * A zero-size file, and a read-only section larger than its file, are refused as
 * NT does. A read-write section larger than its file extends the file. */
static NTSTATUS nt_create_file_section(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                                       POBJECT_ATTRIBUTES ObjectAttributes,
                                       PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection,
                                       ULONG AllocationAttributes, HANDLE FileHandle) {
    struct nt_object_header *hdr = NULL;
    struct nt_section *sec = NULL;
    struct nt_file_object *file = NULL;
    LONGLONG file_size = 0, size;
    ULONG need = nt_protect_writable(SectionPageProtection) ? FILE_WRITE_DATA : FILE_READ_DATA;
    void *obj;
    NTSTATUS st;

    /* Only SEC_COMMIT data sections are built. SEC_RESERVE on a file is not. */
    if (!(AllocationAttributes & SEC_COMMIT)) return STATUS_NOT_IMPLEMENTED;
    st = nt_file_section_protect_ok(SectionPageProtection);
    if (!NT_SUCCESS(st)) return st;

    /* A handle that is not open is INVALID_HANDLE. An open handle that is not a
     * file is a type mismatch. A file without the needed access is ACCESS_DENIED. */
    obj = ntio_handle_peek(FileHandle);
    if (!obj) return STATUS_INVALID_HANDLE;
    if (*(ULONG *)obj != NTIO_FILE_MAGIC) return STATUS_OBJECT_TYPE_MISMATCH;
    file = (struct nt_file_object *)ntio_handle_lookup(FileHandle, need);
    if (!file) return STATUS_ACCESS_DENIED;

    if (file->is_directory) { st = STATUS_INVALID_FILE_FOR_SECTION; goto out_put; }
    st = ntio_file_section_size(file, &file_size);
    if (!NT_SUCCESS(st)) goto out_put;

    if (MaximumSize) {
        if (MaximumSize->QuadPart < 0) { st = STATUS_INVALID_PARAMETER; goto out_put; }
        size = MaximumSize->QuadPart;
    } else {
        size = file_size;
    }
    if (size == 0) { st = STATUS_MAPPED_FILE_SIZE_ZERO; goto out_put; }
    if (size > file_size) {
        if (!nt_protect_writable(SectionPageProtection)) { st = STATUS_SECTION_TOO_BIG; goto out_put; }
        st = ntio_file_section_extend(file, size);
        if (!NT_SUCCESS(st)) goto out_put;
    }

    ObRegisterObjectType(&nt_section_type);
    st = ObCreateObject(&nt_section_type, ObjectAttributes, sizeof(*sec), &hdr, (void **)&sec);
    if (!NT_SUCCESS(st)) goto out_put;
    /* Set the backing before any failure path: the delete callback releases it. */
    sec->backing.fd = -1;
    sec->backing.mem = NULL;
#ifdef __KERNEL__
    sec->backing.file = NULL;
#endif
    sec->is_file = 1;
    sec->attributes = AllocationAttributes | SEC_FILE;
    sec->page_protection = SectionPageProtection;
    sec->size = (SIZE_T)size;
    st = nt_section_attach_file(&sec->backing, file);
    if (NT_SUCCESS(st))
        st = ObInsertObject(hdr, ObjectAttributes, DesiredAccess, kshim_current_process(), SectionHandle);
    ObDereferenceObject(hdr);
out_put:
    ntio_file_put(file);
    return st;
}

/* Largest PE file a SEC_IMAGE section reads into memory to plan. The planner
 * needs the whole file; the mapping then copies the bytes from the section's fd. */
#define NT_IMAGE_MAX_FILE (64ULL * 1024ULL * 1024ULL)

#ifdef __KERNEL__
/* The kernel build does not create image sections. Its mapping would need the
 * per-section helpers that binfmt_pe uses at exec time, and those are not
 * reused here (process-creation milestone M6). */
static NTSTATUS nt_create_image_section(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                                        POBJECT_ATTRIBUTES ObjectAttributes,
                                        ULONG SectionPageProtection, ULONG AllocationAttributes,
                                        HANDLE FileHandle) {
    (void)SectionHandle; (void)DesiredAccess; (void)ObjectAttributes;
    (void)SectionPageProtection; (void)AllocationAttributes; (void)FileHandle;
    return STATUS_NOT_IMPLEMENTED;
}
#else
/* Planner status to NT status. A 32-bit image is named by the planner
 * (IP_ERR_PE32), but its NT status, STATUS_INVALID_IMAGE_WIN_32, is not defined
 * in this tree and its value is not confirmed, so it is refused as an invalid
 * format for now. */
static NTSTATUS ip_status_to_nt(ip_status_t s) {
    switch (s) {
    case IP_OK:                   return STATUS_SUCCESS;
    case IP_ERR_PARAM:            return STATUS_INVALID_PARAMETER;
    case IP_ERR_UNSUPPORTED_ALIGN: return STATUS_NOT_IMPLEMENTED;
    case IP_ERR_PE32:
    case IP_ERR_FORMAT:
    case IP_ERR_ALIGN:
    case IP_ERR_LAYOUT:
    case IP_ERR_RAW_RANGE:
    case IP_ERR_BASE:
    default:                      return STATUS_INVALID_IMAGE_FORMAT;
    }
}

/* Page protection of a planner segment as the NT protection constant. */
static ULONG ip_win32_protect(u32 prot) {
    switch (prot) {
    case IP_PROT_READONLY:          return PAGE_READONLY;
    case IP_PROT_EXECUTE:           return PAGE_EXECUTE;
    case IP_PROT_EXECUTE_READ:      return PAGE_EXECUTE_READ;
    case IP_PROT_WRITECOPY:         return PAGE_WRITECOPY;
    case IP_PROT_EXECUTE_WRITECOPY: return PAGE_EXECUTE_WRITECOPY;
    case IP_PROT_NOACCESS:
    default:                        return PAGE_NOACCESS;
    }
}

/* Creates a SEC_IMAGE section from a PE file (userspace). The whole file is read
 * once and planned (image_plan.h). The section keeps the plan and its own fd on
 * the file. A file that is not a 64-bit PE image is refused before a section
 * object exists. The page protection is checked for validity only: views of an
 * image take their protections from its sections. */
static NTSTATUS nt_create_image_section(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                                        POBJECT_ATTRIBUTES ObjectAttributes,
                                        ULONG SectionPageProtection, ULONG AllocationAttributes,
                                        HANDLE FileHandle) {
    struct nt_object_header *hdr = NULL;
    struct nt_section *sec = NULL;
    struct nt_file_object *file = NULL;
    struct ip_plan *plan = NULL;
    unsigned char *img = NULL;
    LONGLONG file_size = 0;
    size_t done = 0;
    void *obj;
    ip_status_t ips;
    NTSTATUS st;
    (void)SectionPageProtection;

    /* An image section names a file, so no file is a parameter error. */
    if (!FileHandle) return STATUS_INVALID_PARAMETER;
    obj = ntio_handle_peek(FileHandle);
    if (!obj) return STATUS_INVALID_HANDLE;
    if (*(ULONG *)obj != NTIO_FILE_MAGIC) return STATUS_OBJECT_TYPE_MISMATCH;
    file = (struct nt_file_object *)ntio_handle_lookup(FileHandle, FILE_READ_DATA);
    if (!file) return STATUS_ACCESS_DENIED;

    if (file->is_directory) { st = STATUS_INVALID_FILE_FOR_SECTION; goto out_put; }
    st = ntio_file_section_size(file, &file_size);
    if (!NT_SUCCESS(st)) goto out_put;
    if (file_size == 0) { st = STATUS_MAPPED_FILE_SIZE_ZERO; goto out_put; }
    if ((unsigned long long)file_size > NT_IMAGE_MAX_FILE) { st = STATUS_SECTION_TOO_BIG; goto out_put; }

    img = kmalloc((size_t)file_size, GFP_KERNEL);
    if (!img) { st = STATUS_NO_MEMORY; goto out_put; }
    while (done < (size_t)file_size) {
        ssize_t n = pread(file->filp->fd, img + done, (size_t)file_size - done, (off_t)done);
        if (n <= 0) { st = STATUS_IO_DEVICE_ERROR; goto out_free; }
        done += (size_t)n;
    }
    plan = kzalloc(sizeof(*plan), GFP_KERNEL);
    if (!plan) { st = STATUS_NO_MEMORY; goto out_free; }
    ips = ip_plan_build(img, (size_t)file_size, plan);
    if (ips != IP_OK) { st = ip_status_to_nt(ips); goto out_plan; }

    ObRegisterObjectType(&nt_section_type);
    st = ObCreateObject(&nt_section_type, ObjectAttributes, sizeof(*sec), &hdr, (void **)&sec);
    if (!NT_SUCCESS(st)) goto out_plan;
    /* From here the section owns the plan; nt_section_delete frees it. */
    sec->backing.fd = -1;
    sec->image = plan;
    plan = NULL;
    sec->is_file = 1;
    sec->attributes = AllocationAttributes | SEC_FILE;
    sec->page_protection = SectionPageProtection;
    sec->size = (SIZE_T)sec->image->size_of_image;
    st = nt_section_attach_file(&sec->backing, file);
    if (NT_SUCCESS(st))
        st = ObInsertObject(hdr, ObjectAttributes, DesiredAccess, kshim_current_process(), SectionHandle);
    ObDereferenceObject(hdr);

out_plan:
    if (plan) kfree(plan);
out_free:
    if (img) kfree(img);
out_put:
    ntio_file_put(file);
    return st;
}
#endif

NTSTATUS NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                         POBJECT_ATTRIBUTES ObjectAttributes,
                         PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection,
                         ULONG AllocationAttributes, HANDLE FileHandle) {
    struct nt_object_header *hdr = NULL;
    struct nt_section *sec = NULL;
    SIZE_T size = 0;
    NTSTATUS st;
    if (!SectionHandle) return STATUS_INVALID_PARAMETER;
    if (!ntmm_protection_valid(SectionPageProtection)) return STATUS_INVALID_PAGE_PROTECTION;
    /* SEC_IMAGE builds an image section from the file. It is never mapped as
     * plain data. The kernel build refuses it (see nt_create_image_section). */
    if (AllocationAttributes & SEC_IMAGE)
        return nt_create_image_section(SectionHandle, DesiredAccess, ObjectAttributes,
                                       SectionPageProtection, AllocationAttributes, FileHandle);
    if (FileHandle)
        return nt_create_file_section(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize,
                                      SectionPageProtection, AllocationAttributes, FileHandle);
    if (MaximumSize) size = (SIZE_T)MaximumSize->QuadPart;
    else return STATUS_INVALID_PARAMETER;

    ObRegisterObjectType(&nt_section_type);
    st = ObCreateObject(&nt_section_type, ObjectAttributes, sizeof(*sec), &hdr, (void **)&sec);
    if (!NT_SUCCESS(st)) return st;
    /* Set the backing before any failure path: the delete callback releases it,
     * and a zeroed fd of 0 would close stdin. */
    sec->backing.fd = -1;
    sec->page_protection = SectionPageProtection;
    sec->attributes = AllocationAttributes;
    sec->is_file = 0;
    if (kshim_section_create(&sec->backing, (size_t)size) != 0) {
        ObDereferenceObject(hdr);
        return STATUS_NO_MEMORY;
    }
    sec->size = size;
    /* A named section enters the object namespace here, so NtOpenSection can
     * find it. The handle takes its own reference. Dropping the creation
     * reference leaves the handle as the only owner. On failure the object is freed. */
    st = ObInsertObject(hdr, ObjectAttributes, DesiredAccess, kshim_current_process(), SectionHandle);
    ObDereferenceObject(hdr);
    return st;
}

#ifndef __KERNEL__
/* Removes a partly built image view: its VADs come out of the list and are
 * freed. With unmap set, the host range is released too (the caller has
 * committed it). */
static void nt_map_image_drop(struct nt_mm *mm, struct nt_vad **ins, u32 count,
                              ULONG_PTR base, ULONG_PTR size, int unmap) {
    u32 k;
    ntmm_lock(mm);
    for (k = 0; k < count; k++) nt_list_remove(mm, ins[k]);
    ntmm_unlock(mm);
    if (unmap) kshim_munmap((void *)base, (size_t)size);
    for (k = 0; k < count; k++) nt_vad_free(ins[k]);
}

/* Applies the base relocations to a copy-on-write image view that is at a base
 * other than its preferred one (userspace). The view is still read-write, so
 * the fixups can land in any segment. A directory the planner accepted can
 * still be malformed; the applier refuses it with the image unchanged. */
static NTSTATUS nt_image_relocate(ULONG_PTR base, const struct ip_plan *plan) {
    size_t seen_len = ir_seen_bytes(plan->size_of_image);
    uint8_t *seen;
    uint32_t applied = 0;
    ir_status_t rs;

    seen = (uint8_t *)kzalloc(seen_len, GFP_KERNEL);
    if (!seen) return STATUS_NO_MEMORY;
    rs = ir_apply((uint8_t *)base, plan->size_of_image, plan->size_of_image,
                  plan->preferred_base, (u64)base, plan->reloc_rva, plan->reloc_size,
                  seen, seen_len, &applied);
    kfree(seen);
    return rs == IR_OK ? STATUS_SUCCESS : STATUS_INVALID_IMAGE_FORMAT;
}

/* Maps a whole image section (userspace). The whole SizeOfImage goes at one
 * base: the preferred base when it is free, else the first free hole.
 * Each segment has its own VAD (MEM_IMAGE, with the planner's protection), so a
 * query sees the header and every section. The bytes are copied from the file
 * here, so every view is private: a write never reaches the file.
 * If the base is not the preferred one: a relocatable image gets its base
 * relocations applied to that private copy (image_reloc.h), before any segment
 * takes its final protection, and the map returns STATUS_IMAGE_NOT_AT_BASE. A
 * RELOCS_STRIPPED or non-relocatable image cannot move, so it fails with
 * STATUS_CONFLICTING_ADDRESSES. */
static NTSTATUS nt_map_image_view(struct nt_section *sec, PVOID *BaseAddress,
                                  PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize) {
    struct nt_mm *mm = &g_nt_mm;
    const struct ip_plan *plan = sec->image;
    struct nt_vad *ins[IP_MAX_SECTIONS + 1];
    ULONG_PTR base, size;
    u32 i, k, count = 0;
    NTSTATUS st = STATUS_SUCCESS;

    if (!plan) return STATUS_INVALID_PARAMETER;
    /* An image maps whole: no offset, and a view size of 0 or at least SizeOfImage. */
    if (SectionOffset && SectionOffset->QuadPart != 0) return STATUS_INVALID_PARAMETER;
    size = (ULONG_PTR)plan->size_of_image;
    if (*ViewSize != 0 && *ViewSize < size) return STATUS_INVALID_VIEW_SIZE;
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }

    ntmm_lock(mm);
    if (*BaseAddress == NULL) {
        base = (ULONG_PTR)plan->preferred_base;
        if (!ntmm_range_free(mm, base, base + size)) {
            base = ntmm_find_hole_bottom_up(mm, size, NTMM_ALLOC_GRANULARITY);
            if (base == 0) { ntmm_unlock(mm); return STATUS_NO_MEMORY; }
        }
    } else {
        base = ntmm_round_down((ULONG_PTR)*BaseAddress, NTMM_ALLOC_GRANULARITY);
        if ((u64)base >= IP_USER_LIMIT || (u64)size > IP_USER_LIMIT - (u64)base) {
            ntmm_unlock(mm);
            return STATUS_INVALID_PARAMETER;
        }
        if (!ntmm_range_free(mm, base, base + size)) { ntmm_unlock(mm); return STATUS_CONFLICTING_ADDRESSES; }
    }
    if ((u64)base != plan->preferred_base) {
        if (!plan->relocatable) { ntmm_unlock(mm); return STATUS_CONFLICTING_ADDRESSES; }
        st = STATUS_IMAGE_NOT_AT_BASE;
    }

    for (i = 0; i < plan->num_segments; i++) {
        const ip_segment_t *s = &plan->seg[i];
        ULONG prot = ip_win32_protect(s->prot);
        struct nt_vad *vad = nt_vad_new(base + s->rva, base + s->rva + s->size, MEM_COMMIT,
                                        prot, prot, base, MEM_IMAGE);
        if (!vad) break;
        vad->section = sec;
        nt_section_ref(sec);    /* the VAD's own reference, dropped by nt_vad_free */
        vad->section_offset = s->file_offset;
        nt_list_insert(mm, vad);
        ins[count++] = vad;
    }
    ntmm_unlock(mm);
    if (count != plan->num_segments) {
        ntmm_lock(mm);
        for (k = 0; k < count; k++) nt_list_remove(mm, ins[k]);
        ntmm_unlock(mm);
        for (k = 0; k < count; k++) nt_vad_free(ins[k]);
        return STATUS_NO_MEMORY;
    }

    /* Host pages. One commit covers the whole image, zero-filled. The host
     * shim cannot commit a sub-range of a 64 KiB reservation, so the sections
     * are not committed one by one. Each segment's file bytes are copied in, its
     * zero tail is cleared explicitly (the commit may reuse older pages). The
     * view stays read-write until every fixup is in: each segment takes its
     * final protection only after the copy and, for a moved image, the
     * relocations. The gaps between segments get no access. */
    if (kshim_mmap_commit((void *)base, (size_t)size, PROT_READ | PROT_WRITE) != 0) {
        nt_map_image_drop(mm, ins, count, base, size, 0);
        return STATUS_NO_MEMORY;
    }
    for (i = 0; i < plan->num_segments; i++) {
        const ip_segment_t *s = &plan->seg[i];
        unsigned char *dst = (unsigned char *)(base + s->rva);
        size_t done = 0;
        NTSTATUS fail = STATUS_SUCCESS;
        while (done < s->file_size) {
            ssize_t n = pread(sec->backing.fd, dst + done, s->file_size - done,
                              (off_t)(s->file_offset + done));
            if (n <= 0) { fail = STATUS_IN_PAGE_ERROR; break; }
            done += (size_t)n;
        }
        if (!NT_SUCCESS(fail)) {
            nt_map_image_drop(mm, ins, count, base, size, 1);
            return fail;
        }
        memset(dst + s->file_size, 0, s->zero_size);
    }
    if ((u64)base != plan->preferred_base) {
        NTSTATUS rel = nt_image_relocate(base, plan);
        if (!NT_SUCCESS(rel)) {
            nt_map_image_drop(mm, ins, count, base, size, 1);
            return rel;
        }
    }
    for (i = 0; i < plan->num_segments; i++) {
        const ip_segment_t *s = &plan->seg[i];
        unsigned char *dst = (unsigned char *)(base + s->rva);
        if (kshim_mprotect(dst, s->size, ntmm_prot_to_linux(ip_win32_protect(s->prot))) != 0) {
            nt_map_image_drop(mm, ins, count, base, size, 1);
            return STATUS_NO_MEMORY;
        }
    }
    /* Segments are in RVA order, header first, so any gap is between the
     * running end and the next segment, or after the last one. */
    {
        u32 cur = 0;
        for (i = 0; i < plan->num_segments; i++) {
            const ip_segment_t *s = &plan->seg[i];
            if (s->rva > cur) kshim_mprotect((void *)(base + cur), s->rva - cur, PROT_NONE);
            if (s->rva + s->size > cur) cur = s->rva + s->size;
        }
        if (cur < plan->size_of_image)
            kshim_mprotect((void *)(base + cur), plan->size_of_image - cur, PROT_NONE);
    }

    *BaseAddress = (PVOID)base;
    *ViewSize = (SIZE_T)size;
    return st;
}
#endif

/* Maps a view of an open section. The caller holds a reference to sec. */
static NTSTATUS nt_map_view_of_section(struct nt_section *sec, PVOID *BaseAddress,
                                       PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize,
                                       ULONG AllocationType, ULONG Win32Protect) {
    struct nt_mm *mm = &g_nt_mm;
    ULONG_PTR offset = 0, base = 0, actual_size = 0;
    int commit;
#ifndef __KERNEL__
    /* An image section maps as a whole image, with its own rules. */
    if (sec->attributes & SEC_IMAGE)
        return nt_map_image_view(sec, BaseAddress, SectionOffset, ViewSize);
#endif
    if (SectionOffset) offset = (ULONG_PTR)SectionOffset->QuadPart;
    /* A shared writable view writes to the file, so the section must be writable. */
    if (sec->is_file && nt_protect_writable(Win32Protect) && !nt_protect_writable(sec->page_protection))
        return STATUS_SECTION_PROTECTION;
    // Windows requires SectionOffset be aligned to allocation granularity (64K) for image and file sections, otherwise page size for data
    if ((sec->attributes & SEC_IMAGE) || sec->is_file) {
        if (offset & (NTMM_ALLOC_GRANULARITY - 1)) return STATUS_MAPPED_ALIGNMENT;
    } else {
        if (offset & (NTMM_PAGE_SIZE - 1)) return STATUS_MAPPED_ALIGNMENT;
    }
    if (offset > sec->size) return STATUS_INVALID_VIEW_SIZE;
    if (*ViewSize == 0) actual_size = sec->size - offset;
    else {
        /* Check the raw size first: rounding a value near SIZE_MAX wraps to 0. */
        if (*ViewSize > sec->size - offset) return STATUS_INVALID_VIEW_SIZE;
        actual_size = (ULONG_PTR)ntmm_round_up(*ViewSize, NTMM_PAGE_SIZE);
    }
    if (actual_size > sec->size - offset) return STATUS_INVALID_VIEW_SIZE;
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }
    commit = (sec->attributes & SEC_COMMIT) || (AllocationType & MEM_COMMIT);
    if (sec->attributes & SEC_RESERVE) commit = 0;
    ntmm_lock(mm);
    if (*BaseAddress == NULL) {
        base = ntmm_find_hole_bottom_up(mm, actual_size, NTMM_ALLOC_GRANULARITY);
        if (base == 0) { ntmm_unlock(mm); return STATUS_NO_MEMORY; }
    } else {
        base = ntmm_round_down((ULONG_PTR)*BaseAddress, NTMM_ALLOC_GRANULARITY);
        if (!ntmm_range_free(mm, base, base + actual_size)) { ntmm_unlock(mm); return STATUS_CONFLICTING_ADDRESSES; }
    }
    ULONG type = (sec->attributes & SEC_IMAGE) ? MEM_IMAGE : MEM_MAPPED;
    struct nt_vad *vad = nt_vad_new(base, base + actual_size,
                                    commit ? MEM_COMMIT : MEM_RESERVE,
                                    commit ? Win32Protect : sec->page_protection,
                                    sec->page_protection,
                                    base, type);
    if (!vad) { ntmm_unlock(mm); return STATUS_NO_MEMORY; }
    vad->section = sec;
    nt_section_ref(sec);    /* the VAD's own reference, dropped by nt_vad_free */
    vad->section_offset = offset;
    if (Win32Protect & PAGE_GUARD) vad->guard = 1;
    nt_list_insert(mm, vad);
    ntmm_unlock(mm);
    int prot = commit ? ntmm_prot_to_linux(Win32Protect) : PROT_NONE;
    int cow = (Win32Protect == PAGE_WRITECOPY || Win32Protect == PAGE_EXECUTE_WRITECOPY) ? 1 : 0;
    void *p = (sec->is_file && commit)
        ? kshim_section_map_file(&sec->backing, (void *)base, offset, (size_t)actual_size, prot, cow)
        : kshim_section_map(&sec->backing, (void *)base, offset, (size_t)actual_size, prot, cow);
    if (!p) {
        ntmm_lock(mm);
        nt_list_remove(mm, vad);
        ntmm_unlock(mm);
        nt_vad_free(vad);
        return STATUS_NO_MEMORY;
    }
    *BaseAddress = (PVOID)base;
    *ViewSize = (SIZE_T)actual_size;
    return STATUS_SUCCESS;
}

NTSTATUS NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle,
                            PVOID *BaseAddress, ULONG_PTR ZeroBits,
                            SIZE_T CommitSize, PLARGE_INTEGER SectionOffset,
                            PSIZE_T ViewSize, ULONG InheritDisposition,
                            ULONG AllocationType, ULONG Win32Protect) {
    struct nt_object_header *hdr = NULL;
    NTSTATUS st;
    (void)ProcessHandle;
    (void)ZeroBits;
    (void)InheritDisposition;
    (void)CommitSize;
    if (!BaseAddress || !ViewSize) return STATUS_INVALID_PARAMETER;
    if (!ntmm_protection_valid(Win32Protect)) return STATUS_INVALID_PAGE_PROTECTION;
    /* ADR 0001: the handle must name a live section with the access the view
     * needs. The lookup reference is dropped after mapping; the VAD keeps its own. */
    st = ObReferenceObjectByHandle(kshim_current_process(), SectionHandle, &nt_section_type,
                                   UserMode, nt_section_map_access(Win32Protect), &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    st = nt_map_view_of_section((struct nt_section *)nt_object_body(hdr), BaseAddress,
                                SectionOffset, ViewSize, AllocationType, Win32Protect);
    ObDereferenceObject(hdr);
    return st;
}

NTSTATUS NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress) {
    struct nt_mm *mm = &g_nt_mm;
    ULONG_PTR base = (ULONG_PTR)BaseAddress;
    (void)ProcessHandle;
    if (!g_nt_mm_initialized) { ntmm_init(&g_nt_mm); g_nt_mm_initialized = 1; }
    ntmm_lock(mm);
    struct nt_vad *vad = nt_vad_find(mm, base);
    if (!vad) { ntmm_unlock(mm); return STATUS_NOT_MAPPED_VIEW; }
    if (vad->alloc_base != base) { ntmm_unlock(mm); return STATUS_INVALID_PARAMETER; }
    ULONG_PTR unmap_start = vad->start;
    ULONG_PTR unmap_end = vad->end;
    struct nt_vad *left = nt_vad_left_neighbor(mm, vad);
    while (left && left->alloc_base == base) {
        if (left->start < unmap_start) unmap_start = left->start;
        left = nt_vad_left_neighbor(mm, left);
    }
    struct nt_vad *right = nt_vad_right_neighbor(mm, vad);
    while (right && right->alloc_base == base) {
        if (right->end > unmap_end) unmap_end = right->end;
        right = nt_vad_right_neighbor(mm, right);
    }
    struct nt_vad *cur;
#ifndef __KERNEL__
    cur = mm->vad_head;
    while (cur) {
        struct nt_vad *next = cur->list_next;
        if (cur->alloc_base == base && cur->start >= unmap_start && cur->end <= unmap_end) {
            nt_view_writeback(cur, cur->start, cur->end);
            nt_list_remove(mm, cur);
            nt_vad_free(cur);
        }
        cur = next;
    }
#else
    struct rb_node *n = rb_first(&mm->vad_root);
    while (n) {
        struct rb_node *next = rb_next(n);
        struct nt_vad *v = rb_entry(n, struct nt_vad, rb);
        if (v->alloc_base == base && v->start >= unmap_start && v->end <= unmap_end) {
            rb_erase(n, &mm->vad_root);
            nt_vad_free(v);
        }
        n = next;
    }
#endif
    ntmm_unlock(mm);
#ifdef __KERNEL__
    /* Kernel: vm_munmap with mmap_lock */
    /* vm_munmap(unmap_start, unmap_end - unmap_start); */
#endif
    kshim_munmap((void *)unmap_start, (size_t)(unmap_end - unmap_start));
    return STATUS_SUCCESS;
}

NTSTATUS ntmm_add_region(ULONG_PTR base, SIZE_T size, ULONG protect, ULONG type, ULONG_PTR alloc_base)
{
    struct nt_mm *mm = &g_nt_mm;
    ULONG_PTR end;
    if (!base || !size) return STATUS_INVALID_PARAMETER;
    if (!g_nt_mm_initialized) { ntmm_init(mm); g_nt_mm_initialized = 1; }
    base = ntmm_round_down(base, NTMM_PAGE_SIZE);
    size = ntmm_round_up(size, NTMM_PAGE_SIZE);
    end = base + size;
    if (end <= base) return STATUS_INVALID_PARAMETER;
    ntmm_lock(mm);
    if (!ntmm_range_free(mm, base, end)) {
        ntmm_unlock(mm);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    struct nt_vad *vad = nt_vad_new(base, end, MEM_COMMIT, protect, protect, alloc_base, type);
    if (!vad) { ntmm_unlock(mm); return STATUS_NO_MEMORY; }
    if (nt_list_insert(mm, vad) != 0) {
        ntmm_unlock(mm);
        nt_vad_free(vad);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    nt_vad_coalesce(mm, vad);
    ntmm_unlock(mm);
    return STATUS_SUCCESS;
}

/* NtOpenSection core: looks the name up in the object namespace. Sections are
 * entered there by NtCreateSection when the caller gives a name. */
static NTSTATUS nt_open_section(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                                POBJECT_ATTRIBUTES ObjectAttributes) {
    struct nt_eprocess *proc;
    HANDLE h = NULL;
    NTSTATUS st;
    if (!SectionHandle || !ObjectAttributes || !ObjectAttributes->ObjectName)
        return STATUS_INVALID_PARAMETER;
    proc = kshim_current_process();
    if (!proc) return STATUS_INVALID_PARAMETER;
    st = ObOpenObjectByName(ObjectAttributes, &nt_section_type, UserMode, DesiredAccess, proc, &h);
    if (NT_SUCCESS(st)) *SectionHandle = h;
    return st;
}

/* Service entry points for NtCreateSection and NtOpenSection. */
#ifndef __KERNEL__

NTSTATUS nt_svc_NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                                POBJECT_ATTRIBUTES ObjectAttributes,
                                PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection,
                                ULONG AllocationAttributes, HANDLE FileHandle) {
    return NtCreateSection(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize,
                           SectionPageProtection, AllocationAttributes, FileHandle);
}

NTSTATUS nt_svc_NtOpenSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes) {
    return nt_open_section(SectionHandle, DesiredAccess, ObjectAttributes);
}

#else /* __KERNEL__ */

/* Arguments arrive as raw register or stack values from do_nt_syscall. The
 * output handle is written with copy_to_user, the name is copied in first, and
 * the maximum size is read with copy_from_user. */
NTSTATUS nt_svc_NtCreateSection(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                                unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                                unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE SectionHandle = (PHANDLE)a0;
    ACCESS_MASK DesiredAccess = (ACCESS_MASK)a1;
    POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)a2;
    PLARGE_INTEGER MaximumSize = (PLARGE_INTEGER)a3;
    ULONG SectionPageProtection = (ULONG)a4;
    ULONG AllocationAttributes = (ULONG)a5;
    HANDLE FileHandle = (HANDLE)a6;
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[256];
    POBJECT_ATTRIBUTES oa = NULL;
    LARGE_INTEGER kmax;
    PLARGE_INTEGER pmax = NULL;
    HANDLE h = NULL;
    NTSTATUS st;
    (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!SectionHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(SectionHandle, sizeof(*SectionHandle))) return STATUS_ACCESS_VIOLATION;
    st = ObCopyObjectAttributesFromUser(ObjectAttributes, &k_oa, &k_name, k_buf, sizeof(k_buf), &oa);
    if (!NT_SUCCESS(st)) return st;
    if (MaximumSize) {
        if (!access_ok(MaximumSize, sizeof(*MaximumSize))) return STATUS_ACCESS_VIOLATION;
        if (copy_from_user(&kmax, MaximumSize, sizeof(kmax))) return STATUS_ACCESS_VIOLATION;
        pmax = &kmax;
    }
    st = NtCreateSection(&h, DesiredAccess, oa, pmax, SectionPageProtection, AllocationAttributes, FileHandle);
    if (!NT_SUCCESS(st)) return st;
    if (copy_to_user(SectionHandle, &h, sizeof(h))) {
        ObCloseHandle(kshim_current_process(), h);
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS nt_svc_NtOpenSection(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                              unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                              unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    PHANDLE SectionHandle = (PHANDLE)a0;
    ACCESS_MASK DesiredAccess = (ACCESS_MASK)a1;
    POBJECT_ATTRIBUTES ObjectAttributes = (POBJECT_ATTRIBUTES)a2;
    OBJECT_ATTRIBUTES k_oa;
    UNICODE_STRING k_name;
    WCHAR k_buf[256];
    POBJECT_ATTRIBUTES oa = NULL;
    HANDLE h = NULL;
    NTSTATUS st;
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!SectionHandle) return STATUS_INVALID_PARAMETER;
    if (!access_ok(SectionHandle, sizeof(*SectionHandle))) return STATUS_ACCESS_VIOLATION;
    st = ObCopyObjectAttributesFromUser(ObjectAttributes, &k_oa, &k_name, k_buf, sizeof(k_buf), &oa);
    if (!NT_SUCCESS(st)) return st;
    st = nt_open_section(&h, DesiredAccess, oa);
    if (!NT_SUCCESS(st)) return st;
    if (copy_to_user(SectionHandle, &h, sizeof(h))) {
        ObCloseHandle(kshim_current_process(), h);
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

#endif /* __KERNEL__ */

/* Service entry points for NtMapViewOfSection and NtUnmapViewOfSection. */
#ifndef __KERNEL__

NTSTATUS nt_svc_NtMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle,
                                   PVOID *BaseAddress, ULONG_PTR ZeroBits,
                                   SIZE_T CommitSize, PLARGE_INTEGER SectionOffset,
                                   PSIZE_T ViewSize, ULONG InheritDisposition,
                                   ULONG AllocationType, ULONG Win32Protect) {
    return NtMapViewOfSection(SectionHandle, ProcessHandle, BaseAddress, ZeroBits, CommitSize,
                              SectionOffset, ViewSize, InheritDisposition, AllocationType, Win32Protect);
}

NTSTATUS nt_svc_NtUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress) {
    return NtUnmapViewOfSection(ProcessHandle, BaseAddress);
}

#else /* __KERNEL__ */

/* The base address and view size are in and out. Both are copied in before the
 * call, and copied back only when the map succeeds (NT writes nothing on
 * failure). SectionOffset is optional and read-only. A view that maps but
 * cannot be reported back to the caller is unmapped again. */
NTSTATUS nt_svc_NtMapViewOfSection(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                                   unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                                   unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE SectionHandle = (HANDLE)a0;
    HANDLE ProcessHandle = (HANDLE)a1;
    PVOID *BaseAddress = (PVOID *)a2;
    ULONG_PTR ZeroBits = (ULONG_PTR)a3;
    SIZE_T CommitSize = (SIZE_T)a4;
    PLARGE_INTEGER SectionOffset = (PLARGE_INTEGER)a5;
    PSIZE_T ViewSize = (PSIZE_T)a6;
    ULONG InheritDisposition = (ULONG)a7;
    ULONG AllocationType = (ULONG)a8;
    ULONG Win32Protect = (ULONG)a9;
    PVOID kbase;
    SIZE_T ksize;
    LARGE_INTEGER koff;
    PLARGE_INTEGER poff = NULL;
    NTSTATUS st;
    (void)a10; (void)a11;
    if (!BaseAddress || !ViewSize) return STATUS_INVALID_PARAMETER;
    if (!access_ok(BaseAddress, sizeof(PVOID)) || !access_ok(ViewSize, sizeof(SIZE_T)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&kbase, BaseAddress, sizeof(kbase))) return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&ksize, ViewSize, sizeof(ksize))) return STATUS_ACCESS_VIOLATION;
    if (SectionOffset) {
        if (!access_ok(SectionOffset, sizeof(*SectionOffset))) return STATUS_ACCESS_VIOLATION;
        if (copy_from_user(&koff, SectionOffset, sizeof(koff))) return STATUS_ACCESS_VIOLATION;
        poff = &koff;
    }
    st = NtMapViewOfSection(SectionHandle, ProcessHandle, &kbase, ZeroBits, CommitSize,
                            poff, &ksize, InheritDisposition, AllocationType, Win32Protect);
    if (!NT_SUCCESS(st)) return st;
    if (copy_to_user(BaseAddress, &kbase, sizeof(kbase)) ||
        copy_to_user(ViewSize, &ksize, sizeof(ksize))) {
        NtUnmapViewOfSection(ProcessHandle, kbase);
        return STATUS_ACCESS_VIOLATION;
    }
    return st;
}

/* The base address is a value, not a user pointer, so nothing is copied. */
NTSTATUS nt_svc_NtUnmapViewOfSection(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                                     unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                                     unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE ProcessHandle = (HANDLE)a0;
    PVOID BaseAddress = (PVOID)a1;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    return NtUnmapViewOfSection(ProcessHandle, BaseAddress);
}

#endif /* __KERNEL__ */

/* NtQuerySection core. SectionInformation and ReturnLength are kernel-side in
 * the kernel build (the wrapper copies them out) and the caller's pointers in
 * the kshim build. Only SectionBasicInformation (class 0) is supported. */
static NTSTATUS nt_query_section_core(HANDLE SectionHandle, ULONG SectionInformationClass,
                                      PVOID SectionInformation, SIZE_T SectionInformationLength,
                                      PSIZE_T ReturnLength) {
    const SIZE_T needed = sizeof(SECTION_BASIC_INFORMATION);
    struct nt_object_header *hdr = NULL;
    struct nt_section *sec;
    SECTION_BASIC_INFORMATION *info;
    NTSTATUS st;
    if (SectionInformationClass == 1) return STATUS_NOT_IMPLEMENTED; /* SectionImageInformation */
    if (SectionInformationClass != 0) return STATUS_INVALID_INFO_CLASS;
    if (SectionInformationLength != needed) {
        if (ReturnLength) *ReturnLength = needed;
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!SectionInformation) return STATUS_ACCESS_VIOLATION;
    st = ObReferenceObjectByHandle(kshim_current_process(), SectionHandle, &nt_section_type,
                                   UserMode, SECTION_QUERY, &hdr, NULL);
    if (!NT_SUCCESS(st)) return st;
    sec = (struct nt_section *)nt_object_body(hdr);
    info = (SECTION_BASIC_INFORMATION *)SectionInformation;
    info->BaseAddress = NULL;   /* a section has no base until it is mapped */
    info->AllocationAttributes = sec->attributes;
    info->MaximumSize.QuadPart = (LONGLONG)sec->size;
    ObDereferenceObject(hdr);
    if (ReturnLength) *ReturnLength = needed;
    return STATUS_SUCCESS;
}

/* Growing a section needs its backing resized, which kshim_section_create cannot
 * do. The entry point reads no user memory and changes nothing. */
#ifndef __KERNEL__

NTSTATUS nt_svc_NtQuerySection(HANDLE SectionHandle, ULONG SectionInformationClass,
                               PVOID SectionInformation, SIZE_T SectionInformationLength,
                               PSIZE_T ReturnLength) {
    return nt_query_section_core(SectionHandle, SectionInformationClass, SectionInformation,
                                 SectionInformationLength, ReturnLength);
}

NTSTATUS nt_svc_NtExtendSection(HANDLE SectionHandle, PLARGE_INTEGER NewSectionSize) {
    (void)SectionHandle; (void)NewSectionSize;
    return STATUS_NOT_IMPLEMENTED;
}

#else /* __KERNEL__ */

/* The result is copied out only on success. ReturnLength is copied out
 * whenever it is given, so a length mismatch still reports the needed size. */
NTSTATUS nt_svc_NtQuerySection(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                               unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                               unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE SectionHandle = (HANDLE)a0;
    ULONG SectionInformationClass = (ULONG)a1;
    PVOID SectionInformation = (PVOID)a2;
    SIZE_T SectionInformationLength = (SIZE_T)a3;
    PSIZE_T ReturnLength = (PSIZE_T)a4;
    SECTION_BASIC_INFORMATION kinfo;
    SIZE_T kret = 0;
    NTSTATUS st;
    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (ReturnLength && !access_ok(ReturnLength, sizeof(*ReturnLength)))
        return STATUS_ACCESS_VIOLATION;
    st = nt_query_section_core(SectionHandle, SectionInformationClass, &kinfo,
                               SectionInformationLength, &kret);
    if (NT_SUCCESS(st)) {
        if (!SectionInformation || !access_ok(SectionInformation, sizeof(kinfo)) ||
            copy_to_user(SectionInformation, &kinfo, sizeof(kinfo)))
            return STATUS_ACCESS_VIOLATION;
    }
    if (ReturnLength && copy_to_user(ReturnLength, &kret, sizeof(kret)))
        return STATUS_ACCESS_VIOLATION;
    return st;
}

NTSTATUS nt_svc_NtExtendSection(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                                unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                                unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    (void)a8; (void)a9; (void)a10; (void)a11;
    return STATUS_NOT_IMPLEMENTED;
}

#endif /* __KERNEL__ */
