/* linux/bug.h - syntax-check stub. Assertion helpers. */
#ifndef SYNTAX_SHIM_LINUX_BUG_H
#define SYNTAX_SHIM_LINUX_BUG_H

#include <linux/compiler.h>

#ifndef KSHIM_H
#ifndef BUG_ON
#define BUG_ON(x) ((void)(x))
#endif
#ifndef WARN_ON
#define WARN_ON(x) ((x) ? 1 : 0)
#endif
#endif /* KSHIM_H */

#ifndef WARN_ON_ONCE
#define WARN_ON_ONCE(x) WARN_ON(x)
#endif
#ifndef BUG
#define BUG() __builtin_trap()
#endif

#endif
