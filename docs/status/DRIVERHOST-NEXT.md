# Driver host: where the work stands and what comes next

Status date: 2026-10-28. The driver host is still the userspace-hosted model; it is not linked into the kernel. Its tests
(`make -C tests/kernel-drivers check`) pass: unit 101, sync 145, mm 36, ioext 121, reg 110, host 97 checks, and the WDK layout
oracle matches 202 measurements. These are host tests, not evidence that a vendor driver loads.

Done this session: the Rtl library port (see `RTL-PORT.md`); mutexes, semaphores, resources, SLISTs, lookaside lists, device
queues; MDLs and contiguous memory; remove locks, the cancel protocol, `IoStartPacket`, IRP re-initialization, named events,
shutdown notification; the registry services (`ZwOpenKey` and the other key calls, `RtlQueryRegistryValues` and the Rtl registry
helpers) over a bridge to the personality registry.

## Next, in order

1. Object manager basics for host objects: reference counts for events and other host-made objects, `ZwCreateEvent`,
   `ZwWaitForSingleObject`, `ObReferenceObjectByName`, `ObOpenObjectByPointer`, `ObRegisterCallbacks` (validate and keep).
   A draft of this file was started and is not in the tree because it was never built.
2. Work items (`IoAllocateWorkItem`, `IoQueueWorkItem`, `ExQueueWorkItem`) on a separate passive-level worker thread.
3. Process, thread and image notification registries, with the image routine called when the host loads a driver.
4. File calls over devices: `ZwCreateFile`, `ZwReadFile`, `ZwWriteFile`, `ZwDeviceIoControlFile`, `ZwQueryInformationFile`,
   and `IoBuildSynchronousFsdRequest`/`IoBuildAsynchronousFsdRequest`.
5. Device interfaces and properties (`IoRegisterDeviceInterface`, `IoSetDeviceInterfaceState`, `IoGetDeviceProperty`,
   `IoOpenDeviceRegistryKey`) and the `Po*` calls. These need a device-identity model in the PnP step.
6. Link the host into the kernel build. The registry bridge opens keys in the current process's handle table, so every host
   thread has to run in one process context there.

What copying from Wine or ReactOS cannot supply: the PCI/MMIO/interrupt plumbing a GPU driver needs, WDDM (`dxgkrnl`), and a
verified run on a VM. Those are the blockers for the NVIDIA driver.

## Other work left from the Wine and ReactOS survey

- ReactOS `avltable`, `prefix`, `rangelist` and `ppb` routines for the Rtl library.
- Double-byte code pages and Wine's locale data, for kernel32 NLS (session A's area).
- `KUSER_SHARED_DATA` at `0x7ffe0000` is not mapped by the personality; the ported `time.c` reads it.
- Thread pool (`Tp*`), activation contexts, and resource functions from Wine ntdll.
