/*
 * test_k32_sys_pure.c - host tests for src/ntdll/k32_sys_pure.h: thread and
 * process priority mapping, handle flags, and the PE binary type from header
 * bytes.
 */
#include "minitest.h"
#include "k32_sys_pure.h"

#include <string.h>

/* The seven SetThreadPriority levels are valid; anything else is not. */
static void thread_priority_valid(void)
{
    MT_EXPECT(k32y_thread_priority_valid(0) == 1);
    MT_EXPECT(k32y_thread_priority_valid(-2) == 1);
    MT_EXPECT(k32y_thread_priority_valid(2) == 1);
    MT_EXPECT(k32y_thread_priority_valid(-15) == 1);
    MT_EXPECT(k32y_thread_priority_valid(15) == 1);
    MT_EXPECT(k32y_thread_priority_valid(3) == 0);
    MT_EXPECT(k32y_thread_priority_valid(-3) == 0);
    MT_EXPECT(k32y_thread_priority_valid(-1) == 1);
}

/* The inverse: bases 6..10 give -2..2, 1 and 15 give IDLE and TIME_CRITICAL,
 * and anything else is THREAD_PRIORITY_ERROR_RETURN. */
static void priority_from_thread_base(void)
{
    MT_EXPECT_EQ(k32y_priority_from_thread_base(8), 0);
    MT_EXPECT_EQ(k32y_priority_from_thread_base(6), -2);
    MT_EXPECT_EQ(k32y_priority_from_thread_base(10), 2);
    MT_EXPECT_EQ(k32y_priority_from_thread_base(1), -15);
    MT_EXPECT_EQ(k32y_priority_from_thread_base(15), 15);
    MT_EXPECT_EQ(k32y_priority_from_thread_base(5), K32Y_THREAD_PRIORITY_ERROR_RETURN);
    MT_EXPECT_EQ(k32y_priority_from_thread_base(11), K32Y_THREAD_PRIORITY_ERROR_RETURN);
}

/* Each Win32 class has an NT value, and the NT value maps back to the class. */
static void priority_class_round_trip(void)
{
    DWORD classes[6] = { K32Y_IDLE_PRIORITY_CLASS, K32Y_BELOW_NORMAL_PRIORITY_CLASS,
                         K32Y_NORMAL_PRIORITY_CLASS, K32Y_ABOVE_NORMAL_PRIORITY_CLASS,
                         K32Y_HIGH_PRIORITY_CLASS, K32Y_REALTIME_PRIORITY_CLASS };
    for (int i = 0; i < 6; ++i) {
        unsigned int nt = k32y_nt_class_from_priority(classes[i]);
        MT_EXPECT(nt != 0);
        MT_EXPECT_EQ(k32y_priority_class_from_nt(nt), classes[i]);
    }
    MT_EXPECT_EQ(k32y_nt_class_from_priority(0x1234), 0);
    MT_EXPECT_EQ(k32y_priority_class_from_nt(0), K32Y_NORMAL_PRIORITY_CLASS);
    MT_EXPECT_EQ(k32y_priority_class_from_nt(99), K32Y_NORMAL_PRIORITY_CLASS);
}

/* HANDLE_FLAG_INHERIT is 1, HANDLE_FLAG_PROTECT_FROM_CLOSE is 2. */
static void handle_flags_bits(void)
{
    MT_EXPECT_EQ(k32y_handle_flags(0, 0), 0);
    MT_EXPECT_EQ(k32y_handle_flags(1, 0), 1);
    MT_EXPECT_EQ(k32y_handle_flags(0, 1), 2);
    MT_EXPECT_EQ(k32y_handle_flags(1, 1), 3);
}

/* A minimal PE: MZ, e_lfanew at 0x40, PE\0\0, then the machine word. */
static void make_pe(unsigned char *buf, size_t n, unsigned int machine)
{
    memset(buf, 0, n);
    buf[0] = 'M';
    buf[1] = 'Z';
    buf[0x3c] = 0x80;
    buf[0x80] = 'P';
    buf[0x81] = 'E';
    buf[0x82] = 0;
    buf[0x83] = 0;
    buf[0x84] = (unsigned char)(machine & 0xFF);
    buf[0x85] = (unsigned char)(machine >> 8);
}

/* x64 is SCS_64BIT_BINARY, i386 is SCS_32BIT_BINARY; another machine, a bad
 * signature or a truncated header is not a supported binary. */
static void binary_type_from_header(void)
{
    unsigned char buf[0x100];
    DWORD type = 99;
    make_pe(buf, sizeof(buf), 0x8664);
    MT_EXPECT(k32y_binary_type(buf, sizeof(buf), &type) == 1);
    MT_EXPECT_EQ(type, K32Y_SCS_64BIT_BINARY);
    make_pe(buf, sizeof(buf), 0x014C);
    MT_EXPECT(k32y_binary_type(buf, sizeof(buf), &type) == 1);
    MT_EXPECT_EQ(type, K32Y_SCS_32BIT_BINARY);
    make_pe(buf, sizeof(buf), 0xAA64);
    MT_EXPECT(k32y_binary_type(buf, sizeof(buf), &type) == 0);
    make_pe(buf, sizeof(buf), 0x8664);
    buf[0x81] = 'X';
    MT_EXPECT(k32y_binary_type(buf, sizeof(buf), &type) == 0);
    make_pe(buf, sizeof(buf), 0x8664);
    MT_EXPECT(k32y_binary_type(buf, 0x40, &type) == 0);
    make_pe(buf, sizeof(buf), 0x8664);
    buf[0x3c] = 0xF0;
    buf[0x3d] = 0xFF;
    MT_EXPECT(k32y_binary_type(buf, sizeof(buf), &type) == 0);
    buf[0] = 'Q';
    MT_EXPECT(k32y_binary_type(buf, sizeof(buf), &type) == 0);
}

static const mt_case_t cases[] = {
    { "thread_priority_valid", thread_priority_valid },
    { "priority_from_thread_base", priority_from_thread_base },
    { "priority_class_round_trip", priority_class_round_trip },
    { "handle_flags_bits", handle_flags_bits },
    { "binary_type_from_header", binary_type_from_header },
};

int main(void)
{
    return MT_RUN("k32_sys_pure", cases);
}
