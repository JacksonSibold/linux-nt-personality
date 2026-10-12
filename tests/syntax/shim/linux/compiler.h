/*
 * linux/compiler.h - syntax-check stub (not a runtime model).
 * Annotation and attribute macros used by the kernel-facing sources.
 * Values match the mainline kernel where it matters for type checking
 * (e.g. __weak is the weak attribute); address-space and section
 * annotations are empty because nothing is linked or run.
 */
#ifndef SYNTAX_SHIM_LINUX_COMPILER_H
#define SYNTAX_SHIM_LINUX_COMPILER_H

#define __user
#define __kernel
#define __force
#define __iomem
#define __visible
#define __init
#define __exit
#define __initdata
#define __maybe_unused __attribute__((__unused__))
#define __always_inline inline __attribute__((__always_inline__))
#define __always_unused __attribute__((__unused__))
#define __weak __attribute__((__weak__))
#define __noinline __attribute__((__noinline__))
#define noinstr
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#endif
