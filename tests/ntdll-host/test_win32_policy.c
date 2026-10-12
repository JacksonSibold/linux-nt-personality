/*
 * test_win32_policy.c - host tests for the argument mapping in
 * src/ntdll/win32_policy.h, used by the kernel32 shims.
 */
#include "minitest.h"
#include "win32_policy.h"

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

static const mt_case_t cases[] = {
    MT_CASE(protect_maps_each_base_value),
    MT_CASE(protect_ignores_modifier_bits),
    MT_CASE(protect_rejects_invalid_values),
};

int main(void)
{
    return MT_RUN("win32_policy", cases);
}
