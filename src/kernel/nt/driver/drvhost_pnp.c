/* drvhost_pnp.c - device-stack attachment, power and HAL exports.
 *
 * This is the skeleton the PnP manager needs: a device stack is built by
 * attaching each FDO to the PDO below it, and the top of the stack receives
 * IRP_MJ_PNP and IRP_MJ_POWER.  Power is recorded per device, not managed.
 */

#include "drvhost_int.h"

DRVCALL PDEVICE_OBJECT IoAttachDeviceToDeviceStack(PDEVICE_OBJECT SourceDevice, PDEVICE_OBJECT TargetDevice)
{
    if (!SourceDevice || !TargetDevice) return NULL;
    dh_lock();
    PDEVICE_OBJECT lower = TargetDevice;
    while (lower->AttachedDevice) lower = lower->AttachedDevice;
    lower->AttachedDevice = SourceDevice;
    SourceDevice->StackSize = (CCHAR)(lower->StackSize + 1);
    dh_unlock();
    /* Real Windows returns the device the source was attached above, i.e. the
     * previous top of the target's stack. */
    return lower;
}

DRVCALL void IoDetachDevice(PDEVICE_OBJECT TargetDevice)
{
    if (!TargetDevice) return;
    dh_lock();
    PDEVICE_OBJECT top = TargetDevice->AttachedDevice;
    if (top) {
        TargetDevice->AttachedDevice = top->AttachedDevice;
        top->AttachedDevice = NULL;
    }
    dh_unlock();
}

DRVCALL PDEVICE_OBJECT IoGetAttachedDevice(PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_OBJECT d = DeviceObject;
    if (!d) return NULL;
    dh_lock();
    while (d->AttachedDevice) d = d->AttachedDevice;
    dh_unlock();
    return d;
}

DRVCALL PDEVICE_OBJECT IoGetAttachedDeviceReference(PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_OBJECT top = IoGetAttachedDevice(DeviceObject);
    if (top) {
        dh_lock();
        top->ReferenceCount++;
        dh_unlock();
    }
    return top;
}

DRVCALL NTSTATUS PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    return IoCallDriver(DeviceObject, Irp);
}

DRVCALL void PoStartNextPowerIrp(PIRP Irp)
{
    /* Power IRPs are not queued by this host, so there is no next one to start. */
    (void)Irp;
}

DRVCALL ULONG PoSetPowerState(PDEVICE_OBJECT DeviceObject, POWER_STATE_TYPE Type, DEVICE_POWER_STATE State)
{
    /* The host does not model real device power: the previous state is not
     * tracked, so this reports the requested state back as the result. */
    (void)DeviceObject; (void)Type;
    return (ULONG)State;
}

DRVCALL void HalDisplayString(const CHAR *String)
{
    if (!String) return;
    dh_emit(String, strlen(String));
}
