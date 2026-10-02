/*
 * syscall_stub.S
 *
 * x86-64 NT syscall stubs generated from syscall_table.def.
 *
 * Stub shape (byte-identical to Windows ntdll on Server 2022):
 *      mov  r10, rcx      ; 49 89 CA
 *      mov  eax, <SSN>    ; B8 XX XX XX XX
 *      syscall            ; 0F 05
 *      ret                ; C3
 *
 * The SSN is encoded as a 32-bit immediate even when the value fits in 8 bits,
 * matching the fixed 11-byte stub layout that anti-tamper code hashes.
 */

    .text
    .align 16

/*
 * NT_SYSCALL macro: emits one naked stub.  Arg types are ignored by the
 * assembler; only Name and Ssn are used.
 */
#define NT_SYSCALL(Name, Ssn, Arity, ...) \
    .globl Name;                          \
    .set Name##_ssn, Ssn;                 \
    Name:                                 \
        mov %rcx, %r10;                   \
        mov $Ssn, %eax;                   \
        syscall;                          \
        ret

#include "syscall_table.def"

#undef NT_SYSCALL
