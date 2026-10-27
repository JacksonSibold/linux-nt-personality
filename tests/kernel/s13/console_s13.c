/*
 * console_s13.c - S13: a userspace PE drives a kernel-loaded driver end to end
 * (driver/hello_drv.sys, tests/kernel-drivers/hello_drv). Every expected value is
 * written in this file, not read from a recorded output.
 *
 * The service record is installed at boot from the kernel command line,
 *   ntpers_driver=hello_drv:\SystemRoot\drivers\hello_drv.sys
 * because user mode cannot write HKLM (driver/drvhost_svc.c). Steps, through ntdll.dll
 * imports (tests/kernel/Makefile target s13). Each step writes "S13 <step> begin" and
 * "S13 <step> end <status>" to the standard output, so the serial log shows which step
 * ran last:
 *   load    NtLoadDriver on the service key (the driver host loads and binds hello_drv.sys)
 *   open    NtCreateFile on \??\HelloDrv (the driver's symbolic link)
 *   ping    IOCTL_HELLO_PING: the reply is 'HELO' and version 0x00010000
 *   add     IOCTL_HELLO_ADD with 0xFFFFFFFF and 1: the reply is 0x100000000
 *   close   NtClose the device
 *   unload  NtUnloadDriver
 * Exit status: 0 when every check passes; 12 to 17 name the first step that failed.
 */

typedef long NTSTATUS;
typedef void *HANDLE;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned short WCHAR;
typedef unsigned long long ULONGLONG;
#define NULL ((void *)0)

typedef struct {
    USHORT Length;
    USHORT MaximumLength;
    WCHAR *Buffer;
} UNICODE_STRING;

typedef struct {
    ULONG Length;
    HANDLE RootDirectory;
    UNICODE_STRING *ObjectName;
    ULONG Attributes;
    void *SecurityDescriptor;
    void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

typedef struct {
    NTSTATUS Status;
    ULONGLONG Information;
} IO_STATUS_BLOCK;

__declspec(dllimport) NTSTATUS NtSetValueKey(HANDLE KeyHandle, UNICODE_STRING *ValueName,
                                             ULONG TitleIndex, ULONG Type, void *Data, ULONG DataSize);
__declspec(dllimport) NTSTATUS NtLoadDriver(UNICODE_STRING *DriverServiceName);
__declspec(dllimport) NTSTATUS NtUnloadDriver(UNICODE_STRING *DriverServiceName);
__declspec(dllimport) NTSTATUS NtCreateFile(HANDLE *FileHandle, ULONG DesiredAccess,
                                            OBJECT_ATTRIBUTES *ObjectAttributes,
                                            IO_STATUS_BLOCK *IoStatusBlock, void *AllocationSize,
                                            ULONG FileAttributes, ULONG ShareAccess,
                                            ULONG CreateDisposition, ULONG CreateOptions,
                                            void *EaBuffer, ULONG EaLength);
__declspec(dllimport) NTSTATUS NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, void *ApcRoutine,
                                                     void *ApcContext, IO_STATUS_BLOCK *IoStatusBlock,
                                                     ULONG IoControlCode, void *InputBuffer,
                                                     ULONG InputBufferLength, void *OutputBuffer,
                                                     ULONG OutputBufferLength);
__declspec(dllimport) NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, void *ApcRoutine,
                                           void *ApcContext, IO_STATUS_BLOCK *IoStatusBlock,
                                           const void *Buffer, ULONG Length, void *ByteOffset, void *Key);
__declspec(dllimport) NTSTATUS NtClose(HANDLE Handle);
__declspec(dllimport) NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);

#define GENERIC_READ_WRITE 0xC0000000u
#define IOCTL_HELLO_PING 0x80002000u   /* CTL_CODE(0x8000, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS) */
#define IOCTL_HELLO_ADD  0x80002008u   /* CTL_CODE(0x8000, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS) */
#define HELLO_MAGIC 0x4F4C4548u        /* 'H' 'E' 'L' 'O', little-endian */
#define HELLO_VERSION 0x00010000u

static const WCHAR key_path[] = u"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\hello_drv";
static const WCHAR dev_path[] = u"\\\?\?\\HelloDrv";   /* \??\HelloDrv (\? avoids a trigraph) */

static ULONG wlen(const WCHAR *s)
{
    ULONG n = 0;
    while (s[n]) n++;
    return n;
}

/* Builds a UNICODE_STRING over a static string (Length in bytes, no terminator counted). */
static void ustr(UNICODE_STRING *u, const WCHAR *s)
{
    u->Length = (USHORT)(wlen(s) * 2);
    u->MaximumLength = (USHORT)(u->Length + 2);
    u->Buffer = (WCHAR *)s;
}

static void oa_init(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *name)
{
    oa->Length = sizeof *oa;
    oa->RootDirectory = 0;
    oa->ObjectName = name;
    oa->Attributes = 0;
    oa->SecurityDescriptor = 0;
    oa->SecurityQualityOfService = 0;
}

/* The standard output handle, read from the PEB as S1 and S2 do. */
static HANDLE stdout_handle(void)
{
    void *peb;
    void *params;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    params = *(void **)((char *)peb + 0x20);
    return *(HANDLE *)((char *)params + 0x28);
}

static void say(const char *s)
{
    ULONG n = 0;
    IO_STATUS_BLOCK iosb = {0, 0};
    while (s[n]) n++;
    NtWriteFile(stdout_handle(), 0, 0, 0, &iosb, s, n, 0, 0);
}

static void say_num(ULONG v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && i > 0);
    say(&b[i]);
}

/* Logs the start of a step. */
static void begin(const char *step)
{
    say("S13 ");
    say(step);
    say(" begin\n");
}

/* Logs the end of a step with its status, then exits with `code` when it failed. */
static void end(const char *step, NTSTATUS st, ULONG fail_code)
{
    say("S13 ");
    say(step);
    say(" end status ");
    say_num((ULONG)st);
    say("\n");
    if (fail_code) {
        say("S13 FAIL ");
        say(step);
        say("\n");
        NtTerminateProcess((HANDLE)(long long)-1, (NTSTATUS)fail_code);
        for (;;) {
        }
    }
}

void _start(void)
{
    UNICODE_STRING kn, dn;
    OBJECT_ATTRIBUTES oa;
    HANDLE dev = 0;
    NTSTATUS st;
    IO_STATUS_BLOCK iosb = {0, 0};
    ULONG ping[2] = {0, 0};
    ULONGLONG add_out = 0;
    ULONG add_in[2] = {0xFFFFFFFFu, 1u};

    ustr(&kn, key_path);
    ustr(&dn, dev_path);
    oa_init(&oa, &dn);

    begin("load");
    st = NtLoadDriver(&kn);
    end("load", st, st != 0 ? 12 : 0);

    begin("open");
    st = NtCreateFile(&dev, GENERIC_READ_WRITE, &oa, &iosb, NULL, 0, 3, 1, 0, NULL, 0);
    end("open", st, st != 0 ? 13 : 0);

    begin("ping");
    st = NtDeviceIoControlFile(dev, NULL, NULL, NULL, &iosb, IOCTL_HELLO_PING, NULL, 0,
                               ping, sizeof ping);
    end("ping", st, (st != 0 || iosb.Status != 0 || ping[0] != HELLO_MAGIC || ping[1] != HELLO_VERSION) ? 14 : 0);

    begin("add");
    st = NtDeviceIoControlFile(dev, NULL, NULL, NULL, &iosb, IOCTL_HELLO_ADD, add_in,
                               sizeof add_in, &add_out, sizeof add_out);
    end("add", st, (st != 0 || add_out != 0x100000000ull) ? 15 : 0);

    begin("close");
    st = NtClose(dev);
    end("close", st, st != 0 ? 16 : 0);

    begin("unload");
    st = NtUnloadDriver(&kn);
    end("unload", st, st != 0 ? 17 : 0);

    say("S13 PASS\n");
    NtTerminateProcess((HANDLE)(long long)-1, 0);
    for (;;) {
    }
}
