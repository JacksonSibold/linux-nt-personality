/* linux/mmap_lock.h - syntax-check stub. mmap lock helpers. */
#ifndef SYNTAX_SHIM_LINUX_MMAP_LOCK_H
#define SYNTAX_SHIM_LINUX_MMAP_LOCK_H

#include <linux/types.h>

struct mm_struct;

void mmap_write_lock(struct mm_struct *mm);
int mmap_write_lock_killable(struct mm_struct *mm);
void mmap_write_unlock(struct mm_struct *mm);
void mmap_read_lock(struct mm_struct *mm);
void mmap_read_unlock(struct mm_struct *mm);

#endif
