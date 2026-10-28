/* drvhost_rtl.c - Rtl string and version services, C-library-style memory and
 * string functions for drivers, and the Mm address probes.
 */

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

/* Upper-case mapping for the scripts drivers meet in device and registry names: ASCII, Latin-1, Greek, Cyrillic. The
 * complete Unicode table lives in the user-mode Rtl (src/ntdll/rtl/nls.c); the host does not carry it. */
WCHAR dh_upcase(WCHAR c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
    if (c == 0xb5) return 0x39c;
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return (WCHAR)(c - 0x20);
    if (c == 0xff) return 0x178;
    if (c >= 0x3b1 && c <= 0x3c9) return c == 0x3c2 ? 0x3a3 : (WCHAR)(c - 0x20);
    if (c >= 0x430 && c <= 0x44f) return (WCHAR)(c - 0x20);
    if (c >= 0x450 && c <= 0x45f) return (WCHAR)(c - 0x50);
    return c;
}

static WCHAR fold(WCHAR c)
{
    return dh_upcase(c);
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
#ifdef __KERNEL__
    {
        /* One-byte read that returns an error instead of faulting. */
        unsigned char b;
        return copy_from_kernel_nofault(&b, VirtualAddress, 1) == 0;
    }
#else
    uintptr_t page = (uintptr_t)VirtualAddress & ~(uintptr_t)(getpagesize() - 1);
    unsigned char vec = 0;
    if (mincore((void *)page, (size_t)getpagesize(), &vec) != 0) return FALSE;
    return (vec & 1) != 0;
#endif
}

DRVCALL PVOID MmGetSystemRoutineAddress(PUNICODE_STRING SystemRoutineName)
{
    if (!SystemRoutineName || !SystemRoutineName->Buffer) return NULL;
    char name[DH_NAME_MAX];
    dh_ascii_from_unicode(SystemRoutineName, name, sizeof name);
    return dh_host_export(name, strlen(name));
}

DRVCALL WCHAR RtlUpcaseUnicodeChar(WCHAR Source)
{
    return dh_upcase(Source);
}

DRVCALL NTSTATUS RtlAppendUnicodeToString(PUNICODE_STRING Dest, PCWSTR Source)
{
    if (!Source) return STATUS_SUCCESS;
    SIZE_T n = drv_wcslen(Source) * sizeof(WCHAR);
    if ((SIZE_T)Dest->Length + n > Dest->MaximumLength) return STATUS_BUFFER_TOO_SMALL;
    memcpy((char *)Dest->Buffer + Dest->Length, Source, n);
    Dest->Length = (USHORT)(Dest->Length + n);
    if (Dest->Length + sizeof(WCHAR) <= Dest->MaximumLength)
        *(WCHAR *)((char *)Dest->Buffer + Dest->Length) = 0;
    return STATUS_SUCCESS;
}

DRVCALL SIZE_T RtlCompareMemory(const VOID *Source1, const VOID *Source2, SIZE_T Length)
{
    const unsigned char *a = Source1, *b = Source2;
    SIZE_T i = 0;
    while (i < Length && a[i] == b[i]) i++;
    return i;
}

/* ---- time conversion ------------------------------------------------------------------------------------------- */
#define DH_TICKS_PER_MS   10000ull
#define DH_TICKS_PER_SEC  10000000ull
#define DH_TICKS_PER_DAY  (86400ull * DH_TICKS_PER_SEC)

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int days_in_month(int y, int m)
{
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && is_leap(y) ? 29 : dim[m - 1];
}

/* Days from 1601-01-01 to January 1 of year y (y >= 1601), on the proleptic Gregorian calendar. */
static unsigned long long days_before_year(int y)
{
    unsigned long long n = (unsigned long long)(y - 1601);
    return n * 365 + n / 4 - n / 100 + n / 400;
}

DRVCALL void RtlTimeToTimeFields(PLARGE_INTEGER Time, PTIME_FIELDS TimeFields)
{
    unsigned long long t = (unsigned long long)Time->QuadPart;
    unsigned long long days = t / DH_TICKS_PER_DAY;
    unsigned long long rem = t % DH_TICKS_PER_DAY;

    TimeFields->Milliseconds = (CSHORT)((rem / DH_TICKS_PER_MS) % 1000);
    unsigned long long secs = rem / DH_TICKS_PER_SEC;
    TimeFields->Second = (CSHORT)(secs % 60);
    TimeFields->Minute = (CSHORT)((secs / 60) % 60);
    TimeFields->Hour = (CSHORT)(secs / 3600);
    TimeFields->Weekday = (CSHORT)((days + 1) % 7);        /* 1601-01-01 was a Monday */

    int y = 1601 + (int)(days / 366);                      /* never too large; walk forward */
    while (days >= days_before_year(y + 1)) y++;
    unsigned long long d = days - days_before_year(y);
    int m = 1;
    while (d >= (unsigned long long)days_in_month(y, m)) { d -= (unsigned long long)days_in_month(y, m); m++; }
    TimeFields->Year = (CSHORT)y;
    TimeFields->Month = (CSHORT)m;
    TimeFields->Day = (CSHORT)(d + 1);
}

DRVCALL BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS TimeFields, PLARGE_INTEGER Time)
{
    const TIME_FIELDS *tf = TimeFields;
    if (tf->Milliseconds < 0 || tf->Milliseconds > 999 || tf->Second < 0 || tf->Second > 59 ||
        tf->Minute < 0 || tf->Minute > 59 || tf->Hour < 0 || tf->Hour > 23 ||
        tf->Month < 1 || tf->Month > 12 || tf->Day < 1 || tf->Year < 1601)
        return FALSE;
    if (tf->Day > days_in_month(tf->Year, tf->Month)) return FALSE;

    unsigned long long days = days_before_year(tf->Year);
    for (int m = 1; m < tf->Month; m++) days += (unsigned long long)days_in_month(tf->Year, m);
    days += (unsigned long long)(tf->Day - 1);
    unsigned long long secs = (unsigned long long)tf->Hour * 3600 + (unsigned long long)tf->Minute * 60 +
                              (unsigned long long)tf->Second;
    Time->QuadPart = (LONGLONG)(days * DH_TICKS_PER_DAY + secs * DH_TICKS_PER_SEC +
                                (unsigned long long)tf->Milliseconds * DH_TICKS_PER_MS);
    return TRUE;
}

/* The personality keeps one time zone, UTC, until the time-zone information service exists. */
DRVCALL void ExSystemTimeToLocalTime(PLARGE_INTEGER SystemTime, PLARGE_INTEGER LocalTime)
{
    LocalTime->QuadPart = SystemTime->QuadPart;
}

DRVCALL void ExLocalTimeToSystemTime(PLARGE_INTEGER LocalTime, PLARGE_INTEGER SystemTime)
{
    SystemTime->QuadPart = LocalTime->QuadPart;
}
