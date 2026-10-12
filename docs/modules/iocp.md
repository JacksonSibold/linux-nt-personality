# Module: iocp (I/O completion ports and user APCs)

Sources: `src/kernel/nt/iocp.c`, `src/kernel/nt/iocp.h`
Related: `io.md` (files associate with ports and post completion packets), `sync.md` (alertable waits), `proc_thread.md` (thread objects that APCs target).

## Purpose

Provides NT I/O completion ports, which are FIFO packet queues with a concurrency limit, and per-thread user APCs. Producers (I/O completion, or user code via `NtSetIoCompletion`) post packets. Consumers (`NtRemoveIoCompletion`) dequeue them, blocking when the queue is empty. APCs are queued on a thread and delivered by `NtTestAlert` or by an alertable wait. The file header describes the queue as kernel-producer and user-consumer, with removal honouring the concurrency limit.

## Public API (iocp.h)

Port lifecycle and object helpers (used by `io.c` and the dispatch table):

- `void ntio_iocp_get(struct nt_iocp *iocp)` - take a reference (kref).
- `void ntio_iocp_put(struct nt_iocp *iocp)` - drop a reference; the last drop frees all queued packets and the port.
- `void ntio_iocp_disassociate(struct nt_iocp **iocpp)` - clear an association pointer and drop its reference.
- `NTSTATUS ntio_iocp_post(struct nt_iocp *iocp, ULONG key, void *overlapped, NTSTATUS status, ULONG_PTR information)` - enqueue a completion packet.

APC delivery:

- `void ntio_apc_deliver_all(void)` - run and free every APC on the current thread's list.
- `extern struct list_head ntio_current_apc_list` and `extern spinlock_t ntio_apc_lock` - the per-task APC list and its lock (shim: a single task).

Service entry points (native-style signatures):

- `nt_svc_NtCreateIoCompletion(PHANDLE CompletionPortHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG NumberOfConcurrentThreads)`.
- `nt_svc_NtSetIoCompletion(HANDLE IoCompletionHandle, ULONG CompletionKey, PVOID CompletionValue, NTSTATUS CompletionStatus, ULONG_PTR CompletionInformation)` and `nt_svc_NtSetIoCompletionEx(HANDLE IoCompletionHandle, PVOID Reserved1, ULONG CompletionKey, PVOID CompletionValue, NTSTATUS CompletionStatus, ULONG_PTR CompletionInformation)` - post a packet. The Ex form adds an unused `Reserved1` argument.
- `nt_svc_NtRemoveIoCompletion(HANDLE IoCompletionHandle, PULONG CompletionKey, PULONG CompletionValue, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER Timeout)` - dequeue one packet. `nt_svc_NtRemoveIoCompletionEx(HANDLE IoCompletionHandle, PVOID CompletionPortEntries, ULONG Count, PULONG NumEntriesRemoved, PLARGE_INTEGER Timeout, BOOLEAN Alertable)` - dequeue up to `Count` packets. The Ex form can be alertable, in which case queued APCs are delivered before blocking.
- `nt_svc_NtQueryIoCompletion(HANDLE IoCompletionHandle, ...)` - query port information (see gaps).
- `nt_svc_NtQueueApcThread(HANDLE ThreadHandle, ...)` and `nt_svc_NtQueueApcThreadEx(HANDLE ThreadHandle, ...)` - queue a user APC on a thread.
- `nt_svc_NtTestAlert(void)` - deliver pending APCs for the current thread.
- `nt_svc_NtAlertThread(HANDLE ThreadHandle)` and `nt_svc_NtAlertThreadByThreadId(HANDLE ThreadId)` - alert a thread.

## Key data structures

- `struct nt_iocp` - `magic` (set to `0x494F4350`, "IOCP", at `iocp.c:71`), `ref` (kref), `lock` (spinlock_t), `queue` (list of packets), `waitq` (wait queue), `concurrency`, `active`, `queued`.
- `struct nt_iocp_packet` - `key`, `overlapped`, `status`, `information`, `entry` (list link).
- `struct nt_user_apc` - `routine`, `arg1`, `arg2`, `arg3`, `entry`.

## Invariants and locking (as implemented)

- Port lifetime: the port is kref-counted. `ntio_iocp_release` (`iocp.c` near the top) drains and frees the queue under `iocp->lock`, then frees the port.
- The queue is FIFO. Dequeue returns NULL when empty. The helper comment says the caller must hold the port's lock (`iocp.c:106`).
- Concurrency: when `concurrency` is non-zero, a packet is returned only if `active < concurrency` (`iocp.c:117`, and the same test at `iocp.c:266` and `iocp.c:482`).
- Blocking removal waits on the port's wait queue. In the kernel build this uses `wait_event_interruptible` and `wait_event_interruptible_timeout`. In the shim it uses `pthread_cond_timedwait`.
- APCs are delivered by `ntio_apc_deliver_all`, which is called from `nt_svc_NtTestAlert` (`iocp.c:705-708`) and from the alertable branch of the Ex removal path, before the thread blocks (`iocp.c:455-462`).
- `ntio_apc_lock` is a spinlock, initialised with `spin_lock_init` at `iocp.c:640` and taken at `iocp.c:648` for APC list access.

## Known gaps (evident in code)

- `nt_svc_NtQueryIoCompletion` returns `STATUS_NOT_IMPLEMENTED` after validating the handle (`iocp.c:620-628`). It does not fill the information buffers.
- `ntio_iocp_release_concurrency` is defined with `__attribute__((unused))` (`iocp.c:130`). Nothing calls it, so the active count is not decremented through that helper.
- The shim's APC list is a single global list (`ntio_current_apc_list`). The header comment says "userspace shim: single current task". Per-thread APC queues exist only in the kernel plan described in the comment ("real kernel integration attaches the list to struct nt_thread"); this file does not do that.
- `nt_svc_NtRemoveIoCompletion` and `nt_svc_NtRemoveIoCompletionEx` each appear twice in the source, once per build branch. I did not trace the guards.
- Test coverage: `io_test.c` references the completion-port services. No separate `iocp_test.c` exists.
