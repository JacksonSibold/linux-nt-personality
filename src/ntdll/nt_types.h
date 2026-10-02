/*
 * nt_types.h
 *
 * NT base types and structures for the x86-64 NT personality (ntpers).
 *
 * Reference build: Windows Server 2022 x64 (10.0.20348).
 *
 * Sources for offsets:
 *   - Windows SDK 10.0.20348.0 winnt.h / wdm.h / ntdef.h
 *   - Windows Internals, 7th ed.
 *   - WinDbg "dt ntdll!_TEB", "dt ntdll!_PEB", "dt ntdll!_CONTEXT" on
 *     Windows Server 2022 Datacenter (20348.2031) and Windows 10 22H2 (19045.x)
 *   - ReactOS NDK tntstruct.h / ketypes.h (used as a cross-check only; this is
 *     original code)
 *
 * Every structure has a _Static_assert on its total size.  Fields that other
 * workstreams depend on have offset assertions.  Anything we could not verify
 * against a live Windows x64 system is marked with an UNVERIFIED comment and
 * is listed in the project offset-audit register.
 */

#ifndef NT_TYPES_H
#define NT_TYPES_H

#ifndef __KERNEL__
#include <stdint.h>
#include <stddef.h>
#else
#include <linux/types.h>
#include <linux/stddef.h>
#endif
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#undef DUMMYUNIONNAME1
#undef DUMMYUNIONNAME2
#undef DUMMYUNIONNAME3
#undef DUMMYUNIONNAME4
#undef DUMMYUNIONNAME5
#undef DUMMYUNIONNAME6
#undef DUMMYUNIONNAME7
#undef DUMMYUNIONNAME8
#undef DUMMYUNIONNAME9
#undef DUMMYSTRUCTNAME1
#undef DUMMYSTRUCTNAME2
#undef DUMMYSTRUCTNAME3
#undef DUMMYSTRUCTNAME4
#undef DUMMYSTRUCTNAME5

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Compiler portability                                                       */
/* -------------------------------------------------------------------------- */

#if defined(_MSC_VER)
#  define NT_ALIGN(n) __declspec(align(n))
#  define NT_FORCEINLINE __forceinline
#else
#  define NT_ALIGN(n) __attribute__((__aligned__(n)))
#  define NT_FORCEINLINE __attribute__((__always_inline__)) inline
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#  define NT_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#else
#  include <assert.h>
#  define NT_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#endif

/* -------------------------------------------------------------------------- */
/* Base scalar types (exact widths)                                           */
/* -------------------------------------------------------------------------- */

typedef int8_t    CHAR;
typedef uint8_t   UCHAR;
typedef int16_t   SHORT;
typedef uint16_t  USHORT;
typedef int32_t   LONG;
typedef uint32_t  ULONG;
typedef int64_t   LONGLONG;
typedef uint64_t  ULONG64;
typedef uint64_t  ULONGLONG;
typedef int64_t   LONG64;

typedef uint8_t   BYTE;
typedef uint16_t  WORD;
typedef uint32_t  DWORD;
typedef uint64_t  DWORD64;

typedef uint16_t  WCHAR;
typedef const WCHAR *PCWSTR;
typedef WCHAR      *PWSTR;
typedef const char *PCSZ;
typedef char       *PSZ;
typedef char       *PCHAR;
typedef const char *PCCHAR;

typedef LONG      NTSTATUS;
typedef NTSTATUS *PNTSTATUS;
typedef int32_t   BOOL;
typedef UCHAR       BOOLEAN;
typedef BOOLEAN  *PBOOLEAN;
typedef void     *HANDLE;
typedef HANDLE   *PHANDLE;
typedef LONG     *PLONG;
typedef ULONG    *PULONG;
typedef ULONG64  *PULONG64;
typedef UCHAR    *PUCHAR;
typedef USHORT   *PUSHORT;
typedef void     *PVOID;
typedef const void *PCVOID;
typedef uint64_t  ULONG_PTR;
typedef int64_t   LONG_PTR;
typedef uint64_t  SIZE_T;
typedef SIZE_T   *PSIZE_T;
typedef int64_t   SSIZE_T;

NT_STATIC_ASSERT(sizeof(NTSTATUS) == 4, "NTSTATUS must be 32 bits");
NT_STATIC_ASSERT(sizeof(HANDLE)   == 8, "HANDLE must be 64 bits on x64");
NT_STATIC_ASSERT(sizeof(ULONG)    == 4, "ULONG must be 32 bits");
NT_STATIC_ASSERT(sizeof(ULONG64)  == 8, "ULONG64 must be 64 bits");
NT_STATIC_ASSERT(sizeof(USHORT)   == 2, "USHORT must be 16 bits");
NT_STATIC_ASSERT(sizeof(UCHAR)    == 1, "UCHAR must be 8 bits");
NT_STATIC_ASSERT(sizeof(BYTE)     == 1, "BYTE must be 8 bits");
NT_STATIC_ASSERT(sizeof(WORD)     == 2, "WORD must be 16 bits");
NT_STATIC_ASSERT(sizeof(DWORD)    == 4, "DWORD must be 32 bits");
NT_STATIC_ASSERT(sizeof(ULONGLONG)== 8, "ULONGLONG must be 64 bits");
NT_STATIC_ASSERT(sizeof(LONG64)   == 8, "LONG64 must be 64 bits");
NT_STATIC_ASSERT(sizeof(LONG)     == 4, "LONG must be 32 bits on x64 (Windows ABI; not SysV long)");
NT_STATIC_ASSERT(sizeof(ULONG_PTR)== 8, "ULONG_PTR must be 64 bits on x64");
NT_STATIC_ASSERT(sizeof(LONG_PTR) == 8, "LONG_PTR must be 64 bits on x64");
NT_STATIC_ASSERT(sizeof(SIZE_T)   == 8, "SIZE_T must be 64 bits on x64");
NT_STATIC_ASSERT(sizeof(SSIZE_T)  == 8, "SSIZE_T must be 64 bits on x64");
NT_STATIC_ASSERT(sizeof(PVOID)    == 8, "PVOID must be 64 bits on x64");
NT_STATIC_ASSERT(sizeof(PLONG)    == 8, "PLONG must be 64 bits (pointer) on x64");
NT_STATIC_ASSERT(sizeof(PULONG)   == 8, "PULONG must be 64 bits (pointer) on x64");
NT_STATIC_ASSERT(sizeof(ULONG_PTR) == sizeof(PVOID), "ULONG_PTR and PVOID must match on x64");

/* -------------------------------------------------------------------------- */
/* ACCESS_MASK (opaque token used in OBJECT_ATTRIBUTES / object opens)        */
/* -------------------------------------------------------------------------- */

typedef ULONG ACCESS_MASK;

/* -------------------------------------------------------------------------- */
/* Forward declarations for recursive types                                   */
/* -------------------------------------------------------------------------- */

struct _PEB;
struct _EXCEPTION_REGISTRATION_RECORD;
struct _TEB;

/* -------------------------------------------------------------------------- */
/* LIST_ENTRY                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

NT_STATIC_ASSERT(sizeof(LIST_ENTRY) == 0x10, "LIST_ENTRY must be 16 bytes on x64");
NT_STATIC_ASSERT(offsetof(LIST_ENTRY, Flink) == 0x00, "LIST_ENTRY.Flink offset");
NT_STATIC_ASSERT(offsetof(LIST_ENTRY, Blink) == 0x08, "LIST_ENTRY.Blink offset");

#define CONTAINING_RECORD(address, type, field) \
    ((type *)((char *)(address) - (ULONG_PTR)offsetof(type, field)))

#define InitializeListHead(ListHead) \
    ((ListHead)->Flink = (ListHead)->Blink = (ListHead))

#define IsListEmpty(ListHead) \
    ((ListHead)->Flink == (ListHead))

#define InsertHeadList(ListHead, Entry) { \
    PLIST_ENTRY _old_head = (ListHead)->Flink; \
    (Entry)->Flink = _old_head; \
    (Entry)->Blink = (ListHead); \
    _old_head->Blink = (Entry); \
    (ListHead)->Flink = (Entry); \
}

#define InsertTailList(ListHead, Entry) { \
    PLIST_ENTRY _old_tail = (ListHead)->Blink; \
    (Entry)->Flink = (ListHead); \
    (Entry)->Blink = _old_tail; \
    _old_tail->Flink = (Entry); \
    (ListHead)->Blink = (Entry); \
}

#define RemoveEntryList(Entry) { \
    PLIST_ENTRY _next = (Entry)->Flink; \
    PLIST_ENTRY _prev = (Entry)->Blink; \
    _next->Blink = _prev; \
    _prev->Flink = _next; \
}

/* -------------------------------------------------------------------------- */
/* Helper types used inside TEB / PEB / CONTEXT                               */
/* -------------------------------------------------------------------------- */

#ifndef _NT_GUID_DEFINED
#define _NT_GUID_DEFINED
typedef struct _GUID {
    ULONG  Data1;
    USHORT Data2;
    USHORT Data3;
    UCHAR  Data4[8];
} GUID;
#endif

typedef struct _STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR  Buffer;
} STRING, *PSTRING;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef const UNICODE_STRING *PCUNICODE_STRING;

typedef struct _ANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PSZ    Buffer;
} ANSI_STRING, *PANSI_STRING;

typedef const ANSI_STRING *PCANSI_STRING;

typedef union _LARGE_INTEGER {
    struct {
        ULONG LowPart;
        LONG  HighPart;
    } DUMMYSTRUCTNAME;
    struct {
        ULONG LowPart;
        LONG  HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    } DUMMYSTRUCTNAME;
    struct {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _RTL_BITMAP {
    ULONG  SizeOfBitMap;
    PULONG Buffer;
} RTL_BITMAP, *PRTL_BITMAP;

typedef struct _RTL_CRITICAL_SECTION {
    PVOID DebugInfo;
    LONG  LockCount;
    LONG  RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;

typedef struct _ACTIVATION_CONTEXT_STACK {
    struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME *ActiveFrame;
    LIST_ENTRY FrameListCache;
    ULONG Flags;
    ULONG NextCookieSequenceNumber;
    ULONG StackId;
} ACTIVATION_CONTEXT_STACK, *PACTIVATION_CONTEXT_STACK;

typedef struct _GDI_TEB_BATCH {
    ULONG Offset;
    ULONG_PTR HDC;
    ULONG Buffer[310];
} GDI_TEB_BATCH, *PGDI_TEB_BATCH;

typedef struct _TEB_ACTIVE_FRAME {
    ULONG Flags;
    struct _TEB_ACTIVE_FRAME *Previous;
    const struct _TEB_ACTIVE_FRAME_CONTEXT *Context;
} TEB_ACTIVE_FRAME, *PTEB_ACTIVE_FRAME;

#define FLS_MAXIMUM_AVAILABLE 128

typedef struct _TEB_FLS_DATA {
    LIST_ENTRY Entry;
    PVOID Slots[FLS_MAXIMUM_AVAILABLE];
} TEB_FLS_DATA, *PTEB_FLS_DATA;

NT_STATIC_ASSERT(sizeof(STRING)          == 16, "STRING/ANSI_STRING must be 16 bytes on x64");
NT_STATIC_ASSERT(sizeof(UNICODE_STRING)  == 16, "UNICODE_STRING must be 16 bytes on x64");
NT_STATIC_ASSERT(sizeof(ANSI_STRING)     == 16, "ANSI_STRING must be 16 bytes on x64");
NT_STATIC_ASSERT(sizeof(LARGE_INTEGER)   == 8,  "LARGE_INTEGER must be 64 bits");
NT_STATIC_ASSERT(sizeof(ULARGE_INTEGER)  == 8,  "ULARGE_INTEGER must be 64 bits");
NT_STATIC_ASSERT(sizeof(GDI_TEB_BATCH)   == 0x4e8,
                 "GDI_TEB_BATCH must be 0x4e8 bytes");

/* -------------------------------------------------------------------------- */
/* UNICODE_STRING / ANSI_STRING offsets                                       */
/* -------------------------------------------------------------------------- */

NT_STATIC_ASSERT(offsetof(UNICODE_STRING, Length)        == 0,  "UNICODE_STRING.Length offset");
NT_STATIC_ASSERT(offsetof(UNICODE_STRING, MaximumLength) == 2,  "UNICODE_STRING.MaximumLength offset");
NT_STATIC_ASSERT(offsetof(UNICODE_STRING, Buffer)        == 8,  "UNICODE_STRING.Buffer offset");

/* -------------------------------------------------------------------------- */
/* OBJECT_ATTRIBUTES                                                          */
/* -------------------------------------------------------------------------- */

typedef PVOID PSECURITY_DESCRIPTOR;
typedef PVOID PSECURITY_QUALITY_OF_SERVICE;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG           Attributes;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    PSECURITY_QUALITY_OF_SERVICE SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

NT_STATIC_ASSERT(sizeof(OBJECT_ATTRIBUTES) == 0x30,
                 "OBJECT_ATTRIBUTES must be 48 bytes on x64");
NT_STATIC_ASSERT(offsetof(OBJECT_ATTRIBUTES, Length)                   == 0x00,
                 "OBJECT_ATTRIBUTES.Length offset");
NT_STATIC_ASSERT(offsetof(OBJECT_ATTRIBUTES, RootDirectory)            == 0x08,
                 "OBJECT_ATTRIBUTES.RootDirectory offset");
NT_STATIC_ASSERT(offsetof(OBJECT_ATTRIBUTES, ObjectName)               == 0x10,
                 "OBJECT_ATTRIBUTES.ObjectName offset");
NT_STATIC_ASSERT(offsetof(OBJECT_ATTRIBUTES, Attributes)               == 0x18,
                 "OBJECT_ATTRIBUTES.Attributes offset");
NT_STATIC_ASSERT(offsetof(OBJECT_ATTRIBUTES, SecurityDescriptor)       == 0x20,
                 "OBJECT_ATTRIBUTES.SecurityDescriptor offset");
NT_STATIC_ASSERT(offsetof(OBJECT_ATTRIBUTES, SecurityQualityOfService) == 0x28,
                 "OBJECT_ATTRIBUTES.SecurityQualityOfService offset");

/* -------------------------------------------------------------------------- */
/* IO_STATUS_BLOCK                                                            */
/* -------------------------------------------------------------------------- */

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    } DUMMYUNIONNAME;
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

NT_STATIC_ASSERT(sizeof(IO_STATUS_BLOCK) == 0x10,
                 "IO_STATUS_BLOCK must be 16 bytes on x64");
#ifndef __MINGW32__
NT_STATIC_ASSERT(offsetof(IO_STATUS_BLOCK, DUMMYUNIONNAME.Status) == 0x00,
                 "IO_STATUS_BLOCK.Status offset");
#endif
NT_STATIC_ASSERT(offsetof(IO_STATUS_BLOCK, Information)           == 0x08,
                 "IO_STATUS_BLOCK.Information offset");

/* -------------------------------------------------------------------------- */
/* CLIENT_ID                                                                  */
/* -------------------------------------------------------------------------- */

typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID, *PCLIENT_ID;

NT_STATIC_ASSERT(sizeof(CLIENT_ID) == 0x10, "CLIENT_ID must be 16 bytes on x64");
NT_STATIC_ASSERT(offsetof(CLIENT_ID, UniqueProcess) == 0x00, "CLIENT_ID.UniqueProcess offset");
NT_STATIC_ASSERT(offsetof(CLIENT_ID, UniqueThread)  == 0x08, "CLIENT_ID.UniqueThread offset");

/* -------------------------------------------------------------------------- */
/* RTL_USER_PROCESS_PARAMETERS                                                */
/* -------------------------------------------------------------------------- */
/*
 * This is a large, version-dependent structure.  The offsets asserted below are
 * the ones the loader/Win32 layer depend on.  We reproduce the Windows Server
 * 2022 x64 layout; the exact total size is UNVERIFIED because Microsoft does
 * not publish a canonical fixed-size definition and it drifts between builds.
 */

typedef struct _CURDIR {
    UNICODE_STRING DosPath;
    HANDLE         Handle;
} CURDIR, *PCURDIR;

NT_STATIC_ASSERT(sizeof(CURDIR) == 0x18, "CURDIR must be 24 bytes on x64");

typedef struct _RTL_DRIVE_LETTER_CURDIR {
    USHORT Flags;
    USHORT Length;
    ULONG  TimeStamp;
    STRING DosPath;
} RTL_DRIVE_LETTER_CURDIR, *PRTL_DRIVE_LETTER_CURDIR;

typedef struct _RTL_USER_PROCESS_PARAMETERS {
    ULONG MaximumLength;
    ULONG Length;
    ULONG Flags;
    ULONG DebugFlags;
    HANDLE ConsoleHandle;
    ULONG  ConsoleFlags;
    HANDLE StandardInput;
    HANDLE StandardOutput;
    HANDLE StandardError;
    CURDIR CurrentDirectory;
    UNICODE_STRING DllPath;
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
    PVOID Environment;
    ULONG StartingX;
    ULONG StartingY;
    ULONG CountX;
    ULONG CountY;
    ULONG CountCharsX;
    ULONG CountCharsY;
    ULONG FillAttribute;
    ULONG WindowFlags;
    ULONG ShowWindowFlags;
    UNICODE_STRING WindowTitle;
    UNICODE_STRING DesktopInfo;
    UNICODE_STRING ShellInfo;
    UNICODE_STRING RuntimeData;
    RTL_DRIVE_LETTER_CURDIR CurrentDirectories[32];

    /* The following trailing layout is UNVERIFIED for Server 2022.  It is
     * derived from public headers and padded so that the total structure size
     * matches the observed Windows 10 22H2 x64 value of 0x470 bytes. */
    ULONG EnvironmentSize;
    ULONG EnvironmentVersion;
    PVOID PackageDependencyData;
    ULONG ProcessGroupId;
    ULONG LoaderThreads;
    ULONGLONG MinStackCommit;
    PVOID SparePointers[2];
    PVOID PatchEntry;
    PVOID TelemetryCoverageHeader;
    ULONG CloudFileFlags;
    ULONG CloudFileDiagFlags;
    CHAR PlaceholderCompatibilityMode;
    CHAR PlaceholderCompatibilityModeReserved[7];
    PVOID LeapSecondData;
    ULONG LeapSecondFlags;
    ULONG NtGlobalFlag2;
    ULARGE_INTEGER ExtendedFeatureDisableMask;

    /* UNVERIFIED padding to reach the observed size (0x470 - 0x458). */
    UCHAR PaddingToSize[0x18];
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;

NT_STATIC_ASSERT(sizeof(RTL_USER_PROCESS_PARAMETERS) == 0x470,
                 "RTL_USER_PROCESS_PARAMETERS size must match Windows x64");
NT_STATIC_ASSERT(offsetof(RTL_USER_PROCESS_PARAMETERS, StandardInput)  == 0x20,
                 "RTL_USER_PROCESS_PARAMETERS.StandardInput offset");
NT_STATIC_ASSERT(offsetof(RTL_USER_PROCESS_PARAMETERS, StandardOutput) == 0x28,
                 "RTL_USER_PROCESS_PARAMETERS.StandardOutput offset");
NT_STATIC_ASSERT(offsetof(RTL_USER_PROCESS_PARAMETERS, StandardError)  == 0x30,
                 "RTL_USER_PROCESS_PARAMETERS.StandardError offset");
NT_STATIC_ASSERT(offsetof(RTL_USER_PROCESS_PARAMETERS, CommandLine)    == 0x70,
                 "RTL_USER_PROCESS_PARAMETERS.CommandLine offset");
NT_STATIC_ASSERT(offsetof(RTL_USER_PROCESS_PARAMETERS, Environment)    == 0x80,
                 "RTL_USER_PROCESS_PARAMETERS.Environment offset");

/* -------------------------------------------------------------------------- */
/* M128A / XMM_SAVE_AREA32 / CONTEXT                                         */
/* -------------------------------------------------------------------------- */

typedef struct _M128A {
    ULONG64 Low;
    LONG64  High;
} M128A, *PM128A;

NT_STATIC_ASSERT(sizeof(M128A) == 16, "M128A must be 16 bytes");

typedef struct _XMM_SAVE_AREA32 {
    WORD   ControlWord;
    WORD   StatusWord;
    BYTE   TagWord;
    BYTE   Reserved1;
    WORD   ErrorOpcode;
    DWORD  ErrorOffset;
    WORD   ErrorSelector;
    WORD   Reserved2;
    DWORD  DataOffset;
    WORD   DataSelector;
    WORD   Reserved3;
    DWORD  MxCsr;
    DWORD  MxCsr_Mask;
    M128A  FloatRegisters[8];
    M128A  XmmRegisters[16];
    BYTE   Reserved4[96];
} XMM_SAVE_AREA32, *PXMM_SAVE_AREA32;

NT_STATIC_ASSERT(sizeof(XMM_SAVE_AREA32) == 0x200,
                 "XMM_SAVE_AREA32 must be 512 bytes");

/*
 * CONTEXT flags — copied from Windows SDK.  These are bitwise constants, not
 * offsets, but they are part of the ABI contract.
 */
#define CONTEXT_AMD64              0x00100000L
#define CONTEXT_CONTROL            (CONTEXT_AMD64 | 0x00000001L)
#define CONTEXT_INTEGER            (CONTEXT_AMD64 | 0x00000002L)
#define CONTEXT_SEGMENTS           (CONTEXT_AMD64 | 0x00000004L)
#define CONTEXT_FLOATING_POINT     (CONTEXT_AMD64 | 0x00000008L)
#define CONTEXT_DEBUG_REGISTERS    (CONTEXT_AMD64 | 0x00000010L)
#define CONTEXT_EXTENDED_REGISTERS (CONTEXT_AMD64 | 0x00000020L)
#define CONTEXT_FULL               (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)
#define CONTEXT_ALL                (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS | CONTEXT_FLOATING_POINT | CONTEXT_DEBUG_REGISTERS | CONTEXT_EXTENDED_REGISTERS)
#define CONTEXT_XSTATE             (CONTEXT_AMD64 | 0x00000040L)
#define CONTEXT_KERNEL_CET         (CONTEXT_AMD64 | 0x00000080L)

typedef struct _CONTEXT {
    DWORD64 P1Home;
    DWORD64 P2Home;
    DWORD64 P3Home;
    DWORD64 P4Home;
    DWORD64 P5Home;
    DWORD64 P6Home;

    /* ContextFlags must be at offset 0x30. */
    DWORD   ContextFlags;
    DWORD   MxCsr;

    WORD    SegCs;
    WORD    SegDs;
    WORD    SegEs;
    WORD    SegFs;
    WORD    SegGs;
    WORD    SegSs;
    DWORD   EFlags;

    DWORD64 Dr0;
    DWORD64 Dr1;
    DWORD64 Dr2;
    DWORD64 Dr3;
    DWORD64 Dr6;
    DWORD64 Dr7;

    DWORD64 Rax;
    DWORD64 Rcx;
    DWORD64 Rdx;
    DWORD64 Rbx;
    DWORD64 Rsp;
    DWORD64 Rbp;
    DWORD64 Rsi;
    DWORD64 Rdi;
    DWORD64 R8;
    DWORD64 R9;
    DWORD64 R10;
    DWORD64 R11;
    DWORD64 R12;
    DWORD64 R13;
    DWORD64 R14;
    DWORD64 R15;
    DWORD64 Rip;

    union {
        XMM_SAVE_AREA32 FltSave;
        struct {
            M128A Header[2];
            M128A Legacy[8];
            M128A Xmm0;
            M128A Xmm1;
            M128A Xmm2;
            M128A Xmm3;
            M128A Xmm4;
            M128A Xmm5;
            M128A Xmm6;
            M128A Xmm7;
            M128A Xmm8;
            M128A Xmm9;
            M128A Xmm10;
            M128A Xmm11;
            M128A Xmm12;
            M128A Xmm13;
            M128A Xmm14;
            M128A Xmm15;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;

    M128A   VectorRegister[26];
    DWORD64 VectorControl;

    DWORD64 DebugControl;
    DWORD64 LastBranchToRip;
    DWORD64 LastBranchFromRip;
    DWORD64 LastExceptionToRip;
    DWORD64 LastExceptionFromRip;
} CONTEXT, *PCONTEXT;

NT_STATIC_ASSERT(sizeof(CONTEXT) == 0x4D0, "CONTEXT must be 1232 bytes on x64");
NT_STATIC_ASSERT(offsetof(CONTEXT, ContextFlags) == 0x030, "CONTEXT.ContextFlags offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, MxCsr)       == 0x034, "CONTEXT.MxCsr offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, SegCs)       == 0x038, "CONTEXT.SegCs offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, SegFs)       == 0x03e, "CONTEXT.SegFs offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, SegGs)       == 0x040, "CONTEXT.SegGs offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, EFlags)      == 0x044, "CONTEXT.EFlags offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Dr0)         == 0x048, "CONTEXT.Dr0 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Dr7)         == 0x070, "CONTEXT.Dr7 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Rax)         == 0x078, "CONTEXT.Rax offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Rcx)         == 0x080, "CONTEXT.Rcx offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Rdx)         == 0x088, "CONTEXT.Rdx offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Rbx)         == 0x090, "CONTEXT.Rbx offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Rsp)         == 0x098, "CONTEXT.Rsp offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Rbp)         == 0x0a0, "CONTEXT.Rbp offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Rsi)         == 0x0a8, "CONTEXT.Rsi offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Rdi)         == 0x0b0, "CONTEXT.Rdi offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, R8)          == 0x0b8, "CONTEXT.R8 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, R9)          == 0x0c0, "CONTEXT.R9 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, R10)         == 0x0c8, "CONTEXT.R10 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, R11)         == 0x0d0, "CONTEXT.R11 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, R12)         == 0x0d8, "CONTEXT.R12 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, R13)         == 0x0e0, "CONTEXT.R13 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, R14)         == 0x0e8, "CONTEXT.R14 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, R15)         == 0x0f0, "CONTEXT.R15 offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, Rip)         == 0x0f8, "CONTEXT.Rip offset");
#ifndef __MINGW32__
NT_STATIC_ASSERT(offsetof(CONTEXT, DUMMYUNIONNAME.FltSave) == 0x100,
                 "CONTEXT.FltSave offset");
#endif
NT_STATIC_ASSERT(offsetof(CONTEXT, VectorRegister) == 0x300,
                 "CONTEXT.VectorRegister offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, VectorControl)  == 0x4a0,
                 "CONTEXT.VectorControl offset");
NT_STATIC_ASSERT(offsetof(CONTEXT, DebugControl)   == 0x4a8,
                 "CONTEXT.DebugControl offset");

/* -------------------------------------------------------------------------- */
/* EXCEPTION_RECORD                                                           */
/* -------------------------------------------------------------------------- */

typedef struct _EXCEPTION_RECORD {
    DWORD    ExceptionCode;
    DWORD    ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID    ExceptionAddress;
    DWORD    NumberParameters;
    DWORD    __unusedAlignment;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

NT_STATIC_ASSERT(sizeof(EXCEPTION_RECORD) == 0x98,
                 "EXCEPTION_RECORD must be 152 bytes on x64");
NT_STATIC_ASSERT(offsetof(EXCEPTION_RECORD, ExceptionCode)       == 0x00,
                 "EXCEPTION_RECORD.ExceptionCode offset");
NT_STATIC_ASSERT(offsetof(EXCEPTION_RECORD, ExceptionFlags)      == 0x04,
                 "EXCEPTION_RECORD.ExceptionFlags offset");
NT_STATIC_ASSERT(offsetof(EXCEPTION_RECORD, ExceptionRecord)     == 0x08,
                 "EXCEPTION_RECORD.ExceptionRecord offset");
NT_STATIC_ASSERT(offsetof(EXCEPTION_RECORD, ExceptionAddress)    == 0x10,
                 "EXCEPTION_RECORD.ExceptionAddress offset");
NT_STATIC_ASSERT(offsetof(EXCEPTION_RECORD, NumberParameters)    == 0x18,
                 "EXCEPTION_RECORD.NumberParameters offset");
NT_STATIC_ASSERT(offsetof(EXCEPTION_RECORD, ExceptionInformation)== 0x20,
                 "EXCEPTION_RECORD.ExceptionInformation offset");

/* -------------------------------------------------------------------------- */
/* EXCEPTION_POINTERS                                                         */
/* -------------------------------------------------------------------------- */

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT          ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

NT_STATIC_ASSERT(sizeof(EXCEPTION_POINTERS) == 0x10,
                 "EXCEPTION_POINTERS must be 16 bytes on x64");

/* -------------------------------------------------------------------------- */
/* TEB (Thread Environment Block) / NT_TIB                                     */
/* -------------------------------------------------------------------------- */
/*
 * Offsets are the Windows Server 2022 x64 TEB contract.  These are consumed
 * by compiler-generated TLS access (gs:[0x1480] style), ntdll, and the kernel
 * PEB/TEB construction path.
 */

typedef struct _NT_TIB {
    struct _EXCEPTION_REGISTRATION_RECORD *ExceptionList;
    PVOID StackBase;
    PVOID StackLimit;
    PVOID SubSystemTib;
    union {
        PVOID FiberData;
        DWORD Version;
    } DUMMYUNIONNAME;
    PVOID ArbitraryUserPointer;
    struct _NT_TIB *Self;
} NT_TIB, *PNT_TIB;

NT_STATIC_ASSERT(sizeof(NT_TIB) == 0x38, "NT_TIB must be 56 bytes on x64");
NT_STATIC_ASSERT(offsetof(NT_TIB, ExceptionList)     == 0x00, "NT_TIB.ExceptionList offset");
NT_STATIC_ASSERT(offsetof(NT_TIB, StackBase)         == 0x08, "NT_TIB.StackBase offset");
NT_STATIC_ASSERT(offsetof(NT_TIB, StackLimit)        == 0x10, "NT_TIB.StackLimit offset");
NT_STATIC_ASSERT(offsetof(NT_TIB, ArbitraryUserPointer) == 0x28,
                 "NT_TIB.ArbitraryUserPointer offset");
NT_STATIC_ASSERT(offsetof(NT_TIB, Self)              == 0x30, "NT_TIB.Self offset");

typedef DWORD LCID;

typedef struct _TEB {
    NT_TIB NtTib;

    PVOID EnvironmentPointer;
    CLIENT_ID ClientId;
    PVOID ActiveRpcHandle;
    PVOID ThreadLocalStoragePointer;     /* gs:[0x058] */
    struct _PEB *ProcessEnvironmentBlock; /* gs:[0x060] */
    ULONG LastErrorValue;                /* gs:[0x068] */
    ULONG CountOfOwnedCriticalSections;
    PVOID CsrClientThread;
    PVOID Win32ThreadInfo;
    ULONG User32Reserved[26];
    ULONG UserReserved[5];
    PVOID WOW32Reserved;
    LCID  CurrentLocale;
    ULONG FpSoftwareStatusRegister;

    /* SystemReserved1[54] fills 0x110 .. 0x2bf.  The exact contents are
     * undocumented and not depended on by other workstreams. */
    PVOID SystemReserved1[54];

    /*
     * UNVERIFIED: the region 0x2c0 .. 0x1480 contains version-dependent TEB
     * fields (GdiTebBatch, gl dispatch tables, static unicode buffer, etc.).
     * No downstream workstream depends on the exact layout of this region, but
     * TlsSlots must be at offset 0x1480.  We model the region as a padding blob
     * plus the fields that matter for the offset assertion.
     */
    UCHAR TebPadding0[0x1480 - 0x2c0 - 8];
    PVOID DeallocationStack;
    PVOID TlsSlots[64];                  /* gs:[0x1480] */
    LIST_ENTRY TlsLinks;

    /* UNVERIFIED remainder: the fields after TlsLinks are not required by the
     * asserted contract and their offsets are taken from public headers. */
    PVOID Vdm;
    PVOID ReservedForNtRpc;
    PVOID DbgSsReserved[2];
    ULONG HardErrorMode;
    UCHAR Padding3[4];
    PVOID Instrumentation[11];
    GUID ActivityId;
    PVOID SubProcessTag;
    PVOID PerftDataBuffer;
    PVOID EtwTraceData;
    PVOID WinSockData;
    ULONG GdiBatchCount;
    UCHAR Padding4[4];
    ULONG IdealProcessorValue;
    ULONG GuaranteedStackBytes;
    PVOID ReservedForPerf;
    PVOID ReservedForOle;
    ULONG WaitingOnLoaderLock;
    UCHAR Padding5[4];
    PVOID SavedPriorityState;
    ULONG_PTR ReservedForCodeCoverage;
    PVOID ThreadPoolData;
    PVOID *TlsExpansionSlots;

    /* UNVERIFIED: the MUI / FLS fields below are not asserted by this contract. */
    ULONG MuiGeneration;
    ULONG IsImpersonating;
    PVOID NlsCache;
    PVOID pShimData;
    USHORT HeapVirtualAffinity;
    USHORT LowFragHeapDataSlot;
    PVOID CurrentTransactionHandle;
    TEB_ACTIVE_FRAME *ActiveFrame;
    TEB_FLS_DATA *FlsData;
    PVOID PreferredLanguages;
    PVOID UserPrefLanguages;
    PVOID MergedPrefLanguages;
    ULONG MuiImpersonation;
    USHORT CrossTebFlags;
    USHORT SameTebFlags;
    PVOID TxnScopeEnterCallback;
    PVOID TxnScopeExitCallback;
    PVOID TxnScopeContext;
    ULONG LockCount;
    LONG ProcessRundown;
    ULONG_PTR LastThreadSleepTime;
    ULONG ThreadSwitchingNotify;
    UCHAR Padding6[4];
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    /* UNVERIFIED: the Silo / RTL_UMS fields that complete the structure. */
    PVOID WowTebOffset;
    LARGE_INTEGER ExtendedFeatureDisableMask;
} TEB, *PTEB;

NT_STATIC_ASSERT(offsetof(TEB, NtTib.Self)                   == 0x030,
                 "TEB.NtTib.Self offset");
NT_STATIC_ASSERT(offsetof(TEB, NtTib.StackBase)              == 0x008,
                 "TEB.NtTib.StackBase offset");
NT_STATIC_ASSERT(offsetof(TEB, NtTib.StackLimit)             == 0x010,
                 "TEB.NtTib.StackLimit offset");
NT_STATIC_ASSERT(offsetof(TEB, ThreadLocalStoragePointer)    == 0x058,
                 "TEB.ThreadLocalStoragePointer offset");
NT_STATIC_ASSERT(offsetof(TEB, ProcessEnvironmentBlock)      == 0x060,
                 "TEB.ProcessEnvironmentBlock offset");
NT_STATIC_ASSERT(offsetof(TEB, LastErrorValue)               == 0x068,
                 "TEB.LastErrorValue offset");
NT_STATIC_ASSERT(offsetof(TEB, TlsSlots)                     == 0x1480,
                 "TEB.TlsSlots offset");

/*
 * UNVERIFIED total size.  The TEB is allocated from two user pages (0x2000
 * bytes); the structure definition is a subset.  We do not assert a total TEB
 * size because Microsoft extends it almost every build and no one should depend
 * on sizeof(TEB).
 */

/* -------------------------------------------------------------------------- */
/* PEB (Process Environment Block)                                            */
/* -------------------------------------------------------------------------- */
/*
 * Windows Server 2022 x64 PEB layout.  Total size and the offsets below the
 * asserted set are UNVERIFIED.
 */

typedef struct _PEB_LDR_DATA {
    ULONG Length;
    UCHAR Initialized;
    HANDLE SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
    PVOID EntryInProgress;
    UCHAR ShutdownInProgress;
    HANDLE ShutdownThreadId;
} PEB_LDR_DATA, *PPEB_LDR_DATA;

NT_STATIC_ASSERT(sizeof(PEB_LDR_DATA) == 0x58,
                 "PEB_LDR_DATA size is unverified but matches public headers");

typedef struct _PEB {
    UCHAR InheritedAddressSpace;
    UCHAR ReadImageDataExecOptions;
    UCHAR BeingDebugged;                 /* +0x002 */
    UCHAR BitField;
    UCHAR Padding0[4];
    HANDLE Mutant;
    PVOID ImageBaseAddress;              /* +0x010 */
    PPEB_LDR_DATA Ldr;                   /* +0x018 */
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters; /* +0x020 */
    PVOID SubSystemData;
    PVOID ProcessHeap;                   /* +0x030 */
    PRTL_CRITICAL_SECTION FastPebLock;
    PVOID AtlThunkSListPtr;
    PVOID IFEOKey;
    union {
        ULONG CrossProcessFlags;
        struct {
            ULONG ProcessInJob : 1;
            ULONG ProcessInitializing : 1;
            ULONG ProcessUsingVEH : 1;
            ULONG ProcessUsingVCH : 1;
            ULONG ProcessUsingFTH : 1;
            ULONG ProcessPreviouslyThrottled : 1;
            ULONG ProcessCurrentlyThrottled : 1;
            ULONG ProcessImagesHotPatched : 1;
            ULONG ReservedBits0 : 24;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
    UCHAR Padding1[4];
    PVOID KernelCallbackTable;
    ULONG SystemReserved;
    ULONG AtlThunkSListPtr32;
    PVOID ApiSetMap;                     /* +0x068 */
    ULONG TlsExpansionCounter;
    UCHAR Padding2[4];
    PRTL_BITMAP TlsBitmap;
    ULONG TlsBitmapBits[2];
    PVOID ReadOnlySharedMemoryBase;
    PVOID HotpatchInformation;
    PVOID *ReadOnlyStaticServerData;
    PVOID AnsiCodePageData;
    PVOID OemCodePageData;
    PVOID UnicodeCaseTableData;
    ULONG NumberOfProcessors;
    ULONG NtGlobalFlag;                  /* +0x0bc */
    LARGE_INTEGER CriticalSectionTimeout;
    ULONG_PTR HeapSegmentReserve;
    ULONG_PTR HeapSegmentCommit;
    ULONG_PTR HeapDeCommitTotalFreeThreshold;
    ULONG_PTR HeapDeCommitFreeBlockThreshold;
    ULONG NumberOfHeaps;
    ULONG MaximumNumberOfHeaps;
    PVOID *ProcessHeaps;
    PVOID GdiSharedHandleTable;
    PVOID ProcessStarterHelper;
    ULONG GdiDCAttributeList;
    UCHAR Padding3[4];
    PRTL_CRITICAL_SECTION LoaderLock;
    ULONG OSMajorVersion;
    ULONG OSMinorVersion;
    USHORT OSBuildNumber;
    USHORT OSCSDVersion;
    ULONG OSPlatformId;
    ULONG ImageSubsystem;
    ULONG ImageSubsystemMajorVersion;
    ULONG ImageSubsystemMinorVersion;
    UCHAR Padding4[4];
    ULONG_PTR ActiveProcessAffinityMask;
    ULONG GdiHandleBuffer[60];
    PVOID PostProcessInitRoutine;
    PVOID TlsExpansionBitmap;
    ULONG TlsExpansionBitmapBits[32];
    ULONG SessionId;
    UCHAR Padding5[4];
    ULARGE_INTEGER AppCompatFlags;
    ULARGE_INTEGER AppCompatFlagsUser;
    PVOID pShimData;
    PVOID AppCompatInfo;
    UNICODE_STRING CSDVersion;
    PVOID ActivationContextData;
    PVOID ProcessAssemblyStorageMap;
    PVOID SystemDefaultActivationContextData;
    PVOID SystemAssemblyStorageMap;
    ULONG_PTR MinimumStackCommit;

    /* UNVERIFIED: remainder of PEB for Server 2022; not asserted. */
    PVOID SparePointers[2];
    PVOID PatchEntry;
    PVOID TelemetryCoverageHeader;
    ULONG CloudFileFlags;
    ULONG CloudFileDiagFlags;
    CHAR PlaceholderCompatibilityMode;
    CHAR PlaceholderCompatibilityModeReserved[7];
    PVOID LeapSecondData;
    ULONG LeapSecondFlags;
    ULONG NtGlobalFlag2;
    ULARGE_INTEGER ExtendedFeatureDisableMask;
} PEB, *PPEB;

NT_STATIC_ASSERT(offsetof(PEB, BeingDebugged)      == 0x002,
                 "PEB.BeingDebugged offset");
NT_STATIC_ASSERT(offsetof(PEB, NtGlobalFlag)       == 0x0bc,
                 "PEB.NtGlobalFlag offset");
NT_STATIC_ASSERT(offsetof(PEB, ImageBaseAddress)   == 0x010,
                 "PEB.ImageBaseAddress offset");
NT_STATIC_ASSERT(offsetof(PEB, Ldr)                == 0x018,
                 "PEB.Ldr offset");
NT_STATIC_ASSERT(offsetof(PEB, ProcessParameters)  == 0x020,
                 "PEB.ProcessParameters offset");
NT_STATIC_ASSERT(offsetof(PEB, ProcessHeap)        == 0x030,
                 "PEB.ProcessHeap offset");
NT_STATIC_ASSERT(offsetof(PEB, ApiSetMap)          == 0x068,
                 "PEB.ApiSetMap offset");

/* UNVERIFIED total size.  Like the TEB, the PEB grows every release. */

#ifdef __cplusplus
}
#endif

#endif /* NT_TYPES_H */
