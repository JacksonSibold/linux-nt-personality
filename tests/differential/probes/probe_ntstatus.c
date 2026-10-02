#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>

typedef DWORD (WINAPI *PFN_RtlNtStatusToDosError)(NTSTATUS);

// NtQueryInformationFile etc for buffer-probe tests
typedef NTSTATUS (NTAPI *PFN_NtQueryInformationFile)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
typedef NTSTATUS (NTAPI *PFN_NtQueryObject)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_NtQuerySystemInformation)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

static void print_u32_hex(uint32_t v) { printf("0x%08lX", (unsigned long)v); }

int main() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) { fprintf(stderr, "no ntdll\n"); return 1; }

    // Get Windows build
    OSVERSIONINFOEXW ovi = { sizeof(ovi) };
    // Use RtlGetVersion for accurate build
    typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW*);
    PFN_RtlGetVersion pRtlGetVersion = (PFN_RtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
    DWORD build = 0, major=0, minor=0, ubr=0;
    if (pRtlGetVersion) {
        pRtlGetVersion(&ovi);
        major = ovi.dwMajorVersion; minor = ovi.dwMinorVersion; build = ovi.dwBuildNumber;
    } else {
        major=10; minor=0; build=26200;
    }
    // Try to read UBR from registry
    HKEY hkey; DWORD ubrVal=0; DWORD cb=sizeof(ubrVal);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",0,KEY_READ,&hkey)==ERROR_SUCCESS) {
        RegQueryValueExA(hkey,"UBR",NULL,NULL,(BYTE*)&ubrVal,&cb);
        RegCloseKey(hkey);
        ubr = ubrVal;
    }

    printf("{\n");
    printf("  \"probe\": \"probe_ntstatus\",\n");
    printf("  \"windows_build\": \"%lu.%lu\",\n", (unsigned long)build, (unsigned long)ubr);
    printf("  \"windows_version\": \"%lu.%lu.%lu\",\n", (unsigned long)major,(unsigned long)minor,(unsigned long)build);
    printf("  \"arch\": \"x64\",\n");

    // ---- RtlNtStatusToDosError table dump ----
    struct { NTSTATUS status; const char *name; } cases[] = {
        { 0x00000000, "STATUS_SUCCESS" },
        { 0x00000001, "STATUS_WAIT_1" },
        { 0x00000002, "STATUS_WAIT_2" },
        { 0x00000003, "STATUS_WAIT_3" },
        { 0x0000003F, "STATUS_WAIT_63" },
        { 0x00000080, "STATUS_ABANDONED" },
        { 0x000000BF, "STATUS_ABANDONED_WAIT_63" },
        { 0x000000C0, "STATUS_USER_APC" },
        { 0x00000100, "STATUS_KERNEL_APC" },
        { 0x00000101, "STATUS_ALERTED" },
        { 0x00000102, "STATUS_TIMEOUT" },
        { 0x00000103, "STATUS_PENDING" },
        { 0x00000104, "STATUS_REPARSE" },
        { 0x00000105, "STATUS_MORE_ENTRIES" },
        { 0x00000106, "STATUS_NOT_ALL_ASSIGNED" },
        { 0x00000107, "STATUS_SOME_NOT_MAPPED" },
        { 0x00000108, "STATUS_OPLOCK_BREAK_IN_PROGRESS" },
        { 0x00000109, "STATUS_VOLUME_MOUNTED" },
        { 0x0000010A, "STATUS_RXACT_COMMITTED" },
        { 0x0000010B, "STATUS_NOTIFY_CLEANUP" },
        { 0x0000010C, "STATUS_NOTIFY_ENUM_DIR" },
        { 0x0000010D, "STATUS_NO_QUOTAS_FOR_ACCOUNT" },
        { 0x0000010E, "STATUS_PRIMARY_TRANSPORT_CONNECT_FAILED" },
        { 0x00000110, "STATUS_PAGE_FAULT_TRANSITION" },
        { 0x00000111, "STATUS_PAGE_FAULT_DEMAND_ZERO" },
        { 0x00000112, "STATUS_PAGE_FAULT_COPY_ON_WRITE" },
        { 0x00000113, "STATUS_PAGE_FAULT_GUARD_PAGE" },
        { 0x00000114, "STATUS_PAGE_FAULT_PAGING_FILE" },
        { 0x00000115, "STATUS_CACHE_PAGE_FLUSHED" },
        { 0x00000367, "STATUS_WAIT_FOR_OPLOCK" },
        { 0x80000001, "STATUS_GUARD_PAGE_VIOLATION" },
        { 0x80000002, "STATUS_DATATYPE_MISALIGNMENT" },
        { 0x80000003, "STATUS_BREAKPOINT" },
        { 0x80000004, "STATUS_SINGLE_STEP" },
        { 0x80000005, "STATUS_BUFFER_OVERFLOW" },
        { 0x80000006, "STATUS_NO_MORE_FILES" },
        { 0x8000000A, "STATUS_HANDLES_CLOSED" },
        { 0x8000000B, "STATUS_NO_INHERITANCE" },
        { 0x8000000C, "STATUS_GUID_SUBSTITUTION_MADE" },
        { 0x8000000D, "STATUS_PARTIAL_COPY" },
        { 0x8000000E, "STATUS_DEVICE_PAPER_EMPTY" },
        { 0x8000000F, "STATUS_DEVICE_POWERED_OFF" },
        { 0x80000010, "STATUS_DEVICE_OFF_LINE" },
        { 0x80000011, "STATUS_DEVICE_BUSY" },
        { 0x80000012, "STATUS_NO_MORE_EAS" },
        { 0x80000013, "STATUS_INVALID_EA_NAME" },
        { 0x80000014, "STATUS_EA_LIST_INCONSISTENT" },
        { 0x80000015, "STATUS_INVALID_EA_FLAG" },
        { 0x80000016, "STATUS_VERIFY_REQUIRED" },
        { 0x80000017, "STATUS_EXTRANEOUS_INFORMATION" },
        { 0x80000018, "STATUS_RXACT_COMMIT_NECESSARY" },
        { 0x8000001A, "STATUS_NO_MORE_ENTRIES" },
        { 0x8000001B, "STATUS_FILEMARK_DETECTED" },
        { 0x8000001C, "STATUS_MEDIA_CHANGED" },
        { 0x8000001D, "STATUS_BUS_RESET" },
        { 0x8000001E, "STATUS_END_OF_MEDIA" },
        { 0x8000001F, "STATUS_BEGINNING_OF_MEDIA" },
        { 0x80000020, "STATUS_MEDIA_CHECK" },
        { 0x80000021, "STATUS_SETMARK_DETECTED" },
        { 0x80000022, "STATUS_NO_DATA_DETECTED" },
        { 0x80000023, "STATUS_REDIRECTOR_HAS_OPEN_HANDLES" },
        { 0x80000024, "STATUS_SERVER_HAS_OPEN_HANDLES" },
        { 0x80000025, "STATUS_ALREADY_COMPLETE" },
        { 0xC0000001, "STATUS_UNSUCCESSFUL" },
        { 0xC0000002, "STATUS_NOT_IMPLEMENTED" },
        { 0xC0000003, "STATUS_INVALID_INFO_CLASS" },
        { 0xC0000004, "STATUS_INFO_LENGTH_MISMATCH" },
        { 0xC0000005, "STATUS_ACCESS_VIOLATION" },
        { 0xC0000006, "STATUS_IN_PAGE_ERROR" },
        { 0xC0000007, "STATUS_PAGEFILE_QUOTA" },
        { 0xC0000008, "STATUS_INVALID_HANDLE" },
        { 0xC0000009, "STATUS_BAD_INITIAL_STACK" },
        { 0xC000000A, "STATUS_BAD_INITIAL_PC" },
        { 0xC000000B, "STATUS_INVALID_CID" },
        { 0xC000000C, "STATUS_TIMER_NOT_CANCELED" },
        { 0xC000000D, "STATUS_INVALID_PARAMETER" },
        { 0xC000000E, "STATUS_NO_SUCH_DEVICE" },
        { 0xC000000F, "STATUS_NO_SUCH_FILE" },
        { 0xC0000010, "STATUS_INVALID_DEVICE_REQUEST" },
        { 0xC0000011, "STATUS_END_OF_FILE" },
        { 0xC0000012, "STATUS_WRONG_VOLUME" },
        { 0xC0000013, "STATUS_NO_MEDIA_IN_DEVICE" },
        { 0xC0000014, "STATUS_UNRECOGNIZED_MEDIA" },
        { 0xC0000015, "STATUS_NONEXISTENT_SECTOR" },
        { 0xC0000016, "STATUS_MORE_PROCESSING_REQUIRED" },
        { 0xC0000017, "STATUS_NO_MEMORY" },
        { 0xC0000018, "STATUS_CONFLICTING_ADDRESSES" },
        { 0xC0000019, "STATUS_NOT_MAPPED_VIEW" },
        { 0xC000001A, "STATUS_UNABLE_TO_FREE_VM" },
        { 0xC000001B, "STATUS_UNABLE_TO_DELETE_SECTION" },
        { 0xC000001C, "STATUS_INVALID_SYSTEM_SERVICE" },
        { 0xC000001D, "STATUS_ILLEGAL_INSTRUCTION" },
        { 0xC000001E, "STATUS_INVALID_LOCK_SEQUENCE" },
        { 0xC000001F, "STATUS_INVALID_VIEW_SIZE" },
        { 0xC0000020, "STATUS_INVALID_FILE_FOR_SECTION" },
        { 0xC0000021, "STATUS_ALREADY_COMMITTED" },
        { 0xC0000022, "STATUS_ACCESS_DENIED" },
        { 0xC0000023, "STATUS_BUFFER_TOO_SMALL" },
        { 0xC0000024, "STATUS_OBJECT_TYPE_MISMATCH" },
        { 0xC0000025, "STATUS_NONCONTINUABLE_EXCEPTION" },
        { 0xC0000026, "STATUS_INVALID_DISPOSITION" },
        { 0xC0000027, "STATUS_UNWIND" },
        { 0xC0000028, "STATUS_BAD_STACK" },
        { 0xC0000029, "STATUS_INVALID_UNWIND_TARGET" },
        { 0xC000002A, "STATUS_NOT_LOCKED" },
        { 0xC000002B, "STATUS_PARITY_ERROR" },
        { 0xC000002C, "STATUS_UNABLE_TO_DECOMMIT_VM" },
        { 0xC000002D, "STATUS_NOT_COMMITTED" },
        { 0xC000002E, "STATUS_INVALID_PORT_ATTRIBUTES" },
        { 0xC000002F, "STATUS_PORT_MESSAGE_TOO_LONG" },
        { 0xC0000030, "STATUS_INVALID_PARAMETER_MIX" },
        { 0xC0000031, "STATUS_INVALID_QUOTA_LOWER" },
        { 0xC0000032, "STATUS_DISK_CORRUPT_ERROR" },
        { 0xC0000033, "STATUS_OBJECT_NAME_INVALID" },
        { 0xC0000034, "STATUS_OBJECT_NAME_NOT_FOUND" },
        { 0xC0000035, "STATUS_OBJECT_NAME_COLLISION" },
        { 0xC0000037, "STATUS_PORT_DISCONNECTED" },
        { 0xC0000038, "STATUS_DEVICE_ALREADY_ATTACHED" },
        { 0xC0000039, "STATUS_OBJECT_PATH_INVALID" },
        { 0xC000003A, "STATUS_OBJECT_PATH_NOT_FOUND" },
        { 0xC000003B, "STATUS_OBJECT_PATH_SYNTAX_BAD" },
        { 0xC000003C, "STATUS_DATA_OVERRUN" },
        { 0xC000003D, "STATUS_DATA_LATE_ERROR" },
        { 0xC000003E, "STATUS_DATA_ERROR" },
        { 0xC000003F, "STATUS_CRC_ERROR" },
        { 0xC0000040, "STATUS_SECTION_TOO_BIG" },
        { 0xC0000041, "STATUS_PORT_CONNECTION_REFUSED" },
        { 0xC0000042, "STATUS_INVALID_PORT_HANDLE" },
        { 0xC0000043, "STATUS_SHARING_VIOLATION" },
        { 0xC0000044, "STATUS_QUOTA_EXCEEDED" },
        { 0xC0000045, "STATUS_INVALID_PAGE_PROTECTION" },
        { 0xC0000046, "STATUS_MUTANT_NOT_OWNED" },
        { 0xC0000047, "STATUS_SEMAPHORE_LIMIT_EXCEEDED" },
        { 0xC0000048, "STATUS_PORT_ALREADY_SET" },
        { 0xC0000049, "STATUS_SECTION_NOT_IMAGE" },
        { 0xC000004A, "STATUS_SUSPEND_COUNT_EXCEEDED" },
        { 0xC000004B, "STATUS_THREAD_IS_TERMINATING" },
        { 0xC000004C, "STATUS_BAD_WORKING_SET_LIMIT" },
        { 0xC000004D, "STATUS_INCOMPATIBLE_FILE_MAP" },
        { 0xC000004E, "STATUS_SECTION_PROTECTION" },
        { 0xC000004F, "STATUS_EAS_NOT_SUPPORTED" },
        { 0xC0000050, "STATUS_EA_TOO_LARGE" },
        { 0xC0000051, "STATUS_NONEXISTENT_EA_ENTRY" },
        { 0xC0000052, "STATUS_NO_EAS_ON_FILE" },
        { 0xC0000053, "STATUS_EA_CORRUPT_ERROR" },
        { 0xC0000054, "STATUS_FILE_LOCK_CONFLICT" },
        { 0xC0000055, "STATUS_LOCK_NOT_GRANTED" },
        { 0xC0000056, "STATUS_DELETE_PENDING" },
        { 0xC0000057, "STATUS_CTL_FILE_NOT_SUPPORTED" },
        { 0xC0000058, "STATUS_UNKNOWN_REVISION" },
        { 0xC0000059, "STATUS_REVISION_MISMATCH" },
        { 0xC000005A, "STATUS_INVALID_OWNER" },
        { 0xC000005B, "STATUS_INVALID_PRIMARY_GROUP" },
        { 0xC000005C, "STATUS_NO_IMPERSONATION_TOKEN" },
        { 0xC000005D, "STATUS_CANT_DISABLE_MANDATORY" },
        { 0xC000005E, "STATUS_NO_LOGON_SERVERS" },
        { 0xC000005F, "STATUS_NO_SUCH_LOGON_SESSION" },
        { 0xC0000060, "STATUS_NO_SUCH_PRIVILEGE" },
        { 0xC0000061, "STATUS_PRIVILEGE_NOT_HELD" },
        { 0xC0000062, "STATUS_INVALID_ACCOUNT_NAME" },
        { 0xC0000063, "STATUS_USER_EXISTS" },
        { 0xC0000064, "STATUS_NO_SUCH_USER" },
        { 0xC0000065, "STATUS_GROUP_EXISTS" },
        { 0xC0000066, "STATUS_NO_SUCH_GROUP" },
        { 0xC0000067, "STATUS_MEMBER_IN_GROUP" },
        { 0xC0000068, "STATUS_MEMBER_NOT_IN_GROUP" },
        { 0xC0000069, "STATUS_LAST_ADMIN" },
        { 0xC000006A, "STATUS_WRONG_PASSWORD" },
        { 0xC000006B, "STATUS_ILL_FORMED_PASSWORD" },
        { 0xC000006C, "STATUS_PASSWORD_RESTRICTION" },
        { 0xC000006D, "STATUS_LOGON_FAILURE" },
        { 0xC000006E, "STATUS_ACCOUNT_RESTRICTION" },
        { 0xC000006F, "STATUS_INVALID_LOGON_HOURS" },
        { 0xC0000070, "STATUS_INVALID_WORKSTATION" },
        { 0xC0000071, "STATUS_PASSWORD_EXPIRED" },
        { 0xC0000072, "STATUS_ACCOUNT_DISABLED" },
        { 0xC0000073, "STATUS_NONE_MAPPED" },
        { 0xC0000074, "STATUS_TOO_MANY_LUIDS_REQUESTED" },
        { 0xC0000075, "STATUS_LUIDS_EXHAUSTED" },
        { 0xC0000076, "STATUS_INVALID_SUB_AUTHORITY" },
        { 0xC0000077, "STATUS_INVALID_ACL" },
        { 0xC0000078, "STATUS_INVALID_SID" },
        { 0xC0000079, "STATUS_INVALID_SECURITY_DESCR" },
        { 0xC000007A, "STATUS_PROCEDURE_NOT_FOUND" },
        { 0xC000007B, "STATUS_INVALID_IMAGE_FORMAT" },
        { 0xC000007C, "STATUS_NO_TOKEN" },
        { 0xC000007D, "STATUS_BAD_INHERITANCE_ACL" },
        { 0xC000007E, "STATUS_RANGE_NOT_LOCKED" },
        { 0xC000007F, "STATUS_DISK_FULL" },
        { 0xC0000080, "STATUS_SERVER_DISABLED" },
        { 0xC0000081, "STATUS_SERVER_NOT_DISABLED" },
        { 0xC0000082, "STATUS_TOO_MANY_GUIDS_REQUESTED" },
        { 0xC0000083, "STATUS_GUIDS_EXHAUSTED" },
        { 0xC0000084, "STATUS_INVALID_ID_AUTHORITY" },
        { 0xC0000085, "STATUS_AGENTS_EXHAUSTED" },
        { 0xC0000086, "STATUS_INVALID_VOLUME_LABEL" },
        { 0xC0000087, "STATUS_SECTION_NOT_EXTENDED" },
        { 0xC0000088, "STATUS_NOT_MAPPED_DATA" },
        { 0xC0000089, "STATUS_RESOURCE_DATA_NOT_FOUND" },
        { 0xC000008A, "STATUS_RESOURCE_TYPE_NOT_FOUND" },
        { 0xC000008B, "STATUS_RESOURCE_NAME_NOT_FOUND" },
        { 0xC000008C, "STATUS_ARRAY_BOUNDS_EXCEEDED" },
        { 0xC000008D, "STATUS_FLOAT_DENORMAL_OPERAND" },
        { 0xC000008E, "STATUS_FLOAT_DIVIDE_BY_ZERO" },
        { 0xC000008F, "STATUS_FLOAT_INEXACT_RESULT" },
        { 0xC0000090, "STATUS_FLOAT_INVALID_OPERATION" },
        { 0xC0000091, "STATUS_FLOAT_OVERFLOW" },
        { 0xC0000092, "STATUS_FLOAT_STACK_CHECK" },
        { 0xC0000093, "STATUS_FLOAT_UNDERFLOW" },
        { 0xC0000094, "STATUS_INTEGER_DIVIDE_BY_ZERO" },
        { 0xC0000095, "STATUS_INTEGER_OVERFLOW" },
        { 0xC0000096, "STATUS_PRIVILEGED_INSTRUCTION" },
        { 0xC0000097, "STATUS_TOO_MANY_PAGING_FILES" },
        { 0xC0000098, "STATUS_FILE_INVALID" },
        { 0xC0000099, "STATUS_ALLOTTED_SPACE_EXCEEDED" },
        { 0xC000009A, "STATUS_INSUFFICIENT_RESOURCES" },
        { 0xC000009B, "STATUS_DFS_EXIT_PATH_FOUND" },
        { 0xC000009C, "STATUS_DEVICE_DATA_ERROR" },
        { 0xC000009D, "STATUS_DEVICE_NOT_CONNECTED" },
        { 0xC000009E, "STATUS_DEVICE_POWER_FAILURE" },
        { 0xC000009F, "STATUS_FREE_VM_NOT_AT_BASE" },
        { 0xC00000A0, "STATUS_MEMORY_NOT_ALLOCATED" },
        { 0xC00000A1, "STATUS_WORKING_SET_QUOTA" },
        { 0xC00000A2, "STATUS_MEDIA_WRITE_PROTECTED" },
        { 0xC00000A3, "STATUS_DEVICE_NOT_READY" },
        { 0xC00000A4, "STATUS_INVALID_GROUP_ATTRIBUTES" },
        { 0xC00000A5, "STATUS_BAD_IMPERSONATION_LEVEL" },
        { 0xC00000A6, "STATUS_CANT_OPEN_ANONYMOUS" },
        { 0xC00000A7, "STATUS_BAD_VALIDATION_CLASS" },
        { 0xC00000A8, "STATUS_BAD_TOKEN_TYPE" },
        { 0xC00000A9, "STATUS_BAD_MASTER_BOOT_RECORD" },
        { 0xC00000AA, "STATUS_INSTRUCTION_MISALIGNMENT" },
        { 0xC00000AB, "STATUS_INSTANCE_NOT_AVAILABLE" },
        { 0xC00000AC, "STATUS_PIPE_NOT_AVAILABLE" },
        { 0xC00000AD, "STATUS_INVALID_PIPE_STATE" },
        { 0xC00000AE, "STATUS_PIPE_BUSY" },
        { 0xC00000AF, "STATUS_ILLEGAL_FUNCTION" },
        { 0xC00000B0, "STATUS_PIPE_DISCONNECTED" },
        { 0xC00000B1, "STATUS_PIPE_CLOSING" },
        { 0xC00000B2, "STATUS_PIPE_CONNECTED" },
        { 0xC00000B3, "STATUS_PIPE_LISTENING" },
        { 0xC00000B4, "STATUS_INVALID_READ_MODE" },
        { 0xC00000B5, "STATUS_IO_TIMEOUT" },
        { 0xC00000B6, "STATUS_FILE_FORCED_CLOSED" },
        { 0xC00000B7, "STATUS_PROFILING_NOT_STARTED" },
        { 0xC00000B8, "STATUS_PROFILING_NOT_STOPPED" },
        { 0xC00000B9, "STATUS_COULD_NOT_INTERPRET" },
        { 0xC00000BA, "STATUS_FILE_IS_A_DIRECTORY" },
        { 0xC00000BB, "STATUS_NOT_SUPPORTED" },
        { 0xC00000BC, "STATUS_REMOTE_NOT_LISTENING" },
        { 0xC00000BD, "STATUS_DUPLICATE_NAME" },
        { 0xC00000BE, "STATUS_BAD_NETWORK_PATH" },
        { 0xC00000BF, "STATUS_NETWORK_BUSY" },
        { 0xC00000C0, "STATUS_DEVICE_DOES_NOT_EXIST" },
        { 0xC00000C1, "STATUS_TOO_MANY_COMMANDS" },
        { 0xC00000C2, "STATUS_ADAPTER_HARDWARE_ERROR" },
        { 0xC00000C3, "STATUS_INVALID_NETWORK_RESPONSE" },
        { 0xC00000C4, "STATUS_UNEXPECTED_NETWORK_ERROR" },
        { 0xC00000C5, "STATUS_BAD_REMOTE_ADAPTER" },
        { 0xC00000C6, "STATUS_PRINT_QUEUE_FULL" },
        { 0xC00000C7, "STATUS_NO_SPOOL_SPACE" },
        { 0xC00000C8, "STATUS_PRINT_CANCELLED" },
        { 0xC00000C9, "STATUS_NETWORK_NAME_DELETED" },
        { 0xC00000CA, "STATUS_NETWORK_ACCESS_DENIED" },
        { 0xC00000CB, "STATUS_BAD_DEVICE_TYPE" },
        { 0xC00000CC, "STATUS_BAD_NETWORK_NAME" },
        { 0xC00000CD, "STATUS_TOO_MANY_NAMES" },
        { 0xC00000CE, "STATUS_TOO_MANY_SESSIONS" },
        { 0xC00000CF, "STATUS_SHARING_PAUSED" },
        { 0xC00000D0, "STATUS_REQUEST_NOT_ACCEPTED" },
        { 0xC00000D1, "STATUS_REDIRECTOR_PAUSED" },
        { 0xC00000D2, "STATUS_NET_WRITE_FAULT" },
        { 0xC00000D3, "STATUS_PROFILING_AT_LIMIT" },
        { 0xC00000D4, "STATUS_NOT_SAME_DEVICE" },
        { 0xC00000D5, "STATUS_FILE_RENAMED" },
        { 0xC00000D6, "STATUS_VIRTUAL_CIRCUIT_CLOSED" },
        { 0xC00000D7, "STATUS_NO_SECURITY_EOBJECT" },
        { 0xC00000D8, "STATUS_CANT_WAIT" },
        { 0xC00000D9, "STATUS_PIPE_EMPTY" },
        { 0xC00000DA, "STATUS_CANT_ACCESS_DOMAIN_INFO" },
        { 0xC00000DB, "STATUS_CANT_TERMINATE_SELF" },
        { 0xC00000DC, "STATUS_INVALID_SERVER_STATE" },
        { 0xC00000DD, "STATUS_INVALID_DOMAIN_STATE" },
        { 0xC00000DE, "STATUS_INVALID_DOMAIN_ROLE" },
        { 0xC00000DF, "STATUS_NO_SUCH_DOMAIN" },
        { 0xC00000E0, "STATUS_DOMAIN_EXISTS" },
        { 0xC00000E1, "STATUS_DOMAIN_LIMIT_EXCEEDED" },
        { 0xC00000E2, "STATUS_OPLOCK_NOT_GRANTED" },
        { 0xC00000E3, "STATUS_INVALID_OPLOCK_PROTOCOL" },
        { 0xC00000E4, "STATUS_INTERNAL_DB_CORRUPTION" },
        { 0xC00000E5, "STATUS_INTERNAL_ERROR" },
        { 0xC00000E6, "STATUS_GENERIC_NOT_MAPPED" },
        { 0xC00000E7, "STATUS_BAD_DESCRIPTOR_FORMAT" },
        { 0xC00000E8, "STATUS_INVALID_USER_BUFFER" },
        { 0xC00000E9, "STATUS_UNEXPECTED_IO_ERROR" },
        { 0xC00000EA, "STATUS_UNEXPECTED_MM_CREATE_ERR" },
        { 0xC00000EB, "STATUS_UNEXPECTED_MM_MAP_ERROR" },
        { 0xC00000EC, "STATUS_UNEXPECTED_MM_EXTEND_ERR" },
        { 0xC00000ED, "STATUS_NOT_LOGON_PROCESS" },
        { 0xC00000EE, "STATUS_LOGON_SESSION_EXISTS" },
        { 0xC00000EF, "STATUS_INVALID_PARAMETER_1" },
        { 0xC00000F0, "STATUS_INVALID_PARAMETER_2" },
        { 0xC00000F1, "STATUS_INVALID_PARAMETER_3" },
        { 0xC00000F2, "STATUS_INVALID_PARAMETER_4" },
        { 0xC00000F3, "STATUS_INVALID_PARAMETER_5" },
        { 0xC00000F4, "STATUS_INVALID_PARAMETER_6" },
        { 0xC00000F5, "STATUS_INVALID_PARAMETER_7" },
        { 0xC00000F6, "STATUS_INVALID_PARAMETER_8" },
        { 0xC00000F7, "STATUS_INVALID_PARAMETER_9" },
        { 0xC00000F8, "STATUS_INVALID_PARAMETER_10" },
        { 0xC00000F9, "STATUS_INVALID_PARAMETER_11" },
        { 0xC00000FA, "STATUS_INVALID_PARAMETER_12" },
        { 0xC00000FB, "STATUS_REDIRECTOR_NOT_STARTED" },
        { 0xC00000FC, "STATUS_REDIRECTOR_STARTED" },
        { 0xC00000FD, "STATUS_STACK_OVERFLOW" },
        { 0xC00000FE, "STATUS_NO_SUCH_PACKAGE" },
        { 0xC00000FF, "STATUS_BAD_FUNCTION_TABLE" },
        { 0xC0000100, "STATUS_VARIABLE_NOT_FOUND" },
        { 0xC0000101, "STATUS_DIRECTORY_NOT_EMPTY" },
        { 0xC0000102, "STATUS_FILE_CORRUPT_ERROR" },
        { 0xC0000103, "STATUS_NOT_A_DIRECTORY" },
        { 0xC0000104, "STATUS_BAD_LOGON_SESSION_STATE" },
        { 0xC0000105, "STATUS_LOGON_SESSION_COLLISION" },
        { 0xC0000106, "STATUS_NAME_TOO_LONG" },
        { 0xC0000107, "STATUS_FILES_OPEN" },
        { 0xC0000108, "STATUS_CONNECTION_IN_USE" },
        { 0xC0000109, "STATUS_MESSAGE_NOT_FOUND" },
        { 0xC000010A, "STATUS_PROCESS_IS_TERMINATING" },
        { 0xC000010B, "STATUS_INVALID_LOGON_TYPE" },
        { 0xC000010C, "STATUS_NO_GUID_TRANSLATION" },
        { 0xC000010D, "STATUS_CANNOT_IMPERSONATE" },
        { 0xC000010E, "STATUS_IMAGE_ALREADY_LOADED" },
        { 0xC0000110, "STATUS_ABANDONED_HIBERFILE_TEST" },
        { 0xC0000111, "STATUS_NO_LDT" },
        { 0xC0000112, "STATUS_INVALID_LDT_SIZE" },
        { 0xC0000113, "STATUS_INVALID_LDT_OFFSET" },
        { 0xC0000114, "STATUS_INVALID_LDT_DESCRIPTOR" },
        { 0xC0000115, "STATUS_INVALID_IMAGE_NE_FORMAT" },
        { 0xC0000118, "STATUS_RXACT_INVALID_STATE" },
        { 0xC0000119, "STATUS_RXACT_COMMIT_FAILURE" },
        { 0xC000011A, "STATUS_MAPPED_FILE_SIZE_ZERO" },
        { 0xC000011B, "STATUS_TOO_MANY_OPENED_FILES" },
        { 0xC0000120, "STATUS_CANCELLED" },
        { 0xC0000121, "STATUS_CANNOT_DELETE" },
        { 0xC0000122, "STATUS_INVALID_COMPUTER_NAME" },
        { 0xC0000123, "STATUS_FILE_DELETED" },
        { 0xC0000124, "STATUS_SPECIAL_ACCOUNT" },
        { 0xC0000125, "STATUS_SPECIAL_GROUP" },
        { 0xC0000126, "STATUS_SPECIAL_USER" },
        { 0xC0000127, "STATUS_MEMBERS_PRIMARY_GROUP" },
        { 0xC000012C, "STATUS_FILE_CLOSED" },
        { 0xC000012D, "STATUS_TOO_MANY_THREADS" },
        { 0xC000012E, "STATUS_THREAD_NOT_IN_PROCESS" },
        { 0xC000012F, "STATUS_TOKEN_ALREADY_IN_USE" },
        { 0xC0000130, "STATUS_PAGEFILE_QUOTA_EXCEEDED" },
        { 0xC0000131, "STATUS_COMMITMENT_LIMIT" },
        { 0xC0000132, "STATUS_INVALID_IMAGE_LE_FORMAT" },
        { 0xC0000133, "STATUS_INVALID_IMAGE_NOT_MZ" },
        { 0xC0000134, "STATUS_INVALID_IMAGE_PROTECT" },
        { 0xC0000135, "STATUS_INVALID_IMAGE_WIN_16" },
        { 0xC0000136, "STATUS_LOGON_SERVER_CONFLICT" },
        { 0xC0000137, "STATUS_TIME_DIFFERENCE_AT_DC" },
        { 0xC0000138, "STATUS_PIPE_BROKEN" },
        { 0xC0000139, "STATUS_REGISTRY_CORRUPT" },
        { 0xC000013A, "STATUS_TOO_LATE" },
        { 0xC000013B, "STATUS_NO_SUCH_MEMBER" },
        { 0xC000013C, "STATUS_INVALID_MEMBER" },
        { 0xC000013D, "STATUS_TOO_MANY_SIDS" },
        { 0xC000013E, "STATUS_LM_CROSS_ENCRYPTION_REQUIRED" },
        { 0xC000013F, "STATUS_NO_LOGON_SERVER" },
        { 0xC000014D, "STATUS_CURRENT_DOMAIN_NOT_ALLOWED" },
        { 0xC000017C, "STATUS_KEY_DELETED" },
        { 0xC000017D, "STATUS_NO_LOG_SPACE" },
        { 0xC0000180, "STATUS_KEY_HAS_CHILDREN" },
        { 0xC0000181, "STATUS_CHILD_MUST_BE_VOLATILE" },
        { 0xC0000182, "STATUS_DEVICE_CONFIGURATION_ERROR" },
        { 0xC0000183, "STATUS_DRIVER_INTERNAL_ERROR" },
        { 0xC0000184, "STATUS_INVALID_DEVICE_STATE" },
        { 0xC0000185, "STATUS_IO_DEVICE_ERROR" },
        { 0xC0000186, "STATUS_DEVICE_PROTOCOL_ERROR" },
        { 0xC0000187, "STATUS_BACKUP_CONTROLLER" },
        { 0xC0000188, "STATUS_LOG_FILE_FULL" },
        { 0xC000018A, "STATUS_NO_TRUST_LSA_SECRET" },
        { 0xC000018B, "STATUS_NO_TRUST_SAM_ACCOUNT" },
        { 0xC000018C, "STATUS_TRUSTED_DOMAIN_FAILURE" },
        { 0xC000018D, "STATUS_TRUSTED_RELATIONSHIP_FAILURE" },
        { 0xC000018F, "STATUS_EVENTLOG_FILE_CORRUPT" },
        { 0xC0000190, "STATUS_EVENTLOG_CANT_START" },
        { 0xC0000191, "STATUS_TRUST_FAILURE" },
        { 0xC0000192, "STATUS_MUTANT_LIMIT_EXCEEDED" },
        { 0xC0000193, "STATUS_ACCOUNT_EXPIRED" },
        { 0xC0000194, "STATUS_NETLOGON_NOT_STARTED" },
        { 0xC0000195, "STATUS_POSSIBLE_DEADLOCK" },
        { 0xC0000196, "STATUS_NETWORK_CREDENTIAL_CONFLICT" },
        { 0xC0000197, "STATUS_REMOTE_SESSION_LIMIT" },
        { 0xC0000198, "STATUS_EVENTLOG_FILE_CHANGED" },
        { 0xC0000199, "STATUS_NOLOGON_INTERDOMAIN_TRUST_ACCOUNT" },
        { 0xC000019A, "STATUS_NOLOGON_WORKSTATION_TRUST_ACCOUNT" },
        { 0xC000019B, "STATUS_NOLOGON_SERVER_TRUST_ACCOUNT" },
        { 0xC000019C, "STATUS_DOMAIN_TRUST_INCONSISTENT" },
        { 0xC000019D, "STATUS_FS_DRIVER_REQUIRED" },
        { 0xC000019E, "STATUS_IMAGE_ALREADY_LOADED_AS_DLL" },
        { 0xC000019F, "STATUS_INCOMPATIBLE_WITH_GLOBAL_SHORT_NAME_REGISTRY_SETTING" },
        { 0xC00001A0, "STATUS_SHORT_NAMES_NOT_ENABLED_ON_VOLUME" },
        { 0xC00001A1, "STATUS_SECURITY_STREAM_IS_INCONSISTENT" },
        { 0xC00001A2, "STATUS_INVALID_LOCK_RANGE" },
        { 0xC00001A3, "STATUS_INVALID_ACE_CONDITION" },
        { 0xC00001A4, "STATUS_IMAGE_SUBSYSTEM_NOT_PRESENT" },
        { 0xC00001A5, "STATUS_NOTIFICATION_GUID_ALREADY_DEFINED" },
        { 0xC00001A7, "STATUS_NOTIFY_OBJECT_REMOVED" },
        { 0xC00001A8, "STATUS_NO_CALLBACK_ACTIVE" },
        { 0xC00001A9, "STATUS_PWD_TOO_SHORT" },
        { 0xC00001AA, "STATUS_PWD_TOO_RECENT" },
        { 0xC00001AB, "STATUS_PWD_HISTORY_CONFLICT" },
        { 0xC00001AE, "STATUS_PLUGPLAY_NO_DEVICE" },
        { 0xC00001AF, "STATUS_UNSUPPORTED_COMPRESSION" },
        { 0xC00001B0, "STATUS_INVALID_HW_PROFILE" },
        { 0xC00001B1, "STATUS_INVALID_PLUGPLAY_DEVICE_PATH" },
        { 0xC00001B2, "STATUS_DRIVER_ORDINAL_NOT_FOUND" },
        { 0xC00001B3, "STATUS_DRIVER_ENTRYPOINT_NOT_FOUND" },
        { 0xC00001B4, "STATUS_RESOURCE_NOT_OWNED" },
        { 0xC00001B5, "STATUS_TOO_MANY_LINKS" },
        { 0xC00001B6, "STATUS_QUOTA_LIST_INCONSISTENT" },
        { 0xC00001B7, "STATUS_FILE_IS_OFFLINE" },
        { 0xC00001B8, "STATUS_EVALUATION_EXPIRATION" },
        { 0xC00001B9, "STATUS_ILLEGAL_DLL_RELOCATION" },
        { 0xC00001BA, "STATUS_DLL_INIT_FAILED" },
        { 0xC00001BB, "STATUS_MISSING_SYSTEMFILE" },
        { 0xC00001BC, "STATUS_UNHANDLED_EXCEPTION" },
        { 0xC00001BD, "STATUS_APP_INIT_FAILURE" },
        { 0xC00001BE, "STATUS_PAGEFILE_CREATE_FAILED" },
        { 0xC00001BF, "STATUS_NO_PAGEFILE" },
        { 0xC00001C0, "STATUS_INVALID_LEVEL" },
        { 0xC00001C1, "STATUS_WRONG_PASSWORD_CORE" },
        { 0xC00001C2, "STATUS_ILLEGAL_FLOAT_CONTEXT" },
        { 0xC00001C3, "STATUS_REGISTRY_IO_FAILED" },
        { 0xC00001C4, "STATUS_NO_EVENT_PAIR" },
        { 0xC00001C5, "STATUS_UNRECOGNIZED_VOLUME" },
        { 0xC00001C6, "STATUS_SERIAL_NO_DEVICE_INITED" },
        { 0xC00001C7, "STATUS_NO_SUCH_ALIAS" },
        { 0xC00001C8, "STATUS_MEMBER_NOT_IN_ALIAS" },
        { 0xC00001C9, "STATUS_MEMBER_IN_ALIAS" },
        { 0xC00001CA, "STATUS_ALIAS_EXISTS" },
        { 0xC00001CB, "STATUS_LOGON_NOT_GRANTED" },
        { 0xC00001CC, "STATUS_TOO_MANY_SECRETS" },
        { 0xC00001CD, "STATUS_SECRET_TOO_LONG" },
        { 0xC00001CE, "STATUS_INTERNAL_DB_ERROR" },
        { 0xC00001CF, "STATUS_FULLSCREEN_MODE" },
        { 0xC00001D0, "STATUS_TOO_MANY_CONTEXT_IDS" },
        { 0xC00001D1, "STATUS_LOGON_TYPE_NOT_GRANTED" },
        { 0xC00001D2, "STATUS_NOT_REGISTRY_FILE" },
        { 0xC00001D3, "STATUS_NT_CROSS_ENCRYPTION_REQUIRED" },
        { 0xC00001D4, "STATUS_DOMAIN_CTRLR_CONFIG_ERROR" },
        { 0xC00001D5, "STATUS_FT_MISSING_MEMBER" },
        { 0xC00001D6, "STATUS_ILL_FORMED_SERVICE_ENTRY" },
        { 0xC00001D7, "STATUS_ILLEGAL_CHARACTER" },
        { 0xC00001D8, "STATUS_UNMAPPABLE_CHARACTER" },
        { 0xC00001D9, "STATUS_UNDEFINED_CHARACTER" },
        { 0xC00001DA, "STATUS_FLOPPY_VOLUME" },
        { 0xC00001DB, "STATUS_FLOPPY_ID_MARK_NOT_FOUND" },
        { 0xC00001DC, "STATUS_FLOPPY_WRONG_CYLINDER" },
        { 0xC00001DD, "STATUS_FLOPPY_UNKNOWN_ERROR" },
        { 0xC00001DE, "STATUS_FLOPPY_BAD_REGISTERS" },
        { 0xC00001DF, "STATUS_DISK_RECALIBRATE_FAILED" },
        { 0xC00001E0, "STATUS_DISK_OPERATION_FAILED" },
        { 0xC00001E1, "STATUS_DISK_RESET_FAILED" },
        { 0xC00001E2, "STATUS_SHARED_IRQ_BUSY" },
        { 0xC00001E3, "STATUS_FT_ORPHANING" },
        { 0xC00001E6, "STATUS_PARTITION_FAILURE" },
        { 0xC00001E7, "STATUS_INVALID_BLOCK_LENGTH" },
        { 0xC00001E8, "STATUS_DEVICE_NOT_PARTITIONED" },
        { 0xC00001E9, "STATUS_UNABLE_TO_LOCK_MEDIA" },
        { 0xC00001EA, "STATUS_UNABLE_TO_UNLOAD_MEDIA" },
        { 0xC00001EB, "STATUS_EOM_OVERFLOW" },
        { 0xC00001EC, "STATUS_NO_MEDIA" },
        { 0xC0000227, "STATUS_NO_UNICODE_TRANSLATION" },
        { 0xC0000228, "STATUS_DLL_NOT_FOUND" },
        { 0xC0000229, "STATUS_OPEN_FAILED" },
        { 0xC000022B, "STATUS_IO_PRIVILEGE_FAILED" },
        { 0xC0000232, "STATUS_ORDINAL_NOT_FOUND" },
        { 0xC0000233, "STATUS_ENTRYPOINT_NOT_FOUND" },
        { 0xC000023B, "STATUS_LOCAL_DISCONNECT" },
        { 0xC000023C, "STATUS_REMOTE_DISCONNECT" },
        { 0xC000023D, "STATUS_REMOTE_RESOURCES" },
        { 0xC000023E, "STATUS_LINK_FAILED" },
        { 0xC000023F, "STATUS_LINK_TIMEOUT" },
        { 0xC0000240, "STATUS_INVALID_CONNECTION" },
        { 0xC0000241, "STATUS_INVALID_ADDRESS" },
        { 0xC0000243, "STATUS_USER_MAPPED_FILE" },
        { 0xC0000244, "STATUS_AUDIT_FAILED" },
        { 0xC0000245, "STATUS_TIMER_RESOLUTION_NOT_SET" },
        { 0xC0000246, "STATUS_CONNECTION_COUNT_LIMIT" },
        { 0xC0000247, "STATUS_LOGIN_TIME_RESTRICTION" },
        { 0xC0000248, "STATUS_LOGIN_WKSTA_RESTRICTION" },
        { 0xC0000249, "STATUS_IMAGE_MP_UP_MISMATCH" },
        { 0xC0000268, "STATUS_DLL_INIT_FAILED_LOGOFF" },
        { 0xC000026C, "STATUS_DRIVER_UNABLE_TO_LOAD" },
        { 0xC000026D, "STATUS_DFS_UNAVAILABLE" },
        { 0xC000026E, "STATUS_VOLUME_DISMOUNTED" },
        { 0xC000026F, "STATUS_WX86_INTERNAL_ERROR" },
        { 0xC0000270, "STATUS_WX86_FLOAT_STACK_CHECK" },
        { 0xC0000271, "STATUS_VALIDATE_CONTINUE" },
        { 0xC0000272, "STATUS_NO_MATCH" },
        { 0xC0000273, "STATUS_NO_MORE_MATCHES" },
        { 0xC0000275, "STATUS_NOT_A_REPARSE_POINT" },
        { 0xC0000276, "STATUS_IO_REPARSE_TAG_INVALID" },
        { 0xC0000277, "STATUS_IO_REPARSE_TAG_MISMATCH" },
        { 0xC0000278, "STATUS_IO_REPARSE_DATA_INVALID" },
        { 0xC0000279, "STATUS_IO_REPARSE_TAG_NOT_HANDLED" },
        { 0xC0000280, "STATUS_REPARSE_POINT_NOT_RESOLVED" },
        { 0xC0000281, "STATUS_DIRECTORY_IS_A_REPARSE_POINT" },
        { 0xC03A0001, "FACILITY_NULL customer code" },
        { 0xC0070005, "FACILITY_NTWIN32 ERROR_ACCESS_DENIED (0xC0070005)" },
        { 0xC0070002, "FACILITY_NTWIN32 ERROR_FILE_NOT_FOUND" },
        { 0x80070005, "HRESULT E_ACCESSDENIED pattern (not NTSTATUS)" },
        { 0xC0000200, "fallback C0000200" },
        { 0xC0001234, "fallback C0001234" },
        { 0x00000080, "fallback 00000080" },
        { 0x80000003, "fallback 80000003" },
        { 0x40000000, "fallback 40000000" },
        { 0xC0070020, "fallback C0070020" },
    };
int ncases = sizeof(cases)/sizeof(cases[0]);
    PFN_RtlNtStatusToDosError pRtlNtStatusToDosError = (PFN_RtlNtStatusToDosError)GetProcAddress(ntdll, "RtlNtStatusToDosError");
    printf("  \"rtl_mappings\": [\n");
    for (int i=0;i<ncases;i++) {
        DWORD dos = pRtlNtStatusToDosError ? pRtlNtStatusToDosError(cases[i].status) : 0;
        printf("    {\"name\":\"%s\",\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"dos\":%lu}%s\n",
            cases[i].name, (long)cases[i].status, (unsigned long)(uint32_t)cases[i].status, (unsigned long)dos, i+1<ncases?",":"");
    }
    printf("  ],\n");

    // ---- Two-call size-probe idiom tests ----
    printf("  \"size_probe\": [\n");
    int probe_idx=0;
    // Helper macro to emit probe
#define EMIT_PROBE(api, st, info_val, len_val) \
    do { \
        printf("    {\"api\":\"%s\",\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%lu,\"len\":%lu}%s\n", \
            api, (long)(st), (unsigned long)(uint32_t)(st), (unsigned long)(info_val), (unsigned long)(len_val), ","); \
        probe_idx++; \
    } while(0)

    // NtQueryInformationFile: query FileNameInformation with various buffer sizes
    {
        char tmpPath[MAX_PATH];
        GetTempPathA(sizeof(tmpPath), tmpPath);
        char tmpFile[MAX_PATH];
        GetTempFileNameA(tmpPath, "ntp", 0, tmpFile);
        HANDLE h = CreateFileA(tmpFile, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            PFN_NtQueryInformationFile pNtQIF = (PFN_NtQueryInformationFile)GetProcAddress(ntdll, "NtQueryInformationFile");
            if (pNtQIF) {
                IO_STATUS_BLOCK iosb;
                // FileNameInformation = 9
                char buf[512];
                ULONG retLen=0;
                // Len 0
                NTSTATUS st = pNtQIF(h, &iosb, buf, 0, 9);
                printf("    {\"api\":\"NtQueryInformationFile(FileNameInformation)\",\"buffer_len\":0,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%llu},\n",
                    (long)st, (unsigned long)(uint32_t)st, (unsigned long long)iosb.Information);
                // Len 4 (too small for header)
                st = pNtQIF(h, &iosb, buf, 4, 9);
                printf("    {\"api\":\"NtQueryInformationFile(FileNameInformation)\",\"buffer_len\":4,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%llu},\n",
                    (long)st, (unsigned long)(uint32_t)st, (unsigned long long)iosb.Information);
                // Len sizeof(FILE_NAME_INFORMATION)-1
                st = pNtQIF(h, &iosb, buf, 8, 9);
                printf("    {\"api\":\"NtQueryInformationFile(FileNameInformation)\",\"buffer_len\":8,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%llu},\n",
                    (long)st, (unsigned long)(uint32_t)st, (unsigned long long)iosb.Information);
                // Adequate buffer
                st = pNtQIF(h, &iosb, buf, sizeof(buf), 9);
                printf("    {\"api\":\"NtQueryInformationFile(FileNameInformation)\",\"buffer_len\":512,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%llu},\n",
                    (long)st, (unsigned long)(uint32_t)st, (unsigned long long)iosb.Information);
                // FileBasicInformation (4) with len 0 and len sizeof-1
                st = pNtQIF(h, &iosb, buf, 0, 4);
                printf("    {\"api\":\"NtQueryInformationFile(FileBasicInformation)\",\"buffer_len\":0,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%llu},\n",
                    (long)st, (unsigned long)(uint32_t)st, (unsigned long long)iosb.Information);
                st = pNtQIF(h, &iosb, buf, 4, 4);
                printf("    {\"api\":\"NtQueryInformationFile(FileBasicInformation)\",\"buffer_len\":4,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%llu},\n",
                    (long)st, (unsigned long)(uint32_t)st, (unsigned long long)iosb.Information);
                // FileStandardInformation (5)
                st = pNtQIF(h, &iosb, buf, 4, 5);
                printf("    {\"api\":\"NtQueryInformationFile(FileStandardInformation)\",\"buffer_len\":4,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%llu},\n",
                    (long)st, (unsigned long)(uint32_t)st, (unsigned long long)iosb.Information);
                // FileAllInformation (18) - known to return BUFFER_OVERFLOW vs BUFFER_TOO_SMALL?
                st = pNtQIF(h, &iosb, buf, 4, 18);
                printf("    {\"api\":\"NtQueryInformationFile(FileAllInformation)\",\"buffer_len\":4,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%llu},\n",
                    (long)st, (unsigned long)(uint32_t)st, (unsigned long long)iosb.Information);
                st = pNtQIF(h, &iosb, buf, sizeof(buf), 18);
                printf("    {\"api\":\"NtQueryInformationFile(FileAllInformation)\",\"buffer_len\":512,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"info\":%llu},\n",
                    (long)st, (unsigned long)(uint32_t)st, (unsigned long long)iosb.Information);
            }
            CloseHandle(h);
            DeleteFileA(tmpFile);
        }
    }
    // NtQueryObject: ObjectNameInformation (1) with small buffer
    {
        PFN_NtQueryObject pNtQO = (PFN_NtQueryObject)GetProcAddress(ntdll, "NtQueryObject");
        if (pNtQO) {
            HANDLE h = GetCurrentProcess();
            char buf[512];
            ULONG retLen=0;
            NTSTATUS st = pNtQO(h, 1, buf, 0, &retLen);
            printf("    {\"api\":\"NtQueryObject(ObjectNameInformation)\",\"buffer_len\":0,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
            st = pNtQO(h, 1, buf, 4, &retLen);
            printf("    {\"api\":\"NtQueryObject(ObjectNameInformation)\",\"buffer_len\":4,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
            st = pNtQO(h, 1, buf, sizeof(buf), &retLen);
            printf("    {\"api\":\"NtQueryObject(ObjectNameInformation)\",\"buffer_len\":512,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
            // ObjectTypeInformation (2)
            st = pNtQO(h, 2, buf, 0, &retLen);
            printf("    {\"api\":\"NtQueryObject(ObjectTypeInformation)\",\"buffer_len\":0,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
            st = pNtQO(h, 2, buf, 4, &retLen);
            printf("    {\"api\":\"NtQueryObject(ObjectTypeInformation)\",\"buffer_len\":4,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
            // ObjectBasicInformation (0)
            st = pNtQO(h, 0, buf, 0, &retLen);
            printf("    {\"api\":\"NtQueryObject(ObjectBasicInformation)\",\"buffer_len\":0,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
            st = pNtQO(h, 0, buf, 4, &retLen);
            printf("    {\"api\":\"NtQueryObject(ObjectBasicInformation)\",\"buffer_len\":4,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
        }
    }
    // NtQuerySystemInformation with bogus class
    {
        PFN_NtQuerySystemInformation pNtQSI = (PFN_NtQuerySystemInformation)GetProcAddress(ntdll, "NtQuerySystemInformation");
        if (pNtQSI) {
            char buf[16];
            ULONG retLen=0;
            NTSTATUS st = pNtQSI(9999, buf, 0, &retLen);
            printf("    {\"api\":\"NtQuerySystemInformation(bogus)\",\"buffer_len\":0,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
            st = pNtQSI(0, buf, 0, &retLen);
            printf("    {\"api\":\"NtQuerySystemInformation(SystemBasicInformation)\",\"buffer_len\":0,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
            st = pNtQSI(0, buf, 4, &retLen);
            printf("    {\"api\":\"NtQuerySystemInformation(SystemBasicInformation)\",\"buffer_len\":4,\"ntstatus\":%ld,\"ntstatus_hex\":\"0x%08lX\",\"retLen\":%lu},\n",
                (long)st, (unsigned long)(uint32_t)st, (unsigned long)retLen);
        }
    }
    // Dummy last element without trailing comma trick: we already have commas, need to close
    // Remove trailing comma by emitting a sentinel
    printf("    {\"api\":\"END\",\"ntstatus\":0,\"ntstatus_hex\":\"0x00000000\",\"info\":0}\n");
    printf("  ],\n");

    // ---- Fallback behavior test: RtlNtStatusToDosError for unmapped codes ----
    printf("  \"fallback_tests\": [\n");
    NTSTATUS fallback_cases[] = { 0xC0000200, 0xC0001234, 0x00000080, 0x80000003, 0x40000000, 0xC0070020 };
    for (int i=0;i<6;i++) {
        DWORD dos = pRtlNtStatusToDosError ? pRtlNtStatusToDosError(fallback_cases[i]) : 0;
        printf("    {\"ntstatus_hex\":\"0x%08lX\",\"dos\":%lu}%s\n", (unsigned long)(uint32_t)fallback_cases[i], (unsigned long)dos, i<5?",":"");
    }
    printf("  ]\n");
    printf("}\n");
    return 0;
}
