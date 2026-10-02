/* handles_probe.c - handle table probe via NtClose and pseudo-handles */
void _start() {
    __asm__ volatile(
        "sub $64, %%rsp\n"
        // Test 1: NtClose with invalid handle 0x1234 -> STATUS_INVALID_HANDLE (0xC0000008)
        "mov $0x1234, %%rcx\n"
        "mov %%rcx, %%r10\n"
        "mov $0x0f, %%eax\n"
        "syscall\n"
        "cmp $0xC0000008, %%eax\n"
        "jne fail\n"
        // Test 2: NtClose with NULL (0) -> should fail (invalid handle)
        "xor %%ecx, %%ecx\n"
        "mov %%rcx, %%r10\n"
        "mov $0x0f, %%eax\n"
        "syscall\n"
        "test %%eax, %%eax\n"
        "jz fail\n"
        // Test 3: NtClose with pseudo handle -1 (GetCurrentProcess) -> should fail STATUS_INVALID_HANDLE (pseudo not closable)
        "mov $-1, %%rcx\n"
        "mov %%rcx, %%r10\n"
        "mov $0x0f, %%eax\n"
        "syscall\n"
        "cmp $0xC0000008, %%eax\n"
        "jne fail\n"
        // Test 4: NtClose with pseudo -2 (GetCurrentThread) -> should fail
        "mov $-2, %%rcx\n"
        "mov %%rcx, %%r10\n"
        "mov $0x0f, %%eax\n"
        "syscall\n"
        "cmp $0xC0000008, %%eax\n"
        "jne fail\n"
        // Success: handle table correctly distinguishes real vs pseudo vs invalid
        "mov $0x2c, %%eax\n"
        "mov $-1, %%rcx\n"
        "xor %%edx, %%edx\n"
        "mov %%rcx, %%r10\n"
        "syscall\n"
        "hlt\n"
        "fail:\n"
        "mov $0x2c, %%eax\n"
        "mov $-1, %%rcx\n"
        "mov $2, %%edx\n"
        "mov %%rcx, %%r10\n"
        "syscall\n"
        "hlt\n"
        ::: "rax", "rcx", "rdx", "r10", "memory", "cc"
    );
    while(1){}
}
