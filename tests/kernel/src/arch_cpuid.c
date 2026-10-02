void _start() {
    __asm__ volatile(
        "mov $1, %%eax\n"          // CPUID leaf 1
        "xor %%ecx, %%ecx\n"
        "cpuid\n"                  // results in ECX bit 31 = hypervisor
        "bt $31, %%ecx\n"          // CF = hypervisor bit
        "jnc 1f\n"
        "mov $1, %%edx\n"          // hypervisor=1 -> edx=1
        "jmp 2f\n"
        "1: xor %%edx, %%edx\n"    // hypervisor=0 -> edx=0
        "2: mov $0x2c, %%eax\n"     // NtTerminateProcess
        "mov $-1, %%rcx\n"
        // edx already holds hypervisor bit (0 or 1) as exit code
        "mov %%rcx, %%r10\n"
        "syscall\n"
        "hlt\n"
        ::: "rax", "rcx", "rdx", "r10", "rbx", "cc", "memory"
    );
    while(1){}
}
