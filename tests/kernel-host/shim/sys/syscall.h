/* sys/syscall.h - host (UCRT) stub. Linux syscall numbers are not used by
 * the host build; code paths that need them are guarded by __linux__ and
 * are not compiled for Windows. Provided only so proc_thread.c parses.
 */
#ifndef KHOST_SYS_SYSCALL_H
#define KHOST_SYS_SYSCALL_H
#endif
