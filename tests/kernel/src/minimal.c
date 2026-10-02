__attribute__((naked, noreturn)) void _start() {
    __asm__ volatile(
        "mov $0x2c, %eax\n"
        "mov $-1, %rcx\n"
        "mov $42, %edx\n"
        "mov %rcx, %r10\n"
        "syscall\n"
        "hlt\n"
    );
    __builtin_unreachable();
}
