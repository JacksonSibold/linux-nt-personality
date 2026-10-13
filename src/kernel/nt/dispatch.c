// SPDX-License-Identifier: GPL-2.0
/*
 * ntpers - NT system-call dispatch
 * Per-CPU counters for DOD-ARCH-B001: exact, cheap, debugfs-exposed.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/thread_info.h>
#include <linux/uaccess.h>
#include <linux/nospec.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/init.h>

#include "nt_internal.h"
#include "sync.h"
#include "object.h"
extern void nt_thread_check_suspend(void);

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002L)
#endif
#ifndef STATUS_INVALID_SYSTEM_SERVICE
#define STATUS_INVALID_SYSTEM_SERVICE ((NTSTATUS)0xC000001BL)
#endif
#ifndef STATUS_ACCESS_VIOLATION
#define STATUS_ACCESS_VIOLATION ((NTSTATUS)0xC0000005L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017L)
#endif

/* Per-CPU counters - essentially free via this_cpu_inc */
DEFINE_PER_CPU(u64, nt_dispatch_count);
DEFINE_PER_CPU(u64, nt_host_syscall_count);
DEFINE_PER_CPU(u64, nt_dispatch_per_ssn[NT_NR_SERVICES]);

/* Summed accessors for debugfs and tests */
u64 nt_sum_dispatch_count(void)
{
	u64 sum = 0;
	int cpu;
	for_each_possible_cpu(cpu)
		sum += per_cpu(nt_dispatch_count, cpu);
	return sum;
}

u64 nt_sum_host_count(void)
{
	u64 sum = 0;
	int cpu;
	for_each_possible_cpu(cpu)
		sum += per_cpu(nt_host_syscall_count, cpu);
	return sum;
}

/* Called from host syscall path for NT tasks */
void nt_host_syscall_inc(void)
{
	this_cpu_inc(nt_host_syscall_count);
}

/* Weak declarations for all services - unimplemented will be NULL */
#define NT_SERVICE(ssn, svc_name, argc) extern NTSTATUS nt_svc_##svc_name() __attribute__((weak));
#include "nt_service_list.h"
#undef NT_SERVICE

#define NT_SERVICE(ssn, svc_name, nargs) [ssn] = { \
	.fn = (nt_service_fn)nt_svc_##svc_name, \
	.argc = (nargs), \
	.name = #svc_name, \
},
static const struct nt_service nt_service_table[NT_NR_SERVICES] = {
#include "nt_service_list.h"
};

#undef NT_SERVICE

static __always_inline bool nt_fetch_stack_args(struct pt_regs *regs,
						unsigned int argc,
						unsigned long *args)
{
	unsigned long __user *sp;
	unsigned int i;
	if (argc <= 4)
		return true;
	sp = (unsigned long __user *)(regs->sp + 0x28);
	if (!access_ok(sp, (argc - 4) * sizeof(unsigned long)))
		return false;
	for (i = 4; i < argc; i++) {
		if (__get_user(args[i], &sp[i - 4]))
			return false;
	}
	return true;
}

__visible noinstr void do_nt_syscall(struct pt_regs *regs, unsigned int ssn)
{
	const struct nt_service *svc;
	unsigned long args[NT_MAX_ARGS] = { };
	u32 status;
	instrumentation_begin();
	if (ssn == 0x53 || ssn == 0x1c8 || ssn == 0xc5 || ssn == 0x1c4 || ssn == 0x52 || ssn == 0x34) {
		pr_debug("do_nt_syscall: ssn=0x%x TIF_NTABI=%d pid=%d comm=%s\n", ssn, test_thread_flag(TIF_NTABI), current->pid, current->comm);
	}
	/* Exact per-CPU count - cheap, no printk, no sampling */
	this_cpu_inc(nt_dispatch_count);
	if (ssn < NT_NR_SERVICES)
		__this_cpu_add(nt_dispatch_per_ssn[ssn], 1);
	if (unlikely(ssn >= NT_NR_SERVICES)) {
		regs->ax = STATUS_INVALID_SYSTEM_SERVICE;
		goto out;
	}
	ssn = array_index_nospec(ssn, NT_NR_SERVICES);
	svc = &nt_service_table[ssn];
	if (unlikely(!svc->fn)) {
		nt_report_missing_service(ssn);
		regs->ax = STATUS_NOT_IMPLEMENTED;
		goto out;
	}
	args[0] = regs->r10;
	args[1] = regs->dx;
	args[2] = regs->r8;
	args[3] = regs->r9;
	if (unlikely(!nt_fetch_stack_args(regs, svc->argc, args))) {
		regs->ax = STATUS_ACCESS_VIOLATION;
		goto out;
	}
	status = svc->fn(args[0], args[1], args[2], args[3],
			 args[4], args[5], args[6], args[7],
			 args[8], args[9], args[10], args[11]);
	regs->ax = (unsigned long)status;
out:
	if (test_thread_flag(TIF_NTSUSPEND))
		nt_thread_check_suspend();
	instrumentation_end();
}

/* NT types from arch/ntdll/nt_types.h via sync.h */

extern NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
extern NTSTATUS NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
/* MEM_RELEASE as defined in mm.h (this file does not include it). */
#ifndef MEM_RELEASE
#define MEM_RELEASE 0x00008000UL
#endif
extern NTSTATUS NtProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG NewProtect, ULONG *OldProtect);
extern NTSTATUS NtQueryVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress, ULONG MemoryInformationClass, PVOID MemoryInformation, SIZE_T MemoryInformationLength, PSIZE_T ReturnLength);
extern NTSTATUS NtReadVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer, SIZE_T BufferSize, PSIZE_T NumberOfBytesRead);
extern NTSTATUS NtWriteVirtualMemory(HANDLE ProcessHandle, PVOID BaseAddress, PVOID Buffer, SIZE_T BufferSize, PSIZE_T NumberOfBytesWritten);
extern NTSTATUS NtFlushVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, PIO_STATUS_BLOCK IoStatus);
extern NTSTATUS NtAllocateVirtualMemoryEx(HANDLE ProcessHandle, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG AllocationType, ULONG PageProtection, PVOID ExtendedParameters, ULONG ExtendedParameterCount);

NTSTATUS nt_svc_NtAllocateVirtualMemory(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE ProcessHandle = (HANDLE)a0;
    PVOID __user *BaseAddress = (PVOID __user *)a1;
    ULONG_PTR ZeroBits = (ULONG_PTR)a2;
    PSIZE_T RegionSize = (PSIZE_T)a3;
    ULONG AllocationType = (ULONG)a4;
    ULONG Protect = (ULONG)a5;
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;
    if (!access_ok(BaseAddress, sizeof(PVOID)) || !access_ok(RegionSize, sizeof(SIZE_T)))
        return STATUS_ACCESS_VIOLATION;
    PVOID base;
    SIZE_T size;
    if (copy_from_user(&base, BaseAddress, sizeof(base)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&size, RegionSize, sizeof(size)))
        return STATUS_ACCESS_VIOLATION;
    PVOID kbase = base;
    SIZE_T ksize = size;
    NTSTATUS st = NtAllocateVirtualMemory(ProcessHandle, &kbase, ZeroBits, &ksize, AllocationType, Protect);
    if (copy_to_user(BaseAddress, &kbase, sizeof(kbase)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user(RegionSize, &ksize, sizeof(ksize)))
        return STATUS_ACCESS_VIOLATION;
    return st;
}

NTSTATUS nt_svc_NtFreeVirtualMemory(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE ProcessHandle = (HANDLE)a0;
    PVOID __user *BaseAddress = (PVOID __user *)a1;
    PSIZE_T RegionSize = (PSIZE_T)a2;
    ULONG FreeType = (ULONG)a3;
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;
    if (!access_ok(BaseAddress, sizeof(PVOID)) || !access_ok(RegionSize, sizeof(SIZE_T)))
        return STATUS_ACCESS_VIOLATION;
    PVOID base;
    SIZE_T size;
    if (copy_from_user(&base, BaseAddress, sizeof(base)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&size, RegionSize, sizeof(size)))
        return STATUS_ACCESS_VIOLATION;
    PVOID kbase = base;
    SIZE_T ksize = size;
    NTSTATUS st = NtFreeVirtualMemory(ProcessHandle, &kbase, &ksize, FreeType);
    if (copy_to_user(BaseAddress, &kbase, sizeof(kbase)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user(RegionSize, &ksize, sizeof(ksize)))
        return STATUS_ACCESS_VIOLATION;
    return st;
}

NTSTATUS nt_svc_NtProtectVirtualMemory(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE ProcessHandle = (HANDLE)a0;
    PVOID __user *BaseAddress = (PVOID __user *)a1;
    PSIZE_T RegionSize = (PSIZE_T)a2;
    ULONG NewProtect = (ULONG)a3;
    PULONG OldProtect = (PULONG)a4;
    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!BaseAddress || !RegionSize || !OldProtect)
        return STATUS_INVALID_PARAMETER;
    if (!access_ok(BaseAddress, sizeof(PVOID)) || !access_ok(RegionSize, sizeof(SIZE_T)) || !access_ok(OldProtect, sizeof(ULONG)))
        return STATUS_ACCESS_VIOLATION;
    PVOID base;
    SIZE_T size;
    if (copy_from_user(&base, BaseAddress, sizeof(base)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&size, RegionSize, sizeof(size)))
        return STATUS_ACCESS_VIOLATION;
    PVOID kbase = base;
    SIZE_T ksize = size;
    ULONG kold = 0;
    NTSTATUS st = NtProtectVirtualMemory(ProcessHandle, &kbase, &ksize, NewProtect, &kold);
    if (copy_to_user(BaseAddress, &kbase, sizeof(kbase)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user(RegionSize, &ksize, sizeof(ksize)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user(OldProtect, &kold, sizeof(kold)))
        return STATUS_ACCESS_VIOLATION;
    return st;
}

NTSTATUS nt_svc_NtQueryVirtualMemory(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE ProcessHandle = (HANDLE)a0;
    PVOID BaseAddress = (PVOID)a1;
    ULONG MemoryInformationClass = (ULONG)a2;
    PVOID MemoryInformation = (PVOID)a3;
    SIZE_T MemoryInformationLength = (SIZE_T)a4;
    PSIZE_T ReturnLength = (PSIZE_T)a5;
    (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!MemoryInformation)
        return STATUS_INVALID_PARAMETER;
    if (!access_ok(MemoryInformation, MemoryInformationLength))
        return STATUS_ACCESS_VIOLATION;
    if (ReturnLength && !access_ok(ReturnLength, sizeof(SIZE_T)))
        return STATUS_ACCESS_VIOLATION;
    PVOID kbuf = kzalloc(MemoryInformationLength, GFP_KERNEL);
    if (!kbuf)
        return STATUS_NO_MEMORY;
    SIZE_T kret = 0;
    NTSTATUS st = NtQueryVirtualMemory(ProcessHandle, BaseAddress, MemoryInformationClass, kbuf, MemoryInformationLength, &kret);
    if (st == 0) {
        if (copy_to_user(MemoryInformation, kbuf, MemoryInformationLength))
            st = STATUS_ACCESS_VIOLATION;
        else if (ReturnLength) {
            if (copy_to_user(ReturnLength, &kret, sizeof(kret)))
                st = STATUS_ACCESS_VIOLATION;
        }
    }
    kfree(kbuf);
    return st;
}

NTSTATUS nt_svc_NtReadVirtualMemory(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE ProcessHandle = (HANDLE)a0;
    PVOID BaseAddress = (PVOID)a1;
    PVOID Buffer = (PVOID)a2;
    SIZE_T BufferSize = (SIZE_T)a3;
    PSIZE_T NumberOfBytesRead = (PSIZE_T)a4;
    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (NumberOfBytesRead && !access_ok(NumberOfBytesRead, sizeof(SIZE_T)))
        return STATUS_ACCESS_VIOLATION;
    SIZE_T done = 0;
    NTSTATUS st = NtReadVirtualMemory(ProcessHandle, BaseAddress, Buffer, BufferSize, &done);
    if (st != STATUS_NOT_SUPPORTED && NumberOfBytesRead &&
        copy_to_user(NumberOfBytesRead, &done, sizeof(done)))
        return STATUS_ACCESS_VIOLATION;
    return st;
}

NTSTATUS nt_svc_NtWriteVirtualMemory(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE ProcessHandle = (HANDLE)a0;
    PVOID BaseAddress = (PVOID)a1;
    PVOID Buffer = (PVOID)a2;
    SIZE_T BufferSize = (SIZE_T)a3;
    PSIZE_T NumberOfBytesWritten = (PSIZE_T)a4;
    (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (NumberOfBytesWritten && !access_ok(NumberOfBytesWritten, sizeof(SIZE_T)))
        return STATUS_ACCESS_VIOLATION;
    SIZE_T done = 0;
    NTSTATUS st = NtWriteVirtualMemory(ProcessHandle, BaseAddress, Buffer, BufferSize, &done);
    if (st != STATUS_NOT_SUPPORTED && NumberOfBytesWritten &&
        copy_to_user(NumberOfBytesWritten, &done, sizeof(done)))
        return STATUS_ACCESS_VIOLATION;
    return st;
}

NTSTATUS nt_svc_NtFlushVirtualMemory(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE ProcessHandle = (HANDLE)a0;
    PVOID __user *BaseAddress = (PVOID __user *)a1;
    PSIZE_T RegionSize = (PSIZE_T)a2;
    PIO_STATUS_BLOCK IoStatus = (PIO_STATUS_BLOCK)a3;
    (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!BaseAddress || !RegionSize || !IoStatus)
        return STATUS_INVALID_PARAMETER;
    if (!access_ok(BaseAddress, sizeof(PVOID)) || !access_ok(RegionSize, sizeof(SIZE_T)) ||
        !access_ok(IoStatus, sizeof(IO_STATUS_BLOCK)))
        return STATUS_ACCESS_VIOLATION;
    PVOID base;
    SIZE_T size;
    if (copy_from_user(&base, BaseAddress, sizeof(base)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&size, RegionSize, sizeof(size)))
        return STATUS_ACCESS_VIOLATION;
    PVOID kbase = base;
    SIZE_T ksize = size;
    IO_STATUS_BLOCK kiosb = { 0 };
    NTSTATUS st = NtFlushVirtualMemory(ProcessHandle, &kbase, &ksize, &kiosb);
    if (st != STATUS_NOT_SUPPORTED) {
        if (copy_to_user(BaseAddress, &kbase, sizeof(kbase)))
            return STATUS_ACCESS_VIOLATION;
        if (copy_to_user(RegionSize, &ksize, sizeof(ksize)))
            return STATUS_ACCESS_VIOLATION;
        if (copy_to_user(IoStatus, &kiosb, sizeof(kiosb)))
            return STATUS_ACCESS_VIOLATION;
    }
    return st;
}

NTSTATUS nt_svc_NtAllocateVirtualMemoryEx(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7, unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11)
{
    HANDLE ProcessHandle = (HANDLE)a0;
    PVOID __user *BaseAddress = (PVOID __user *)a1;
    PSIZE_T RegionSize = (PSIZE_T)a2;
    ULONG AllocationType = (ULONG)a3;
    ULONG PageProtection = (ULONG)a4;
    PVOID ExtendedParameters = (PVOID)a5;
    ULONG ExtendedParameterCount = (ULONG)a6;
    (void)a7; (void)a8; (void)a9; (void)a10; (void)a11;
    if (!BaseAddress || !RegionSize)
        return STATUS_INVALID_PARAMETER;
    if (!access_ok(BaseAddress, sizeof(PVOID)) || !access_ok(RegionSize, sizeof(SIZE_T)))
        return STATUS_ACCESS_VIOLATION;
    PVOID base;
    SIZE_T size;
    if (copy_from_user(&base, BaseAddress, sizeof(base)))
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&size, RegionSize, sizeof(size)))
        return STATUS_ACCESS_VIOLATION;
    PVOID kbase = base;
    SIZE_T ksize = size;
    NTSTATUS st = NtAllocateVirtualMemoryEx(ProcessHandle, &kbase, &ksize, AllocationType,
                                            PageProtection, ExtendedParameters, ExtendedParameterCount);
    if (st != STATUS_NOT_SUPPORTED) {
        if (copy_to_user(BaseAddress, &kbase, sizeof(kbase)) ||
            copy_to_user(RegionSize, &ksize, sizeof(ksize))) {
            /* The caller cannot learn the address of a region it was given
             * no way to name, so give the region back. MEM_RELEASE takes a
             * zero size. */
            if (NT_SUCCESS(st)) {
                SIZE_T zero = 0;
                PVOID rel = kbase;
                (void)NtFreeVirtualMemory(ProcessHandle, &rel, &zero, MEM_RELEASE);
            }
            return STATUS_ACCESS_VIOLATION;
        }
    }
    return st;
}

void nt_enter_personality(struct task_struct *tsk)
{
	set_tsk_thread_flag(tsk, TIF_NTABI);
	{
		extern struct nt_ethread *nt_current_ethread(void);
		extern NTSTATUS nt_thread_create(struct nt_thread **);
		extern void nt_set_current_thread(struct nt_thread *);
		struct nt_ethread *ethr = nt_current_ethread();
		if (ethr && ethr->sync_thread) {
			nt_set_current_thread(ethr->sync_thread);
		} else {
			struct nt_thread *t;
			if (NT_SUCCESS(nt_thread_create(&t))) {
				if (ethr) ethr->sync_thread = t;
				nt_set_current_thread(t);
			}
		}
	}
}

void nt_exit_personality(struct task_struct *tsk)
{
	if (!test_tsk_thread_flag(tsk, TIF_NTABI))
		return;
	nt_handle_table_destroy(tsk);
	{
		struct nt_thread *thr = nt_current_thread();
		if (thr)
			nt_abandon_owned_mutants(thr);
		nt_set_current_thread(NULL);
	}
	clear_tsk_thread_flag(tsk, TIF_NTABI);
}

void nt_report_missing_service(unsigned int ssn)
{
	pr_info("nt: missing service %u\n", ssn);
}

bool nt_can_sysret(struct pt_regs *regs)
{
	return true;
}

/* Debugfs exposure - distinct from debugfs_tests.c's ntpers dir handling */
static int nt_dispatch_count_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%llu\n", (unsigned long long)nt_sum_dispatch_count());
	return 0;
}
static int nt_dispatch_count_open(struct inode *inode, struct file *file)
{
	return single_open(file, nt_dispatch_count_show, NULL);
}
static const struct file_operations nt_dispatch_count_fops = {
	.owner = THIS_MODULE,
	.open = nt_dispatch_count_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int nt_host_count_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%llu\n", (unsigned long long)nt_sum_host_count());
	return 0;
}
static int nt_host_count_open(struct inode *inode, struct file *file)
{
	return single_open(file, nt_host_count_show, NULL);
}
static const struct file_operations nt_host_count_fops = {
	.owner = THIS_MODULE,
	.open = nt_host_count_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int nt_per_ssn_show(struct seq_file *m, void *v)
{
	int i;
	for (i = 0; i < NT_NR_SERVICES; i++) {
		u64 sum = 0;
		int cpu;
		for_each_possible_cpu(cpu)
			sum += per_cpu(nt_dispatch_per_ssn[i], cpu);
		if (sum)
			seq_printf(m, "%d %s %llu\n", i, nt_service_table[i].name ? nt_service_table[i].name : "?", (unsigned long long)sum);
	}
	return 0;
}
static int nt_per_ssn_open(struct inode *inode, struct file *file)
{
	return single_open(file, nt_per_ssn_show, NULL);
}
static const struct file_operations nt_per_ssn_fops = {
	.owner = THIS_MODULE,
	.open = nt_per_ssn_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init nt_counters_debugfs_init(void)
{
	struct dentry *dir;
	dir = debugfs_lookup("ntpers", NULL);
	if (!dir)
		dir = debugfs_create_dir("ntpers", NULL);
	if (!dir)
		return 0;
	/* These will fail harmlessly if already created */
	debugfs_create_file("dispatch_count", 0444, dir, NULL, &nt_dispatch_count_fops);
	debugfs_create_file("host_count", 0444, dir, NULL, &nt_host_count_fops);
	debugfs_create_file("dispatch_per_ssn", 0444, dir, NULL, &nt_per_ssn_fops);
	return 0;
}
late_initcall(nt_counters_debugfs_init);
