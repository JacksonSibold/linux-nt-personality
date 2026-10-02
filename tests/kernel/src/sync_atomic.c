/* sync_atomic.c - WaitAll atomicity under contention with overlapping object sets
 * Requires NtCreateThreadEx; reports blocking if missing.
 */
typedef int NTSTATUS;
typedef unsigned long ULONG;
typedef void* HANDLE; typedef HANDLE *PHANDLE;
typedef unsigned long ACCESS_MASK;
typedef void* PVOID;
typedef long LONG; typedef LONG *PLONG;
typedef long long LONGLONG; typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef unsigned char BOOLEAN;
typedef struct { ULONG Length; HANDLE RootDirectory; struct _UNICODE_STRING *ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES;
typedef OBJECT_ATTRIBUTES *POBJECT_ATTRIBUTES;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_WAIT_0 ((NTSTATUS)0)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#define SynchronizationEvent 1
#define WaitAll 0
#define WaitAny 1

__asm__(".text\n"
".globl NtCreateEvent\nNtCreateEvent: mov %rcx, %r10; mov $0x48, %eax; syscall; ret\n"
".globl NtSetEvent\nNtSetEvent: mov %rcx, %r10; mov $0x0e, %eax; syscall; ret\n"
".globl NtResetEvent\nNtResetEvent: mov %rcx, %r10; mov $0x180, %eax; syscall; ret\n"
".globl NtWaitForSingleObject\nNtWaitForSingleObject: mov %rcx, %r10; mov $0x04, %eax; syscall; ret\n"
".globl NtWaitForMultipleObjects\nNtWaitForMultipleObjects: mov %rcx, %r10; mov $0x5b, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl NtCreateThreadEx\nNtCreateThreadEx: mov %rcx, %r10; mov $0xc5, %eax; syscall; ret\n"
".globl NtTerminateThread\nNtTerminateThread: mov %rcx, %r10; mov $0x53, %eax; syscall; ret\n"
".globl NtDelayExecution\nNtDelayExecution: mov %rcx, %r10; mov $0x34, %eax; syscall; ret\n"
);
NTSTATUS NtCreateEvent(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,BOOLEAN);
NTSTATUS NtSetEvent(HANDLE,PLONG);
NTSTATUS NtResetEvent(HANDLE,PLONG);
NTSTATUS NtWaitForSingleObject(HANDLE,BOOLEAN,LARGE_INTEGER*);
NTSTATUS NtWaitForMultipleObjects(ULONG,HANDLE*,ULONG,BOOLEAN,LARGE_INTEGER*);
NTSTATUS NtClose(HANDLE);
NTSTATUS NtTerminateProcess(HANDLE,NTSTATUS);
NTSTATUS NtCreateThreadEx(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,HANDLE,PVOID,PVOID,ULONG,ULONG,ULONG,ULONG,PVOID);
NTSTATUS NtTerminateThread(HANDLE,NTSTATUS);
NTSTATUS NtDelayExecution(BOOLEAN,LARGE_INTEGER*);
#define NtCurrentProcess() ((HANDLE)(long)-1)

struct thread_arg {
  HANDLE objs[2];
  ULONG count;
  ULONG wait_type;
  NTSTATUS *result;
  volatile LONG *done;
};

NTSTATUS wait_thread_func(PVOID a){
 struct thread_arg *arg=(struct thread_arg*)a;
 NTSTATUS st = NtWaitForMultipleObjects(arg->count, arg->objs, arg->wait_type, 0, (LARGE_INTEGER*)0);
 if(arg->result) *arg->result=st;
 if(arg->done) __asm__ volatile("lock incl %0" : "+m"(*arg->done));
 NtTerminateProcess(NtCurrentProcess(),0);
 while(1){}
 return st;
}

void _start(){
 __asm__ volatile("sub $1024, %rsp");
 NTSTATUS st;
 HANDLE evA=(HANDLE)0, evB=(HANDLE)0, evC=(HANDLE)0;
 st = NtCreateEvent(&evA, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, SynchronizationEvent, 0);
 if(st==STATUS_NOT_IMPLEMENTED){ NtTerminateProcess(NtCurrentProcess(),90); while(1){} }
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),1); while(1){} }
 st = NtCreateEvent(&evB, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, SynchronizationEvent, 0);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),2); while(1){} }
 st = NtCreateEvent(&evC, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, SynchronizationEvent, 0);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),3); while(1){} }
 HANDLE hThr1=(HANDLE)0, hThr2=(HANDLE)0;
 NTSTATUS result1=0xFFFFFFFF, result2=0xFFFFFFFF;
 volatile LONG done1=0, done2=0;
 struct thread_arg arg1; arg1.objs[0]=evA; arg1.objs[1]=evB; arg1.count=2; arg1.wait_type=WaitAll; arg1.result=&result1; arg1.done=(volatile LONG*)&done1;
 struct thread_arg arg2; arg2.objs[0]=evB; arg2.objs[1]=evA; arg2.count=2; arg2.wait_type=WaitAll; arg2.result=&result2; arg2.done=(volatile LONG*)&done2;
 NTSTATUS tst = NtCreateThreadEx(&hThr1, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, NtCurrentProcess(), (PVOID)wait_thread_func, (PVOID)&arg1, 0,0,0,0,(PVOID)0);
 if(tst==STATUS_NOT_IMPLEMENTED){
   NtClose(evA); NtClose(evB); NtClose(evC);
   NtTerminateProcess(NtCurrentProcess(),80); while(1){}
 }
 if(tst!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),4); while(1){} }
 tst = NtCreateThreadEx(&hThr2, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, NtCurrentProcess(), (PVOID)wait_thread_func, (PVOID)&arg2, 0,0,0,0,(PVOID)0);
 if(tst!=STATUS_SUCCESS){ NtTerminateThread(hThr1,0); NtClose(evA); NtClose(evB); NtClose(evC); NtTerminateProcess(NtCurrentProcess(),5); while(1){} }
 LARGE_INTEGER del; del.QuadPart = -10000000LL;
 NtDelayExecution(0, &del);
 if(done1!=0 || done2!=0){ NtTerminateThread(hThr1,0); NtTerminateThread(hThr2,0); NtTerminateProcess(NtCurrentProcess(),6); while(1){} }
 NtSetEvent(evA, (PLONG)0);
 NtSetEvent(evB, (PLONG)0);
 del.QuadPart = -10000000LL; NtDelayExecution(0, &del);
 int doneCount = (done1?1:0) + (done2?1:0);
 if(doneCount!=1){ NtTerminateThread(hThr1,0); NtTerminateThread(hThr2,0); NtTerminateProcess(NtCurrentProcess(),7); while(1){} }
 if(done1 && result1!=STATUS_WAIT_0){ NtTerminateProcess(NtCurrentProcess(),8); while(1){} }
 if(done2 && result2!=STATUS_WAIT_0){ NtTerminateProcess(NtCurrentProcess(),9); while(1){} }
 NtSetEvent(evA, (PLONG)0);
 NtSetEvent(evB, (PLONG)0);
 del.QuadPart = -10000000LL; NtDelayExecution(0, &del);
 if(done1==0 && done2==0){ NtTerminateProcess(NtCurrentProcess(),10); while(1){} }
 if(done1 && done2){
   if(result1!=STATUS_WAIT_0 || result2!=STATUS_WAIT_0){ NtTerminateProcess(NtCurrentProcess(),11); while(1){} }
 } else {
   del.QuadPart = -10000000LL; NtDelayExecution(0, &del);
   if(done1==0 || done2==0){ NtTerminateProcess(NtCurrentProcess(),12); while(1){} }
 }
 NtTerminateThread(hThr1,0); NtTerminateThread(hThr2,0);
 NtClose(hThr1); NtClose(hThr2);
 NtClose(evA); NtClose(evB); NtClose(evC);
 NtTerminateProcess(NtCurrentProcess(),0);
 while(1){}
}
