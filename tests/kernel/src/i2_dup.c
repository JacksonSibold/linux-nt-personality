/* i2_dup.c - PE test for handle duplication with access narrowing and NtQueryObject type names */
typedef int NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef void* HANDLE; typedef HANDLE *PHANDLE;
typedef unsigned long ACCESS_MASK;
typedef void* PVOID; typedef unsigned long *PULONG;
typedef struct { USHORT Length; USHORT MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes; void *SecurityDescriptor; void *SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef unsigned char BOOLEAN;
typedef long LONG; typedef LONG *PLONG;
typedef OBJECT_ATTRIBUTES *POBJECT_ATTRIBUTES;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008L)

/* Stubs */
__asm__(".text\n"
".globl NtCreateEvent\nNtCreateEvent: mov %rcx, %r10; mov $0x48, %eax; syscall; ret\n"
".globl NtDuplicateObject\nNtDuplicateObject: mov %rcx, %r10; mov $0x3c, %eax; syscall; ret\n"
".globl NtQueryObject\nNtQueryObject: mov %rcx, %r10; mov $0x10, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
);
NTSTATUS NtCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG EventType, BOOLEAN InitialState);
NTSTATUS NtDuplicateObject(HANDLE SourceProcessHandle, HANDLE SourceHandle, HANDLE TargetProcessHandle, PHANDLE TargetHandle, ACCESS_MASK DesiredAccess, ULONG HandleAttributes, ULONG Options);
NTSTATUS NtQueryObject(HANDLE Handle, ULONG ObjectInformationClass, PVOID ObjectInformation, ULONG Length, PULONG ReturnLength);
NTSTATUS NtClose(HANDLE Handle);
NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
#define NtCurrentProcess() ((HANDLE)(long)-1)
#define DUPLICATE_SAME_ACCESS 0x02
#define DUPLICATE_CLOSE_SOURCE 0x01
#define NotificationEvent 0
#define ObjectTypeInformation 2
void _start() {
    __asm__ volatile("sub $128, %rsp");
    HANDLE hEv = (HANDLE)0;
    WCHAR name[] = { '\\','B','a','s','e','N','a','m','e','d','O','b','j','e','c','t','s','\\','I','2','D','u','p','E','v',0 };
    USHORT len=0; while(name[len]) len++;
    UNICODE_STRING us; us.Buffer = name; us.Length = len*sizeof(WCHAR); us.MaximumLength = us.Length+2;
    OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=(HANDLE)0; oa.ObjectName=&us; oa.Attributes=0x40; oa.SecurityDescriptor=(void*)0; oa.SecurityQualityOfService=(void*)0;
    NTSTATUS st = NtCreateEvent(&hEv, 0x03, &oa, NotificationEvent, 0);
    if (st!=STATUS_SUCCESS && st!=STATUS_OBJECT_NAME_COLLISION) { NtTerminateProcess(NtCurrentProcess(), 2); while(1){} }
    HANDLE hDup=(HANDLE)0;
    st = NtDuplicateObject(NtCurrentProcess(), hEv, NtCurrentProcess(), &hDup, 0x01, 0, 0);
    if (st!=STATUS_SUCCESS) { NtTerminateProcess(NtCurrentProcess(), 3); while(1){} }
    HANDLE hFail=(HANDLE)0;
    st = NtDuplicateObject(NtCurrentProcess(), hDup, NtCurrentProcess(), &hFail, 0x02, 0, 0);
    if (st!=STATUS_ACCESS_DENIED) { NtTerminateProcess(NtCurrentProcess(), 4); while(1){} }
    HANDLE hSame=(HANDLE)0;
    st = NtDuplicateObject(NtCurrentProcess(), hEv, NtCurrentProcess(), &hSame, 0, 0, DUPLICATE_SAME_ACCESS);
    if (st!=STATUS_SUCCESS) { NtTerminateProcess(NtCurrentProcess(), 5); while(1){} }
    HANDLE hClose=(HANDLE)0;
    st = NtDuplicateObject(NtCurrentProcess(), hEv, NtCurrentProcess(), &hClose, 0x01, 0, DUPLICATE_CLOSE_SOURCE);
    if (st!=STATUS_SUCCESS) { NtTerminateProcess(NtCurrentProcess(), 6); while(1){} }
    char qbuf[64]; ULONG ret;
    st = NtQueryObject(hEv, ObjectTypeInformation, qbuf, sizeof(qbuf), &ret);
    if (st!=STATUS_INVALID_HANDLE) { NtTerminateProcess(NtCurrentProcess(), 7); while(1){} }
    char buf[512]; ULONG rlen;
    st = NtQueryObject(hDup, ObjectTypeInformation, buf, sizeof(buf), &rlen);
    if (st!=STATUS_SUCCESS) { NtTerminateProcess(NtCurrentProcess(), 8); while(1){} }
    UNICODE_STRING *tn = (UNICODE_STRING*)buf;
    if (tn->Length != 10) { NtTerminateProcess(NtCurrentProcess(), 9); while(1){} }
    if (tn->Buffer[0]!='E' || tn->Buffer[1]!='v') { NtTerminateProcess(NtCurrentProcess(), 10); while(1){} }
    char small[4];
    st = NtQueryObject(hDup, ObjectTypeInformation, small, sizeof(small), &rlen);
    if (st!=STATUS_INFO_LENGTH_MISMATCH) { NtTerminateProcess(NtCurrentProcess(), 11); while(1){} }
    NtClose(hDup); NtClose(hSame); NtClose(hClose);
    NtTerminateProcess(NtCurrentProcess(), 0);
    while(1){}
}
