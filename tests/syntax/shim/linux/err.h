/* linux/err.h - syntax-check stub. Error-pointer encoding. */
#ifndef SYNTAX_SHIM_LINUX_ERR_H
#define SYNTAX_SHIM_LINUX_ERR_H

#include <errno.h>
#include <linux/compiler.h>

#define MAX_ERRNO 4095
#ifndef IS_ERR_VALUE
#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
#endif
#define IS_ERR(ptr) IS_ERR_VALUE((unsigned long)(ptr))
#define PTR_ERR(ptr) ((long)(ptr))
#define ERR_PTR(err) ((void *)(long)(err))

#endif
