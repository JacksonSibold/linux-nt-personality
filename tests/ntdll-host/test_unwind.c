/*
 * test_unwind.c - host tests for RUNTIME_FUNCTION lookup in unwind.c.
 *
 * Registers synthetic modules whose image is a host buffer and whose
 * ImageBase is an arbitrary 64-bit address. RtlAddFunctionTable (ntdll_main.c)
 * registers dynamic tables with sizeOfImage = 0 because it has no image size.
 * A zero size must mean "bounded by the table", not "contains no address".
 */
#include "minitest.h"
#include "unwind.h"

#include <stdint.h>
#include <string.h>

#define FAKE_BASE 0x140000000ULL

static uint8_t g_image[0x3000];

static IMAGE_RUNTIME_FUNCTION_ENTRY g_table[2];

static void setup_table(void)
{
    memset(g_image, 0, sizeof(g_image));
    memset(g_table, 0, sizeof(g_table));
    g_table[0].BeginAddress = 0x1000;
    g_table[0].EndAddress = 0x1100;
    g_table[0].DUMMYUNIONNAME.UnwindInfoAddress = 0x2000;
    g_table[1].BeginAddress = 0x1200;
    g_table[1].EndAddress = 0x1300;
    g_table[1].DUMMYUNIONNAME.UnwindInfoAddress = 0x2010;
    unwind_clear_modules();
}

static void sized_module_finds_function(void)
{
    setup_table();
    MT_ASSERT(unwind_register_module(FAKE_BASE, 0x3000, g_table, 2, g_image) == 0);

    ULONG64 base = 0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e = RtlLookupFunctionEntry(FAKE_BASE + 0x1050, &base, NULL);
    MT_EXPECT(e == &g_table[0]);
    MT_EXPECT(base == FAKE_BASE);
    unwind_clear_modules();
}

static void zero_size_module_finds_function(void)
{
    setup_table();
    MT_ASSERT(unwind_register_module(FAKE_BASE, 0, g_table, 2, g_image) == 0);

    ULONG64 base = 0;
    IMAGE_RUNTIME_FUNCTION_ENTRY *e = RtlLookupFunctionEntry(FAKE_BASE + 0x1250, &base, NULL);
    MT_EXPECT(e == &g_table[1]);
    MT_EXPECT(base == FAKE_BASE);
    unwind_clear_modules();
}

static void zero_size_module_misses_outside_table(void)
{
    setup_table();
    MT_ASSERT(unwind_register_module(FAKE_BASE, 0, g_table, 2, g_image) == 0);

    /* Between the two entries and past the last one: no function. */
    MT_EXPECT(RtlLookupFunctionEntry(FAKE_BASE + 0x1150, NULL, NULL) == NULL);
    MT_EXPECT(RtlLookupFunctionEntry(FAKE_BASE + 0x1500, NULL, NULL) == NULL);
    unwind_clear_modules();
}

static void address_below_base_is_not_found(void)
{
    setup_table();
    MT_ASSERT(unwind_register_module(FAKE_BASE, 0, g_table, 2, g_image) == 0);

    MT_EXPECT(RtlLookupFunctionEntry(FAKE_BASE - 1, NULL, NULL) == NULL);
    unwind_clear_modules();
}

static const mt_case_t cases[] = {
    { "sized_module_finds_function", sized_module_finds_function },
    { "zero_size_module_finds_function", zero_size_module_finds_function },
    { "zero_size_module_misses_outside_table", zero_size_module_misses_outside_table },
    { "address_below_base_is_not_found", address_below_base_is_not_found },
};

int main(void)
{
    return mt_run("unwind", cases, sizeof(cases) / sizeof(cases[0]));
}
