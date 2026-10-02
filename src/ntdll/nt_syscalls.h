/*
 * nt_syscalls.h
 *
 * Auto-generated prototypes for the frozen NT syscall table.
 * Include syscall_table.def to expand the X-macro.
 */

#ifndef NT_SYSCALLS_H
#define NT_SYSCALLS_H

#include "nt_types.h"
#include "ntstatus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Service count and max SSN for dispatch table sizing. */
#define NT_SYSCALL_COUNT 171
#define NT_SYSCALL_MAX_SSN 0x01db

#define NT_SYSCALL(Name, Ssn, Arity, ...) \
    __attribute__((naked)) NTSTATUS Name(__VA_ARGS__);

#include "syscall_table.def"

#undef NT_SYSCALL

#ifdef __cplusplus
}
#endif

#endif /* NT_SYSCALLS_H */
