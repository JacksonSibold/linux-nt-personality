/*
 * linux/types.h - syntax-check stub (not a runtime model).
 * Basic kernel integer typedefs. Each name is guarded so the userspace
 * kshim.h macros (u8..s64) can be in the same translation unit.
 */
#ifndef SYNTAX_SHIM_LINUX_TYPES_H
#define SYNTAX_SHIM_LINUX_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <linux/compiler.h>

#ifndef u8
typedef uint8_t u8;
#endif
#ifndef u16
typedef uint16_t u16;
#endif
#ifndef u32
typedef uint32_t u32;
#endif
#ifndef u64
typedef uint64_t u64;
#endif
#ifndef s8
typedef int8_t s8;
#endif
#ifndef s16
typedef int16_t s16;
#endif
#ifndef s32
typedef int32_t s32;
#endif
#ifndef s64
typedef int64_t s64;
#endif

/*
 * gfp_t is int here, not the kernel's unsigned int: the userspace model in
 * src/kernel/nt/kshim.h and src/kernel/fs/kshim.h defines kmalloc() and
 * friends with an int flags argument, and both models are included in the
 * same translation units as these declarations.
 */
typedef int gfp_t;
typedef unsigned short umode_t;
/* loff_t is also defined by kernel_compat.h; identical redefinition is valid C11. */
typedef long long loff_t;

#endif
