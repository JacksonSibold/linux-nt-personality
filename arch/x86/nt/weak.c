// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include "nt_internal.h"
struct nt_thread;
#include "sync.h"

void __weak nt_handle_table_destroy(struct task_struct *tsk)
{
    (void)tsk;
}

void __weak nt_abandon_owned_mutants(struct nt_thread *thr)
{
    (void)thr;
}
