/* minimal allocation test */
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef void* HANDLE;
typedef void* PVOID;
typedef long NTSTATUS;
#define STATUS_SUCCESS 0
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_READWRITE 0x04
#define NULL ((void*)0)
__asm__(".text\n"
".globl NtAllocateVirtualMemory\nNtAllocateVirtualMemory: mov %rcx, %r10; mov $0x18, %eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx, %r10; mov $0x2c, %eax; syscall; ret\n"
".globl linux_write\nlinux_write: mov $1, %eax; syscall; ret\n"
);
NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress, ULONGLONG ZeroBits, ULONGLONG *RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);
long linux_write(int fd, const void *buf, unsigned long len);
#define NtCurrentProcess() ((HANDLE)(long)-1)

void _start(void){
    __asm__ volatile("sub $128, %rsp");
    PVOID base = 0;
    ULONGLONG sz = 1; // request 1 byte, should round to 4096
    NTSTATUS st = NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &sz, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    char out[256];
    // simple hex print
    // check results
    // if st !=0 => exit 10
    // if base==0 => exit 20 (bug)
    // if sz != 4096 => exit 30 (rounding bug)
    // try write to base
    // exit 42 success, else other
    if(st != STATUS_SUCCESS){
        const char *msg="FAIL status not success\n";
        linux_write(1, msg, 24);
        NtTerminateProcess(NtCurrentProcess(), 10);
    }
    if(base == 0){
        const char *msg="FAIL base is 0 (writeback bug)\n";
        linux_write(1, msg, 29);
        NtTerminateProcess(NtCurrentProcess(), 20);
    }
    if(sz != 4096){
        const char *msg="FAIL size not rounded\n";
        linux_write(1, msg, 22);
        NtTerminateProcess(NtCurrentProcess(), 30);
    }
    // try write
    volatile int *p = (volatile int*)base;
    *p = 0x12345678;
    if(*p != 0x12345678){
        const char *msg="FAIL mem write\n";
        linux_write(1, msg, 14);
        NtTerminateProcess(NtCurrentProcess(), 40);
    }
    const char *msg="PASS alloc ok base non-zero size 4096 write ok\n";
    linux_write(1, msg, 44);
    NtTerminateProcess(NtCurrentProcess(), 42);
    __asm__ volatile("hlt");
}
