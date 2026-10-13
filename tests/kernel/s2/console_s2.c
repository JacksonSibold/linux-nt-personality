/*
 * console_s2.c - S2 of the console milestone (docs/status/CONSOLE-MILESTONE.md).
 *
 * The same write as S1, but NtWriteFile and NtTerminateProcess are imports from
 * ntdll.dll. The exe is linked against an import library made from
 * src/ntdll/out/ntdll.def (tests/kernel/Makefile target s2), so the PE has an
 * import directory. LdrInitializeThunk (src/ntdll/ntdll_main.c) patches the IAT
 * and then calls _start with no arguments.
 *
 * The PEB is read from gs:0x60 (TEB+0x60, set by binfmt_pe pe_init_teb).
 * PEB+0x20 is ProcessParameters, and its +0x28 is StandardOutput (S1).
 */

typedef long NTSTATUS;
typedef void *HANDLE;
typedef unsigned long ULONG;

typedef struct {
    NTSTATUS Status;
    unsigned long long Information;
} IO_STATUS_BLOCK;

__declspec(dllimport) NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, void *ApcRoutine,
                                           void *ApcContext, IO_STATUS_BLOCK *IoStatusBlock,
                                           const void *Buffer, ULONG Length,
                                           void *ByteOffset, void *Key);
__declspec(dllimport) NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);

static const char msg[] = "hello from NT via ntdll\n";

void _start(void)
{
    void *peb;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    void *params = *(void **)((char *)peb + 0x20);
    HANDLE out = *(HANDLE *)((char *)params + 0x28);
    IO_STATUS_BLOCK iosb = {0, 0};
    NTSTATUS st = NtWriteFile(out, 0, 0, 0, &iosb, msg, sizeof(msg) - 1, 0, 0);
    NtTerminateProcess((HANDLE)(long long)-1, st);
    for (;;) {
    }
}
