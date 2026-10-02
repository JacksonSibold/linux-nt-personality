/* ntstatus_probe.c - exercises NTSTATUS handling via invalid handle
 * Calls NtClose with invalid handle and checks for STATUS_INVALID_HANDLE, then tests
 * NtQueryObject with small buffer for STATUS_INFO_LENGTH_MISMATCH etc.
 * This exercises the NTSTATUS -> error path without needing ntdll.
 */
void _start() {
    __asm__ volatile(
        "sub $64, %%rsp\n"
        // NtClose with invalid handle 0x1234 should return STATUS_INVALID_HANDLE (0xC0000008)
        "mov $0x1234, %%rcx\n"
        "mov %%rcx, %%r10\n"
        "mov $0x0f, %%eax\n"
        "syscall\n"
        "cmp $0xC0000008, %%eax\n"
        "jne fail\n"
        // NtClose with NULL should fail
        "xor %%ecx, %%ecx\n"
        "mov %%rcx, %%r10\n"
        "mov $0x0f, %%eax\n"
        "syscall\n"
        // Should be invalid handle as well (maybe not, but check not success)
        "test %%eax, %%eax\n"
        "jz fail\n"
        // NtQueryObject with invalid handle small buffer should give INFO_LENGTH_MISMATCH or INVALID_HANDLE
        "mov $0x1234, %%rcx\n"
        "mov %%rcx, %%r10\n"
        "xor %%edx, %%edx\n"
        "xor %%r8d, %%r8d\n"
        "xor %%r9d, %%r9d\n"
        "mov $0, %%rax\n"
        "mov %%rax, 0x28(%%rsp)\n"
        "mov $0x10, %%eax\n"  // NtQueryObject SSN 0x10
        "syscall\n"
        // Any non-zero is expected (not success)
        "test %%eax, %%eax\n"
        "jz fail\n"
        // Success
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
        ::: "rax", "rcx", "rdx", "r10", "r8", "r9", "memory", "cc"
    );
    while(1){}
}
