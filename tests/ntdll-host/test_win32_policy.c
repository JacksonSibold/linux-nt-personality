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

static const mt_case_t cases[] = {
    MT_CASE(protect_maps_each_base_value),
    MT_CASE(protect_ignores_modifier_bits),
    MT_CASE(protect_rejects_invalid_values),
    MT_CASE(create_disposition_maps_win32_values),
};

int main(void)
{
    return MT_RUN("win32_policy", cases);
}
