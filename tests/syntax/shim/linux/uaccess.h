/* linux/uaccess.h - syntax-check stub. User-space access helpers. */
#ifndef SYNTAX_SHIM_LINUX_UACCESS_H
#define SYNTAX_SHIM_LINUX_UACCESS_H

#include <stddef.h>
#include <linux/compiler.h>
#include <linux/types.h>

#ifndef KSHIM_H
#ifndef access_ok
#define access_ok(addr, size) (1)
#endif
#endif

#ifndef copy_from_user
unsigned long copy_from_user(void *to, const void __user *from, unsigned long n);
#endif
#ifndef copy_to_user
unsigned long copy_to_user(void __user *to, const void *from, unsigned long n);
#endif
#ifndef KSHIM_H
unsigned long clear_user(void __user *to, unsigned long n);
#endif
#ifndef __get_user
#define __get_user(x, ptr) ({ (x) = *(ptr); 0; })
#endif
#ifndef get_user
#define get_user(x, ptr) ({ (x) = *(ptr); 0; })
#endif
#ifndef put_user
#define put_user(x, ptr) ({ *(ptr) = (x); 0; })
#endif

#endif
