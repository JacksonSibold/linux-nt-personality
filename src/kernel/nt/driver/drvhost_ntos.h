/* drvhost_ntos.h - ntoskrnl.exe and hal.dll exports implemented by the driver host.
 *
 * Every name here is the WDK export name.  drvhost_exports.c maps the names to
 * these functions; tools/gen_ntos_surface.py reads that table to produce
 * docs/status/DRIVERHOST-EXPORTS.md.  Keep the prototypes in this file, the
 * definitions in drvhost_{ex,io,rtl,pnp}.c, and the table in sync: the build
 * fails on a mismatch because each definition includes this header.
 */
#ifndef DRVHOST_NTOS_H
#define DRVHOST_NTOS_H

#include "drvhost_types.h"

typedef struct _RTL_OSVERSIONINFOW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} RTL_OSVERSIONINFOW, *PRTL_OSVERSIONINFOW;


/* -- Pool and fast mutex (drvhost_ex.c) ------------------------------------ */
DRVCALL PVOID  ExAllocatePoolWithTag(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
DRVCALL PVOID  ExAllocatePool2(ULONGLONG Flags, SIZE_T NumberOfBytes, ULONG Tag);
DRVCALL PVOID  ExAllocatePool(POOL_TYPE PoolType, SIZE_T NumberOfBytes);
DRVCALL void   ExFreePoolWithTag(PVOID P, ULONG Tag);
DRVCALL void   ExFreePool(PVOID P);
DRVCALL void   ExInitializeFastMutex(PFAST_MUTEX FastMutex);
DRVCALL void   ExAcquireFastMutex(PFAST_MUTEX FastMutex);
DRVCALL BOOLEAN ExTryToAcquireFastMutex(PFAST_MUTEX FastMutex);
DRVCALL void   ExReleaseFastMutex(PFAST_MUTEX FastMutex);
DRVCALL void   ExRaiseStatus(NTSTATUS Status);

/* -- Spin locks, IRQL, events, DPCs, timers, threads (drvhost_ex.c) ---------- */
DRVCALL void   KeInitializeSpinLock(PKSPIN_LOCK SpinLock);
DRVCALL void   KeAcquireSpinLock(PKSPIN_LOCK SpinLock, PKIRQL OldIrql);
DRVCALL KIRQL  KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK SpinLock);
DRVCALL void   KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql);
DRVCALL void   KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock);
DRVCALL void   KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK SpinLock);
DRVCALL KIRQL  KeRaiseIrqlToDpcLevel(void);
DRVCALL void   KeRaiseIrql(KIRQL NewIrql, PKIRQL OldIrql);
DRVCALL void   KeLowerIrql(KIRQL NewIrql);
DRVCALL KIRQL  KeGetCurrentIrql(void);
DRVCALL void   KeStallExecutionProcessor(ULONG MicroSeconds);

DRVCALL void   KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State);
DRVCALL LONG   KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
DRVCALL LONG   KeResetEvent(PKEVENT Event);
DRVCALL void   KeClearEvent(PKEVENT Event);
DRVCALL LONG   KeReadStateEvent(PKEVENT Event);
DRVCALL NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason, KPROCESSOR_MODE WaitMode,
                                       BOOLEAN Alertable, PLARGE_INTEGER Timeout);
DRVCALL NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                        PLARGE_INTEGER Interval);

DRVCALL void   KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext);
DRVCALL BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2);
DRVCALL BOOLEAN KeRemoveQueueDpc(PKDPC Dpc);
DRVCALL void   KeFlushQueuedDpcs(void);

DRVCALL void   KeInitializeTimer(PKTIMER Timer);
DRVCALL void   KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type);
DRVCALL BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc);
DRVCALL BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc);
DRVCALL BOOLEAN KeCancelTimer(PKTIMER Timer);

DRVCALL PVOID  KeGetCurrentThread(void);
DRVCALL void   KeQuerySystemTime(PLARGE_INTEGER CurrentTime);
DRVCALL LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER PerformanceFrequency);
DRVCALL void   KeBugCheckEx(ULONG BugCheckCode, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4);

DRVCALL NTSTATUS PsCreateSystemThread(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
                                      POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle,
                                      PCLIENT_ID ClientId, PKSTART_ROUTINE StartRoutine,
                                      PVOID StartContext);
DRVCALL void   PsTerminateSystemThread(NTSTATUS ExitStatus);
DRVCALL HANDLE PsGetCurrentProcessId(void);
DRVCALL HANDLE PsGetCurrentThreadId(void);
DRVCALL PVOID  PsGetCurrentProcess(void);

/* The kernel object manager (handle.c) defines ObReferenceObjectByHandle with
 * its own signature, so the host's implementation gets a distinct C symbol.  The
 * driver build keeps the WDK name, because a .sys imports by the name it calls. */
#ifndef DRVHOST_DRIVER_BUILD
#define ObReferenceObjectByHandle dh_ObReferenceObjectByHandle
#endif
DRVCALL NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, ACCESS_MASK DesiredAccess,
                                           PVOID ObjectType, KPROCESSOR_MODE AccessMode,
                                           PVOID *Object, PVOID HandleInformation);
DRVCALL void   ObfReferenceObject(PVOID Object);
DRVCALL void   ObfDereferenceObject(PVOID Object);
DRVCALL NTSTATUS ZwClose(HANDLE Handle);
DRVCALL NTSTATUS NtClose(HANDLE Handle);

/* -- Rtl, string, memory, Mm (drvhost_rtl.c) ------------------------------ */
DRVCALL void   RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Source);
DRVCALL void   RtlInitAnsiString(PANSI_STRING Dest, const CHAR *Source);
DRVCALL NTSTATUS RtlCopyUnicodeString(PUNICODE_STRING Dest, PCUNICODE_STRING Src);
DRVCALL BOOLEAN RtlEqualUnicodeString(PCUNICODE_STRING A, PCUNICODE_STRING B, BOOLEAN CaseInsensitive);
DRVCALL LONG   RtlCompareUnicodeString(PCUNICODE_STRING A, PCUNICODE_STRING B, BOOLEAN CaseInsensitive);
DRVCALL void   RtlFreeUnicodeString(PUNICODE_STRING Str);
DRVCALL NTSTATUS RtlAppendUnicodeStringToString(PUNICODE_STRING Dest, PCUNICODE_STRING Src);
DRVCALL NTSTATUS RtlUnicodeStringToAnsiString(PANSI_STRING Dest, PCUNICODE_STRING Src, BOOLEAN Alloc);
DRVCALL NTSTATUS RtlAnsiStringToUnicodeString(PUNICODE_STRING Dest, PANSI_STRING Src, BOOLEAN Alloc);
DRVCALL NTSTATUS RtlGetVersion(PRTL_OSVERSIONINFOW VersionInformation);
DRVCALL void  *drv_memcpy(void *Dst, const void *Src, SIZE_T N);
DRVCALL void  *drv_memmove(void *Dst, const void *Src, SIZE_T N);
DRVCALL void  *drv_memset(void *Dst, int C, SIZE_T N);
DRVCALL int    drv_memcmp(const void *A, const void *B, SIZE_T N);
DRVCALL SIZE_T drv_strlen(const CHAR *S);
DRVCALL SIZE_T drv_wcslen(const WCHAR *S);
DRVCALL BOOLEAN MmIsAddressValid(PVOID VirtualAddress);
DRVCALL PVOID  MmGetSystemRoutineAddress(PUNICODE_STRING SystemRoutineName);

/* -- MDLs and memory mapping (drvhost_mm.c) ----------------------------------- */
DRVCALL PMDL   IoAllocateMdl(PVOID VirtualAddress, ULONG Length, BOOLEAN SecondaryBuffer, BOOLEAN ChargeQuota, PIRP Irp);
DRVCALL void   IoFreeMdl(PMDL Mdl);
DRVCALL void   MmBuildMdlForNonPagedPool(PMDL Mdl);
DRVCALL void   MmProbeAndLockPages(PMDL Mdl, KPROCESSOR_MODE AccessMode, ULONG Operation);
DRVCALL void   MmUnlockPages(PMDL Mdl);
DRVCALL PVOID  MmMapLockedPagesSpecifyCache(PMDL Mdl, KPROCESSOR_MODE AccessMode, ULONG CacheType,
                                            PVOID RequestedAddress, ULONG BugCheckOnFailure, ULONG Priority);
DRVCALL PVOID  MmMapLockedPages(PMDL Mdl, KPROCESSOR_MODE AccessMode);
DRVCALL void   MmUnmapLockedPages(PVOID BaseAddress, PMDL Mdl);
DRVCALL PVOID  MmAllocateContiguousMemory(SIZE_T NumberOfBytes, ULONGLONG HighestAcceptableAddress);
DRVCALL PVOID  MmAllocateContiguousMemorySpecifyCache(SIZE_T NumberOfBytes, ULONGLONG LowestAcceptableAddress,
                                                      ULONGLONG HighestAcceptableAddress,
                                                      ULONGLONG BoundaryAddressMultiple, ULONG CacheType);
DRVCALL void   MmFreeContiguousMemory(PVOID BaseAddress);
DRVCALL ULONGLONG MmGetPhysicalAddress(PVOID BaseAddress);

/* -- Mutexes, semaphores, multi-object waits, guarded mutexes, resources,
 *    SLISTs, lookaside lists (drvhost_sync.c) ---------------------------------- */
DRVCALL void   KeInitializeMutex(PKMUTEX Mutex, ULONG Level);
DRVCALL LONG   KeReadStateMutex(PKMUTEX Mutex);
DRVCALL LONG   KeReleaseMutex(PKMUTEX Mutex, BOOLEAN Wait);
DRVCALL void   KeInitializeSemaphore(PKSEMAPHORE Semaphore, LONG Count, LONG Limit);
DRVCALL LONG   KeReadStateSemaphore(PKSEMAPHORE Semaphore);
DRVCALL LONG   KeReleaseSemaphore(PKSEMAPHORE Semaphore, LONG Increment, LONG Adjustment, BOOLEAN Wait);
DRVCALL NTSTATUS KeWaitForMultipleObjects(ULONG Count, PVOID Object[], WAIT_TYPE WaitType, ULONG WaitReason,
                                          KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout,
                                          PKWAIT_BLOCK WaitBlockArray);
DRVCALL NTSTATUS KeWaitForMutexObject(PVOID Mutex, ULONG WaitReason, KPROCESSOR_MODE WaitMode,
                                      BOOLEAN Alertable, PLARGE_INTEGER Timeout);
DRVCALL void   KeEnterCriticalRegion(void);
DRVCALL void   KeLeaveCriticalRegion(void);
DRVCALL void   KeEnterGuardedRegion(void);
DRVCALL void   KeLeaveGuardedRegion(void);
DRVCALL BOOLEAN KeAreApcsDisabled(void);
DRVCALL void   KeInitializeGuardedMutex(PKGUARDED_MUTEX Mutex);
DRVCALL void   KeAcquireGuardedMutex(PKGUARDED_MUTEX Mutex);
DRVCALL void   KeAcquireGuardedMutexUnsafe(PKGUARDED_MUTEX Mutex);
DRVCALL BOOLEAN KeTryToAcquireGuardedMutex(PKGUARDED_MUTEX Mutex);
DRVCALL void   KeReleaseGuardedMutex(PKGUARDED_MUTEX Mutex);
DRVCALL void   KeReleaseGuardedMutexUnsafe(PKGUARDED_MUTEX Mutex);
DRVCALL void   KeInitializeDeviceQueue(PKDEVICE_QUEUE Queue);
DRVCALL BOOLEAN KeInsertDeviceQueue(PKDEVICE_QUEUE Queue, PKDEVICE_QUEUE_ENTRY Entry);
DRVCALL BOOLEAN KeInsertByKeyDeviceQueue(PKDEVICE_QUEUE Queue, PKDEVICE_QUEUE_ENTRY Entry, ULONG SortKey);
DRVCALL PKDEVICE_QUEUE_ENTRY KeRemoveDeviceQueue(PKDEVICE_QUEUE Queue);
DRVCALL PKDEVICE_QUEUE_ENTRY KeRemoveByKeyDeviceQueue(PKDEVICE_QUEUE Queue, ULONG SortKey);
DRVCALL BOOLEAN KeRemoveEntryDeviceQueue(PKDEVICE_QUEUE Queue, PKDEVICE_QUEUE_ENTRY Entry);

DRVCALL NTSTATUS ExInitializeResourceLite(PERESOURCE Resource);
DRVCALL NTSTATUS ExReinitializeResourceLite(PERESOURCE Resource);
DRVCALL NTSTATUS ExDeleteResourceLite(PERESOURCE Resource);
DRVCALL ERESOURCE_THREAD ExGetCurrentResourceThread(void);
DRVCALL BOOLEAN ExAcquireResourceExclusiveLite(PERESOURCE Resource, BOOLEAN Wait);
DRVCALL BOOLEAN ExTryToAcquireResourceExclusiveLite(PERESOURCE Resource);
DRVCALL BOOLEAN ExAcquireResourceSharedLite(PERESOURCE Resource, BOOLEAN Wait);
DRVCALL BOOLEAN ExAcquireSharedStarveExclusive(PERESOURCE Resource, BOOLEAN Wait);
DRVCALL BOOLEAN ExAcquireSharedWaitForExclusive(PERESOURCE Resource, BOOLEAN Wait);
DRVCALL void   ExReleaseResourceLite(PERESOURCE Resource);
DRVCALL void   ExReleaseResourceForThreadLite(PERESOURCE Resource, ERESOURCE_THREAD ResourceThreadId);
DRVCALL void   ExConvertExclusiveToSharedLite(PERESOURCE Resource);
DRVCALL BOOLEAN ExIsResourceAcquiredExclusiveLite(PERESOURCE Resource);
DRVCALL ULONG  ExIsResourceAcquiredSharedLite(PERESOURCE Resource);
DRVCALL ULONG  ExGetExclusiveWaiterCount(PERESOURCE Resource);
DRVCALL ULONG  ExGetSharedWaiterCount(PERESOURCE Resource);
DRVCALL void   ExSetResourceOwnerPointer(PERESOURCE Resource, PVOID OwnerPointer);

DRVCALL void   RtlInitializeSListHead(PSLIST_HEADER ListHead);
DRVCALL PSLIST_ENTRY RtlFirstEntrySList(const SLIST_HEADER *ListHead);
DRVCALL USHORT RtlQueryDepthSList(PSLIST_HEADER ListHead);
DRVCALL PSLIST_ENTRY RtlInterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry);
DRVCALL PSLIST_ENTRY RtlInterlockedPopEntrySList(PSLIST_HEADER ListHead);
DRVCALL PSLIST_ENTRY RtlInterlockedFlushSList(PSLIST_HEADER ListHead);
DRVCALL PSLIST_ENTRY ExpInterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry);
DRVCALL PSLIST_ENTRY ExpInterlockedPopEntrySList(PSLIST_HEADER ListHead);
DRVCALL PSLIST_ENTRY ExpInterlockedFlushSList(PSLIST_HEADER ListHead);
DRVCALL PSLIST_ENTRY ExInterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry, PKSPIN_LOCK Lock);
DRVCALL PSLIST_ENTRY ExInterlockedPopEntrySList(PSLIST_HEADER ListHead, PKSPIN_LOCK Lock);
DRVCALL PSLIST_ENTRY ExInterlockedFlushSList(PSLIST_HEADER ListHead);
DRVCALL PLIST_ENTRY ExInterlockedInsertHeadList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry, PKSPIN_LOCK Lock);
DRVCALL PLIST_ENTRY ExInterlockedInsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY ListEntry, PKSPIN_LOCK Lock);
DRVCALL PLIST_ENTRY ExInterlockedRemoveHeadList(PLIST_ENTRY ListHead, PKSPIN_LOCK Lock);
DRVCALL PSINGLE_LIST_ENTRY ExInterlockedPushEntryList(PSINGLE_LIST_ENTRY ListHead, PSINGLE_LIST_ENTRY ListEntry,
                                                      PKSPIN_LOCK Lock);
DRVCALL PSINGLE_LIST_ENTRY ExInterlockedPopEntryList(PSINGLE_LIST_ENTRY ListHead, PKSPIN_LOCK Lock);
DRVCALL void   ExInitializeNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside, PALLOCATE_FUNCTION Allocate,
                                               PFREE_FUNCTION Free, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth);
DRVCALL void   ExDeleteNPagedLookasideList(PNPAGED_LOOKASIDE_LIST Lookaside);
DRVCALL void   ExInitializePagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside, PALLOCATE_FUNCTION Allocate,
                                              PFREE_FUNCTION Free, ULONG Flags, SIZE_T Size, ULONG Tag, USHORT Depth);
DRVCALL void   ExDeletePagedLookasideList(PPAGED_LOOKASIDE_LIST Lookaside);

/* -- Mm (declared for the MDL macros of drvhost_types.h) ------------------- */
DRVCALL PVOID  MmMapLockedPagesSpecifyCache(PMDL Mdl, KPROCESSOR_MODE AccessMode, ULONG CacheType,
                                            PVOID RequestedAddress, ULONG BugCheckOnFailure, ULONG Priority);

/* -- I/O manager (drvhost_io.c) ------------------------------------------- */
DRVCALL NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize,
                                PUNICODE_STRING DeviceName, ULONG DeviceType,
                                ULONG DeviceCharacteristics, BOOLEAN Exclusive,
                                PDEVICE_OBJECT *DeviceObject);
DRVCALL void   IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
DRVCALL NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName);
DRVCALL NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING SymbolicLinkName);
DRVCALL PIRP   IoAllocateIrp(CCHAR StackSize, BOOLEAN ChargeQuota);
DRVCALL void   IoFreeIrp(PIRP Irp);
DRVCALL NTSTATUS IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
DRVCALL NTSTATUS IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
DRVCALL void   IoCompleteRequest(PIRP Irp, CCHAR PriorityBoost);
DRVCALL void   IofCompleteRequest(PIRP Irp, CCHAR PriorityBoost);
DRVCALL void   IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE Routine, PVOID Context,
                                      BOOLEAN InvokeOnSuccess, BOOLEAN InvokeOnError,
                                      BOOLEAN InvokeOnCancel);
DRVCALL PIRP   IoBuildDeviceIoControlRequest(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject,
                                             PVOID InputBuffer, ULONG InputBufferLength,
                                             PVOID OutputBuffer, ULONG OutputBufferLength,
                                             BOOLEAN InternalDeviceIoControl, PKEVENT Event,
                                             PIO_STATUS_BLOCK IoStatusBlock);
DRVCALL BOOLEAN IoCancelIrp(PIRP Irp);
DRVCALL void   IoInitializeTimer(PDEVICE_OBJECT DeviceObject, PIO_TIMER_ROUTINE TimerRoutine,
                                 PVOID Context);
DRVCALL void   IoStartTimer(PDEVICE_OBJECT DeviceObject);
DRVCALL void   IoStopTimer(PDEVICE_OBJECT DeviceObject);

/* -- PnP and power skeleton, HAL (drvhost_pnp.c) -------------------------- */
DRVCALL PDEVICE_OBJECT IoAttachDeviceToDeviceStack(PDEVICE_OBJECT SourceDevice,
                                                   PDEVICE_OBJECT TargetDevice);
DRVCALL void   IoDetachDevice(PDEVICE_OBJECT TargetDevice);
DRVCALL PDEVICE_OBJECT IoGetAttachedDevice(PDEVICE_OBJECT DeviceObject);
DRVCALL PDEVICE_OBJECT IoGetAttachedDeviceReference(PDEVICE_OBJECT DeviceObject);
DRVCALL NTSTATUS PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
DRVCALL void   PoStartNextPowerIrp(PIRP Irp);
DRVCALL ULONG  PoSetPowerState(PDEVICE_OBJECT DeviceObject, POWER_STATE_TYPE Type,
                               DEVICE_POWER_STATE State);
DRVCALL void   HalDisplayString(const CHAR *String);

/* -- Debug output (drvhost_fmt.c) ----------------------------------------- */
DRVCALL NTSTATUS DbgPrint(const CHAR *Format, ...);
DRVCALL NTSTATUS DbgPrintEx(ULONG ComponentId, ULONG Level, const CHAR *Format, ...);
DRVCALL void   DbgBreakPoint(void);

/* -- Data exports (their address is what the import thunk holds) ----------- */
extern DRVIMPORT PVOID *PsThreadType;     /* POBJECT_TYPE *: drivers write *PsThreadType */
extern DRVIMPORT PVOID *IoFileObjectType;
extern DRVIMPORT PVOID *IoDeviceObjectType;
extern DRVIMPORT PVOID *ExEventObjectType;
extern DRVIMPORT ULONG  NtBuildNumber;
extern DRVIMPORT ULONG  KeNumberProcessors;

/* Host-only entry points (not WDK exports) are declared in drvhost.h. */

#endif /* DRVHOST_NTOS_H */
