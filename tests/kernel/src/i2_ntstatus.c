/* i2_ntstatus.c - NTSTATUS->DOS spot check via real syscall */
typedef int NTSTATUS;
typedef unsigned long ULONG;
typedef void* HANDLE;
typedef unsigned long *PULONG;
#define STATUS_SUCCESS ((NTSTATUS)0)
__asm__(".text\n"
".globl NtRtlNtStatusToDosError\nNtRtlNtStatusToDosError: mov %rcx, %r10; mov $0x1db, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
);
NTSTATUS NtRtlNtStatusToDosError(NTSTATUS Status, PULONG DosError);
NTSTATUS NtTerminateProcess(HANDLE,NTSTATUS);
#define NtCurrentProcess() ((HANDLE)(long)-1)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008L)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005L)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017L)
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#define STATUS_MUTANT_NOT_OWNED ((NTSTATUS)0xC0000046L)
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED ((NTSTATUS)0xC0000047L)
#define STATUS_SHARING_VIOLATION ((NTSTATUS)0xC0000043L)
void _start(){
 __asm__ volatile("sub $128, %rsp");
 struct { NTSTATUS st; ULONG expect; } cases[] = {
   { STATUS_SUCCESS, 0 },
   { STATUS_ACCESS_DENIED, 5 },
   { STATUS_INVALID_HANDLE, 6 },
   { STATUS_OBJECT_NAME_NOT_FOUND, 2 },
   { STATUS_OBJECT_NAME_COLLISION, 183 },
   { STATUS_BUFFER_TOO_SMALL, 122 },
   { STATUS_BUFFER_OVERFLOW, 234 },
   { STATUS_NO_MEMORY, 8 },
   { STATUS_TIMEOUT, 1460 },
   { STATUS_MUTANT_NOT_OWNED, 288 },
   { STATUS_SEMAPHORE_LIMIT_EXCEEDED, 298 },
   { STATUS_SHARING_VIOLATION, 32 },
   { (NTSTATUS)0xC0000022L, 5 },
   { (NTSTATUS)0xC0000008L, 6 },
   { (NTSTATUS)0x80000005L, 234 },
   { (NTSTATUS)0xC0000035L, 183 },
   { (NTSTATUS)0xC0000034L, 2 },
   { (NTSTATUS)0xC000000DL, 87 },
   { (NTSTATUS)0xC0000043L, 32 },
   { (NTSTATUS)0xC0000023L, 122 },
 };
 int n = sizeof(cases)/sizeof(cases[0]);
 for(int i=0;i<n;i++){
   ULONG dos=0xFFFFFFFF;
   NTSTATUS st = NtRtlNtStatusToDosError(cases[i].st, &dos);
   if(st!=STATUS_SUCCESS || dos!=cases[i].expect){ NtTerminateProcess(NtCurrentProcess(), 10+i); while(1){} }
 }
 ULONG dos;
 NTSTATUS st = NtRtlNtStatusToDosError((NTSTATUS)0xC1234567, &dos);
 if(st!=STATUS_SUCCESS || dos!=317){ NtTerminateProcess(NtCurrentProcess(), 40); while(1){} }
 st = NtRtlNtStatusToDosError((NTSTATUS)0x20000001, &dos);
 if(st!=STATUS_SUCCESS || dos!=0x20000001){ NtTerminateProcess(NtCurrentProcess(), 41); while(1){} }
 st = NtRtlNtStatusToDosError((NTSTATUS)0x80070005, &dos);
 if(st!=STATUS_SUCCESS || dos!=5){ NtTerminateProcess(NtCurrentProcess(), 42); while(1){} }
 for(int i=0;i<50;i++){
   NTSTATUS s = (NTSTATUS)(0xC0000000 + i);
   st = NtRtlNtStatusToDosError(s, &dos);
   if(st!=STATUS_SUCCESS){ NtTerminateProcess(NtCurrentProcess(), 43); while(1){} }
 }
 NtTerminateProcess(NtCurrentProcess(), 0);
 while(1){}
}
