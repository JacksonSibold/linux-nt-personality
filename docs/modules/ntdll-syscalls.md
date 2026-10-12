# Module: ntdll-syscalls (system-call stubs and the export surface)

Sources: `src/ntdll/syscall_stub.S`, `src/ntdll/syscall_table.def`, `src/ntdll/nt_syscalls.h`, `src/ntdll/ntdll.def`
Related: `docs/modules/dispatch.md` (the kernel-side dispatch table; this review did not check whether it is built from `syscall_table.def`), `ntdll-startup.md` (the non-syscall ntdll exports), `docs/review/ntdll.md`.

## Purpose

Each `Nt*` and `Zw*` function that user code calls is a thin stub. The stub moves the first argument into R10 (the x64 syscall convention), loads the system service number (SSN) into EAX, and executes `syscall`. The table in `syscall_table.def` is the single list of stubs and prototypes. The header says the SSNs are frozen for Windows Server 2022 (build 20348), taken from a public list, and that new services are added only at the end.

## Files

- `syscall_table.def`: 171 `NT_SYSCALL(Name, Ssn, Arity, ArgTypes...)` lines. This is an X-macro, so each file that includes it defines the macro first.
- `syscall_stub.S`: expands the table into global functions. The macro emits the symbol `Name`, a `Name_ssn` assembler symbol, and the body.
- `nt_syscalls.h`: expands the same table into prototypes, declared `__attribute__((naked))`. It also defines `NT_SYSCALL_COUNT` (171) and `NT_SYSCALL_MAX_SSN` (`0x01db`).
- `ntdll.def`: the export list. It has about 370 entries. The `Nt*` names are exported with `Zw*` aliases (`ZwCreateFile=NtCreateFile`). The non-syscall exports are described in `ntdll-startup.md`, `ntdll-except.md` and `ntdll-unwind.md`.

## Stub shape

Each stub is 11 bytes:

```
49 89 CA          mov  r10, rcx        ; move first argument to r10 for syscall
B8 xx xx xx xx    mov  eax, <SSN>      ; service number as a 32-bit immediate
0F 05             syscall
C3                ret
```

The SSN is always a 32-bit immediate, so the stub layout is fixed. The header notes that this matches Windows' stub bytes, which anti-tamper code may hash.

## Table properties (checked for this page)

- 171 entries. Every name begins with `Nt` and is unique. Every SSN is unique. The largest SSN is `0x01db`, matching `NT_SYSCALL_MAX_SSN`.
- Arity ranges from 0 (`NtTestAlert`) to 11 (`NtCreateUserProcess`). The arity is documentation only; the stub does not check it.
- The table is the frozen contract with the kernel dispatcher. A change to an existing SSN breaks every binary built against the old value.

## Behaviour and invariants

- The stub does not touch the stack. It relies on the caller's shadow space and argument layout being the same as the kernel expects.
- Return values are NTSTATUS in EAX, as the kernel sets them.
- Calls that the kernel does not implement return `STATUS_NOT_IMPLEMENTED` from the dispatcher, not from the stub.

## Known gaps

- The SSN values, and the argument types in the table, were not checked against any reference in this review (see the "Not verified" section of `docs/review/ntdll.md`).
- `ntdll.def` was checked for names with a source definition. The `Nt*` and `Zw*` names come from the generated stub macro, and were not checked one by one against the table.
- `KiUserApcDispatcher` is empty, so the APC-related stubs (`NtQueueApcThread`, `NtTestAlert`) deliver no user APCs in this build (see `ntdll-startup.md`).
- `ntdll.def` exported `_DllMainCRTStartup@12`, an x86 decoration that x64 builds do not produce. Fixed: the export is now undecorated (DEF-1).
