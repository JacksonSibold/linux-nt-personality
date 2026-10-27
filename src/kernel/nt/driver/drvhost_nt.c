/* drvhost_nt.c - NT file and IOCTL surface over the driver host's device
 * namespace.
 *
 * NtCreateFile opens a device through a symbolic link (\??\X or \DosDevices\X)
 * or by device name (\Device\X), and sends IRP_MJ_CREATE.  NtDeviceIoControlFile
 * sends IRP_MJ_DEVICE_CONTROL for METHOD_BUFFERED and METHOD_NEITHER codes.
 * DIRECT_IO codes are passed like NEITHER (no MDL is built).  The Win32 names
 * DeviceIoControl, CreateFileA and CloseHandle sit on top of these.
 */

#include "drvhost_int.h"

#define DH_FILE_OPENED 1ull

static FILE_OBJECT *file_new(PDEVICE_OBJECT dev, ACCESS_MASK access)
{
    FILE_OBJECT *f = dh_alloc(sizeof *f);
    if (!f) return NULL;
    f->Type = 5;                                  /* IO_TYPE_FILE */
    f->Size = (CSHORT)sizeof(FILE_OBJECT);
    f->DeviceObject = dev;
    f->ReadAccess = (access & (FILE_READ_ACCESS | 0x80000000u)) != 0;
    f->WriteAccess = (access & (FILE_WRITE_ACCESS | 0x40000000u)) != 0;
    return f;
}

/* Sends one IRP to the top device and returns its final status.  Waits when the
 * driver pends it.  The caller owns the IRP. */
static NTSTATUS send_sync(PDEVICE_OBJECT dev, PIRP irp, ULONG_PTR *info_out)
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
    if (info_out) *info_out = irp->IoStatus.Information;
    return st;
}

/* Resolves a path to a device: symbolic link first, then a device name. */
static PDEVICE_OBJECT resolve_device(const char *name)
{
    char norm[2 * DH_NAME_MAX];
    if (strncmp(name, "\\??\\", 4) == 0) {
        snprintf(norm, sizeof norm, "\\DosDevices\\%s", name + 4);
    } else {
        snprintf(norm, sizeof norm, "%s", name);
    }
    char target[DH_NAME_MAX];
    const char *dev_name = norm;
    if (dh_symlink_lookup(norm, target, sizeof target)) dev_name = target;
    dh_device_t *d = dh_device_by_name(dev_name);
    return d ? &d->dev : NULL;
}

DRVCALL NTSTATUS NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                              POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock,
                              PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
                              ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer,
                              ULONG EaLength)
{
    (void)AllocationSize; (void)FileAttributes; (void)ShareAccess; (void)EaBuffer; (void)EaLength;
    if (!FileHandle || !ObjectAttributes || !ObjectAttributes->ObjectName)
        return STATUS_INVALID_PARAMETER;
    char name[DH_NAME_MAX];
    dh_ascii_from_unicode(ObjectAttributes->ObjectName, name, sizeof name);
    PDEVICE_OBJECT dev = resolve_device(name);
    if (!dev) {
        if (IoStatusBlock) { IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND; IoStatusBlock->Information = 0; }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    FILE_OBJECT *file = file_new(dev, DesiredAccess);
    if (!file) return STATUS_INSUFFICIENT_RESOURCES;

    PIRP irp = IoAllocateIrp(dev->StackSize ? dev->StackSize : 1, FALSE);
    if (!irp) { dh_free(file); return STATUS_INSUFFICIENT_RESOURCES; }
    irp->IoStatus.Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION sl = irp->Tail.Overlay.CurrentStackLocation - 1;
    sl->MajorFunction = IRP_MJ_CREATE;
    sl->FileObject = file;
    sl->Parameters.Create.Options = CreateOptions | (CreateDisposition << 24);
    sl->Parameters.Create.ShareAccess = (USHORT)ShareAccess;

    NTSTATUS st = send_sync(dev, irp, NULL);
    IoFreeIrp(irp);
    if (!NT_SUCCESS(st)) {
        dh_free(file);
        if (IoStatusBlock) { IoStatusBlock->Status = st; IoStatusBlock->Information = 0; }
        return st;
    }
    HANDLE h = dh_handle_insert(DH_KIND_FILE, file);
    if (!h) {
        dh_free(file);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *FileHandle = h;
    if (IoStatusBlock) { IoStatusBlock->Status = STATUS_SUCCESS; IoStatusBlock->Information = DH_FILE_OPENED; }
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine,
                                       PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
                                       ULONG IoControlCode, PVOID InputBuffer, ULONG InputBufferLength,
                                       PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext;
    FILE_OBJECT *file = dh_handle_lookup(FileHandle, DH_KIND_FILE);
    if (!file || !file->DeviceObject) return STATUS_INVALID_HANDLE;
    if (!IoStatusBlock) return STATUS_INVALID_PARAMETER;
    PDEVICE_OBJECT dev = file->DeviceObject;

    PIRP irp = IoAllocateIrp(dev->StackSize ? dev->StackSize : 1, FALSE);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->RequestorMode = UserMode;

    ULONG method = METHOD_FROM_CTL_CODE(IoControlCode);
    PIO_STACK_LOCATION sl = irp->Tail.Overlay.CurrentStackLocation - 1;
    sl->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    sl->FileObject = file;
    sl->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    sl->Parameters.DeviceIoControl.InputBufferLength = InputBufferLength;
    sl->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferLength;

    if (method == METHOD_BUFFERED) {
        ULONG n = InputBufferLength > OutputBufferLength ? InputBufferLength : OutputBufferLength;
        if (n) {
            irp->AssociatedIrp.SystemBuffer = dh_alloc(n);
            if (!irp->AssociatedIrp.SystemBuffer) { IoFreeIrp(irp); return STATUS_INSUFFICIENT_RESOURCES; }
            irp->Flags |= DH_IRP_OWNS_SYSBUF;
            if (InputBuffer && InputBufferLength)
                memcpy(irp->AssociatedIrp.SystemBuffer, InputBuffer, InputBufferLength);
        }
    } else {
        sl->Parameters.DeviceIoControl.Type3InputBuffer = InputBuffer;
        irp->UserBuffer = OutputBuffer;
    }

    ULONG_PTR info = 0;
    NTSTATUS st = send_sync(dev, irp, &info);
    if (method == METHOD_BUFFERED && OutputBuffer && OutputBufferLength && irp->AssociatedIrp.SystemBuffer) {
        ULONG n = (ULONG)(info < OutputBufferLength ? info : OutputBufferLength);
        if (n) memcpy(OutputBuffer, irp->AssociatedIrp.SystemBuffer, n);
    }
    IoStatusBlock->Status = st;
    IoStatusBlock->Information = info;
    IoFreeIrp(irp);
    return st;
}

DRVCALL BOOL_DH DeviceIoControl(HANDLE Device, ULONG IoControlCode, PVOID InBuffer, ULONG InBufferSize,
                                PVOID OutBuffer, ULONG OutBufferSize, ULONG *BytesReturned, PVOID Overlapped)
{
    (void)Overlapped;
    IO_STATUS_BLOCK iosb;
    memset(&iosb, 0, sizeof iosb);
    NTSTATUS st = NtDeviceIoControlFile(Device, NULL, NULL, NULL, &iosb, IoControlCode,
                                        InBuffer, InBufferSize, OutBuffer, OutBufferSize);
    if (BytesReturned) *BytesReturned = (ULONG)iosb.Information;
    return NT_SUCCESS(st) ? 1 : 0;
}

/* Win32 path to NT object name: \\.\X becomes \??\X. */
DRVCALL HANDLE CreateFileA(const char *Name, ULONG DesiredAccess, ULONG ShareMode, PVOID SecurityAttributes,
                           ULONG CreationDisposition, ULONG FlagsAndAttributes, HANDLE TemplateFile)
{
    (void)ShareMode; (void)SecurityAttributes; (void)FlagsAndAttributes; (void)TemplateFile;
    if (!Name) return (HANDLE)(intptr_t)-1;
    char nt[DH_NAME_MAX];
    if (strncmp(Name, "\\\\.\\", 4) == 0) snprintf(nt, sizeof nt, "\\??\\%s", Name + 4);
    else snprintf(nt, sizeof nt, "%s", Name);

    UNICODE_STRING u;
    WCHAR wbuf[DH_NAME_MAX];
    size_t n = strlen(nt);
    for (size_t i = 0; i < n && i < DH_NAME_MAX - 1; i++) wbuf[i] = (WCHAR)(unsigned char)nt[i];
    wbuf[n < DH_NAME_MAX - 1 ? n : DH_NAME_MAX - 1] = 0;
    u.Buffer = wbuf;
    u.Length = (USHORT)(n * 2);
    u.MaximumLength = (USHORT)(DH_NAME_MAX * 2);

    OBJECT_ATTRIBUTES oa;
    memset(&oa, 0, sizeof oa);
    oa.Length = sizeof oa;
    oa.ObjectName = &u;

    HANDLE h = NULL;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st = NtCreateFile(&h, DesiredAccess, &oa, &iosb, NULL, 0, 0, 1, 0, NULL, 0);
    if (!NT_SUCCESS(st)) return (HANDLE)(intptr_t)-1;
    return h;
}

DRVCALL BOOL_DH CloseHandle(HANDLE Object)
{
    return NT_SUCCESS(ZwClose(Object)) ? 1 : 0;
}

/* Called by ZwClose for file handles: sends IRP_MJ_CLOSE, then frees the object. */
void dh_nt_file_closed(void *file_object)
{
    FILE_OBJECT *file = file_object;
    if (!file) return;
    PDEVICE_OBJECT dev = file->DeviceObject;
    if (dev) {
        PIRP irp = IoAllocateIrp(dev->StackSize ? dev->StackSize : 1, FALSE);
        if (irp) {
            irp->IoStatus.Status = STATUS_SUCCESS;
            PIO_STACK_LOCATION sl = irp->Tail.Overlay.CurrentStackLocation - 1;
            sl->MajorFunction = IRP_MJ_CLOSE;
            sl->FileObject = file;
            send_sync(dev, irp, NULL);
            IoFreeIrp(irp);
        }
    }
    dh_free(file);
}
