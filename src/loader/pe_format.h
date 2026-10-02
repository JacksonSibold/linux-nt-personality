/* pe_format.h - PE32+ type definitions for the native Windows personality loader.
 *
 * This header is designed to compile both as a freestanding kernel component
 * (included from fs/binfmt_pe.c) and as a userspace library.  It uses fixed-width
 * integer types, contains no libc code, no floating point, and no hidden
 * allocations.  Structure layouts match the Microsoft Portable Executable
 * specification for x64 (PE32+).
 */
#ifndef PE_FORMAT_H
#define PE_FORMAT_H

#ifdef __KERNEL__
#  include <linux/types.h>
#else
#  include <stddef.h>
#  include <stdint.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
#endif

/* -------------------------------------------------------------------------- */
/* DOS header                                                                 */
/* -------------------------------------------------------------------------- */
#define IMAGE_DOS_SIGNATURE                 0x5A4D       /* MZ */

typedef struct _IMAGE_DOS_HEADER {
    u16 e_magic;
    u16 e_cblp;
    u16 e_cp;
    u16 e_crlc;
    u16 e_cparhdr;
    u16 e_minalloc;
    u16 e_maxalloc;
    u16 e_ss;
    u16 e_sp;
    u16 e_csum;
    u16 e_ip;
    u16 e_cs;
    u16 e_lfarlc;
    u16 e_ovno;
    u16 e_res[4];
    u16 e_oemid;
    u16 e_oeminfo;
    u16 e_res2[10];
    u32 e_lfanew;
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;

_Static_assert(sizeof(IMAGE_DOS_HEADER) == 64, "IMAGE_DOS_HEADER size");

/* -------------------------------------------------------------------------- */
/* COFF file header                                                           */
/* -------------------------------------------------------------------------- */
#define IMAGE_FILE_MACHINE_AMD64            0x8664
#define IMAGE_FILE_MACHINE_I386             0x014C
#define IMAGE_FILE_MACHINE_ARM64            0xAA64

#define IMAGE_FILE_RELOCS_STRIPPED          0x0001
#define IMAGE_FILE_EXECUTABLE_IMAGE         0x0002
#define IMAGE_FILE_LINE_NUMS_STRIPPED       0x0004
#define IMAGE_FILE_LOCAL_SYMS_STRIPPED      0x0008
#define IMAGE_FILE_AGGRESSIVE_WS_TRIM       0x0010
#define IMAGE_FILE_LARGE_ADDRESS_AWARE      0x0020
#define IMAGE_FILE_BYTES_REVERSED_LO        0x0080
#define IMAGE_FILE_32BIT_MACHINE            0x0100
#define IMAGE_FILE_DEBUG_STRIPPED           0x0200
#define IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP  0x0400
#define IMAGE_FILE_NET_RUN_FROM_SWAP        0x0800
#define IMAGE_FILE_SYSTEM                   0x1000
#define IMAGE_FILE_DLL                      0x2000
#define IMAGE_FILE_UP_SYSTEM_ONLY           0x4000
#define IMAGE_FILE_BYTES_REVERSED_HI        0x8000

typedef struct _IMAGE_FILE_HEADER {
    u16 Machine;
    u16 NumberOfSections;
    u32 TimeDateStamp;
    u32 PointerToSymbolTable;
    u32 NumberOfSymbols;
    u16 SizeOfOptionalHeader;
    u16 Characteristics;
} IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;

_Static_assert(sizeof(IMAGE_FILE_HEADER) == 20, "IMAGE_FILE_HEADER size");

/* -------------------------------------------------------------------------- */
/* Data directory                                                             */
/* -------------------------------------------------------------------------- */
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES    16

typedef struct _IMAGE_DATA_DIRECTORY {
    u32 VirtualAddress;
    u32 Size;
} IMAGE_DATA_DIRECTORY, *PIMAGE_DATA_DIRECTORY;

_Static_assert(sizeof(IMAGE_DATA_DIRECTORY) == 8, "IMAGE_DATA_DIRECTORY size");

/* -------------------------------------------------------------------------- */
/* Optional header (PE32+)                                                    */
/* -------------------------------------------------------------------------- */
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC       0x10B
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC       0x20B
#define IMAGE_ROM_OPTIONAL_HDR_MAGIC        0x107

#define IMAGE_SUBSYSTEM_UNKNOWN             0
#define IMAGE_SUBSYSTEM_NATIVE              1
#define IMAGE_SUBSYSTEM_WINDOWS_GUI         2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI         3
#define IMAGE_SUBSYSTEM_POSIX_CUI           7
#define IMAGE_SUBSYSTEM_WINDOWS_CE_GUI      9
#define IMAGE_SUBSYSTEM_EFI_APPLICATION     10
#define IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER 11
#define IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER  12
#define IMAGE_SUBSYSTEM_EFI_ROM             13
#define IMAGE_SUBSYSTEM_XBOX                14
#define IMAGE_SUBSYSTEM_WINDOWS_BOOT_APPLICATION 16

#define IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA    0x0020
#define IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE       0x0040
#define IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY    0x0080
#define IMAGE_DLLCHARACTERISTICS_NX_COMPAT          0x0100
#define IMAGE_DLLCHARACTERISTICS_NO_ISOLATION       0x0200
#define IMAGE_DLLCHARACTERISTICS_NO_SEH             0x0400
#define IMAGE_DLLCHARACTERISTICS_NO_BIND            0x0800
#define IMAGE_DLLCHARACTERISTICS_APPCONTAINER       0x1000
#define IMAGE_DLLCHARACTERISTICS_WDM_DRIVER         0x2000
#define IMAGE_DLLCHARACTERISTICS_GUARD_CF           0x4000
#define IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE 0x8000

typedef struct _IMAGE_OPTIONAL_HEADER64 {
    u16 Magic;
    u8  MajorLinkerVersion;
    u8  MinorLinkerVersion;
    u32 SizeOfCode;
    u32 SizeOfInitializedData;
    u32 SizeOfUninitializedData;
    u32 AddressOfEntryPoint;
    u32 BaseOfCode;
    u64 ImageBase;
    u32 SectionAlignment;
    u32 FileAlignment;
    u16 MajorOperatingSystemVersion;
    u16 MinorOperatingSystemVersion;
    u16 MajorImageVersion;
    u16 MinorImageVersion;
    u16 MajorSubsystemVersion;
    u16 MinorSubsystemVersion;
    u32 Win32VersionValue;
    u32 SizeOfImage;
    u32 SizeOfHeaders;
    u32 CheckSum;
    u16 Subsystem;
    u16 DllCharacteristics;
    u64 SizeOfStackReserve;
    u64 SizeOfStackCommit;
    u64 SizeOfHeapReserve;
    u64 SizeOfHeapCommit;
    u32 LoaderFlags;
    u32 NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64, *PIMAGE_OPTIONAL_HEADER64;

_Static_assert(sizeof(IMAGE_OPTIONAL_HEADER64) == 240, "IMAGE_OPTIONAL_HEADER64 size");

/* -------------------------------------------------------------------------- */
/* NT headers                                                                 */
/* -------------------------------------------------------------------------- */
#define IMAGE_NT_SIGNATURE                  0x00004550   /* PE\0\0 */

typedef struct _IMAGE_NT_HEADERS64 {
    u32 Signature;
    IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64, *PIMAGE_NT_HEADERS64;

_Static_assert(sizeof(IMAGE_NT_HEADERS64) == 264, "IMAGE_NT_HEADERS64 size");

/* -------------------------------------------------------------------------- */
/* Section header                                                             */
/* -------------------------------------------------------------------------- */
#define IMAGE_SIZEOF_SHORT_NAME             8

typedef struct _IMAGE_SECTION_HEADER {
    u8  Name[IMAGE_SIZEOF_SHORT_NAME];
    union {
        u32 PhysicalAddress;
        u32 VirtualSize;
    } Misc;
    u32 VirtualAddress;
    u32 SizeOfRawData;
    u32 PointerToRawData;
    u32 PointerToRelocations;
    u32 PointerToLinenumbers;
    u16 NumberOfRelocations;
    u16 NumberOfLinenumbers;
    u32 Characteristics;
} IMAGE_SECTION_HEADER, *PIMAGE_SECTION_HEADER;

_Static_assert(sizeof(IMAGE_SECTION_HEADER) == 40, "IMAGE_SECTION_HEADER size");

#define IMAGE_SCN_TYPE_NO_PAD               0x00000008
#define IMAGE_SCN_CNT_CODE                  0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA      0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA    0x00000080
#define IMAGE_SCN_LNK_OTHER                 0x00000100
#define IMAGE_SCN_LNK_INFO                  0x00000200
#define IMAGE_SCN_LNK_REMOVE                0x00000800
#define IMAGE_SCN_LNK_COMDAT                0x00001000
#define IMAGE_SCN_GPREL                     0x00008000
#define IMAGE_SCN_MEM_PURGEABLE             0x00020000
#define IMAGE_SCN_MEM_16BIT                 0x00020000
#define IMAGE_SCN_MEM_LOCKED                0x00040000
#define IMAGE_SCN_MEM_PRELOAD               0x00080000
#define IMAGE_SCN_ALIGN_1BYTES              0x00100000
#define IMAGE_SCN_ALIGN_2BYTES              0x00200000
#define IMAGE_SCN_ALIGN_4BYTES              0x00300000
#define IMAGE_SCN_ALIGN_8BYTES              0x00400000
#define IMAGE_SCN_ALIGN_16BYTES             0x00500000
#define IMAGE_SCN_ALIGN_32BYTES             0x00600000
#define IMAGE_SCN_ALIGN_64BYTES             0x00700000
#define IMAGE_SCN_ALIGN_128BYTES            0x00800000
#define IMAGE_SCN_ALIGN_256BYTES            0x00900000
#define IMAGE_SCN_ALIGN_512BYTES            0x00A00000
#define IMAGE_SCN_ALIGN_1024BYTES           0x00B00000
#define IMAGE_SCN_ALIGN_2048BYTES           0x00C00000
#define IMAGE_SCN_ALIGN_4096BYTES           0x00D00000
#define IMAGE_SCN_ALIGN_8192BYTES           0x00E00000
#define IMAGE_SCN_LNK_NRELOC_OVFL           0x01000000
#define IMAGE_SCN_MEM_DISCARDABLE           0x02000000
#define IMAGE_SCN_MEM_NOT_CACHED            0x04000000
#define IMAGE_SCN_MEM_NOT_PAGED             0x08000000
#define IMAGE_SCN_MEM_SHARED                0x10000000
#define IMAGE_SCN_MEM_EXECUTE               0x20000000
#define IMAGE_SCN_MEM_READ                  0x40000000
#define IMAGE_SCN_MEM_WRITE                 0x80000000

/* -------------------------------------------------------------------------- */
/* Base relocations                                                           */
/* -------------------------------------------------------------------------- */
#define IMAGE_REL_BASED_ABSOLUTE            0
#define IMAGE_REL_BASED_HIGH                1
#define IMAGE_REL_BASED_LOW                 2
#define IMAGE_REL_BASED_HIGHLOW             3
#define IMAGE_REL_BASED_HIGHADJ             4
#define IMAGE_REL_BASED_MACHINE_SPECIFIC_5  5
#define IMAGE_REL_BASED_RESERVED            6
#define IMAGE_REL_BASED_MACHINE_SPECIFIC_7  7
#define IMAGE_REL_BASED_MACHINE_SPECIFIC_8  8
#define IMAGE_REL_BASED_MACHINE_SPECIFIC_9  9
#define IMAGE_REL_BASED_DIR64               10

typedef struct _IMAGE_BASE_RELOCATION {
    u32 VirtualAddress;
    u32 SizeOfBlock;
} IMAGE_BASE_RELOCATION, *PIMAGE_BASE_RELOCATION;

_Static_assert(sizeof(IMAGE_BASE_RELOCATION) == 8, "IMAGE_BASE_RELOCATION size");

/* -------------------------------------------------------------------------- */
/* Imports                                                                    */
/* -------------------------------------------------------------------------- */
typedef struct _IMAGE_IMPORT_DESCRIPTOR {
    union {
        u32 Characteristics;
        u32 OriginalFirstThunk;
    } DUMMYUNIONNAME;
    u32 TimeDateStamp;
    u32 ForwarderChain;
    u32 Name;
    u32 FirstThunk;
} IMAGE_IMPORT_DESCRIPTOR, *PIMAGE_IMPORT_DESCRIPTOR;

_Static_assert(sizeof(IMAGE_IMPORT_DESCRIPTOR) == 20, "IMAGE_IMPORT_DESCRIPTOR size");

#define IMAGE_ORDINAL_FLAG64                0x8000000000000000ULL
#define IMAGE_ORDINAL_FLAG32                0x80000000U

typedef struct _IMAGE_THUNK_DATA64 {
    union {
        u64 ForwarderString;
        u64 Function;
        u64 Ordinal;
        u64 AddressOfData;
    } u1;
} IMAGE_THUNK_DATA64, *PIMAGE_THUNK_DATA64;

_Static_assert(sizeof(IMAGE_THUNK_DATA64) == 8, "IMAGE_THUNK_DATA64 size");

typedef struct _IMAGE_IMPORT_BY_NAME {
    u16 Hint;
    char Name[1];
} IMAGE_IMPORT_BY_NAME, *PIMAGE_IMPORT_BY_NAME;

/* -------------------------------------------------------------------------- */
/* Exports                                                                    */
/* -------------------------------------------------------------------------- */
typedef struct _IMAGE_EXPORT_DIRECTORY {
    u32 Characteristics;
    u32 TimeDateStamp;
    u16 MajorVersion;
    u16 MinorVersion;
    u32 Name;
    u32 Base;
    u32 NumberOfFunctions;
    u32 NumberOfNames;
    u32 AddressOfFunctions;
    u32 AddressOfNames;
    u32 AddressOfNameOrdinals;
} IMAGE_EXPORT_DIRECTORY, *PIMAGE_EXPORT_DIRECTORY;

_Static_assert(sizeof(IMAGE_EXPORT_DIRECTORY) == 40, "IMAGE_EXPORT_DIRECTORY size");

/* -------------------------------------------------------------------------- */
/* TLS directory                                                              */
/* -------------------------------------------------------------------------- */
#define IMAGE_TLS_ALIGN_1                   0x00
#define IMAGE_TLS_ALIGN_2                   0x01
#define IMAGE_TLS_ALIGN_4                   0x02
#define IMAGE_TLS_ALIGN_8                   0x03
#define IMAGE_TLS_ALIGN_16                  0x04
#define IMAGE_TLS_ALIGN_32                  0x05
#define IMAGE_TLS_ALIGN_64                  0x06
#define IMAGE_TLS_ALIGN_128                 0x07
#define IMAGE_TLS_ALIGN_256                 0x08
#define IMAGE_TLS_ALIGN_4096                0x09
#define IMAGE_TLS_ALIGN_8192                0x0A

typedef struct _IMAGE_TLS_DIRECTORY64 {
    u64 StartAddressOfRawData;
    u64 EndAddressOfRawData;
    u64 AddressOfIndex;
    u64 AddressOfCallBacks;
    u32 SizeOfZeroFill;
    u32 Characteristics;
} IMAGE_TLS_DIRECTORY64, *PIMAGE_TLS_DIRECTORY64;

_Static_assert(sizeof(IMAGE_TLS_DIRECTORY64) == 40, "IMAGE_TLS_DIRECTORY64 size");

/* -------------------------------------------------------------------------- */
/* Exception directory / unwind                                               */
/* -------------------------------------------------------------------------- */
typedef struct _IMAGE_RUNTIME_FUNCTION_ENTRY {
    u32 BeginAddress;
    u32 EndAddress;
    union {
        u32 UnwindInfoAddress;
        u32 UnwindData;
    } DUMMYUNIONNAME;
} IMAGE_RUNTIME_FUNCTION_ENTRY, *PIMAGE_RUNTIME_FUNCTION_ENTRY;

_Static_assert(sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) == 12,
               "IMAGE_RUNTIME_FUNCTION_ENTRY size");

#define UNW_FLAG_EHANDLER                   0x01
#define UNW_FLAG_UHANDLER                   0x02
#define UNW_FLAG_CHAININFO                  0x04

typedef union _UNWIND_CODE {
    struct {
        u8 CodeOffset;
        u8 UnwindOp:4;
        u8 OpInfo:4;
    } DUMMYSTRUCTNAME;
    u16 FrameOffset;
} UNWIND_CODE, *PUNWIND_CODE;

_Static_assert(sizeof(UNWIND_CODE) == 2, "UNWIND_CODE size");

typedef struct _UNWIND_INFO {
    u8 Version:3;
    u8 Flags:5;
    u8 SizeOfProlog;
    u8 CountOfCodes;
    u8 FrameRegister:4;
    u8 FrameOffset:4;
    UNWIND_CODE UnwindCode[1];
    /* Optional u32 ExceptionHandler / chained RUNTIME_FUNCTION follow,
     * subject to alignment padding. */
} UNWIND_INFO, *PUNWIND_INFO;

_Static_assert(sizeof(UNWIND_INFO) == 6, "UNWIND_INFO size");

/* -------------------------------------------------------------------------- */
/* Load config directory                                                      */
/* -------------------------------------------------------------------------- */
typedef struct _IMAGE_LOAD_CONFIG_CODE_INTEGRITY {
    u16 Flags;
    u16 Catalog;
    u32 CatalogOffset;
    u32 Reserved;
} IMAGE_LOAD_CONFIG_CODE_INTEGRITY, *PIMAGE_LOAD_CONFIG_CODE_INTEGRITY;

_Static_assert(sizeof(IMAGE_LOAD_CONFIG_CODE_INTEGRITY) == 12,
               "IMAGE_LOAD_CONFIG_CODE_INTEGRITY size");

typedef struct _IMAGE_LOAD_CONFIG_DIRECTORY64 {
    u32 Size;
    u32 TimeDateStamp;
    u16 MajorVersion;
    u16 MinorVersion;
    u32 GlobalFlagsClear;
    u32 GlobalFlagsSet;
    u32 CriticalSectionDefaultTimeout;
    u64 DeCommitFreeBlockThreshold;
    u64 DeCommitTotalFreeThreshold;
    u64 LockPrefixTable;
    u64 MaximumAllocationSize;
    u64 VirtualMemoryThreshold;
    u64 ProcessAffinityMask;
    u32 ProcessHeapFlags;
    u16 CSDVersion;
    u16 DependentLoadFlags;
    u64 EditList;
    u64 SecurityCookie;
    u64 SEHandlerTable;
    u64 SEHandlerCount;
    u64 GuardCFFunctionTable;
    u64 GuardCFFunctionCount;
    u32 GuardFlags;
    IMAGE_LOAD_CONFIG_CODE_INTEGRITY CodeIntegrity;
    u64 GuardAddressTakenIatEntryTable;
    u64 GuardAddressTakenIatEntryCount;
    u64 GuardLongJumpTargetTable;
    u64 GuardLongJumpTargetCount;
    u64 DynamicValueRelocTable;
    u64 CHPEMetadataPointer;
    u64 GuardRFFailureRoutine;
    u64 GuardRFFailureRoutineFunctionPointer;
    u32 DynamicValueRelocTableOffset;
    u16 DynamicValueRelocTableSection;
    u16 Reserved2;
    u64 GuardRFVerifyStackPointerFunctionPointer;
    u32 HotPatchTableOffset;
    u32 Reserved3;
    u64 EnclaveConfigurationPointer;
    u64 VolatileMetadataPointer;
    u64 GuardEHContinuationTable;
    u64 GuardEHContinuationCount;
    u64 GuardXFGCheckFunctionPointer;
    u64 GuardXFGDispatchFunctionPointer;
    u64 GuardXFGTableDispatchFunctionPointer;
    u64 CastGuardOsDeterminedFailureMode;
    u64 GuardMemcpyFunctionPointer;
} IMAGE_LOAD_CONFIG_DIRECTORY64, *PIMAGE_LOAD_CONFIG_DIRECTORY64;

_Static_assert(sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64) == 304,
               "IMAGE_LOAD_CONFIG_DIRECTORY64 size");

/* -------------------------------------------------------------------------- */
/* Delay-load imports                                                         */
/* -------------------------------------------------------------------------- */
typedef struct _IMAGE_DELAYLOAD_DESCRIPTOR {
    union {
        u32 AllAttributes;
        struct {
            u32 RvaBased:1;
            u32 ReservedAttributes:31;
        } DUMMYSTRUCTNAME;
    } Attributes;
    u32 DllNameRVA;
    u32 ModuleHandleRVA;
    u32 ImportAddressTableRVA;
    u32 ImportNameTableRVA;
    u32 BoundImportAddressTableRVA;
    u32 UnloadImportAddressTableRVA;
    u32 TimeDateStamp;
} IMAGE_DELAYLOAD_DESCRIPTOR, *PIMAGE_DELAYLOAD_DESCRIPTOR;

_Static_assert(sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) == 32,
               "IMAGE_DELAYLOAD_DESCRIPTOR size");

/* -------------------------------------------------------------------------- */
/* Resources                                                                  */
/* -------------------------------------------------------------------------- */
#define IMAGE_RESOURCE_NAME_IS_STRING       0x80000000
#define IMAGE_RESOURCE_DATA_IS_DIRECTORY    0x80000000

typedef struct _IMAGE_RESOURCE_DIRECTORY {
    u32 Characteristics;
    u32 TimeDateStamp;
    u16 MajorVersion;
    u16 MinorVersion;
    u16 NumberOfNamedEntries;
    u16 NumberOfIdEntries;
} IMAGE_RESOURCE_DIRECTORY, *PIMAGE_RESOURCE_DIRECTORY;

_Static_assert(sizeof(IMAGE_RESOURCE_DIRECTORY) == 16,
               "IMAGE_RESOURCE_DIRECTORY size");

typedef struct _IMAGE_RESOURCE_DIRECTORY_ENTRY {
    union {
        struct {
            u32 NameOffset:31;
            u32 NameIsString:1;
        } DUMMYSTRUCTNAME;
        u32 Name;
        u16 Id;
    } DUMMYUNIONNAME;
    union {
        u32 OffsetToData;
        struct {
            u32 OffsetToDirectory:31;
            u32 DataIsDirectory:1;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME2;
} IMAGE_RESOURCE_DIRECTORY_ENTRY, *PIMAGE_RESOURCE_DIRECTORY_ENTRY;

_Static_assert(sizeof(IMAGE_RESOURCE_DIRECTORY_ENTRY) == 8,
               "IMAGE_RESOURCE_DIRECTORY_ENTRY size");

typedef struct _IMAGE_RESOURCE_DATA_ENTRY {
    u32 OffsetToData;
    u32 Size;
    u32 CodePage;
    u32 Reserved;
} IMAGE_RESOURCE_DATA_ENTRY, *PIMAGE_RESOURCE_DATA_ENTRY;

_Static_assert(sizeof(IMAGE_RESOURCE_DATA_ENTRY) == 16,
               "IMAGE_RESOURCE_DATA_ENTRY size");

#endif /* PE_FORMAT_H */
