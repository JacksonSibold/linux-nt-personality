/*
 * test_ntstatus_to_dos.c - host tests for RtlNtStatusToDosError.
 *
 * Includes ntstatus_to_dos.c directly so the static table is visible. The
 * binary search relies on the table being strictly ascending by NTSTATUS, so
 * the first group of checks pins that invariant and then checks that every
 * entry round-trips through the lookup.
 *
 * Expected values are the Windows golden column recorded in
 * tests/differential/DISCREPANCIES.md (section 3), or the Win32 error named
 * in the Microsoft documentation for the NTSTATUS value.
 */
#include "minitest.h"
#include "ntstatus_to_dos.c"

static void table_is_strictly_ascending(void)
{
    for (size_t i = 1; i < NTSTATUS_TO_DOS_COUNT; ++i) {
        if (!(NtStatusToDosErrorTable[i - 1].Status < NtStatusToDosErrorTable[i].Status))
            mt_fail(__FILE__, __LINE__, "table not strictly ascending");
    }
}

static void every_entry_round_trips(void)
{
    for (size_t i = 0; i < NTSTATUS_TO_DOS_COUNT; ++i) {
        ULONG got = RtlNtStatusToDosError(NtStatusToDosErrorTable[i].Status);
        ULONG want = (ULONG)NtStatusToDosErrorTable[i].DosError;
        if (got != want)
            mt_fail_eq(__FILE__, __LINE__, "round trip", (long)got, (long)want);
    }
}

typedef struct {
    NTSTATUS status;
    ULONG want;
    const char *name;
} golden_case;

static const golden_case golden[] = {
    { 0x00000000L, 0,    "STATUS_SUCCESS" },
    { 0x00000102L, 1460, "STATUS_TIMEOUT" },
    { 0x00000103L, 997,  "STATUS_PENDING" },
    { 0x80000005L, 234,  "STATUS_BUFFER_OVERFLOW" },
    { 0xC0000002L, 1,    "STATUS_NOT_IMPLEMENTED" },
    { 0xC0000008L, 6,    "STATUS_INVALID_HANDLE" },
    { 0xC000000DL, 87,   "STATUS_INVALID_PARAMETER" },
    { 0xC000000EL, 433,  "STATUS_NO_SUCH_DEVICE" },
    { 0xC000000FL, 2,    "STATUS_NO_SUCH_FILE" },
    { 0xC0000011L, 38,   "STATUS_END_OF_FILE" },
    { 0xC0000017L, 8,    "STATUS_NO_MEMORY" },
    { 0xC0000022L, 5,    "STATUS_ACCESS_DENIED" },
    { 0xC0000023L, 122,  "STATUS_BUFFER_TOO_SMALL" },
    { 0xC0000034L, 2,    "STATUS_OBJECT_NAME_NOT_FOUND" },
    { 0xC0000035L, 183,  "STATUS_OBJECT_NAME_COLLISION" },
    { 0xC000003AL, 3,    "STATUS_OBJECT_PATH_NOT_FOUND" },
    { 0xC0000043L, 32,   "STATUS_SHARING_VIOLATION" },
    { 0xC0000101L, 145,  "STATUS_DIRECTORY_NOT_EMPTY" },
    { 0xC0000103L, 267,  "STATUS_NOT_A_DIRECTORY" },
    { 0xC0000106L, 206,  "STATUS_NAME_TOO_LONG" },
    { 0xC0000120L, 995,  "STATUS_CANCELLED" },
    { 0xC0000135L, 126,  "STATUS_DLL_NOT_FOUND" },
    { 0xC0000139L, 127,  "STATUS_ENTRYPOINT_NOT_FOUND" },
    { 0xC000007BL, 193,  "STATUS_INVALID_IMAGE_FORMAT" },
    { 0xC000009AL, 1450, "STATUS_INSUFFICIENT_RESOURCES" },
    { 0xC0000275L, 4390, "STATUS_NOT_A_REPARSE_POINT" },
    { 0xC0000281L, 161,  "STATUS_DIRECTORY_IS_A_REPARSE_POINT" },
};

static void golden_values_match(void)
{
    for (size_t i = 0; i < sizeof(golden) / sizeof(golden[0]); ++i) {
        ULONG got = RtlNtStatusToDosError(golden[i].status);
        if (got != golden[i].want)
            mt_fail_eq(__FILE__, __LINE__, golden[i].name, (long)got, (long)golden[i].want);
    }
}

static void unmapped_status_falls_back_to_317(void)
{
    /* Not in the table; the golden column gives ERROR_MR_MID_NOT_FOUND. */
    ULONG got = RtlNtStatusToDosError(0xC0000200L);
    if (got != 317)
        mt_fail_eq(__FILE__, __LINE__, "STATUS 0xC0000200", (long)got, 317);
}

static const mt_case_t cases[] = {
    { "table_is_strictly_ascending", table_is_strictly_ascending },
    { "every_entry_round_trips", every_entry_round_trips },
    { "golden_values_match", golden_values_match },
    { "unmapped_status_falls_back_to_317", unmapped_status_falls_back_to_317 },
};

int main(void)
{
    return mt_run("ntstatus_to_dos", cases, sizeof(cases) / sizeof(cases[0]));
}
