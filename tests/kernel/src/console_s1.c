/*
 * console_s1.c - S1 of the console milestone (docs/status/CONSOLE-MILESTONE.md).
 *
 * A nostdlib PE that calls the raw NT syscall interface only:
 *   NtWriteFile(StandardOutput, "hello from NT\n")  (service 0x08, 9 args)
 *   NtTerminateProcess(NtCurrentProcess, status)    (service 0x2c, 2 args)
 *
 * At entry RCX is the PEB (binfmt_pe, no-ntdll path). PEB+0x20 is
 * ProcessParameters and +0x28 of that is StandardOutput, a File handle the
 * kernel installs for the exec-ing task's fd 1. Exit status is the
 * NtWriteFile status, so 0 means the write succeeded and the process ended
 * through NtTerminateProcess.
 *
 * Syscall convention (as tests/kernel/src/minimal.c): eax = service number,
 * r10 = arg 1, rdx = arg 2, r8 = arg 3, r9 = arg 4, args 5 and up at
 * [rsp+0x28] onward at the time of the syscall instruction.
 */

__asm__(
    ".text\n"
    ".globl _start\n"
    "_start:\n"
    "    mov %rcx, %rbx\n"                  /* rbx = PEB */
    "    sub $0x58, %rsp\n"                 /* home + stack-arg area, iosb at 0x10(%rsp) */
    "    mov 0x20(%rbx), %rax\n"            /* ProcessParameters */
    "    mov 0x28(%rax), %r10\n"            /* StandardOutput -> arg 1 */
    "    xor %edx, %edx\n"                  /* Event = NULL */
    "    xor %r8d, %r8d\n"                  /* ApcRoutine = NULL */
    "    xor %r9d, %r9d\n"                  /* ApcContext = NULL */
    "    lea 0x10(%rsp), %rax\n"
    "    mov %rax, 0x28(%rsp)\n"            /* IoStatusBlock */
    "    lea msg(%rip), %rax\n"
    "    mov %rax, 0x30(%rsp)\n"            /* Buffer */
    "    movl $14, 0x38(%rsp)\n"            /* Length: 13 chars + newline */
    "    movq $0, 0x40(%rsp)\n"             /* ByteOffset = NULL */
    "    movq $0, 0x48(%rsp)\n"             /* Key = NULL */
    "    mov $0x08, %eax\n"                 /* NtWriteFile */
    "    syscall\n"
    "    mov %eax, %edx\n"                  /* exit status = NtWriteFile status */
    "    mov $-1, %r10\n"                   /* NtCurrentProcess */
    "    mov $0x2c, %eax\n"                 /* NtTerminateProcess */
    "    syscall\n"
    "    hlt\n"
    "msg:\n"
    "    .ascii \"hello from NT\\n\"\n"
);
