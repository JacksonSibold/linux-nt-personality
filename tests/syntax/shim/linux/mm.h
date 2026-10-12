/*
 * linux/mm.h - syntax-check stub. Memory-management types and helpers used
 * by the NT VM manager and the PE loader. Guarded where src/kernel/fs/kshim.h
 * defines the same names (KSHIM_H).
 */
#ifndef SYNTAX_SHIM_LINUX_MM_H
#define SYNTAX_SHIM_LINUX_MM_H

#include <stddef.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/mman.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/err.h>
#include <linux/mmap_lock.h>
#include <asm/mman.h>

struct file;
struct vm_operations_struct;
struct vma_iterator;

#ifndef KSHIM_H
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & PAGE_MASK)

struct mm_struct {
	unsigned long start_code, end_code;
	unsigned long start_data, end_data;
	unsigned long start_brk, brk;
	unsigned long start_stack;
};

struct vm_area_struct {
	unsigned long vm_start;
	unsigned long vm_end;
	unsigned long vm_flags;
	const struct vm_operations_struct *vm_ops;
};
#endif /* KSHIM_H */

/* Maple-tree VMA iterator API (mainline 6.1+). */
void vma_iter_init(struct vma_iterator *vmi, struct mm_struct *mm, unsigned long addr);
struct vm_area_struct *vma_find(struct vma_iterator *vmi, unsigned long max);
struct vm_area_struct *vma_prev(struct vma_iterator *vmi);

unsigned long vm_mmap(struct file *file, unsigned long addr, unsigned long len,
		      unsigned long prot, unsigned long flag, unsigned long offset);
int vm_munmap(unsigned long start, size_t len);
unsigned long randomize_stack_top(unsigned long stack_top);

#endif
