/* linux/slab.h - syntax-check stub. Kernel memory allocation. */
#ifndef SYNTAX_SHIM_LINUX_SLAB_H
#define SYNTAX_SHIM_LINUX_SLAB_H

#include <stddef.h>
#include <linux/types.h>

#ifndef GFP_KERNEL
#define GFP_KERNEL 0
#endif
#ifndef GFP_ATOMIC
#define GFP_ATOMIC 0
#endif

#ifndef KSHIM_H
#ifndef NTOBJ_KSHIM_H
void *kmalloc(size_t size, gfp_t flags);
void *kzalloc(size_t size, gfp_t flags);
void kfree(const void *objp);
#endif
#endif

#endif
