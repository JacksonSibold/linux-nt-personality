/* drvhost_rtl.c - Rtl string and version services, C-library-style memory and
 * string functions for drivers, and the Mm address probes.
 */
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drvhost_int.h"
#include "os_identity.h"

#define DH_RTL_TAG_UNICODE 0x74735272u  /* 'rStr' */
#define DH_RTL_TAG_ANSI    0x6d745272u  /* 'rMt' */

DRVCALL void RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Source)
{
    if (!Source) {
        Dest->Length = 0;
        Dest->MaximumLength = 0;
        Dest->Buffer = NULL;
        return;
    }
    size_t chars = drv_wcslen(Source);
    if (chars > 0x7FFF - 1) chars = 0x7FFF - 1;
    Dest->Length = (USHORT)(chars * 2);
    Dest->MaximumLength = (USHORT)(chars * 2 + 2);
    Dest->Buffer = (PWSTR)Source;
}

DRVCALL void RtlInitAnsiString(PANSI_STRING Dest, const CHAR *Source)
{
    if (!Source) {
        Dest->Length = 0;
        Dest->MaximumLength = 0;
        Dest->Buffer = NULL;
        return;
    }
    size_t n = drv_strlen(Source);
    if (n > 0xFFFE) n = 0xFFFE;
    Dest->Length = (USHORT)n;
    Dest->MaximumLength = (USHORT)(n + 1);
    Dest->Buffer = (PCHAR)Source;
}

DRVCALL NTSTATUS RtlCopyUnicodeString(PUNICODE_STRING Dest, PCUNICODE_STRING Src)
{
    if (!Dest) return STATUS_INVALID_PARAMETER;
    if (!Src) { Dest->Length = 0; return STATUS_SUCCESS; }
    USHORT n = Src->Length;
    if (n > Dest->MaximumLength) n = Dest->MaximumLength;
    if (n && Dest->Buffer) memcpy(Dest->Buffer, Src->Buffer, n);
    Dest->Length = n;
    return STATUS_SUCCESS;
}

static WCHAR fold(WCHAR c)
{
    if (c >= 'a' && c <= 'z') return (WCHAR)(c - 32);
    return c;
}

DRVCALL BOOLEAN RtlEqualUnicodeString(PCUNICODE_STRING A, PCUNICODE_STRING B, BOOLEAN CaseInsensitive)
{
    if (A->Length != B->Length) return FALSE;
    USHORT n = A->Length / 2;
    for (USHORT i = 0; i < n; i++) {
        WCHAR x = A->Buffer[i], y = B->Buffer[i];
        if (CaseInsensitive) { x = fold(x); y = fold(y); }
        if (x != y) return FALSE;
    }
    return TRUE;
}

DRVCALL LONG RtlCompareUnicodeString(PCUNICODE_STRING A, PCUNICODE_STRING B, BOOLEAN CaseInsensitive)
{
    USHORT na = A->Length / 2, nb = B->Length / 2;
    USHORT n = na < nb ? na : nb;
    for (USHORT i = 0; i < n; i++) {
        WCHAR x = A->Buffer[i], y = B->Buffer[i];
        if (CaseInsensitive) { x = fold(x); y = fold(y); }
        if (x != y) return (LONG)x - (LONG)y;
    }
    return (LONG)na - (LONG)nb;
}

DRVCALL void RtlFreeUnicodeString(PUNICODE_STRING Str)
{
    if (Str && Str->Buffer) {
        ExFreePoolWithTag(Str->Buffer, DH_RTL_TAG_UNICODE);
        Str->Buffer = NULL;
        Str->Length = 0;
        Str->MaximumLength = 0;
    }
}

DRVCALL NTSTATUS RtlAppendUnicodeStringToString(PUNICODE_STRING Dest, PCUNICODE_STRING Src)
{
    if (!Dest || !Src) return STATUS_INVALID_PARAMETER;
    if ((SIZE_T)Dest->Length + Src->Length > Dest->MaximumLength) return STATUS_BUFFER_TOO_SMALL;
    if (Src->Length) memcpy((char *)Dest->Buffer + Dest->Length, Src->Buffer, Src->Length);
    Dest->Length = (USHORT)(Dest->Length + Src->Length);
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS RtlUnicodeStringToAnsiString(PANSI_STRING Dest, PCUNICODE_STRING Src, BOOLEAN Alloc)
{
    if (!Dest || !Src) return STATUS_INVALID_PARAMETER;
    USHORT chars = Src->Length / 2;
    USHORT need = (USHORT)(chars + 1);
    if (Alloc) {
        Dest->Buffer = ExAllocatePoolWithTag(NonPagedPool, need, DH_RTL_TAG_ANSI);
        if (!Dest->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
        Dest->MaximumLength = need;
    } else if (need > Dest->MaximumLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (USHORT i = 0; i < chars; i++) {
        WCHAR c = Src->Buffer[i];
        Dest->Buffer[i] = (c < 0x80) ? (CHAR)c : '?';
    }
    Dest->Length = chars;
    if (Dest->MaximumLength > chars) Dest->Buffer[chars] = 0;
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS RtlAnsiStringToUnicodeString(PUNICODE_STRING Dest, PANSI_STRING Src, BOOLEAN Alloc)
{
    if (!Dest || !Src) return STATUS_INVALID_PARAMETER;
    USHORT chars = Src->Length;
    USHORT need_bytes = (USHORT)((chars + 1) * 2);
    if (Alloc) {
        Dest->Buffer = ExAllocatePoolWithTag(NonPagedPool, need_bytes, DH_RTL_TAG_UNICODE);
        if (!Dest->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
        Dest->MaximumLength = need_bytes;
    } else if (need_bytes > Dest->MaximumLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (USHORT i = 0; i < chars; i++) Dest->Buffer[i] = (WCHAR)(unsigned char)Src->Buffer[i];
    Dest->Length = (USHORT)(chars * 2);
    if (Dest->MaximumLength >= Dest->Length + 2) Dest->Buffer[chars] = 0;
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS RtlGetVersion(PRTL_OSVERSIONINFOW VersionInformation)
{
    if (!VersionInformation) return STATUS_INVALID_PARAMETER;
    if (VersionInformation->dwOSVersionInfoSize < sizeof(RTL_OSVERSIONINFOW))
        return STATUS_INVALID_PARAMETER;
    VersionInformation->dwMajorVersion = NT_OS_MAJOR_VERSION;
    VersionInformation->dwMinorVersion = NT_OS_MINOR_VERSION;
    VersionInformation->dwBuildNumber = NT_OS_BUILD_NUMBER;
    VersionInformation->dwPlatformId = NT_OS_PLATFORM_ID;
    VersionInformation->szCSDVersion[0] = 0;
    return STATUS_SUCCESS;
}

DRVCALL void *drv_memcpy(void *Dst, const void *Src, SIZE_T N)
{
    return memcpy(Dst, Src, N);
}

DRVCALL void *drv_memmove(void *Dst, const void *Src, SIZE_T N)
{
    return memmove(Dst, Src, N);
}

DRVCALL void *drv_memset(void *Dst, int C, SIZE_T N)
{
    return memset(Dst, C, N);
}

DRVCALL int drv_memcmp(const void *A, const void *B, SIZE_T N)
{
    return memcmp(A, B, N);
}

DRVCALL SIZE_T drv_strlen(const CHAR *S)
{
    return strlen(S);
}

DRVCALL SIZE_T drv_wcslen(const WCHAR *S)
{
    SIZE_T n = 0;
    while (S[n]) n++;
    return n;
}

DRVCALL BOOLEAN MmIsAddressValid(PVOID VirtualAddress)
{
    if (!VirtualAddress) return FALSE;
    uintptr_t page = (uintptr_t)VirtualAddress & ~(uintptr_t)(getpagesize() - 1);
    unsigned char vec = 0;
    if (mincore((void *)page, (size_t)getpagesize(), &vec) != 0) return FALSE;
    return (vec & 1) != 0;
}

DRVCALL PVOID MmGetSystemRoutineAddress(PUNICODE_STRING SystemRoutineName)
{
    if (!SystemRoutineName || !SystemRoutineName->Buffer) return NULL;
    char name[DH_NAME_MAX];
    dh_ascii_from_unicode(SystemRoutineName, name, sizeof name);
    return dh_host_export(name, strlen(name));
}
