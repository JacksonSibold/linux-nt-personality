/* linux/nospec.h - syntax-check stub. Speculation barrier helper. */
#ifndef SYNTAX_SHIM_LINUX_NOSPEC_H
#define SYNTAX_SHIM_LINUX_NOSPEC_H

#include <linux/types.h>

/* Mainline clamps index to [0, size). The check only needs the call type. */
#define array_index_nospec(index, size) ((index) < (size) ? (index) : 0)

#endif
