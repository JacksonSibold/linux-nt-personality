/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NT_INTERNAL_H
#define NT_INTERNAL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#else
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t u32;
struct pt_regs; struct task_struct;
#endif

#define NT_NR_SERVICES 476  /* 0x1dc = max SSN 0x1db +1 */
#define NT_MAX_ARGS 12
#define NT_SYSCALL_SKIP ((long)-1)

/* NTSTATUS is u32 in dispatcher; keep unsigned */
#ifndef NTSTATUS_DEFINED
#define NTSTATUS_DEFINED
typedef int32_t NTSTATUS;
#endif

typedef u32 (*nt_service_fn)(unsigned long, unsigned long, unsigned long, unsigned long,
                             unsigned long, unsigned long, unsigned long, unsigned long,
                             unsigned long, unsigned long, unsigned long, unsigned long);

struct nt_service {
    nt_service_fn fn;
    unsigned int argc;
    const char *name;
};

/* core dispatch */
void do_nt_syscall(struct pt_regs *regs, unsigned int ssn);
void nt_enter_personality(struct task_struct *tsk);
void nt_exit_personality(struct task_struct *tsk);
void nt_handle_table_destroy(struct task_struct *tsk);
struct nt_thread;
void nt_abandon_owned_mutants(struct nt_thread *thr);
void nt_report_missing_service(unsigned int ssn);
long nt_syscall_enter(struct pt_regs *regs, unsigned int ssn);
bool nt_can_sysret(struct pt_regs *regs);
void nt_syscall_exit(struct pt_regs *regs);

/* Per-CPU counters (defined in dispatch.c) */
#ifdef __KERNEL__
#include <linux/percpu.h>
/* accessors */
u64 nt_sum_dispatch_count(void);
u64 nt_sum_host_count(void);
void nt_host_syscall_inc(void);
#endif

/* service prototypes - generated from src/ntdll/syscall_table.def */
NTSTATUS nt_svc_NtCreateEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtOpenEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtSetEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtResetEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtPulseEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtQueryEvent(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtCreateMutant(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtOpenMutant(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtReleaseMutant(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtQueryMutant(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtCreateSemaphore(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtOpenSemaphore(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtReleaseSemaphore(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtQuerySemaphore(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtQueryPerformanceCounter(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtWaitForSingleObject(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtWaitForMultipleObjects(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtClose(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtDuplicateObject(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtQueryObject(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);

/* helper for current eprocess */
struct nt_eprocess *nt_get_current_eprocess(void);

#endif /* NT_INTERNAL_H */
