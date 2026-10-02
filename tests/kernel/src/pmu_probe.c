/* pmu_probe.c - PE benchmark for PMU fingerprint comparison
 * Same-source loop as pmu_elf.c: sum 0..N-1, N=10 million, to generate retired instructions profile
 * Exit 0, returns sum low byte via NtTerminate (not used for perf)
 */
void _start() {
    __asm__ volatile(
        "sub $32, %%rsp\n"
        "mov $10000000, %%rcx\n"   // N
        "xor %%rax, %%rax\n"       // sum
        "xor %%rdx, %%rdx\n"       // i
        "1:\n"
        "cmp %%rcx, %%rdx\n"
        "jge 2f\n"
        "add %%rdx, %%rax\n"
        "inc %%rdx\n"
        "jmp 1b\n"
        "2:\n"
        // prevent optimization: store sum to stack and compare
        "mov %%rax, (%%rsp)\n"
        // expected sum = N*(N-1)/2 = 49999995000000 = 0x2D02D67C9C00
        // we just check low 32 bits non-zero to ensure loop ran
        "test %%rax, %%rax\n"
        "jz fail\n"
        "mov $0x2c, %%eax\n"
        "mov $-1, %%rcx\n"
        "xor %%edx, %%edx\n"
        "mov %%rcx, %%r10\n"
        "syscall\n"
        "hlt\n"
        "fail:\n"
        "mov $0x2c, %%eax\n"
        "mov $-1, %%rcx\n"
        "mov $1, %%edx\n"
        "mov %%rcx, %%r10\n"
        "syscall\n"
        "hlt\n"
        ::: "rax","rcx","rdx","r10","memory","cc"
    );
    while(1){}
}
