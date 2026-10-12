/*
 * linux/thread_info.h - syntax-check stub.
 * TIF_NTABI and TIF_NTSUSPEND are not defined anywhere in this repository
 * (they belong to the ntpers kernel patch). They are placeholders here so
 * that the call sites type-check; the numeric values are not meaningful.
 */
#ifndef SYNTAX_SHIM_LINUX_THREAD_INFO_H
#define SYNTAX_SHIM_LINUX_THREAD_INFO_H

#include <linux/types.h>

struct task_struct;
struct task_struct *get_current(void);

#define TIF_NTABI 32
#define TIF_NTSUSPEND 33

int test_tsk_thread_flag(struct task_struct *tsk, int flag);
void set_tsk_thread_flag(struct task_struct *tsk, int flag);
void clear_tsk_thread_flag(struct task_struct *tsk, int flag);
#define test_thread_flag(flag) test_tsk_thread_flag(get_current(), (flag))

#endif
