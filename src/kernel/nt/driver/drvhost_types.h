/* drvhost_types.h - WDK-compatible type subset for the NT driver host.
 *
 * Drivers compiled against the WDK call into the host through the names in
 * drvhost_ntos.h.  The structure layouts below follow the x64 WDK definitions
 * that drivers touch directly (DRIVER_OBJECT, DEVICE_OBJECT, IRP,
 * IO_STACK_LOCATION, FILE_OBJECT, KDPC, KEVENT, FAST_MUTEX).  The layouts were
 * first written from the public WDK headers as remembered by the author. They are
 * now checked mechanically: tests/kernel-drivers/layout compares the size and the
 * field offsets of every structure listed in layout/layout_list.h with Wine's WDK
 * headers (third_party/wine/include, which follow the Windows x64 definitions), and
 * `make -C tests/kernel-drivers layout` fails on a difference. That check found and
 * fixed KDPC, DEVICE_OBJECT, FILE_OBJECT, IRP and the IO_STACK_LOCATION parameter
 * blocks. A vendor-driver run is still the first real verification of the rest; see
 * docs/status/DRIVERHOST-EXPORTS.md "Layout confidence".
 *
 * Every function a driver may call, and every function the host stores as a
 * driver callback, uses the Microsoft x64 calling convention (ms_abi).  On the
 * Linux guest that attribute makes GCC emit the Windows argument registers, so
 * a MinGW-built .sys and a host C caller agree on the ABI.
 */
#ifndef DRVHOST_TYPES_H
#define DRVHOST_TYPES_H

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define DRVCALL __attribute__((ms_abi))

/* Data imports (PsThreadType and friends) need dllimport when a MinGW driver
 * links against the import libraries; the host itself never sees the marker. */
#if defined(__MINGW32__) && !defined(DRVHOST_HOST_BUILD)
#define DRVIMPORT __declspec(dllimport)
#else
#define DRVIMPORT
#endif

typedef char      CHAR;
typedef uint8_t   UCHAR;
typedef uint8_t   BOOLEAN;
typedef int8_t    CCHAR;
typedef int16_t   SHORT;
typedef int16_t   CSHORT;
typedef uint16_t  USHORT;
typedef uint16_t  WCHAR;
typedef WCHAR    *PWSTR;
typedef const WCHAR *PCWSTR;
typedef CHAR     *PCHAR;
typedef int32_t   LONG;
typedef uint32_t  ULONG;
typedef int64_t   LONGLONG;
typedef uint64_t  ULONGLONG;
typedef uintptr_t ULONG_PTR;
typedef intptr_t  LONG_PTR;
typedef uint64_t  SIZE_T;
typedef int32_t   NTSTATUS;
typedef uint32_t  ACCESS_MASK;
typedef uint32_t  DWORD;
typedef void     *PVOID;
typedef void     *HANDLE;
typedef HANDLE   *PHANDLE;
typedef UCHAR     KIRQL;
typedef ULONG_PTR KSPIN_LOCK;
typedef KSPIN_LOCK *PKSPIN_LOCK;
typedef CHAR      KPROCESSOR_MODE;
typedef KIRQL    *PKIRQL;

#define TRUE  1
#define FALSE 0
#define NULL_PTR ((void *)0)

#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((uint32_t)(DeviceType) << 16) | ((uint32_t)(Access) << 14) | \
     ((uint32_t)(Function) << 2) | (uint32_t)(Method))
#define DEVICE_TYPE_FROM_CTL_CODE(c) (((uint32_t)(c) & 0xFFFF0000u) >> 16)
#define FUNCTION_FROM_CTL_CODE(c)    (((uint32_t)(c) & 0x3FFCu) >> 2)
#define METHOD_FROM_CTL_CODE(c)      ((uint32_t)(c) & 3u)
#define ACCESS_FROM_CTL_CODE(c)      (((uint32_t)(c) & 0xC000u) >> 14)

#define METHOD_BUFFERED   0
#define METHOD_IN_DIRECT  1
#define METHOD_OUT_DIRECT 2
#define METHOD_NEITHER    3
#define FILE_ANY_ACCESS    0
#define FILE_READ_ACCESS   0x0001
#define FILE_WRITE_ACCESS  0x0002
#define FILE_DEVICE_UNKNOWN 0x00000022

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

typedef struct _SINGLE_LIST_ENTRY {
    struct _SINGLE_LIST_ENTRY *Next;
} SINGLE_LIST_ENTRY, *PSINGLE_LIST_ENTRY;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;

typedef struct _ANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR  Buffer;
} ANSI_STRING, *PANSI_STRING;

typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { ULONG LowPart; ULONG HighPart; };
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG           Attributes;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID, *PCLIENT_ID;

/* -------------------------------------------------------------------------- */
/* Status codes, IRP and IO constants                                         */
/* -------------------------------------------------------------------------- */
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000)
#define STATUS_PENDING                   ((NTSTATUS)0x00000103)
#define STATUS_TIMEOUT                   ((NTSTATUS)0x00000102)
#define STATUS_MORE_PROCESSING_REQUIRED  ((NTSTATUS)0xC0000016)
#define STATUS_UNSUCCESSFUL              ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED           ((NTSTATUS)0xC0000002)
#define STATUS_INVALID_INFO_CLASS        ((NTSTATUS)0xC0000003)
#define STATUS_INFO_LENGTH_MISMATCH      ((NTSTATUS)0xC0000004)
#define STATUS_ACCESS_VIOLATION          ((NTSTATUS)0xC0000005)
#define STATUS_INVALID_HANDLE            ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000D)
#define STATUS_NO_SUCH_DEVICE            ((NTSTATUS)0xC000000E)
#define STATUS_NO_SUCH_FILE              ((NTSTATUS)0xC000000F)
#define STATUS_INVALID_DEVICE_REQUEST    ((NTSTATUS)0xC0000010)
#define STATUS_END_OF_FILE               ((NTSTATUS)0xC0000011)
#define STATUS_NO_MEMORY                 ((NTSTATUS)0xC0000017)
#define STATUS_ILLEGAL_INSTRUCTION       ((NTSTATUS)0xC000001D)
#define STATUS_ALREADY_COMMITTED         ((NTSTATUS)0xC0000021)
#define STATUS_BUFFER_TOO_SMALL          ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_NAME_INVALID       ((NTSTATUS)0xC0000033)
#define STATUS_OBJECT_NAME_NOT_FOUND     ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION     ((NTSTATUS)0xC0000035)
#define STATUS_OBJECT_TYPE_MISMATCH      ((NTSTATUS)0xC0000024)
#define STATUS_INVALID_IMAGE_FORMAT      ((NTSTATUS)0xC000007B)
#define STATUS_PROCEDURE_NOT_FOUND       ((NTSTATUS)0xC000007A)
#define STATUS_DEVICE_NOT_READY          ((NTSTATUS)0xC00000A3)
#define STATUS_INSUFFICIENT_RESOURCES    ((NTSTATUS)0xC000009A)
#define STATUS_CANCELLED                 ((NTSTATUS)0xC0000120)
#define STATUS_INVALID_DEVICE_STATE      ((NTSTATUS)0xC0000184)
#define STATUS_IMAGE_ALREADY_LOADED      ((NTSTATUS)0xC000010E)
#define STATUS_DRIVER_UNABLE_TO_LOAD     ((NTSTATUS)0xC000026E)
#define STATUS_NOT_SUPPORTED             ((NTSTATUS)0xC00000BB)
#define STATUS_OBJECT_PATH_NOT_FOUND     ((NTSTATUS)0xC000003A)
#define STATUS_ACCESS_DENIED             ((NTSTATUS)0xC0000022)
#define STATUS_NOT_FOUND                 ((NTSTATUS)0xC0000225)
#define STATUS_INVALID_PARAMETER_1       ((NTSTATUS)0xC00000EF)
#define STATUS_SHARING_VIOLATION         ((NTSTATUS)0xC0000043)
#define STATUS_IRQL_NOT_LESS_OR_EQUAL    ((NTSTATUS)0x0000000A)

#define NT_SUCCESS(s)   ((NTSTATUS)(s) >= 0)
#define NT_INFORMATION(s) (((uint32_t)(s) >> 30) == 1)
#define NT_ERROR(s)     (((uint32_t)(s) >> 30) == 3)

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CREATE_NAMED_PIPE        0x01
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_READ                     0x03
#define IRP_MJ_WRITE                    0x04
#define IRP_MJ_QUERY_INFORMATION        0x05
#define IRP_MJ_SET_INFORMATION          0x06
#define IRP_MJ_QUERY_EA                 0x07
#define IRP_MJ_SET_EA                   0x08
#define IRP_MJ_FLUSH_BUFFERS            0x09
#define IRP_MJ_QUERY_VOLUME_INFORMATION 0x0A
#define IRP_MJ_SET_VOLUME_INFORMATION   0x0B
#define IRP_MJ_DIRECTORY_CONTROL        0x0C
#define IRP_MJ_FILE_SYSTEM_CONTROL      0x0D
#define IRP_MJ_DEVICE_CONTROL           0x0E
#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0F
#define IRP_MJ_SHUTDOWN                 0x10
#define IRP_MJ_LOCK_CONTROL             0x11
#define IRP_MJ_CLEANUP                  0x12
#define IRP_MJ_CREATE_MAILSLOT          0x13
#define IRP_MJ_QUERY_SECURITY           0x14
#define IRP_MJ_SET_SECURITY             0x15
#define IRP_MJ_POWER                    0x16
#define IRP_MJ_SYSTEM_CONTROL           0x17
#define IRP_MJ_DEVICE_CHANGE            0x18
#define IRP_MJ_QUERY_QUOTA              0x19
#define IRP_MJ_SET_QUOTA                0x1A
#define IRP_MJ_PNP                      0x1B
#define IRP_MJ_MAXIMUM_FUNCTION         0x1B

#define IRP_MN_START_DEVICE             0x00
#define IRP_MN_QUERY_REMOVE_DEVICE      0x01
#define IRP_MN_REMOVE_DEVICE            0x02
#define IRP_MN_CANCEL_REMOVE_DEVICE     0x03
#define IRP_MN_STOP_DEVICE              0x04
#define IRP_MN_QUERY_STOP_DEVICE        0x05
#define IRP_MN_CANCEL_STOP_DEVICE       0x06
#define IRP_MN_QUERY_DEVICE_RELATIONS   0x07
#define IRP_MN_SET_POWER                0x02
#define IRP_MN_QUERY_POWER              0x01
#define IRP_MN_QUERY_DEVICE_RELATIONS_BUS 0x00

#define SL_PENDING_RETURNED   0x01
#define SL_INVOKE_ON_CANCEL   0x20
#define SL_INVOKE_ON_SUCCESS  0x40
#define SL_INVOKE_ON_ERROR    0x80

#define DO_BUFFERED_IO            0x00000004
#define DO_EXCLUSIVE              0x00000008
#define DO_DIRECT_IO              0x00000010
#define DO_DEVICE_INITIALIZING    0x00000080
#define DO_POWER_PAGABLE          0x00002000
#define DO_POWER_INRUSH           0x00004000
#define FILE_DEVICE_SECURE_OPEN   0x00000100
#define FILE_AUTOGENERATED_DEVICE_NAME 0x00000010

#define DRVO_LEGACY_DRIVER        0x00000010
#define DRVO_BUILTIN_DRIVER       0x00000002

#define IO_NO_INCREMENT           0
#define IO_DISK_INCREMENT         1
#define IO_NO_PRIORITY_BOOST      0

typedef enum _POOL_TYPE {
    NonPagedPool = 0,
    PagedPool = 1,
    NonPagedPoolMustSucceed = 2,
    NonPagedPoolCacheAligned = 4,
    PagedPoolCacheAligned = 5,
    NonPagedPoolNx = 512,
} POOL_TYPE;

#define POOL_FLAG_UNINITIALIZED   0x0000000000000001ULL
#define POOL_FLAG_CACHE_ALIGNED   0x0000000000000004ULL
#define POOL_FLAG_RAISE_ON_FAILURE 0x0000000000000020ULL
#define POOL_FLAG_NON_PAGED       0x0000000000000040ULL
#define POOL_FLAG_PAGED           0x0000000000000100ULL

typedef enum _EVENT_TYPE {
    NotificationEvent = 0,
    SynchronizationEvent = 1,
} EVENT_TYPE;

typedef enum _TIMER_TYPE {
    NotificationTimer = 0,
    SynchronizationTimer = 1,
} TIMER_TYPE;

typedef enum _KWAIT_REASON {
    Executive = 0,
} KWAIT_REASON;

typedef enum _MODE {
    KernelMode = 0,
    UserMode = 1,
} MODE;

typedef enum _DEVICE_POWER_STATE {
    PowerDeviceUnspecified = 0,
    PowerDeviceD0 = 1,
    PowerDeviceD1 = 2,
    PowerDeviceD2 = 3,
    PowerDeviceD3 = 4,
} DEVICE_POWER_STATE;

typedef enum _POWER_STATE_TYPE {
    SystemPowerState = 0,
    DevicePowerState = 1,
} POWER_STATE_TYPE;

typedef enum _DEVICE_RELATION_TYPE {
    BusRelations = 0,
    EjectionRelations = 1,
    PowerRelations = 2,
    RemovalRelations = 3,
    TargetDeviceRelation = 4,
    SingleBusRelations = 5,
} DEVICE_RELATION_TYPE;

/* -------------------------------------------------------------------------- */
/* Dispatcher objects                                                         */
/* -------------------------------------------------------------------------- */
typedef struct _DISPATCHER_HEADER {
    UCHAR      Type;
    UCHAR      Absolute;
    UCHAR      Size;
    UCHAR      Inserted;
    LONG       SignalState;
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;

#define DISPATCHER_EVENT_NOTIFICATION   0
#define DISPATCHER_EVENT_SYNCHRONIZATION 1
#define DISPATCHER_TIMER_NOTIFICATION   8
#define DISPATCHER_TIMER_SYNCHRONIZATION 9
#define DISPATCHER_THREAD_OBJECT        6

typedef struct _KEVENT {
    DISPATCHER_HEADER Header;
} KEVENT, *PKEVENT;

typedef struct _KDPC KDPC, *PKDPC;
typedef void (DRVCALL *PKDEFERRED_ROUTINE)(PKDPC Dpc, PVOID DeferredContext,
                                           PVOID SystemArgument1, PVOID SystemArgument2);

struct _KDPC {
    UCHAR             Type;
    UCHAR             Importance;
    USHORT            Number;
    SINGLE_LIST_ENTRY DpcListEntry;    /* 0x08 */
    ULONG_PTR         ProcessorHistory; /* 0x10 */
    PKDEFERRED_ROUTINE DeferredRoutine; /* 0x18 */
    PVOID             DeferredContext;
    PVOID             SystemArgument1;
    PVOID             SystemArgument2;
    PVOID             DpcData;
};

typedef struct _KTIMER {
    DISPATCHER_HEADER Header;
    ULARGE_INTEGER    DueTime;
    LIST_ENTRY        TimerListEntry;
    PKDPC             Dpc;
    ULONG             Period;
    ULONG             Processor;
} KTIMER, *PKTIMER;

typedef struct _FAST_MUTEX {
    LONG       Count;
    PVOID      Owner;
    ULONG      Contention;
    KEVENT     Event;
    ULONG      OldIrql;
} FAST_MUTEX, *PFAST_MUTEX;

/* -------------------------------------------------------------------------- */
/* Driver, device and file objects                                            */
/* -------------------------------------------------------------------------- */
struct _DRIVER_OBJECT;
struct _DEVICE_OBJECT;
struct _IRP;
struct _FILE_OBJECT;
struct _IO_STACK_LOCATION;
struct _DRIVER_EXTENSION;
struct _VPB;
struct _MDL;
typedef struct _DRIVER_OBJECT   DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _DEVICE_OBJECT   DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _IRP             IRP, *PIRP;
typedef struct _FILE_OBJECT     FILE_OBJECT, *PFILE_OBJECT;
typedef struct _IO_STACK_LOCATION IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct _DRIVER_EXTENSION DRIVER_EXTENSION, *PDRIVER_EXTENSION;
typedef struct _VPB             VPB, *PVPB;
typedef struct _MDL             MDL, *PMDL;

typedef NTSTATUS (DRVCALL *PDRIVER_DISPATCH)(PDEVICE_OBJECT DeviceObject, PIRP Irp);
typedef NTSTATUS (DRVCALL *PDRIVER_INITIALIZE)(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
typedef NTSTATUS (DRVCALL *PDRIVER_ADD_DEVICE)(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject);
typedef void     (DRVCALL *PDRIVER_UNLOAD)(PDRIVER_OBJECT DriverObject);
typedef void     (DRVCALL *PDRIVER_STARTIO)(PDEVICE_OBJECT DeviceObject, PIRP Irp);
typedef void     (DRVCALL *PDRIVER_CANCEL)(PDEVICE_OBJECT DeviceObject, PIRP Irp);
typedef NTSTATUS (DRVCALL *PIO_COMPLETION_ROUTINE)(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context);
typedef void     (DRVCALL *PIO_TIMER_ROUTINE)(PDEVICE_OBJECT DeviceObject, PVOID Context);
typedef void     (DRVCALL *PKSTART_ROUTINE)(PVOID StartContext);

struct _DRIVER_EXTENSION {
    PDRIVER_OBJECT  DriverObject;      /* 0x00 */
    PDRIVER_ADD_DEVICE AddDevice;      /* 0x08 */
    ULONG           Count;             /* 0x10 */
    UNICODE_STRING  ServiceKeyName;    /* 0x18 */
    PVOID           ClientDriverExtension; /* 0x28 */
    PVOID           FsFilterCallbacks; /* 0x30 */
};

struct _DRIVER_OBJECT {
    CSHORT          Type;              /* 0x00 */
    CSHORT          Size;              /* 0x02 */
    PDEVICE_OBJECT  DeviceObject;      /* 0x08 */
    ULONG           Flags;             /* 0x10 */
    PVOID           DriverStart;       /* 0x18 */
    ULONG           DriverSize;        /* 0x20 */
    PVOID           DriverSection;     /* 0x28 */
    PDRIVER_EXTENSION DriverExtension; /* 0x30 */
    UNICODE_STRING  DriverName;        /* 0x38 */
    PUNICODE_STRING HardwareDatabase;  /* 0x48 */
    PVOID           FastIoDispatch;    /* 0x50 */
    PDRIVER_INITIALIZE DriverInit;     /* 0x58 */
    PDRIVER_STARTIO DriverStartIo;     /* 0x60 */
    PDRIVER_UNLOAD  DriverUnload;      /* 0x68 */
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1]; /* 0x70 */
};

struct _WAIT_CONTEXT_BLOCK {
    LIST_ENTRY      WaitQueueEntry_DeviceListEntry; /* KDEVICE_QUEUE_ENTRY: DeviceListEntry, SortKey, Inserted */
    ULONG           WaitQueueEntry_SortKey;
    BOOLEAN         WaitQueueEntry_Inserted;
    PVOID           DeviceRoutine;
    PVOID           DeviceContext;
    ULONG           NumberOfMapRegisters;
    PVOID           DeviceObject;
    PVOID           CurrentIrp;
    PKDPC           BufferChainingDpc;
};
typedef struct _WAIT_CONTEXT_BLOCK WAIT_CONTEXT_BLOCK;

typedef struct _KDEVICE_QUEUE {
    CSHORT          Type;
    CSHORT          Size;
    LIST_ENTRY      DeviceListHead;
    KSPIN_LOCK      Lock;
    BOOLEAN         Busy;
} KDEVICE_QUEUE, *PKDEVICE_QUEUE;

typedef struct _DEVOBJ_EXTENSION {
    CSHORT          Type;              /* 0x00 */
    USHORT          Size;              /* 0x02 */
    PDEVICE_OBJECT  DeviceObject;      /* 0x08 */
    ULONG           PowerFlags;        /* 0x10 */
    PVOID           Dope;              /* 0x18 */
    ULONG           ExtensionFlags;    /* 0x20 */
    PVOID           DeviceNode;        /* 0x28 */
    PDEVICE_OBJECT  AttachedTo;        /* 0x30 */
    LONG            StartIoCount;      /* 0x38 */
    LONG            StartIoKey;        /* 0x3C */
    ULONG           StartIoFlags;      /* 0x40 */
    PVPB            Vpb;               /* 0x48 */
    PVOID           DependencyNode;    /* 0x50 */
    PVOID           InterruptContext;  /* 0x58 */
    LONG            VerifierContext;   /* 0x60 */
} DEVOBJ_EXTENSION, *PDEVOBJ_EXTENSION;

struct _DEVICE_OBJECT {
    CSHORT          Type;              /* 0x00 */
    USHORT          Size;              /* 0x02 */
    LONG            ReferenceCount;    /* 0x04 */
    PDRIVER_OBJECT  DriverObject;      /* 0x08 */
    PDEVICE_OBJECT  NextDevice;        /* 0x10 */
    PDEVICE_OBJECT  AttachedDevice;    /* 0x18 */
    PIRP            CurrentIrp;        /* 0x20 */
    PVOID           Timer;             /* 0x28 */
    ULONG           Flags;             /* 0x30 */
    ULONG           Characteristics;   /* 0x34 */
    PVPB            Vpb;               /* 0x38 */
    PVOID           DeviceExtension;   /* 0x40 */
    ULONG           DeviceType;        /* 0x48 */
    CCHAR           StackSize;         /* 0x4C */
    union {
        LIST_ENTRY         ListEntry;
        WAIT_CONTEXT_BLOCK Wcb;
    } Queue;                           /* 0x50, 0x48 bytes */
    ULONG           AlignmentRequirement; /* 0x98 */
    KDEVICE_QUEUE   DeviceQueue;       /* 0xA0 */
    KDPC            Dpc;               /* 0xC8 */
    ULONG           ActiveThreadCount; /* 0x108 */
    PVOID           SecurityDescriptor; /* 0x110 */
    KEVENT          DeviceLock;        /* 0x118 */
    USHORT          SectorSize;        /* 0x130 */
    USHORT          Spare1;            /* 0x132 */
    PDEVOBJ_EXTENSION DeviceObjectExtension; /* 0x138 */
    PVOID           Reserved;          /* 0x140 */
};

struct _MDL {
    struct _MDL *Next;                 /* 0x00 */
    CSHORT      Size;                  /* 0x08 */
    CSHORT      MdlFlags;              /* 0x0A */
    PVOID       Process;               /* 0x10 */
    PVOID       MappedSystemVa;        /* 0x18 */
    PVOID       StartVa;               /* 0x20 */
    ULONG       ByteCount;             /* 0x28 */
    ULONG       ByteOffset;            /* 0x2C; the array of page frame numbers follows */
};

#define MDL_MAPPED_TO_SYSTEM_VA     0x0001
#define MDL_PAGES_LOCKED            0x0002
#define MDL_SOURCE_IS_NONPAGED_POOL 0x0004
#define MDL_ALLOCATED_FIXED_SIZE    0x0008
#define MDL_PARTIAL                 0x0010
#define MDL_PARTIAL_HAS_BEEN_MAPPED 0x0020
#define MDL_IO_PAGE_READ            0x0040
#define MDL_WRITE_OPERATION         0x0080
#define MDL_PARENT_MAPPED_SYSTEM_VA 0x0100
#define MDL_FREE_EXTRA_PTES         0x0200
#define MDL_DESCRIBES_AWE           0x0400
#define MDL_IO_SPACE                0x0800
#define MDL_NETWORK_HEADER          0x1000
#define MDL_MAPPING_CAN_FAIL        0x2000
#define MDL_ALLOCATED_MUST_SUCCEED  0x4000

typedef ULONG_PTR PFN_NUMBER, *PPFN_NUMBER;
#define PAGE_SIZE   4096ull
#define PAGE_SHIFT  12
#define BYTE_OFFSET(va)              ((ULONG)((ULONG_PTR)(va) & (PAGE_SIZE - 1)))
#define PAGE_ALIGN(va)               ((PVOID)((ULONG_PTR)(va) & ~(PAGE_SIZE - 1)))
#define ADDRESS_AND_SIZE_TO_SPAN_PAGES(va, size) \
    ((ULONG)((((ULONG_PTR)(va) & (PAGE_SIZE - 1)) + (size) + (PAGE_SIZE - 1)) >> PAGE_SHIFT))
#define MmGetMdlPfnArray(Mdl)        ((PPFN_NUMBER)((Mdl) + 1))
#define MmGetMdlVirtualAddress(Mdl)  ((PVOID)((PCHAR)(Mdl)->StartVa + (Mdl)->ByteOffset))
#define MmGetMdlByteCount(Mdl)       ((Mdl)->ByteCount)
#define MmGetMdlByteOffset(Mdl)      ((Mdl)->ByteOffset)
#define MmGetSystemAddressForMdlSafe(Mdl, Priority) \
    (((Mdl)->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL)) \
         ? (Mdl)->MappedSystemVa \
         : MmMapLockedPagesSpecifyCache((Mdl), KernelMode, 1, NULL, 0, (Priority)))

struct _VPB {
    CSHORT          Type;
    CSHORT          Size;
    USHORT          Flags;
    USHORT          VolumeLabelLength;
    PDEVICE_OBJECT  DeviceObject;
    PDEVICE_OBJECT  RealDevice;
    ULONG           SerialNumber;
    ULONG           ReferenceCount;
    WCHAR           VolumeLabel[32];
};

struct _FILE_OBJECT {
    CSHORT          Type;              /* 0x00 */
    CSHORT          Size;              /* 0x02 */
    PDEVICE_OBJECT  DeviceObject;      /* 0x08 */
    PVPB            Vpb;               /* 0x10 */
    PVOID           FsContext;         /* 0x18 */
    PVOID           FsContext2;        /* 0x20 */
    PVOID           SectionObjectPointer; /* 0x28 */
    PVOID           PrivateCacheMap;   /* 0x30 */
    NTSTATUS        FinalStatus;       /* 0x38 */
    PFILE_OBJECT    RelatedFileObject; /* 0x40 */
    BOOLEAN         LockOperation;     /* 0x48 */
    BOOLEAN         DeletePending;     /* 0x49 */
    BOOLEAN         ReadAccess;        /* 0x4A */
    BOOLEAN         WriteAccess;       /* 0x4B */
    BOOLEAN         DeleteAccess;      /* 0x4C */
    BOOLEAN         SharedRead;        /* 0x4D */
    BOOLEAN         SharedWrite;       /* 0x4E */
    BOOLEAN         SharedDelete;      /* 0x4F */
    ULONG           Flags;             /* 0x50 */
    UNICODE_STRING  FileName;          /* 0x58 */
    LARGE_INTEGER   CurrentByteOffset; /* 0x68 */
    ULONG           Waiters;           /* 0x70 */
    ULONG           Busy;              /* 0x74 */
    PVOID           LastLock;          /* 0x78 */
    KEVENT          Lock;              /* 0x80 */
    KEVENT          Event;             /* 0x98 */
    PVOID           CompletionContext; /* 0xB0 */
};

/* -------------------------------------------------------------------------- */
/* IRP and stack locations                                                    */
/* -------------------------------------------------------------------------- */
typedef struct _KDEVICE_QUEUE_ENTRY {
    LIST_ENTRY DeviceListEntry;
    ULONG      SortKey;
    BOOLEAN    Inserted;
} KDEVICE_QUEUE_ENTRY, *PKDEVICE_QUEUE_ENTRY;

typedef struct _IRP_TAIL_OVERLAY {
    union {
        KDEVICE_QUEUE_ENTRY DeviceQueueEntry;
        PVOID               DriverContext[4];
    };
    PVOID  Thread;
    PCHAR  AuxiliaryBuffer;
    LIST_ENTRY ListEntry;
    union {
        struct _IO_STACK_LOCATION *CurrentStackLocation;
        ULONG PacketType;
    };
    PFILE_OBJECT OriginalFileObject;
    PVOID IrpExtension;
} IRP_TAIL_OVERLAY;

struct _IRP {
    CSHORT   Type;                     /* 0x00 */
    USHORT   Size;                     /* 0x02 */
    PMDL     MdlAddress;               /* 0x08 */
    ULONG    Flags;                    /* 0x10 */
    union {
        struct _IRP *MasterIrp;
        LONG         IrpCount;
        PVOID        SystemBuffer;     /* 0x18 */
    } AssociatedIrp;
    LIST_ENTRY ThreadListEntry;        /* 0x20 */
    IO_STATUS_BLOCK IoStatus;          /* 0x30 */
    KPROCESSOR_MODE RequestorMode;     /* 0x40 */
    BOOLEAN  PendingReturned;          /* 0x41 */
    CHAR     StackCount;               /* 0x42 */
    CHAR     CurrentLocation;          /* 0x43 */
    BOOLEAN  Cancel;                   /* 0x44 */
    KIRQL    CancelIrql;               /* 0x45 */
    CCHAR    ApcEnvironment;           /* 0x46 */
    UCHAR    AllocationFlags;          /* 0x47 */
    PIO_STATUS_BLOCK UserIosb;         /* 0x48 */
    PKEVENT  UserEvent;                /* 0x50 */
    union {
        struct { PVOID UserApcRoutine; PVOID UserApcContext; } AsynchronousParameters;
        LARGE_INTEGER AllocationSize;
    } Overlay;                         /* 0x58 */
    PDRIVER_CANCEL CancelRoutine;      /* 0x68 */
    PVOID    UserBuffer;               /* 0x70 */
    union {
        IRP_TAIL_OVERLAY Overlay;      /* 0x78; Tail.Overlay.CurrentStackLocation at 0xB8; IrpExtension at 0xC8 */
        UCHAR  Apc[0x30];
        PVOID  CompletionKey;
    } Tail;
};

#define IRP_HEADER_BYTES 0x78

struct _IO_STACK_LOCATION {
    UCHAR  MajorFunction;              /* 0x00 */
    UCHAR  MinorFunction;              /* 0x01 */
    UCHAR  Flags;                      /* 0x02 */
    UCHAR  Control;                    /* 0x03 */
    union {
        struct {                       /* IRP_MJ_DEVICE_CONTROL / INTERNAL */
            ULONG  OutputBufferLength; /* abs 0x08 */
            _Alignas(8) ULONG InputBufferLength;  /* abs 0x10 */
            _Alignas(8) ULONG IoControlCode;      /* abs 0x18 */
            PVOID  Type3InputBuffer;   /* abs 0x20 */
        } DeviceIoControl;
        struct {                       /* IRP_MJ_CREATE */
            PVOID  SecurityContext;
            ULONG  Options;
            _Alignas(8) USHORT FileAttributes;
            USHORT ShareAccess;
            _Alignas(8) ULONG  EaLength;
        } Create;
        struct {                       /* IRP_MJ_READ / IRP_MJ_WRITE */
            ULONG  Length;
            _Alignas(8) ULONG  Key;
            ULONG  Flags;
            LARGE_INTEGER ByteOffset;
        } Read, Write;
        struct {                       /* IRP_MJ_POWER */
            ULONG  SystemContext;
            _Alignas(8) POWER_STATE_TYPE Type;
            _Alignas(8) DEVICE_POWER_STATE State;
            _Alignas(8) ULONG  ShutdownType;
        } Power;
        struct {                       /* IRP_MJ_PNP QUERY_DEVICE_RELATIONS */
            DEVICE_RELATION_TYPE Type;
        } QueryDeviceRelations;
        struct {
            PVOID Argument1;
            PVOID Argument2;
            PVOID Argument3;
            PVOID Argument4;
        } Others;
    } Parameters;
    PDEVICE_OBJECT DeviceObject;       /* 0x28 */
    PFILE_OBJECT   FileObject;         /* 0x30 */
    PIO_COMPLETION_ROUTINE CompletionRoutine; /* 0x38 */
    PVOID          Context;            /* 0x40 */
};

#define SL_STACK_BYTES 0x48

/* WDK inline helpers (macros in the WDK).  The host and drivers share them. */
static inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp)
{
    return Irp->Tail.Overlay.CurrentStackLocation;
}

static inline PIO_STACK_LOCATION IoGetNextIrpStackLocation(PIRP Irp)
{
    return Irp->Tail.Overlay.CurrentStackLocation - 1;
}

static inline void IoSetNextIrpStackLocation(PIRP Irp)
{
    Irp->CurrentLocation--;
    Irp->Tail.Overlay.CurrentStackLocation--;
}

static inline void IoSkipCurrentIrpStackLocation(PIRP Irp)
{
    Irp->CurrentLocation++;
    Irp->Tail.Overlay.CurrentStackLocation++;
}

static inline void IoMarkIrpPending(PIRP Irp)
{
    IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED;
    Irp->PendingReturned = TRUE;
}

/* -------------------------------------------------------------------------- */
/* Mutexes, semaphores, resources, lookaside lists (drvhost_sync.c)           */
/* -------------------------------------------------------------------------- */
#define DISPATCHER_MUTANT_OBJECT        2
#define DISPATCHER_SEMAPHORE_OBJECT     5
#define DISPATCHER_GATE_OBJECT          7

typedef struct _KMUTANT {
    DISPATCHER_HEADER Header;          /* 0x00 */
    LIST_ENTRY MutantListEntry;        /* 0x18 */
    PVOID      OwnerThread;            /* 0x28 */
    BOOLEAN    Abandoned;              /* 0x30 */
    UCHAR      ApcDisable;             /* 0x31 */
} KMUTANT, *PKMUTANT, KMUTEX, *PKMUTEX;

typedef struct _KSEMAPHORE {
    DISPATCHER_HEADER Header;
    LONG       Limit;
} KSEMAPHORE, *PKSEMAPHORE;

typedef struct _KGUARDED_MUTEX {
    LONG       Count;
    PVOID      Owner;
    ULONG      Contention;
    KEVENT     Event;
    union {
        struct { SHORT KernelApcDisable; SHORT SpecialApcDisable; };
        ULONG CombinedApcDisable;
    };
} KGUARDED_MUTEX, *PKGUARDED_MUTEX;

typedef struct _KWAIT_BLOCK {
    LIST_ENTRY WaitListEntry;
    PVOID      Thread;
    PVOID      Object;
    struct _KWAIT_BLOCK *NextWaitBlock;
    USHORT     WaitKey;
    USHORT     WaitType;
} KWAIT_BLOCK, *PKWAIT_BLOCK;

typedef enum _WAIT_TYPE { WaitAll = 0, WaitAny = 1 } WAIT_TYPE;

#define MAXIMUM_WAIT_OBJECTS     64
#define THREAD_WAIT_OBJECTS      3
#define STATUS_WAIT_0            ((NTSTATUS)0x00000000)
#define STATUS_ABANDONED_WAIT_0  ((NTSTATUS)0x00000080)
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED ((NTSTATUS)0xC0000047)
#define STATUS_MUTANT_NOT_OWNED  ((NTSTATUS)0xC0000046)
#define STATUS_MUTANT_LIMIT_EXCEEDED ((NTSTATUS)0xC0000191)

typedef ULONG_PTR ERESOURCE_THREAD, *PERESOURCE_THREAD;

typedef struct _OWNER_ENTRY {
    ERESOURCE_THREAD OwnerThread;
    union {
        struct {
            ULONG IoPriorityBoosted : 1;
            ULONG OwnerReferenced : 1;
            ULONG IoQoSPriorityBoosted : 1;
            ULONG OwnerCount : 29;
        };
        ULONG TableSize;
    };
} OWNER_ENTRY, *POWNER_ENTRY;

#define ResourceNeverExclusive       0x0010
#define ResourceReleaseByOtherThread 0x0020
#define ResourceOwnedExclusive       0x0080

typedef struct _ERESOURCE {
    LIST_ENTRY SystemResourcesList;    /* 0x00 */
    POWNER_ENTRY OwnerTable;           /* 0x10 */
    SHORT      ActiveCount;            /* 0x18 */
    USHORT     Flag;                   /* 0x1A */
    PKSEMAPHORE SharedWaiters;         /* 0x20 */
    PKEVENT    ExclusiveWaiters;       /* 0x28 */
    OWNER_ENTRY OwnerEntry;            /* 0x30 */
    ULONG      ActiveEntries;          /* 0x40 */
    ULONG      ContentionCount;        /* 0x44 */
    ULONG      NumberOfSharedWaiters;  /* 0x48 */
    ULONG      NumberOfExclusiveWaiters; /* 0x4C */
    PVOID      Reserved2;              /* 0x50 */
    PVOID      Address;                /* 0x58 */
    KSPIN_LOCK SpinLock;               /* 0x60 */
} ERESOURCE, *PERESOURCE;

/* Sequenced singly linked list. x64 layout: Alignment holds the depth in its low 16 bits (what the WDK's inline
 * ExQueryDepthSList reads) and a sequence number above; Region holds the address of the first entry. */
typedef struct _SLIST_ENTRY {
    struct _SLIST_ENTRY *Next;
} SLIST_ENTRY, *PSLIST_ENTRY;

typedef struct _SLIST_HEADER {
    _Alignas(16) ULONGLONG Alignment;
    ULONGLONG  Region;
} SLIST_HEADER, *PSLIST_HEADER;

typedef PVOID (DRVCALL *PALLOCATE_FUNCTION)(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
typedef void  (DRVCALL *PFREE_FUNCTION)(PVOID Buffer);

typedef struct _GENERAL_LOOKASIDE {
    _Alignas(64) SLIST_HEADER ListHead; /* 0x00 */
    USHORT     Depth;                  /* 0x10 */
    USHORT     MaximumDepth;           /* 0x12 */
    ULONG      TotalAllocates;         /* 0x14 */
    union { ULONG AllocateMisses; ULONG AllocateHits; };  /* 0x18 */
    ULONG      TotalFrees;             /* 0x1C */
    union { ULONG FreeMisses; ULONG FreeHits; };          /* 0x20 */
    POOL_TYPE  Type;                   /* 0x24 */
    ULONG      Tag;                    /* 0x28 */
    ULONG      Size;                   /* 0x2C */
    union { PALLOCATE_FUNCTION Allocate; PVOID AllocateEx; }; /* 0x30 */
    union { PFREE_FUNCTION Free; PVOID FreeEx; };             /* 0x38 */
    LIST_ENTRY ListEntry;              /* 0x40 */
    ULONG      LastTotalAllocates;     /* 0x50 */
    union { ULONG LastAllocateMisses; ULONG LastAllocateHits; }; /* 0x54 */
    ULONG      Future[2];              /* 0x58 */
} GENERAL_LOOKASIDE, *PGENERAL_LOOKASIDE;

typedef struct _NPAGED_LOOKASIDE_LIST { GENERAL_LOOKASIDE L; } NPAGED_LOOKASIDE_LIST, *PNPAGED_LOOKASIDE_LIST;
typedef struct _PAGED_LOOKASIDE_LIST  { GENERAL_LOOKASIDE L; } PAGED_LOOKASIDE_LIST,  *PPAGED_LOOKASIDE_LIST;

/* -------------------------------------------------------------------------- */
/* Layout pins (see the file comment).  A failed assert means the layout here  */
/* drifted from the x64 WDK definition this file was written against.         */
/* -------------------------------------------------------------------------- */
_Static_assert(offsetof(struct _DRIVER_EXTENSION, AddDevice) == 0x08, "DRIVER_EXTENSION.AddDevice");
_Static_assert(offsetof(DRIVER_OBJECT, DriverExtension) == 0x30, "DRIVER_OBJECT.DriverExtension");
_Static_assert(offsetof(DRIVER_OBJECT, DriverName) == 0x38, "DRIVER_OBJECT.DriverName");
_Static_assert(offsetof(DRIVER_OBJECT, DriverUnload) == 0x68, "DRIVER_OBJECT.DriverUnload");
_Static_assert(offsetof(DRIVER_OBJECT, MajorFunction) == 0x70, "DRIVER_OBJECT.MajorFunction");
_Static_assert(offsetof(DEVICE_OBJECT, DeviceExtension) == 0x40, "DEVICE_OBJECT.DeviceExtension");
_Static_assert(offsetof(DEVICE_OBJECT, DeviceType) == 0x48, "DEVICE_OBJECT.DeviceType");
_Static_assert(offsetof(DEVICE_OBJECT, StackSize) == 0x4C, "DEVICE_OBJECT.StackSize");
_Static_assert(offsetof(IRP, IoStatus) == 0x30, "IRP.IoStatus");
_Static_assert(offsetof(IRP, RequestorMode) == 0x40, "IRP.RequestorMode");
_Static_assert(offsetof(IRP, UserEvent) == 0x50, "IRP.UserEvent");
_Static_assert(offsetof(IRP, CancelRoutine) == 0x68, "IRP.CancelRoutine");
_Static_assert(offsetof(IRP, UserBuffer) == 0x70, "IRP.UserBuffer");
_Static_assert(offsetof(IRP, Tail) == 0x78, "IRP.Tail");
_Static_assert(offsetof(IRP, Tail.Overlay.CurrentStackLocation) == 0xB8, "IRP.Tail.Overlay.CurrentStackLocation");
_Static_assert(sizeof(IO_STACK_LOCATION) == SL_STACK_BYTES, "IO_STACK_LOCATION size");
_Static_assert(offsetof(IO_STACK_LOCATION, DeviceObject) == 0x28, "IO_STACK_LOCATION.DeviceObject");
_Static_assert(offsetof(IO_STACK_LOCATION, CompletionRoutine) == 0x38, "IO_STACK_LOCATION.CompletionRoutine");
_Static_assert(offsetof(IO_STACK_LOCATION, Parameters.DeviceIoControl.IoControlCode) == 0x18, "IOCTL code offset");
_Static_assert(offsetof(IO_STACK_LOCATION, Parameters.DeviceIoControl.Type3InputBuffer) == 0x20, "IOCTL Type3InputBuffer");
_Static_assert(offsetof(FILE_OBJECT, FsContext) == 0x18, "FILE_OBJECT.FsContext");
_Static_assert(offsetof(FILE_OBJECT, FileName) == 0x58, "FILE_OBJECT.FileName");
_Static_assert(sizeof(KEVENT) == 0x18, "KEVENT size");
_Static_assert(offsetof(KDPC, DeferredRoutine) == 0x18, "KDPC.DeferredRoutine");
_Static_assert(offsetof(KDPC, SystemArgument2) == 0x30, "KDPC.SystemArgument2");
_Static_assert(sizeof(KDPC) == 0x40, "KDPC size");
_Static_assert(sizeof(DEVICE_OBJECT) == 0x148, "DEVICE_OBJECT size");
_Static_assert(offsetof(DEVICE_OBJECT, DeviceLock) == 0x118, "DEVICE_OBJECT.DeviceLock");
_Static_assert(sizeof(FILE_OBJECT) == 0xB8, "FILE_OBJECT size");
_Static_assert(sizeof(IRP) == 0xD0, "IRP size");

_Static_assert(sizeof(KMUTANT) == 0x38, "KMUTANT size");
_Static_assert(sizeof(KSEMAPHORE) == 0x20, "KSEMAPHORE size");
_Static_assert(sizeof(KGUARDED_MUTEX) == 0x38, "KGUARDED_MUTEX size");
_Static_assert(sizeof(ERESOURCE) == 0x68, "ERESOURCE size");
_Static_assert(sizeof(SLIST_HEADER) == 0x10, "SLIST_HEADER size");
_Static_assert(sizeof(GENERAL_LOOKASIDE) == 0x80, "GENERAL_LOOKASIDE size");

#endif /* DRVHOST_TYPES_H */
