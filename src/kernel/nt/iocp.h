/* SPDX-License-Identifier: GPL-2.0 */
/*
 * iocp.h - I/O completion ports and user APCs (kernel/nt workstream).
 */

#ifndef NTIO_IOCP_H
#define NTIO_IOCP_H

#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"
#ifdef __KERNEL__
#include <linux/fs.h>
#include <linux/wait.h>
#endif

#ifndef IO_COMPLETION_ALL_ACCESS
#define IO_COMPLETION_ALL_ACCESS 0x001F0003
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Completion port object                                                       */
/* -------------------------------------------------------------------------- */

struct nt_iocp_packet {
	ULONG key;
	void *overlapped;
	NTSTATUS status;
	ULONG_PTR information;
	struct list_head entry;
};

struct nt_iocp {
	ULONG magic;
	struct kref ref;
	spinlock_t lock;
	struct list_head queue;
	wait_queue_head_t waitq;
	ULONG concurrency;
	ULONG active;
	ULONG queued;
};

/* -------------------------------------------------------------------------- */
/* User APC                                                                     */
/* -------------------------------------------------------------------------- */

struct nt_user_apc {
	void *routine;
	void *arg1;
	void *arg2;
	void *arg3;
	struct list_head entry;
};

/* -------------------------------------------------------------------------- */
/* Service entry points                                                         */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtCreateIoCompletion(PHANDLE CompletionPortHandle,
				     ACCESS_MASK DesiredAccess,
				     POBJECT_ATTRIBUTES ObjectAttributes,
				     ULONG NumberOfConcurrentThreads);

NTSTATUS nt_svc_NtSetIoCompletion(HANDLE IoCompletionHandle,
				  ULONG CompletionKey,
				  PVOID CompletionValue,
				  NTSTATUS CompletionStatus,
				  ULONG_PTR CompletionInformation);

NTSTATUS nt_svc_NtSetIoCompletionEx(HANDLE IoCompletionHandle,
				    PVOID Reserved1,
				    ULONG CompletionKey,
				    PVOID CompletionValue,
				    NTSTATUS CompletionStatus,
				    ULONG_PTR CompletionInformation);

NTSTATUS nt_svc_NtRemoveIoCompletion(HANDLE IoCompletionHandle,
				     PULONG CompletionKey,
				     PULONG CompletionValue,
				     PIO_STATUS_BLOCK IoStatusBlock,
				     PLARGE_INTEGER Timeout);

NTSTATUS nt_svc_NtRemoveIoCompletionEx(HANDLE IoCompletionHandle,
				       PVOID CompletionPortEntries,
				       ULONG Count,
				       PULONG NumEntriesRemoved,
				       PLARGE_INTEGER Timeout,
				       BOOLEAN Alertable);

NTSTATUS nt_svc_NtQueryIoCompletion(HANDLE IoCompletionHandle,
				    ULONG IoCompletionInformationClass,
				    PVOID IoCompletionInformation,
				    ULONG IoCompletionInformationLength,
				    PULONG ReturnLength);

/* -------------------------------------------------------------------------- */
/* APC services                                                                 */
/* -------------------------------------------------------------------------- */

NTSTATUS nt_svc_NtQueueApcThread(HANDLE ThreadHandle,
				 PVOID ApcRoutine,
				 PVOID ApcArgument1,
				 PVOID ApcArgument2,
				 PVOID ApcArgument3);

NTSTATUS nt_svc_NtQueueApcThreadEx(HANDLE ThreadHandle,
				   HANDLE UserApcReserveHandle,
				   PVOID ApcRoutine,
				   PVOID ApcArgument1,
				   PVOID ApcArgument2,
				   PVOID ApcArgument3);

NTSTATUS nt_svc_NtTestAlert(void);

NTSTATUS nt_svc_NtAlertThread(HANDLE ThreadHandle);
NTSTATUS nt_svc_NtAlertThreadByThreadId(HANDLE ThreadId);

/* -------------------------------------------------------------------------- */
/* Internal helpers used by io.c                                                */
/* -------------------------------------------------------------------------- */

void ntio_iocp_get(struct nt_iocp *iocp);
void ntio_iocp_put(struct nt_iocp *iocp);
void ntio_iocp_disassociate(struct nt_iocp **iocpp);
NTSTATUS ntio_iocp_post(struct nt_iocp *iocp, ULONG key, void *overlapped,
			NTSTATUS status, ULONG_PTR information);

/* Per-task APC list (userspace shim: single current task). */
extern struct list_head ntio_current_apc_list;
extern spinlock_t ntio_apc_lock;

void ntio_apc_deliver_all(void);

#ifdef __cplusplus
}
#endif

#endif /* NTIO_IOCP_H */
