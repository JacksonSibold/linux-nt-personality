/* smc_probe.c - Self-modifying code probe: XOR-decrypt-in-place then execute, then read own bytes back
 * Uses payload embedded in .text (RWX due to binfmt_pe mapping prot|PROT_WRITE), so no NtAllocate needed.
 * Payload is mov eax,42; ret encrypted with XOR 0xAA.
 */
void _start() {
    __asm__ volatile(
        // jump over payload to avoid executing encrypted bytes
        "jmp 1f\n"
        "payload:\n"
        ".byte 0x12,0x80,0xAA,0xAA,0xAA,0x69\n" // encrypted B8 2A 00 00 00 C3
        "1:\n"
        "lea payload(%%rip), %%r15\n"
        // decrypt loop: for i=0..5: byte ^= 0xAA
        "xor %%rcx, %%rcx\n"
        "2:\n"
        "cmp $6, %%rcx\n"
        "jge 3f\n"
        "movb (%%r15,%%rcx,1), %%al\n"
        "xor $0xAA, %%al\n"
        "movb %%al, (%%r15,%%rcx,1)\n"
        "inc %%rcx\n"
        "jmp 2b\n"
        "3:\n"
        // cpuid serializing
        "push %%rbx\n"
        "xor %%eax, %%eax\n"
        "cpuid\n"
        "pop %%rbx\n"
        // verify decrypted bytes
        "cmpb $0xB8, (%%r15)\n"
        "jne fail_byte\n"
        "cmpb $0x2A, 1(%%r15)\n"
        "jne fail_byte\n"
        "cmpb $0x00, 2(%%r15)\n"
        "jne fail_byte\n"
        "cmpb $0x00, 3(%%r15)\n"
        "jne fail_byte\n"
        "cmpb $0x00, 4(%%r15)\n"
        "jne fail_byte\n"
        "cmpb $0xC3, 5(%%r15)\n"
        "jne fail_byte\n"
        // execute
        "call *%%r15\n"
        "cmp $42, %%eax\n"
        "jne fail_exec\n"
        // read back again
        "cmpb $0xB8, (%%r15)\n"
        "jne fail_byte2\n"
        "cmpb $0xC3, 5(%%r15)\n"
        "jne fail_byte2\n"
        // success
        "mov $0x2c, %%eax\n"
        "mov $-1, %%rcx\n"
        "xor %%edx, %%edx\n"
        "mov %%rcx, %%r10\n"
        "syscall\n"
        "hlt\n"
        "fail_byte:\n"
        "mov $0x2c, %%eax\n"
        "mov $-1, %%rcx\n"
        "mov $3, %%edx\n"
        "mov %%rcx, %%r10\n"
        "syscall\n"
        "hlt\n"
        "fail_exec:\n"
        "mov $0x2c, %%eax\n"
        "mov $-1, %%rcx\n"
        "mov $4, %%edx\n"
        "mov %%rcx, %%r10\n"
        "syscall\n"
        "hlt\n"
        "fail_byte2:\n"
        "mov $0x2c, %%eax\n"
        "mov $-1, %%rcx\n"
        "mov $5, %%edx\n"
        "mov %%rcx, %%r10\n"
        "syscall\n"
        "hlt\n"
        ::: "rax","rcx","rdx","r10","r15","rbx","memory","cc"
    );
    while(1){}
}
