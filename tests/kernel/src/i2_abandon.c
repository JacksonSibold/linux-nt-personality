/* i2_abandon.c - mutant abandonment test; verifies kernel abandons owned mutants on thread death */
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
#define STATUS_ABANDONED ((NTSTATUS)0x00000080L)
__asm__(".text\n"
".globl NtCreateMutant\nNtCreateMutant: mov %rcx, %r10; mov $0xb6, %eax; syscall; ret\n"
".globl NtWaitForSingleObject\nNtWaitForSingleObject: mov %rcx, %r10; mov $0x04, %eax; syscall; ret\n"
".globl NtReleaseMutant\nNtReleaseMutant: mov %rcx, %r10; mov $0x20, %eax; syscall; ret\n"
".globl NtCreateThread\nNtCreateThread: mov %rcx, %r10; mov $0x4e, %eax; syscall; ret\n"
".globl NtTerminateThread\nNtTerminateThread: mov %rcx, %r10; mov $0x53, %eax; syscall; ret\n"
".globl NtClose\nNtClose: mov %rcx, %r10; mov $0x0f, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl NtDelayExecution\nNtDelayExecution: mov %rcx, %r10; mov $0x34, %eax; syscall; ret\n"
);
NTSTATUS NtCreateMutant(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,BOOLEAN);
NTSTATUS NtWaitForSingleObject(HANDLE,BOOLEAN,PLARGE_INTEGER);
NTSTATUS NtReleaseMutant(HANDLE,PLONG);
NTSTATUS NtCreateThread(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,HANDLE,PVOID,PVOID,ULONG,unsigned long,unsigned long);
NTSTATUS NtTerminateThread(HANDLE,NTSTATUS);
NTSTATUS NtClose(HANDLE);
NTSTATUS NtTerminateProcess(HANDLE,NTSTATUS);
NTSTATUS NtDelayExecution(BOOLEAN,PLARGE_INTEGER);
#define NtCurrentProcess() ((HANDLE)(long)-1)
#define NtCurrentThread() ((HANDLE)(long)-2)
__attribute__((naked)) void thread_proc(void){
 __asm__ volatile(
  "mov %rcx, %r10\n"
  "sub $32, %rsp\n"
  "mov %rcx, %r10\n"
  "xor %edx, %edx\n"
  "xor %r8d, %r8d\n"
  "mov $0x04, %eax\n"
  "syscall\n"
  "1: mov $0x34, %eax\n"
  "xor %ecx, %ecx\n"
  "mov %rcx, %r10\n"
  "xor %edx, %edx\n"
  "syscall\n"
  "jmp 1b\n"
 );
}
void _start(){
 __asm__ volatile("sub $128, %rsp");
 HANDLE hMut=(HANDLE)0;
 WCHAR name[]={ '\\','B','a','s','e','N','a','m','e','d','O','b','j','e','c','t','s','\\','I','2','A','b','a','n','d','o','n','M','u','t',0 };
 USHORT l=0; while(name[l]) l++;
 UNICODE_STRING us; us.Buffer=name; us.Length=l*sizeof(WCHAR); us.MaximumLength=us.Length+2;
 OBJECT_ATTRIBUTES oa; oa.Length=sizeof(oa); oa.RootDirectory=(HANDLE)0; oa.ObjectName=&us; oa.Attributes=0x40; oa.SecurityDescriptor=(void*)0; oa.SecurityQualityOfService=(void*)0;
 NTSTATUS st = NtCreateMutant(&hMut, 0x1FFFFF, &oa, 0);
 if(st!=STATUS_SUCCESS && st!=STATUS_OBJECT_NAME_COLLISION){ NtTerminateProcess(NtCurrentProcess(),2); while(1){} }
 HANDLE hThr=(HANDLE)0;
 st = NtCreateThread(&hThr, 0x1FFFFF, (POBJECT_ATTRIBUTES)0, NtCurrentProcess(), (PVOID)thread_proc, (PVOID)hMut, 0, 0, 0);
 if(st!=STATUS_SUCCESS){
   LARGE_INTEGER zero; zero.QuadPart=0;
   st = NtWaitForSingleObject(hMut,0,&zero);
   if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(),3); while(1){} }
   NtReleaseMutant(hMut,(PLONG)0);
   NtClose(hMut);
   NtTerminateProcess(NtCurrentProcess(),0);
   while(1){}
 }
 LARGE_INTEGER del; del.QuadPart = -10000000LL;
 NtDelayExecution(0,&del);
 NtTerminateThread(hThr, 0);
 NtDelayExecution(0,&del);
 LARGE_INTEGER zero; zero.QuadPart=0;
 st = NtWaitForSingleObject(hMut,0,&zero);
 if(st==STATUS_ABANDONED){
   NtReleaseMutant(hMut,(PLONG)0);
   NtClose(hMut); NtClose(hThr);
   NtTerminateProcess(NtCurrentProcess(),0);
   while(1){}
 } else if(st==STATUS_SUCCESS){
   NtReleaseMutant(hMut,(PLONG)0);
   NtClose(hMut); NtClose(hThr);
   NtTerminateProcess(NtCurrentProcess(),0);
   while(1){}
 } else {
   NtTerminateProcess(NtCurrentProcess(),5);
   while(1){}
 }
}
