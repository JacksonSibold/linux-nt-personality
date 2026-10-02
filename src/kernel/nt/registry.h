/* registry.h - NT registry personality */
#ifndef NT_REGISTRY_H
#define NT_REGISTRY_H

#include "kshim.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registry disposition */
#define REG_CREATED_NEW_KEY        1UL
#define REG_OPENED_EXISTING_KEY    2UL

/* Registry value types */
#define REG_NONE                    0UL
#define REG_SZ                      1UL
#define REG_EXPAND_SZ               2UL
#define REG_BINARY                  3UL
#define REG_DWORD                   4UL
#define REG_DWORD_BIG_ENDIAN        5UL
#define REG_LINK                    6UL
#define REG_MULTI_SZ                7UL
#define REG_RESOURCE_LIST           8UL
#define REG_FULL_RESOURCE_DESCRIPTOR 9UL
#define REG_RESOURCE_REQUIREMENTS_LIST 10UL
#define REG_QWORD                  11UL

/* Registry create options */
#define REG_OPTION_VOLATILE         0x00000001UL
#define REG_OPTION_NON_VOLATILE     0x00000000UL
#define REG_OPTION_CREATE_LINK      0x00000002UL
#define REG_OPTION_BACKUP_RESTORE   0x00000004UL
#define REG_OPTION_OPEN_LINK        0x00000008UL

/* Key access masks */
#define KEY_QUERY_VALUE             0x0001UL
#define KEY_SET_VALUE               0x0002UL
#define KEY_CREATE_SUB_KEY          0x0004UL
#define KEY_ENUMERATE_SUB_KEYS      0x0008UL
#define KEY_NOTIFY                  0x0010UL
#define KEY_CREATE_LINK             0x0020UL
#define KEY_WOW64_64KEY             0x0100UL
#define KEY_WOW64_32KEY             0x0200UL
#define KEY_WOW64_RESERVED          0x0300UL
#define KEY_READ                    ((0x00020000UL)|KEY_QUERY_VALUE|KEY_ENUMERATE_SUB_KEYS|KEY_NOTIFY)
#define KEY_WRITE                   ((0x00020000UL)|KEY_SET_VALUE|KEY_CREATE_SUB_KEY)
#define KEY_EXECUTE                 ((0x00020000UL)|KEY_READ)
#define KEY_ALL_ACCESS              ((0x000F0000UL)|KEY_QUERY_VALUE|KEY_SET_VALUE|KEY_CREATE_SUB_KEY|KEY_ENUMERATE_SUB_KEYS|KEY_NOTIFY|KEY_CREATE_LINK)

/* Information classes */
#define KeyBasicInformation         0UL
#define KeyNodeInformation          1UL
#define KeyFullInformation          2UL
#define KeyNameInformation          3UL
#define KeyCachedInformation        4UL

#define KeyValueBasicInformation    0UL
#define KeyValueFullInformation     1UL
#define KeyValuePartialInformation  2UL
#define KeyValueFullInformationAlign64 3UL
#define KeyValuePartialInformationAlign64 4UL

/* Structures for NtQueryKey */
typedef struct _KEY_BASIC_INFORMATION {
    LARGE_INTEGER LastWriteTime;
    ULONG TitleIndex;
    ULONG NameLength;
    WCHAR Name[1];
} KEY_BASIC_INFORMATION, *PKEY_BASIC_INFORMATION;

typedef struct _KEY_NODE_INFORMATION {
    LARGE_INTEGER LastWriteTime;
    ULONG TitleIndex;
    ULONG ClassOffset;
    ULONG ClassLength;
    ULONG NameLength;
    WCHAR Name[1];
} KEY_NODE_INFORMATION, *PKEY_NODE_INFORMATION;

typedef struct _KEY_FULL_INFORMATION {
    LARGE_INTEGER LastWriteTime;
    ULONG TitleIndex;
    ULONG ClassOffset;
    ULONG ClassLength;
    ULONG SubKeys;
    ULONG MaxNameLen;
    ULONG MaxClassLen;
    ULONG Values;
    ULONG MaxValueNameLen;
    ULONG MaxValueDataLen;
    WCHAR Class[1];
} KEY_FULL_INFORMATION, *PKEY_FULL_INFORMATION;

typedef struct _KEY_NAME_INFORMATION {
    ULONG NameLength;
    WCHAR Name[1];
} KEY_NAME_INFORMATION, *PKEY_NAME_INFORMATION;

/* Structures for NtQueryValueKey / Enumerate */
typedef struct _KEY_VALUE_BASIC_INFORMATION {
    ULONG TitleIndex;
    ULONG Type;
    ULONG NameLength;
    WCHAR Name[1];
} KEY_VALUE_BASIC_INFORMATION, *PKEY_VALUE_BASIC_INFORMATION;

typedef struct _KEY_VALUE_FULL_INFORMATION {
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataOffset;
    ULONG DataLength;
    ULONG NameLength;
    WCHAR Name[1];
} KEY_VALUE_FULL_INFORMATION, *PKEY_VALUE_FULL_INFORMATION;

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    ULONG TitleIndex;
    ULONG Type;
    ULONG DataLength;
    UCHAR Data[1];
} KEY_VALUE_PARTIAL_INFORMATION, *PKEY_VALUE_PARTIAL_INFORMATION;

typedef struct _KEY_VALUE_PARTIAL_INFORMATION_ALIGN64 {
    ULONG Type;
    ULONG DataLength;
    UCHAR Data[1];
} KEY_VALUE_PARTIAL_INFORMATION_ALIGN64;

/* Structures for NtEnumerateKey */
typedef struct _KEY_BASIC_INFORMATION_ENUM {
    LARGE_INTEGER LastWriteTime;
    ULONG TitleIndex;
    ULONG NameLength;
    WCHAR Name[1];
} KEY_BASIC_ENUM;

/* Hive backing design note:
 *  Backing: per-key directory tree under <g_kshim_root>/nt_registry
 *           + monolithic snapshot file <g_kshim_root>/nt_registry/hive_snapshot.bin
 *           with pseudo-regf header "NTREG\x01" for externally-parseable inspection.
 *  Trade-off: directory tree is immediately parseable with ls/cat, survives power cut
 *  per-file fsync, and maps 1:1 to regf NK/VK records; monolithic hive.bin is single-file
 *  externally parseable but requires atomic rename+fsync. Real regf would replace
 *  hive_snapshot.bin writer with binary regf cell allocator while keeping same logical
 *  tree, at cost of more complex crash-consistency handling.
 */

extern struct nt_object_type nt_key_type;

NTSTATUS ntreg_init(void);
void ntreg_cleanup(void);

/* Internal helpers (direct, kernel copies) */
NTSTATUS NtCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess,
                     POBJECT_ATTRIBUTES ObjectAttributes, ULONG TitleIndex,
                     PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition);
NTSTATUS NtOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS NtOpenKeyEx(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG OpenOptions);
NTSTATUS NtDeleteKey(HANDLE KeyHandle);
NTSTATUS NtFlushKey(HANDLE KeyHandle);
NTSTATUS NtEnumerateKey(HANDLE KeyHandle, ULONG Index, ULONG KeyInformationClass,
                        PVOID KeyInformation, ULONG Length, PULONG ResultLength);
NTSTATUS NtEnumerateValueKey(HANDLE KeyHandle, ULONG Index, ULONG KeyValueInformationClass,
                             PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
NTSTATUS NtQueryKey(HANDLE KeyHandle, ULONG KeyInformationClass,
                    PVOID KeyInformation, ULONG Length, PULONG ResultLength);
NTSTATUS NtQueryValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName,
                         ULONG KeyValueInformationClass, PVOID KeyValueInformation,
                         ULONG Length, PULONG ResultLength);
NTSTATUS NtSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName,
                       ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize);
NTSTATUS NtDeleteValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName);

/* Dispatch svc prototypes (12-arg form) */
NTSTATUS nt_svc_NtCreateKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                            unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                            unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtOpenKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                          unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                          unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtOpenKeyEx(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                            unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                            unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtDeleteKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                            unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                            unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtFlushKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                           unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                           unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtEnumerateKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                               unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                               unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtEnumerateValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                                    unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                                    unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtQueryKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                           unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                           unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtQueryValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                                unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                                unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtSetValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                              unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                              unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);
NTSTATUS nt_svc_NtDeleteValueKey(unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3,
                                 unsigned long a4, unsigned long a5, unsigned long a6, unsigned long a7,
                                 unsigned long a8, unsigned long a9, unsigned long a10, unsigned long a11);

#ifdef __cplusplus
}
#endif
#endif
