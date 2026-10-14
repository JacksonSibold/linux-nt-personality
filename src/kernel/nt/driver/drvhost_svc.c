/* drvhost_svc.c - service-control path, driver objects, PnP enumeration, and
 * host lifecycle.
 *
 * The service database is the in-process analogue of the Services key: each
 * entry carries ImagePath, Type and Start.  NtLoadDriver takes the service key
 * path, reads the entry, maps the image, creates a DRIVER_OBJECT and calls
 * DriverEntry with the key path as RegistryPath.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "drvhost_int.h"

#include "drvhost_svcdb.h"

#define DH_REG_PREFIX_BS DRVHOST_SVC_KEY_PREFIX

/* Service records live in the personality registry (drvhost_svcdb.c); there is
 * no in-process service table. */
static dh_driver_t *g_drivers = NULL;
static char g_system_root[DH_NAME_MAX] = "";
static DRIVER_OBJECT g_bus_driver;          /* owner of every PDO the host creates */
static int g_inited = 0;

void drvhost_set_system_root(const char *host_dir)
{
    strncpy(g_system_root, host_dir ? host_dir : "", sizeof g_system_root - 1);
    g_system_root[sizeof g_system_root - 1] = 0;
}

int drvhost_service_add(const char *name, const char *image_path, ULONG type, ULONG start)
{
    if (!name || !*name || !image_path) return -1;
    return NT_SUCCESS(drvhost_svcdb_register(name, image_path, type, start)) ? 0 : -1;
}

int drvhost_service_remove(const char *name)
{
    if (!name || !*name) return -1;
    return NT_SUCCESS(drvhost_svcdb_remove(name)) ? 0 : -1;
}

dh_driver_t *dh_driver_by_service(const char *service)
{
    dh_driver_t *d = NULL;
    dh_lock();
    for (d = g_drivers; d; d = d->next) {
        if (strcasecmp(d->service, service) == 0) break;
    }
    dh_unlock();
    return d;
}

/* Maps an ImagePath value to a host file path.  \SystemRoot\ and \??\ are the
 * two prefixes Windows drivers use; anything else is taken as a host path. */
static int resolve_image_path(const char *image_path, char *out, size_t cap)
{
    const char *rest = image_path;
    char base[DH_NAME_MAX] = "";
    if (strncasecmp(image_path, "\\SystemRoot\\", 12) == 0) {
        rest = image_path + 12;
        strncpy(base, g_system_root, sizeof base - 1);
    } else if (strncmp(image_path, "\\??\\", 4) == 0) {
        rest = image_path + 4;
    }
    size_t bl = strlen(base);
    size_t rl = strlen(rest);
    if (bl + rl + 2 > cap) return -1;
    memcpy(out, base, bl);
    size_t o = bl;
    if (bl && out[bl - 1] != '/') out[o++] = '/';
    for (size_t i = 0; i < rl; i++) out[o++] = rest[i] == '\\' ? '/' : rest[i];
    out[o] = 0;
    return 0;
}

static void ascii_to_wide(const char *s, WCHAR *out, size_t cap_chars)
{
    size_t i = 0;
    for (; s[i] && i + 1 < cap_chars; i++) out[i] = (WCHAR)(unsigned char)s[i];
    out[i] = 0;
}

static void set_unicode(UNICODE_STRING *u, WCHAR *buf, size_t cap_chars, const char *s)
{
    ascii_to_wide(s, buf, cap_chars);
    size_t n = strlen(s);
    if (n >= cap_chars) n = cap_chars - 1;
    u->Buffer = buf;
    u->Length = (USHORT)(n * 2);
    u->MaximumLength = (USHORT)(cap_chars * 2);
}

/* Strips the registry prefix and returns the service key name (last component). */
static const char *service_from_key(const char *key)
{
    const char *last = strrchr(key, '\\');
    return last ? last + 1 : key;
}

static void invalid_all(PDRIVER_OBJECT drv)
{
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv->MajorFunction[i] = dh_invalid_device_request;
}

static void delete_devices_of(PDRIVER_OBJECT drv)
{
    while (drv->DeviceObject) {
        IoDeleteDevice(drv->DeviceObject);
    }
}

static NTSTATUS NtLoadDriverInternal(const char *service_name)
{
    drvhost_svc_record_t rec;
    NTSTATUS lst = drvhost_svcdb_lookup(service_name, &rec);
    if (lst == STATUS_OBJECT_NAME_NOT_FOUND) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (!NT_SUCCESS(lst)) return lst;                 /* record present but malformed */
    if (dh_driver_by_service(service_name)) return STATUS_IMAGE_ALREADY_LOADED;

    char path[DH_NAME_MAX];
    if (resolve_image_path(rec.image_path, path, sizeof path) != 0) return STATUS_OBJECT_NAME_INVALID;

    dh_driver_t *d = dh_alloc(sizeof *d);
    if (!d) return STATUS_INSUFFICIENT_RESOURCES;
    strncpy(d->service, service_name, sizeof d->service - 1);
    strncpy(d->image_path, path, sizeof d->image_path - 1);

    NTSTATUS st = dh_image_load(path, &d->image);
    if (!NT_SUCCESS(st)) { dh_free(d); return st; }

    char name[DH_NAME_MAX], key[DH_NAME_MAX];
    snprintf(name, sizeof name, "\\Driver\\%s", service_name);
    snprintf(key, sizeof key, DH_REG_PREFIX_BS "%s", service_name);
    set_unicode(&d->name_u, d->name_buf, DH_NAME_MAX, name);
    set_unicode(&d->key_u, d->key_buf, DH_NAME_MAX, key);

    DRIVER_OBJECT *drv = &d->drv;
    drv->Type = 4;                                          /* IO_TYPE_DRIVER */
    drv->Size = (CSHORT)sizeof(DRIVER_OBJECT);
    drv->DriverStart = d->image->mem;
    drv->DriverSize = (ULONG)d->image->size;
    drv->DriverName = d->name_u;
    drv->DriverInit = (PDRIVER_INITIALIZE)d->image->entry;
    drv->DriverExtension = &d->ext;
    invalid_all(drv);
    d->ext.DriverObject = drv;
    d->ext.ServiceKeyName = d->key_u;

    dh_lock();
    d->next = g_drivers;
    g_drivers = d;
    g_images_loaded++;
    dh_unlock();

    if (!d->image->entry) {
        st = STATUS_INVALID_IMAGE_FORMAT;
    } else {
        st = ((PDRIVER_INITIALIZE)d->image->entry)(drv, &d->key_u);
    }
    if (!NT_SUCCESS(st)) {
        delete_devices_of(drv);
        dh_lock();
        dh_driver_t **pp = &g_drivers;
        while (*pp && *pp != d) pp = &(*pp)->next;
        if (*pp) *pp = d->next;
        g_images_loaded--;
        dh_unlock();
        dh_image_unload(d->image);
        dh_free(d);
        return st;
    }
    d->loaded = 1;
    return STATUS_SUCCESS;
}

static const char *service_of_registry_path(PUNICODE_STRING RegistryPath, char *buf, size_t cap)
{
    if (!RegistryPath || !RegistryPath->Buffer) return NULL;
    dh_ascii_from_unicode(RegistryPath, buf, cap);
    return service_from_key(buf);
}

DRVCALL NTSTATUS NtLoadDriver(PUNICODE_STRING RegistryPath)
{
    char buf[DH_NAME_MAX];
    const char *svc = service_of_registry_path(RegistryPath, buf, sizeof buf);
    if (!svc || !*svc) return STATUS_INVALID_PARAMETER;
    return NtLoadDriverInternal(svc);
}

DRVCALL NTSTATUS ZwLoadDriver(PUNICODE_STRING RegistryPath)
{
    return NtLoadDriver(RegistryPath);
}

static NTSTATUS unload_driver(dh_driver_t *d)
{
    DRIVER_OBJECT *drv = &d->drv;
    if (drv->DriverUnload) drv->DriverUnload(drv);
    delete_devices_of(drv);
    dh_lock();
    dh_driver_t **pp = &g_drivers;
    while (*pp && *pp != d) pp = &(*pp)->next;
    if (*pp) *pp = d->next;
    g_images_loaded--;
    dh_unlock();
    dh_image_unload(d->image);
    dh_free(d);
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS NtUnloadDriver(PUNICODE_STRING RegistryPath)
{
    char buf[DH_NAME_MAX];
    const char *svc = service_of_registry_path(RegistryPath, buf, sizeof buf);
    if (!svc || !*svc) return STATUS_INVALID_PARAMETER;
    dh_driver_t *d = dh_driver_by_service(svc);
    if (!d) return STATUS_OBJECT_NAME_NOT_FOUND;
    return unload_driver(d);
}

DRVCALL NTSTATUS ZwUnloadDriver(PUNICODE_STRING RegistryPath)
{
    return NtUnloadDriver(RegistryPath);
}

/* -------------------------------------------------------------------------- */
/* Synchronous IRP send (PnP and power start with the IRP's own stack)         */
/* -------------------------------------------------------------------------- */
static NTSTATUS send_and_wait(PDEVICE_OBJECT dev, PIRP irp)
{
    KEVENT done;
    KeInitializeEvent(&done, SynchronizationEvent, FALSE);
    irp->UserEvent = &done;
    NTSTATUS st = IoCallDriver(dev, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&done, 0, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    irp->UserEvent = NULL;
    return st;
}

static NTSTATUS send_pnp(PDEVICE_OBJECT dev, UCHAR minor)
{
    PIRP irp = IoAllocateIrp(dev->StackSize ? dev->StackSize : 1, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    PIO_STACK_LOCATION sl = irp->Tail.Overlay.CurrentStackLocation - 1;
    sl->MajorFunction = IRP_MJ_PNP;
    sl->MinorFunction = minor;
    NTSTATUS st = send_and_wait(dev, irp);
    IoFreeIrp(irp);
    return st;
}

static NTSTATUS send_set_power_d0(PDEVICE_OBJECT dev)
{
    PIRP irp = IoAllocateIrp(dev->StackSize ? dev->StackSize : 1, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    PIO_STACK_LOCATION sl = irp->Tail.Overlay.CurrentStackLocation - 1;
    sl->MajorFunction = IRP_MJ_POWER;
    sl->MinorFunction = IRP_MN_SET_POWER;
    sl->Parameters.Power.Type = DevicePowerState;
    sl->Parameters.Power.State = PowerDeviceD0;
    NTSTATUS st = send_and_wait(dev, irp);
    IoFreeIrp(irp);
    return st;
}

NTSTATUS drvhost_pnp_enumerate(const char *service_name, const char *pdo_name, PDEVICE_OBJECT *pdo_out)
{
    if (!service_name || !pdo_name || !pdo_out) return STATUS_INVALID_PARAMETER;
    dh_driver_t *d = dh_driver_by_service(service_name);
    if (!d) return STATUS_OBJECT_NAME_NOT_FOUND;
    PDRIVER_ADD_DEVICE add = d->ext.AddDevice;
    if (!add) return STATUS_NOT_SUPPORTED;

    char pname[DH_NAME_MAX];
    snprintf(pname, sizeof pname, "\\Device\\%s", pdo_name);
    UNICODE_STRING pdo_u;
    WCHAR pdo_buf[DH_NAME_MAX];
    set_unicode(&pdo_u, pdo_buf, DH_NAME_MAX, pname);

    PDEVICE_OBJECT pdo = NULL;
    NTSTATUS st = IoCreateDevice(&g_bus_driver, 0, &pdo_u, FILE_DEVICE_UNKNOWN, 0, FALSE, &pdo);
    if (!NT_SUCCESS(st)) return st;
    pdo->Flags &= ~DO_DEVICE_INITIALIZING;

    st = add(&d->drv, pdo);
    if (!NT_SUCCESS(st)) { IoDeleteDevice(pdo); return st; }

    PDEVICE_OBJECT top = IoGetAttachedDevice(pdo);
    st = send_pnp(top, IRP_MN_START_DEVICE);
    if (NT_SUCCESS(st)) st = send_set_power_d0(top);
    if (!NT_SUCCESS(st)) return st;
    *pdo_out = pdo;
    return STATUS_SUCCESS;
}

NTSTATUS drvhost_pnp_remove(const char *service_name, PDEVICE_OBJECT pdo)
{
    if (!service_name || !pdo) return STATUS_INVALID_PARAMETER;
    PDEVICE_OBJECT top = IoGetAttachedDevice(pdo);
    NTSTATUS st = send_pnp(top, IRP_MN_REMOVE_DEVICE);
    IoDetachDevice(pdo);
    IoDeleteDevice(pdo);
    return st;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle and statistics                                                   */
/* -------------------------------------------------------------------------- */
int drvhost_init(void)
{
    if (g_inited) return 0;
    memset(&g_bus_driver, 0, sizeof g_bus_driver);
    g_bus_driver.Type = 4;
    g_bus_driver.Size = (CSHORT)sizeof(DRIVER_OBJECT);
    invalid_all(&g_bus_driver);
    if (dh_ex_init() != 0) return -1;
    g_inited = 1;
    return 0;
}

void drvhost_shutdown(void)
{
    if (!g_inited) return;
    for (;;) {
        dh_lock();
        dh_driver_t *d = g_drivers;
        dh_unlock();
        if (!d) break;
        unload_driver(d);
    }
    dh_ex_shutdown();
    dh_ex_join_threads();
    dh_symlinks_clear();
    g_inited = 0;
}

void drvhost_get_stats(drvhost_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->pool_outstanding = g_pool_outstanding;
    out->pool_bytes = g_pool_bytes;
    out->devices_live = g_devices_live;
    out->irps_live = g_irps_live;
    out->images_loaded = g_images_loaded;
    out->dpcs_run = g_dpcs_run;
    out->timers_fired = g_timers_fired;
    out->io_timer_ticks = g_io_timer_ticks;
}
