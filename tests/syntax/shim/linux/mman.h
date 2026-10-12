/*
 * linux/mman.h - syntax-check stub. Protection and mapping flags use the
 * Linux uapi values (asm-generic/mman-common.h, mman.h). Guarded where
 * src/kernel/fs/kshim.h defines the same macros.
 */
#ifndef SYNTAX_SHIM_LINUX_MMAN_H
#define SYNTAX_SHIM_LINUX_MMAN_H

#include <linux/types.h>

#ifndef KSHIM_H
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_DENYWRITE 0x0800
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#endif
