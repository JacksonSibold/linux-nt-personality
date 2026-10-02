/* sync_semaphore.c - Semaphore limits, previous counts, over-release, wait counts
 * Golden: probe_sync.json semaphore section
 */
typedef int NTSTATUS;
typedef unsigned long ULONG;
typedef void* HANDLE; typedef HANDLE *PHANDLE;
typedef unsigned long ACCESS_MASK;
typedef void* PVOID;
typedef struct { ULONG Length; HANDLE RootDirectory; struct _UNICODE_STRING *ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef OBJECT_ATTRIBUTES *POBJECT_ATTRIBUTES;
typedef long LONG; typedef LONG *PLONG;
typedef long long LONGLONG; typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef unsigned char BOOLEAN;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED ((NTSTATUS)0xC0000047)

__asm__(".text\n"
".globl NtCreateSemaphore\nNtCreateSemaphore: mov %rcx, %r10; mov $0xc3, %eax; syscall; ret\n"
".globl NtReleaseSemaphore\nNtReleaseSemaphore: mov %rcx, %r10; mov $0x0a, %eax; syscall; ret\n"
".globl NtWaitForSingleObject\nNtWaitForSingleObject: mov %rcx, %r10; mov $0x04, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
);
NTSTATUS NtCreateSemaphore(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,LONG,LONG);
NTSTATUS NtReleaseSemaphore(HANDLE,LONG,PLONG);
NTSTATUS NtWaitForSingleObject(HANDLE,BOOLEAN,LARGE_INTEGER*);
NTSTATUS NtClose(HANDLE);
NTSTATUS NtTerminateProcess(HANDLE,NTSTATUS);
#define NtCurrentProcess() ((HANDLE)(long)-1)

void _start(){
 __asm__ volatile("sub $256, %rsp");
 HANDLE hSem=(HANDLE)0, hSem2=(HANDLE)0, hSem3=(HANDLE)0;
 NTSTATUS st;
 LARGE_INTEGER zero; zero.QuadPart=0;
 LONG prev=-99;

 // initial 1 max 3
 st = NtCreateSemaphore(&hSem, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, 1, 3);
 if(st==STATUS_NOT_IMPLEMENTED){ NtTerminateProcess(NtCurrentProcess(),90); while(1){} }
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),1); while(1){} }
 prev=-1;
 st = NtReleaseSemaphore(hSem, 1, &prev);
 if(st!=STATUS_SUCCESS || prev!=1){ NtTerminateProcess(NtCurrentProcess(),2); while(1){} }
 prev=-1;
 st = NtReleaseSemaphore(hSem, 1, &prev);
 if(st!=STATUS_SUCCESS || prev!=2){ NtTerminateProcess(NtCurrentProcess(),3); while(1){} }
 prev=-1;
 st = NtReleaseSemaphore(hSem, 1, &prev);
 if(st!=STATUS_SEMAPHORE_LIMIT_EXCEEDED){ NtTerminateProcess(NtCurrentProcess(),4); while(1){} }
 if(prev!=-1){ NtTerminateProcess(NtCurrentProcess(),5); while(1){} } // prev should stay -1 on failure per golden

 // nt_over_release via second semaphore max 2 initial 2 (full)
 st = NtCreateSemaphore(&hSem2, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, 2, 2);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),6); while(1){} }
 prev=-1;
 st = NtReleaseSemaphore(hSem2, 1, &prev);
 if(st!=STATUS_SEMAPHORE_LIMIT_EXCEEDED){ NtTerminateProcess(NtCurrentProcess(),7); while(1){} }
 if(prev!=-1){ NtTerminateProcess(NtCurrentProcess(),8); while(1){} }

 // prev_count_test: initial 0 max 5 release 2 prev 0, then waits
 st = NtCreateSemaphore(&hSem3, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, 0, 5);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),9); while(1){} }
 prev=-1;
 st = NtReleaseSemaphore(hSem3, 2, &prev);
 if(st!=STATUS_SUCCESS || prev!=0){ NtTerminateProcess(NtCurrentProcess(),10); while(1){} }
 st = NtWaitForSingleObject(hSem3, 0, &zero);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),11); while(1){} }
 st = NtWaitForSingleObject(hSem3, 0, &zero);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),12); while(1){} }
 st = NtWaitForSingleObject(hSem3, 0, &zero);
 if(st!=STATUS_TIMEOUT){ NtTerminateProcess(NtCurrentProcess(),13); while(1){} }

 // Additional: test release with count 0? Should be success no effect? Not in golden, skip
 // Test limit exceeded details: release exceeding max by 2
 prev=-1;
 st = NtReleaseSemaphore(hSem3, 5, &prev);
 if(st!=STATUS_SUCCESS){ // max 5 current 0 after consumes? Actually after 2 waits, count 0, release 5 should exceed? max 5 initial 0 after 2 consumes -> 0, release 5 => reaches 5 exactly, should succeed? So not over.
   // if strict, 0+5 =5 = max, should succeed.
 }
 // try over again
 prev=-1;
 st = NtReleaseSemaphore(hSem3, 1, &prev);
 if(st!=STATUS_SEMAPHORE_LIMIT_EXCEEDED){ NtTerminateProcess(NtCurrentProcess(),14); while(1){} }

 NtClose(hSem); NtClose(hSem2); NtClose(hSem3);
 NtTerminateProcess(NtCurrentProcess(),0);
 while(1){}
}
