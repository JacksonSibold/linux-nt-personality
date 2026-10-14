/* drvhost.h - public interface of the NT kernel-mode driver host.
 *
 * The driver host loads a native Windows x64 .sys image (a PE with subsystem
 * NATIVE), binds its imports against the ntoskrnl/hal exports in drvhost_ntos.h,
 * and runs its DriverEntry, PnP and IRP dispatch.  The host-side functions below
 * are the service-control path (NtLoadDriver/ZwLoadDriver), the PnP enumeration
 * step, and the NT file/IOCTL entry points that a userspace caller uses.
 *
 * Status: userspace-hosted model (see docs/status/DRIVERHOST-EXPORTS.md).  The
 * host runs on the Linux guest with pthreads and mmap; it is not yet linked into
 * the personality kernel module.
 */
#ifndef DRVHOST_H
#define DRVHOST_H

#include "drvhost_ntos.h"

/* Lifecycle.  drvhost_init starts the executive worker thread (DPCs, timers,
 * IO timers).  drvhost_shutdown unloads every driver still loaded and stops it. */
int  drvhost_init(void);
void drvhost_shutdown(void);

/* Directory that \SystemRoot\ maps to in ImagePath values. */
void drvhost_set_system_root(const char *host_dir);

/* Debug sink: DbgPrint text goes here when set, else to stderr. */
typedef void (*drvhost_dbg_sink_fn)(void *ctx, const char *text, SIZE_T len);
void drvhost_set_dbg_sink(drvhost_dbg_sink_fn fn, void *ctx);

/* Service-control database (the in-process analogue of
 * HKLM\SYSTEM\CurrentControlSet\Services).  name is the service key name. */
int  drvhost_service_add(const char *name, const char *image_path, ULONG type, ULONG start);
int  drvhost_service_remove(const char *name);

/* Service-control path.  RegistryPath is the service key:
 * \Registry\Machine\System\CurrentControlSet\Services\<name>. */
DRVCALL NTSTATUS NtLoadDriver(PUNICODE_STRING RegistryPath);
DRVCALL NTSTATUS ZwLoadDriver(PUNICODE_STRING RegistryPath);
DRVCALL NTSTATUS NtUnloadDriver(PUNICODE_STRING RegistryPath);
DRVCALL NTSTATUS ZwUnloadDriver(PUNICODE_STRING RegistryPath);

/* PnP skeleton: create a physical device object for a loaded service, call
 * AddDevice, then send IRP_MN_START_DEVICE and an IRP_MJ_POWER set-D0 to the top
 * of the stack.  drvhost_pnp_remove sends IRP_MN_REMOVE_DEVICE and frees the PDO. */
NTSTATUS drvhost_pnp_enumerate(const char *service_name, const char *pdo_name, PDEVICE_OBJECT *pdo_out);
NTSTATUS drvhost_pnp_remove(const char *service_name, PDEVICE_OBJECT pdo);

/* NT file and IOCTL surface, with the NT parameter lists.  Only the device
 * namespace is modelled: \??\X and \DosDevices\X resolve through symbolic links,
 * \Device\X names a device directly. */
DRVCALL NTSTATUS NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                              PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
                              ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer,
                              ULONG EaLength);
DRVCALL NTSTATUS NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine,
                                       PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                                       ULONG IoControlCode, PVOID InputBuffer, ULONG InputBufferLength,
                                       PVOID OutputBuffer, ULONG OutputBufferLength);

/* Win32-shaped wrappers over the NT surface, for callers written against
 * kernel32 names.  Paths use the Win32 device form \\.\NAME. */
typedef int BOOL_DH;
DRVCALL HANDLE CreateFileA(const char *Name, ULONG DesiredAccess, ULONG ShareMode, PVOID SecurityAttributes,
                           ULONG CreationDisposition, ULONG FlagsAndAttributes, HANDLE TemplateFile);
DRVCALL BOOL_DH DeviceIoControl(HANDLE Device, ULONG IoControlCode, PVOID InBuffer, ULONG InBufferSize,
                                PVOID OutBuffer, ULONG OutBufferSize, ULONG *BytesReturned, PVOID Overlapped);
DRVCALL BOOL_DH CloseHandle(HANDLE Object);

/* Counters for tests and diagnostics. */
typedef struct drvhost_stats {
    SIZE_T pool_outstanding;     /* live allocations from ExAllocatePool* */
    SIZE_T pool_bytes;           /* bytes of those allocations */
    ULONG  devices_live;         /* IoCreateDevice minus IoDeleteDevice */
    ULONG  irps_live;            /* IoAllocateIrp minus IoFreeIrp */
    ULONG  images_loaded;        /* loaded driver images */
    ULONG  dpcs_run;             /* DPC routine invocations */
    ULONG  timers_fired;         /* KTIMER expirations */
    ULONG  io_timer_ticks;       /* IoTimer routine invocations */
} drvhost_stats_t;
void drvhost_get_stats(drvhost_stats_t *out);

/* Returns the NT status text for a code, for logs. */
const char *drvhost_status_name(NTSTATUS status);

#endif /* DRVHOST_H */
