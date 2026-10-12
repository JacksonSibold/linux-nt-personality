/* Minimal POSIX/Linux <sys/syscall.h> for syntax-checking on MinGW. */
#ifndef SYNTAX_SHIM_SYS_SYSCALL_H
#define SYNTAX_SHIM_SYS_SYSCALL_H
#define SYS_read        0
#define SYS_write       1
#define SYS_arch_prctl  158
long syscall(long number, ...);
#endif
