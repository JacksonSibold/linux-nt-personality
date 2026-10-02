typedef unsigned long long SIZE_T;
typedef void* HANDLE;
typedef long NTSTATUS;
typedef unsigned long ULONG;
__asm__(".text\n"
".globl NtWriteFile\nNtWriteFile: mov %rcx,%r10; mov $0x08,%eax; syscall; ret\n"
".globl NtTerminateProcess\nNtTerminateProcess: mov %rcx,%r10; mov $0x2c,%eax; syscall; ret\n"
);
NTSTATUS NtWriteFile(HANDLE h, HANDLE e, void *a, void *b, void *ios, void *buf, ULONG len, void *off, ULONG *k);
NTSTATUS NtTerminateProcess(HANDLE h, NTSTATUS s);
void _start(void){
    const char *msg="hello direct\n";
    long ios[2];
    NtWriteFile((HANDLE)(long)-11,0,0,0,ios,(void*)msg,13,0,0);
    NtTerminateProcess((HANDLE)(long)-1, 42);
    while(1){}
}
