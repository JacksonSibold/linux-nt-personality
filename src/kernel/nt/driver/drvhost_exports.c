/* drvhost_exports.c - the ntoskrnl.exe and hal.dll export table.
 *
 * One row per implemented export.  The rows use the X_NT / X_HAL macros so that
 * tools/gen_ntos_surface.py can read the table mechanically to produce
 * docs/status/DRIVERHOST-EXPORTS.md.  Keep one row per name; the loader matches
 * imports by exact name within the module the driver names.
 */

#include "drvhost_int.h"

typedef struct dh_export {
    const char *name;
    int         module;     /* 0 = ntoskrnl.exe, 1 = hal.dll */
    void       *addr;
} dh_export_t;

#define DH_MOD_NTOS 0
#define DH_MOD_HAL  1
#define X_NT(name, fn)  { name, DH_MOD_NTOS, (void *)&fn },
#define X_HAL(name, fn) { name, DH_MOD_HAL,  (void *)&fn },

static const dh_export_t g_exports[] = {
    /* Pool and fast mutex */
    X_NT("ExAllocatePoolWithTag", ExAllocatePoolWithTag)
    X_NT("ExAllocatePool2", ExAllocatePool2)
    X_NT("ExAllocatePool", ExAllocatePool)
    X_NT("ExFreePoolWithTag", ExFreePoolWithTag)
    X_NT("ExFreePool", ExFreePool)
    X_NT("ExInitializeFastMutex", ExInitializeFastMutex)
    X_NT("ExAcquireFastMutex", ExAcquireFastMutex)
    X_NT("ExTryToAcquireFastMutex", ExTryToAcquireFastMutex)
    X_NT("ExReleaseFastMutex", ExReleaseFastMutex)
    X_NT("ExRaiseStatus", ExRaiseStatus)
    /* Spin locks and IRQL */
    X_NT("KeInitializeSpinLock", KeInitializeSpinLock)
    X_NT("KeAcquireSpinLock", KeAcquireSpinLock)
    X_NT("KeAcquireSpinLockRaiseToDpc", KeAcquireSpinLockRaiseToDpc)
    X_NT("KeReleaseSpinLock", KeReleaseSpinLock)
    X_NT("KeAcquireSpinLockAtDpcLevel", KeAcquireSpinLockAtDpcLevel)
    X_NT("KeReleaseSpinLockFromDpcLevel", KeReleaseSpinLockFromDpcLevel)
    X_NT("KeRaiseIrqlToDpcLevel", KeRaiseIrqlToDpcLevel)
    X_NT("KeRaiseIrql", KeRaiseIrql)
    X_NT("KeLowerIrql", KeLowerIrql)
    X_NT("KeGetCurrentIrql", KeGetCurrentIrql)
    X_NT("KeStallExecutionProcessor", KeStallExecutionProcessor)
    /* Events and waits */
    X_NT("KeInitializeEvent", KeInitializeEvent)
    X_NT("KeSetEvent", KeSetEvent)
    X_NT("KeResetEvent", KeResetEvent)
    X_NT("KeClearEvent", KeClearEvent)
    X_NT("KeReadStateEvent", KeReadStateEvent)
    X_NT("KeWaitForSingleObject", KeWaitForSingleObject)
    X_NT("KeDelayExecutionThread", KeDelayExecutionThread)
    /* DPCs and timers */
    X_NT("KeInitializeDpc", KeInitializeDpc)
    X_NT("KeInsertQueueDpc", KeInsertQueueDpc)
    X_NT("KeRemoveQueueDpc", KeRemoveQueueDpc)
    X_NT("KeFlushQueuedDpcs", KeFlushQueuedDpcs)
    X_NT("KeInitializeTimer", KeInitializeTimer)
    X_NT("KeInitializeTimerEx", KeInitializeTimerEx)
    X_NT("KeSetTimer", KeSetTimer)
    X_NT("KeSetTimerEx", KeSetTimerEx)
    X_NT("KeCancelTimer", KeCancelTimer)
    /* Clocks, threads, bugcheck */
    X_NT("KeQuerySystemTime", KeQuerySystemTime)
    X_NT("KeQueryPerformanceCounter", KeQueryPerformanceCounter)
    X_NT("KeGetCurrentThread", KeGetCurrentThread)
    X_NT("KeBugCheckEx", KeBugCheckEx)
    X_NT("PsCreateSystemThread", PsCreateSystemThread)
    X_NT("PsTerminateSystemThread", PsTerminateSystemThread)
    X_NT("PsGetCurrentProcessId", PsGetCurrentProcessId)
    X_NT("PsGetCurrentThreadId", PsGetCurrentThreadId)
    X_NT("PsGetCurrentProcess", PsGetCurrentProcess)
    /* Objects and handles */
    X_NT("ObReferenceObjectByHandle", ObReferenceObjectByHandle)
    X_NT("ObfReferenceObject", ObfReferenceObject)
    X_NT("ObfDereferenceObject", ObfDereferenceObject)
    X_NT("ZwClose", ZwClose)
    X_NT("NtClose", NtClose)
    /* Rtl strings, version, memory, Mm */
    X_NT("RtlInitUnicodeString", RtlInitUnicodeString)
    X_NT("RtlInitAnsiString", RtlInitAnsiString)
    X_NT("RtlCopyUnicodeString", RtlCopyUnicodeString)
    X_NT("RtlEqualUnicodeString", RtlEqualUnicodeString)
    X_NT("RtlCompareUnicodeString", RtlCompareUnicodeString)
    X_NT("RtlFreeUnicodeString", RtlFreeUnicodeString)
    X_NT("RtlAppendUnicodeStringToString", RtlAppendUnicodeStringToString)
    X_NT("RtlUnicodeStringToAnsiString", RtlUnicodeStringToAnsiString)
    X_NT("RtlAnsiStringToUnicodeString", RtlAnsiStringToUnicodeString)
    X_NT("RtlGetVersion", RtlGetVersion)
    X_NT("memcpy", drv_memcpy)
    X_NT("memmove", drv_memmove)
    X_NT("memset", drv_memset)
    X_NT("memcmp", drv_memcmp)
    X_NT("strlen", drv_strlen)
    X_NT("wcslen", drv_wcslen)
    X_NT("MmIsAddressValid", MmIsAddressValid)
    X_NT("MmGetSystemRoutineAddress", MmGetSystemRoutineAddress)
    /* I/O manager */
    X_NT("IoCreateDevice", IoCreateDevice)
    X_NT("IoDeleteDevice", IoDeleteDevice)
    X_NT("IoCreateSymbolicLink", IoCreateSymbolicLink)
    X_NT("IoDeleteSymbolicLink", IoDeleteSymbolicLink)
    X_NT("IoAllocateIrp", IoAllocateIrp)
    X_NT("IoFreeIrp", IoFreeIrp)
    X_NT("IoCallDriver", IoCallDriver)
    X_NT("IofCallDriver", IofCallDriver)
    X_NT("IoCompleteRequest", IoCompleteRequest)
    X_NT("IofCompleteRequest", IofCompleteRequest)
    X_NT("IoSetCompletionRoutine", IoSetCompletionRoutine)
    X_NT("IoBuildDeviceIoControlRequest", IoBuildDeviceIoControlRequest)
    X_NT("IoCancelIrp", IoCancelIrp)
    X_NT("IoInitializeTimer", IoInitializeTimer)
    X_NT("IoStartTimer", IoStartTimer)
    X_NT("IoStopTimer", IoStopTimer)
    /* PnP, power and device stacks */
    X_NT("IoAttachDeviceToDeviceStack", IoAttachDeviceToDeviceStack)
    X_NT("IoDetachDevice", IoDetachDevice)
    X_NT("IoGetAttachedDevice", IoGetAttachedDevice)
    X_NT("IoGetAttachedDeviceReference", IoGetAttachedDeviceReference)
    X_NT("PoCallDriver", PoCallDriver)
    X_NT("PoStartNextPowerIrp", PoStartNextPowerIrp)
    X_NT("PoSetPowerState", PoSetPowerState)
    /* Debug output */
    X_NT("DbgPrint", DbgPrint)
    X_NT("DbgPrintEx", DbgPrintEx)
    X_NT("DbgBreakPoint", DbgBreakPoint)
    /* Data exports (the address of the variable) */
    X_NT("PsThreadType", PsThreadType)
    X_NT("IoFileObjectType", IoFileObjectType)
    X_NT("IoDeviceObjectType", IoDeviceObjectType)
    X_NT("ExEventObjectType", ExEventObjectType)
    X_NT("NtBuildNumber", NtBuildNumber)
    X_NT("KeNumberProcessors", KeNumberProcessors)
    /* hal.dll */
    X_HAL("HalDisplayString", HalDisplayString)
};

#define DH_EXPORT_COUNT (sizeof g_exports / sizeof g_exports[0])

static const dh_export_t *find_export(int module, const char *name, size_t len)
{
    for (size_t i = 0; i < DH_EXPORT_COUNT; i++) {
        const dh_export_t *e = &g_exports[i];
        if (e->module != module) continue;
        if (strlen(e->name) == len && memcmp(e->name, name, len) == 0) return e;
    }
    return NULL;
}

void *dh_host_export_module(const char *module, const char *name, size_t len)
{
    int m = (module && strcmp(module, "hal.dll") == 0) ? DH_MOD_HAL : DH_MOD_NTOS;
    const dh_export_t *e = find_export(m, name, len);
    return e ? e->addr : NULL;
}

void *dh_host_export(const char *name, size_t len)
{
    const dh_export_t *e = find_export(DH_MOD_NTOS, name, len);
    if (!e) e = find_export(DH_MOD_HAL, name, len);
    return e ? e->addr : NULL;
}
