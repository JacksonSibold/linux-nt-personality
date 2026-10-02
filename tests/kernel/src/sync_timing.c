/* sync_timing.c - Timeout accuracy STATUS_TIMEOUT and elapsed
 * Requires NtDelayExecution and NtWaitForSingleObject with timeout
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
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#define SynchronizationEvent 1
#define NotificationEvent 0

__asm__(".text\n"
".globl NtCreateEvent\nNtCreateEvent: mov %rcx, %r10; mov $0x48, %eax; syscall; ret\n"
".globl NtSetEvent\nNtSetEvent: mov %rcx, %r10; mov $0x0e, %eax; syscall; ret\n"
".globl NtWaitForSingleObject\nNtWaitForSingleObject: mov %rcx, %r10; mov $0x04, %eax; syscall; ret\n"
".globl NtDelayExecution\nNtDelayExecution: mov %rcx, %r10; mov $0x34, %eax; syscall; ret\n"
".globl NtQueryPerformanceCounter\nNtQueryPerformanceCounter: mov %rcx, %r10; mov $0x31, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
);
NTSTATUS NtCreateEvent(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,ULONG,BOOLEAN);
NTSTATUS NtSetEvent(HANDLE,PLONG);
NTSTATUS NtWaitForSingleObject(HANDLE,BOOLEAN,LARGE_INTEGER*);
NTSTATUS NtDelayExecution(BOOLEAN,LARGE_INTEGER*);
NTSTATUS NtQueryPerformanceCounter(LARGE_INTEGER*,LARGE_INTEGER*);
NTSTATUS NtClose(HANDLE);
NTSTATUS NtTerminateProcess(HANDLE,NTSTATUS);
#define NtCurrentProcess() ((HANDLE)(long)-1)

void _start(){
 __asm__ volatile("sub $512, %rsp");
 HANDLE hEv=(HANDLE)0;
 NTSTATUS st;
 st = NtCreateEvent(&hEv, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, SynchronizationEvent, 0);
 if(st==STATUS_NOT_IMPLEMENTED){ NtTerminateProcess(NtCurrentProcess(),90); while(1){} }
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),1); while(1){} }

 // Test zero timeout immediate
 LARGE_INTEGER zero; zero.QuadPart=0;
 st = NtWaitForSingleObject(hEv, 0, &zero);
 if(st!=STATUS_TIMEOUT){ NtTerminateProcess(NtCurrentProcess(),2); while(1){} }

 // Test infinite with signaled should succeed immediately
 NtSetEvent(hEv, (PLONG)0);
 // NULL timeout = infinite, but we poll with signaled -> success
 st = NtWaitForSingleObject(hEv, 0, (LARGE_INTEGER*)0);
 if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),3); while(1){} }
 // auto reset consumes, so next should block. Test 100ms timeout accuracy.
 HANDLE hEv2=(HANDLE)0;
 NtCreateEvent(&hEv2, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, SynchronizationEvent, 0);
 LARGE_INTEGER to100; to100.QuadPart = -1000000LL; // 100ms
 // Try QueryPerformanceCounter to measure elapsed
 LARGE_INTEGER freq, start, end;
 NTSTATUS qst = NtQueryPerformanceCounter(&start, &freq);
 if(qst==STATUS_NOT_IMPLEMENTED){
   // Without perf counter, just verify timeout result
   st = NtWaitForSingleObject(hEv2, 0, &to100);
   if(st!=STATUS_TIMEOUT){ NtTerminateProcess(NtCurrentProcess(),4); while(1){} }
   NtClose(hEv); NtClose(hEv2);
   NtTerminateProcess(NtCurrentProcess(),80); while(1){} // partial: no perf counter to verify accuracy
 }
 if(qst!=STATUS_SUCCESS){
   st = NtWaitForSingleObject(hEv2, 0, &to100);
   if(st!=STATUS_TIMEOUT){ NtTerminateProcess(NtCurrentProcess(),5); while(1){} }
   NtClose(hEv); NtClose(hEv2);
   NtTerminateProcess(NtCurrentProcess(),81); while(1){}
 }
 // Measure
 qst = NtQueryPerformanceCounter(&start, &freq);
 st = NtWaitForSingleObject(hEv2, 0, &to100);
 NtQueryPerformanceCounter(&end, (LARGE_INTEGER*)0);
 if(st!=STATUS_TIMEOUT){ NtTerminateProcess(NtCurrentProcess(),6); while(1){} }
 // elapsed = (end-start)*1000/freq should be ~100ms. Check within tolerance 80-150ms per golden 109ms
 LONGLONG elapsed_ms = 0;
 if(freq.QuadPart!=0){
   LONGLONG diff = end.QuadPart - start.QuadPart;
   elapsed_ms = diff * 1000 / freq.QuadPart;
   if(elapsed_ms < 80 || elapsed_ms > 150){ NtTerminateProcess(NtCurrentProcess(),7); while(1){} }
 }
 // Also test that 10ms timeout is approx
 LARGE_INTEGER to10; to10.QuadPart = -100000LL; // 10ms = 100000 *10? Actually 10ms = 100000 (1ms=10000? Wait 1ms=10000, 10ms=100000)
 // correction: 1ms = 10000 (100ns), so 10ms = 100000, 100ms=1000000 previous correct
 qst = NtQueryPerformanceCounter(&start, &freq);
 st = NtWaitForSingleObject(hEv2, 0, &to10);
 NtQueryPerformanceCounter(&end, (LARGE_INTEGER*)0);
 if(st!=STATUS_TIMEOUT){ NtTerminateProcess(NtCurrentProcess(),8); while(1){} }
 if(freq.QuadPart!=0){
   LONGLONG diff = end.QuadPart - start.QuadPart;
   elapsed_ms = diff * 1000 / freq.QuadPart;
   if(elapsed_ms < 5 || elapsed_ms > 50){ NtTerminateProcess(NtCurrentProcess(),9); while(1){} }
 }

 NtClose(hEv); NtClose(hEv2);
 NtTerminateProcess(NtCurrentProcess(),0);
 while(1){}
}
