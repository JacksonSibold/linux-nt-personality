/* drvhost_libc.h - the C library names the driver host uses, per build.
 *
 * Host: the hosted C library. Kernel (__KERNEL__): linux/ headers, with the
 * few names the kernel does not provide mapped to kernel equivalents. The
 * driver files include this header instead of the hosted headers directly.
 */
#ifndef DRVHOST_LIBC_H
#define DRVHOST_LIBC_H

#ifdef __KERNEL__
#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

/* strcasecmp is not in the kernel; the driver host needs it for the
 * ASCII-only comparisons in IO and service names. */
static inline int dh_strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
}
#define strcasecmp(a, b) dh_strcasecmp((a), (b))
#define sched_yield() cond_resched()
/* getpid in the host model is the process id of the caller. */
#define getpid() ((int)current->tgid)
#else
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

#endif /* DRVHOST_LIBC_H */
