/*
 * test_k32_sync_pure.c - host tests for src/ntdll/k32_sync_pure.h: the
 * WaitForMultipleObjects argument check and result mapping, the wait type,
 * the critical-section count rules and GetTickCount arithmetic.
 */
#include "minitest.h"
#include "k32_sync_pure.h"

#include <stdint.h>

/* Count must be 1..64, and no handle may be NULL. */
static void wait_multi_args(void)
{
    void *h[2] = { (void *)0x10, (void *)0x20 };
    void *bad[2] = { (void *)0x10, NULL };
    MT_EXPECT(k32s_wait_multi_args_ok(0, (const void *const *)h) == 0);
    MT_EXPECT(k32s_wait_multi_args_ok(1, NULL) == 0);
    MT_EXPECT(k32s_wait_multi_args_ok(2, (const void *const *)h) == 1);
    MT_EXPECT(k32s_wait_multi_args_ok(2, (const void *const *)bad) == 0);
    MT_EXPECT(k32s_wait_multi_args_ok(K32S_MAXIMUM_WAIT_OBJECTS + 1, (const void *const *)h) == 0);
}

/* WaitAll is 0 and WaitAny is 1 in the NT interface. */
static void wait_type_values(void)
{
    MT_EXPECT_EQ(k32s_wait_type(1), 0);
    MT_EXPECT_EQ(k32s_wait_type(0), 1);
}

/* WaitAny: STATUS_WAIT_0 + i is index i; an abandoned index adds 0x80; a
 * timeout is WAIT_TIMEOUT; anything else is WAIT_FAILED. An index past the
 * count is not an index of this wait. */
static void wait_multi_result_mapping(void)
{
    MT_EXPECT_EQ(k32s_wait_multi_result(STATUS_WAIT_0 + 0, 3), 0);
    MT_EXPECT_EQ(k32s_wait_multi_result(STATUS_WAIT_0 + 2, 3), 2);
    MT_EXPECT_EQ(k32s_wait_multi_result(STATUS_WAIT_0 + 3, 3), K32S_WAIT_FAILED);
    MT_EXPECT_EQ(k32s_wait_multi_result(STATUS_ABANDONED_WAIT_0 + 1, 3), K32S_WAIT_ABANDONED + 1);
    MT_EXPECT_EQ(k32s_wait_multi_result(STATUS_TIMEOUT, 3), K32S_WAIT_TIMEOUT);
    MT_EXPECT_EQ(k32s_wait_multi_result(STATUS_INVALID_PARAMETER, 3), K32S_WAIT_FAILED);
}

/* A free section has LockCount -1. The first enter raises it to 0 and the
 * caller owns the section; a later enter must wait. */
static void cs_enter_wait_decision(void)
{
    MT_EXPECT(k32s_cs_must_wait(0) == 0);
    MT_EXPECT(k32s_cs_must_wait(1) == 1);
    MT_EXPECT(k32s_cs_must_wait(2) == 1);
    MT_EXPECT_EQ(K32S_CS_LOCK_FREE, -1);
}

/* Leave signals the event only while a waiter is counted. */
static void cs_leave_signal_decision(void)
{
    MT_EXPECT(k32s_cs_must_signal(-1) == 0);
    MT_EXPECT(k32s_cs_must_signal(0) == 1);
    MT_EXPECT(k32s_cs_must_signal(3) == 1);
}

/* Ownership is by thread: the same thread re-enters by counting, another
 * thread must wait. A NULL owner is never the caller. */
static void cs_owner_check(void)
{
    HANDLE t1 = (HANDLE)(uintptr_t)0x100;
    HANDLE t2 = (HANDLE)(uintptr_t)0x200;
    MT_EXPECT(k32s_cs_owned_by(t1, t1) == 1);
    MT_EXPECT(k32s_cs_owned_by(t1, t2) == 0);
    MT_EXPECT(k32s_cs_owned_by(NULL, t1) == 0);
    MT_EXPECT(k32s_cs_owned_by(NULL, NULL) == 0);
}

/* GetTickCount: 1 ms is freq/1000 counts. An exact count gives exact ms, and
 * a zero or negative frequency gives 0. */
static void tick_count_arithmetic(void)
{
    MT_EXPECT_EQ(k32s_tick_count_ms(0, 10000000), 0);
    MT_EXPECT_EQ(k32s_tick_count_ms(10000000, 10000000), 1000);
    MT_EXPECT_EQ(k32s_tick_count_ms(12345, 1000), 12345);
    MT_EXPECT_EQ(k32s_tick_count_ms(5, 0), 0);
    MT_EXPECT_EQ(k32s_tick_count_ms(5, -1), 0);
    MT_EXPECT_EQ(k32s_tick_count_ms(-1, 1000), 0);
}

/* A large counter does not overflow the 64-bit product: 2^40 counts at 1 GHz
 * is about 1099 seconds. */
static void tick_count_large_counter(void)
{
    LONGLONG freq = 1000000000LL;
    LONGLONG count = (LONGLONG)1 << 40;
    DWORD ms = k32s_tick_count_ms(count, freq);
    MT_EXPECT_EQ(ms, 1099511);
}

static const mt_case_t cases[] = {
    { "wait_multi_args", wait_multi_args },
    { "wait_type_values", wait_type_values },
    { "wait_multi_result_mapping", wait_multi_result_mapping },
    { "cs_enter_wait_decision", cs_enter_wait_decision },
    { "cs_leave_signal_decision", cs_leave_signal_decision },
    { "cs_owner_check", cs_owner_check },
    { "tick_count_arithmetic", tick_count_arithmetic },
    { "tick_count_large_counter", tick_count_large_counter },
};

int main(void)
{
    return MT_RUN("k32_sync_pure", cases);
}
