/* linux/init.h - syntax-check stub. Initcall registration macros. */
#ifndef SYNTAX_SHIM_LINUX_INIT_H
#define SYNTAX_SHIM_LINUX_INIT_H

#include <linux/compiler.h>

/*
 * Mainline registers the function pointer in an initcall section. Here it
 * is an unused static pointer of the same type, which is enough to
 * type-check the initcall function.
 */
#define core_initcall(fn) \
	static int (*__initcall_##fn)(void) __maybe_unused = (fn)

#endif
