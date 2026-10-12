/*
 * test_win32_policy.c - host tests for the argument mapping in
 * src/ntdll/win32_policy.h, used by the kernel32 shims.
 */
#include "minitest.h"
#include "win32_policy.h"
#include "ntstatus.h"

/* K32-1: VirtualProtect must pass the requested protection to NT. */
static void protect_maps_each_base_value(void)
{
    static const struct { DWORD win; ULONG nt; } cases[] = {
        { 0x01, 1 },  /* PAGE_NOACCESS */
        { 0x02, 2 },  /* PAGE_READONLY */
        { 0x04, 4 },  /* PAGE_READWRITE */
        { 0x08, 8 },  /* PAGE_WRITECOPY */
        { 0x10, 16 }, /* PAGE_EXECUTE */
        { 0x20, 32 }, /* PAGE_EXECUTE_READ */
        { 0x40, 64 }, /* PAGE_EXECUTE_READWRITE */
        { 0x80, 128 },/* PAGE_EXECUTE_WRITECOPY */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ULONG nt = 0xdeadbeef;
        MT_EXPECT(win32_protect_to_nt(cases[i].win, &nt));
        MT_EXPECT_EQ(nt, cases[i].nt);
    }
}

static void protect_ignores_modifier_bits(void)
{
    ULONG nt = 0;
    MT_ASSERT(win32_protect_to_nt(0x20 | 0x100, &nt)); /* EXECUTE_READ | PAGE_GUARD */
    MT_EXPECT_EQ(nt, 32);
}

/* Zero and values with more than one base protection are invalid. */
static void protect_rejects_invalid_values(void)
{
    ULONG nt = 0;
    MT_EXPECT(!win32_protect_to_nt(0x00, &nt));
    MT_EXPECT(!win32_protect_to_nt(0x06, &nt)); /* READONLY | READWRITE */
}

/* K32-3: CREATE_NEW must map to FILE_CREATE and CREATE_ALWAYS to
 * FILE_OVERWRITE_IF. Without this, CREATE_NEW opens an existing file and
 * CREATE_ALWAYS fails on one. */
static void create_disposition_maps_win32_values(void)
{
    MT_EXPECT_EQ(win32_create_disposition_to_nt(1), 2); /* CREATE_NEW -> FILE_CREATE */
    MT_EXPECT_EQ(win32_create_disposition_to_nt(2), 5); /* CREATE_ALWAYS -> FILE_OVERWRITE_IF */
    MT_EXPECT_EQ(win32_create_disposition_to_nt(3), 1); /* OPEN_EXISTING -> FILE_OPEN */
    MT_EXPECT_EQ(win32_create_disposition_to_nt(4), 3); /* OPEN_ALWAYS -> FILE_OPEN_IF */
    MT_EXPECT_EQ(win32_create_disposition_to_nt(5), 4); /* TRUNCATE_EXISTING -> FILE_OVERWRITE */
}

/* K32-2: TlsAlloc must hand out distinct indexes, reuse freed ones, and fail
 * when all 64 are in use. */
static void tls_alloc_hands_out_distinct_indexes(void)
{
    ULONG bits[2] = { 0, 0 };
    MT_EXPECT_EQ(win32_tls_alloc(bits), 0);
    MT_EXPECT_EQ(win32_tls_alloc(bits), 1);
    MT_EXPECT_EQ(win32_tls_alloc(bits), 2);
}

static void tls_free_makes_index_reusable(void)
{
    ULONG bits[2] = { 0, 0 };
    MT_EXPECT_EQ(win32_tls_alloc(bits), 0);
    MT_EXPECT_EQ(win32_tls_alloc(bits), 1);
    MT_ASSERT(win32_tls_free(bits, 0));
    MT_EXPECT_EQ(win32_tls_alloc(bits), 0);
}

static void tls_alloc_fails_when_exhausted(void)
{
    ULONG bits[2] = { 0, 0 };
    for (DWORD i = 0; i < WIN32_TLS_MINIMUM_AVAILABLE; ++i)
        MT_ASSERT(win32_tls_alloc(bits) == i);
    MT_EXPECT_EQ(win32_tls_alloc(bits), WIN32_TLS_OUT_OF_INDEXES);
    /* Bit 63 is the last slot; the high word must be fully used. */
    MT_EXPECT_EQ(bits[0], 0xFFFFFFFFu);
    MT_EXPECT_EQ(bits[1], 0xFFFFFFFFu);
}

static void tls_free_rejects_unallocated_index(void)
{
    ULONG bits[2] = { 0, 0 };
    MT_EXPECT(!win32_tls_free(bits, 0));
    MT_EXPECT(!win32_tls_free(bits, 64));
    MT_EXPECT(!win32_tls_free(bits, 0xFFFFFFFFu));
}

/* K32-5: WaitForSingleObject must wait on the object and report what the
 * wait returned, instead of returning WAIT_OBJECT_0 at once. */
static void wait_timeout_conversion(void)
{
    LARGE_INTEGER t;
    t.QuadPart = 0;
    MT_EXPECT(win32_timeout_to_nt(WIN32_INFINITE, &t) == NULL);

    PLARGE_INTEGER p = win32_timeout_to_nt(0, &t);
    MT_ASSERT(p == &t);
    MT_EXPECT_EQ(p->QuadPart, 0);

    p = win32_timeout_to_nt(1500, &t);
    MT_ASSERT(p == &t);
    MT_EXPECT_EQ(p->QuadPart, -15000000); /* 1500 ms in 100 ns units, relative */
}

static void wait_result_mapping(void)
{
    MT_EXPECT_EQ(win32_wait_result(STATUS_WAIT_0), 0);            /* WAIT_OBJECT_0 */
    MT_EXPECT_EQ(win32_wait_result(STATUS_ABANDONED_WAIT_0), 0x80); /* WAIT_ABANDONED */
    MT_EXPECT_EQ(win32_wait_result(STATUS_TIMEOUT), 258);          /* WAIT_TIMEOUT */
    MT_EXPECT_EQ(win32_wait_result((NTSTATUS)0xC0000008L), 0xFFFFFFFFu); /* WAIT_FAILED */
}

/* K32-7: CreateEvent's bManualReset selects NotificationEvent (0) or
 * SynchronizationEvent (1). A failed NT create must give NULL, never a
 * constant that looks like a handle. */
static void sync_event_type_maps_manual_reset(void)
{
    MT_EXPECT_EQ(win32_event_type_to_nt(1), 0); /* manual reset -> NotificationEvent */
    MT_EXPECT_EQ(win32_event_type_to_nt(0), 1); /* auto reset -> SynchronizationEvent */
}

static void sync_create_result_is_handle_or_null(void)
{
    HANDLE h = (HANDLE)(intptr_t)0x1234;
    MT_EXPECT(win32_sync_create_result(STATUS_SUCCESS, h) == h);
    MT_EXPECT(win32_sync_create_result((NTSTATUS)0xC0000017L, h) == NULL); /* NO_MEMORY */
}

static const mt_case_t cases[] = {
    MT_CASE(sync_event_type_maps_manual_reset),
    MT_CASE(sync_create_result_is_handle_or_null),
    MT_CASE(wait_timeout_conversion),
    MT_CASE(wait_result_mapping),
    MT_CASE(protect_maps_each_base_value),
    MT_CASE(protect_ignores_modifier_bits),
    MT_CASE(protect_rejects_invalid_values),
    MT_CASE(create_disposition_maps_win32_values),
    MT_CASE(tls_alloc_hands_out_distinct_indexes),
    MT_CASE(tls_free_makes_index_reusable),
    MT_CASE(tls_alloc_fails_when_exhausted),
    MT_CASE(tls_free_rejects_unallocated_index),
};

int main(void)
{
    return MT_RUN("win32_policy", cases);
}
