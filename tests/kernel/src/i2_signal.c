/* i2_signal.c - PE test for event/mutant/semaphore create+signal+wait */
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
typedef long long LONGLONG; typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef OBJECT_ATTRIBUTES *POBJECT_ATTRIBUTES;
typedef LARGE_INTEGER *PLARGE_INTEGER;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED ((NTSTATUS)0xC0000047L)
__asm__(".text\n"
".globl NtCreateEvent\nNtCreateEvent: mov %rcx, %r10; mov $0x48, %eax; syscall; ret\n"
".globl NtOpenEvent\nNtOpenEvent: mov %rcx, %r10; mov $0x40, %eax; syscall; ret\n"
".globl NtCreateMutant\nNtCreateMutant: mov %rcx, %r10; mov $0xb6, %eax; syscall; ret\n"
".globl NtOpenMutant\nNtOpenMutant: mov %rcx, %r10; mov $0x12a, %eax; syscall; ret\n"
".globl NtCreateSemaphore\nNtCreateSemaphore: mov %rcx, %r10; mov $0xc3, %eax; syscall; ret\n"
".globl NtOpenSemaphore\nNtOpenSemaphore: mov %rcx, %r10; mov $0x131, %eax; syscall; ret\n"
".globl NtSetEvent\nNtSetEvent: mov %rcx, %r10; mov $0x0e, %eax; syscall; ret\n"
".globl NtResetEvent\nNtResetEvent: mov %rcx, %r10; mov $0x180, %eax; syscall; ret\n"
".globl NtReleaseMutant\nNtReleaseMutant: mov %rcx, %r10; mov $0x20, %eax; syscall; ret\n"
".globl NtReleaseSemaphore\nNtReleaseSemaphore: mov %rcx, %r10; mov $0x0a, %eax; syscall; ret\n"
".globl NtWaitForSingleObject\nNtWaitForSingleObject: mov %rcx, %r10; mov $0x04, %eax; syscall; ret\n"
".globl NtQueryObject\nNtQueryObject: mov %rcx, %r10; mov $0x10, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
);
NTSTATUS NtCreateEvent(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,BOOLEAN);
NTSTATUS NtOpenEvent(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
NTSTATUS NtCreateMutant(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,BOOLEAN);
NTSTATUS NtOpenMutant(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
NTSTATUS NtCreateSemaphore(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,LONG,LONG);
NTSTATUS NtOpenSemaphore(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES);
NTSTATUS NtSetEvent(HANDLE,PLONG);
NTSTATUS NtResetEvent(HANDLE,PLONG);
NTSTATUS NtReleaseMutant(HANDLE,PLONG);
NTSTATUS NtReleaseSemaphore(HANDLE,LONG,PLONG);
NTSTATUS NtWaitForSingleObject(HANDLE,BOOLEAN,PLARGE_INTEGER);
NTSTATUS NtQueryObject(HANDLE,ULONG,PVOID,ULONG,PULONG);
NTSTATUS NtClose(HANDLE);
NTSTATUS NtTerminateProcess(HANDLE,NTSTATUS);
#define NtCurrentProcess() ((HANDLE)(long)-1)
#define NotificationEvent 0
#define SynchronizationEvent 1
#define ObjectTypeInformation 2
void _start(){
 __asm__ volatile("sub $256, %rsp");
 HANDLE hEv=(HANDLE)0,hMut=(HANDLE)0,hSem=(HANDLE)0;
 WCHAR n1[]={ '\\','B','a','s','e','N','a','m','e','d','O','b','j','e','c','t','s','\\','I','2','S','i','g','E','v',0 };
 WCHAR n2[]={ '\\','B','a','s','e','N','a','m','e','d','O','b','j','e','c','t','s','\\','I','2','S','i','g','M','u','t',0 };
 WCHAR n3[]={ '\\','B','a','s','e','N','a','m','e','d','O','b','j','e','c','t','s','\\','I','2','S','i','g','S','e','m',0 };
 USHORT l1=0; while(n1[l1]) l1++; UNICODE_STRING u1; u1.Buffer=n1; u1.Length=l1*sizeof(WCHAR); u1.MaximumLength=u1.Length+2;
 USHORT l2=0; while(n2[l2]) l2++; UNICODE_STRING u2; u2.Buffer=n2; u2.Length=l2*sizeof(WCHAR); u2.MaximumLength=u2.Length+2;
 USHORT l3=0; while(n3[l3]) l3++; UNICODE_STRING u3; u3.Buffer=n3; u3.Length=l3*sizeof(WCHAR); u3.MaximumLength=u3.Length+2;
 OBJECT_ATTRIBUTES oa1; oa1.Length=sizeof(oa1); oa1.RootDirectory=(HANDLE)0; oa1.ObjectName=&u1; oa1.Attributes=0x40; oa1.SecurityDescriptor=(void*)0; oa1.SecurityQualityOfService=(void*)0;
 OBJECT_ATTRIBUTES oa2; oa2.Length=sizeof(oa2); oa2.RootDirectory=(HANDLE)0; oa2.ObjectName=&u2; oa2.Attributes=0x40; oa2.SecurityDescriptor=(void*)0; oa2.SecurityQualityOfService=(void*)0;
 OBJECT_ATTRIBUTES oa3; oa3.Length=sizeof(oa3); oa3.RootDirectory=(HANDLE)0; oa3.ObjectName=&u3; oa3.Attributes=0x40; oa3.SecurityDescriptor=(void*)0; oa3.SecurityQualityOfService=(void*)0;
 NTSTATUS st;
 st=NtCreateEvent(&hEv,0x1FFFFF,&oa1,NotificationEvent,0);
 if(st!=STATUS_SUCCESS && st!=STATUS_OBJECT_NAME_COLLISION){ NtTerminateProcess(NtCurrentProcess(),2); while(1){} }
 char buf[512]; ULONG r;
 st=NtQueryObject(hEv, ObjectTypeInformation, buf, sizeof(buf), &r);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),3); while(1){} }
 UNICODE_STRING *tn=(UNICODE_STRING*)buf;
 if(tn->Length!=10 || tn->Buffer[0]!='E'){ NtTerminateProcess(NtCurrentProcess(),4); while(1){} }
 st=NtCreateMutant(&hMut,0x1FFFFF,&oa2,0);
 if(st!=STATUS_SUCCESS && st!=STATUS_OBJECT_NAME_COLLISION){ NtTerminateProcess(NtCurrentProcess(),5); while(1){} }
 st=NtQueryObject(hMut, ObjectTypeInformation, buf, sizeof(buf), &r);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),6); while(1){} }
 tn=(UNICODE_STRING*)buf;
 if(tn->Length!=12){ NtTerminateProcess(NtCurrentProcess(),7); while(1){} }
 if(tn->Buffer[0]!='M'){ NtTerminateProcess(NtCurrentProcess(),8); while(1){} }
 st=NtCreateSemaphore(&hSem,0x1FFFFF,&oa3,1,5);
 if(st!=STATUS_SUCCESS && st!=STATUS_OBJECT_NAME_COLLISION){ NtTerminateProcess(NtCurrentProcess(),9); while(1){} }
 st=NtQueryObject(hSem, ObjectTypeInformation, buf, sizeof(buf), &r);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),10); while(1){} }
 tn=(UNICODE_STRING*)buf;
 if(tn->Length!=18){ NtTerminateProcess(NtCurrentProcess(),11); while(1){} }
 if(tn->Buffer[0]!='S'){ NtTerminateProcess(NtCurrentProcess(),12); while(1){} }
 LARGE_INTEGER zero; zero.QuadPart=0;
 st=NtWaitForSingleObject(hEv,0,&zero);
 if(st!=STATUS_TIMEOUT){ NtTerminateProcess(NtCurrentProcess(),13); while(1){} }
 NtSetEvent(hEv,(PLONG)0);
 st=NtWaitForSingleObject(hEv,0,&zero);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),14); while(1){} }
 NtResetEvent(hEv,(PLONG)0);
 st=NtWaitForSingleObject(hEv,0,&zero);
 if(st!=STATUS_TIMEOUT){ NtTerminateProcess(NtCurrentProcess(),15); while(1){} }
 st=NtWaitForSingleObject(hMut,0,&zero);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),16); while(1){} }
 st=NtReleaseMutant(hMut,(PLONG)0);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),17); while(1){} }
 st=NtWaitForSingleObject(hSem,0,&zero);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),18); while(1){} }
 LONG prev;
 st=NtReleaseSemaphore(hSem,1,&prev);
 if(st!=STATUS_SUCCESS || prev!=0){ NtTerminateProcess(NtCurrentProcess(),19); while(1){} }
 st=NtReleaseSemaphore(hSem,10,(PLONG)0);
 if(st!=STATUS_SEMAPHORE_LIMIT_EXCEEDED){ NtTerminateProcess(NtCurrentProcess(),20); while(1){} }
 NtClose(hEv); NtClose(hMut); NtClose(hSem);
 NtTerminateProcess(NtCurrentProcess(),0);
 while(1){}
}
