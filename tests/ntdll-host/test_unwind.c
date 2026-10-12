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

/* x64 RUNTIME_FUNCTION has no flag bits in EndAddress (UNW-4). The last
 * bytes of a function, including an end that is not 4-aligned, must match. */
static void end_address_is_not_masked(void)
{
    setup_table();
    IMAGE_RUNTIME_FUNCTION_ENTRY t[1];
    memset(t, 0, sizeof(t));
    t[0].BeginAddress = 0x1400;
    t[0].EndAddress = 0x1407;
    MT_ASSERT(unwind_register_module(FAKE_BASE, 0x3000, t, 1, g_image) == 0);

    MT_EXPECT(RtlLookupFunctionEntry(FAKE_BASE + 0x1406, NULL, NULL) == &t[0]);
    MT_EXPECT(RtlLookupFunctionEntry(FAKE_BASE + 0x1407, NULL, NULL) == NULL);
    unwind_clear_modules();
}

/* RtlVirtualUnwind returns only the handler whose flag matches HandlerType
 * (UNW-3). An exception filter (EHANDLER) must not come back during an unwind
 * request (UHANDLER), and the reverse. */
static void handler_type_selects_matching_handler(void)
{
    setup_table();
    memset(g_image + 0x2100, 0, 8);
    g_image[0x2100] = (uint8_t)(1 | (UNW_FLAG_EHANDLER << 3));
    uint32_t hrva = 0x1500;
    memcpy(g_image + 0x2104, &hrva, sizeof(hrva));

    IMAGE_RUNTIME_FUNCTION_ENTRY t[1];
    memset(t, 0, sizeof(t));
    t[0].BeginAddress = 0x1400;
    t[0].EndAddress = 0x1500;
    t[0].DUMMYUNIONNAME.UnwindInfoAddress = 0x2100;
    MT_ASSERT(unwind_register_module(FAKE_BASE, 0x3000, t, 1, g_image) == 0);

    ULONG64 stack[2] = { 0x7000, 0 };
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.Rip = FAKE_BASE + 0x1450;
    ctx.Rsp = (ULONG64)(uintptr_t)stack;

    ULONG64 est = 0;
    PVOID hd = NULL;
    CONTEXT c = ctx;
    PVOID h = RtlVirtualUnwind(UNW_FLAG_UHANDLER, FAKE_BASE, ctx.Rip, &t[0], &c, &hd, &est, NULL);
    MT_EXPECT(h == NULL);

    c = ctx;
    h = RtlVirtualUnwind(UNW_FLAG_EHANDLER, FAKE_BASE, ctx.Rip, &t[0], &c, &hd, &est, NULL);
    MT_EXPECT(h == (PVOID)(uintptr_t)(g_image + 0x1500));
    unwind_clear_modules();
}

/* A PC with no RUNTIME_FUNCTION and no registered module is a leaf frame: the
 * return address is at RSP (EXC-2). Dispatch depends on this to pass such a
 * frame instead of stopping. */
static void leaf_frame_without_module_pops_return_address(void)
{
    unwind_clear_modules();
    ULONG64 stack[2] = { 0x7000, 0 };
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.Rip = 0x500000;
    ctx.Rsp = (ULONG64)(uintptr_t)stack;

    ULONG64 est = 0;
    RtlVirtualUnwind(0, 0, ctx.Rip, NULL, &ctx, NULL, &est, NULL);
    MT_EXPECT(ctx.Rip == 0x7000);
    MT_EXPECT(ctx.Rsp == (ULONG64)(uintptr_t)stack + 8);
}

/* RtlDeleteFunctionTable must remove the table from the registry (MAIN-7). */
static void deleted_table_is_no_longer_searched(void)
{
    setup_table();
    MT_ASSERT(unwind_register_module(FAKE_BASE, 0, g_table, 2, g_image) == 0);
    MT_ASSERT(RtlLookupFunctionEntry(FAKE_BASE + 0x1050, NULL, NULL) == &g_table[0]);

    MT_EXPECT(unwind_unregister_table(g_table) == 0);
    MT_EXPECT(unwind_module_count() == 0);
    MT_EXPECT(RtlLookupFunctionEntry(FAKE_BASE + 0x1050, NULL, NULL) == NULL);
    MT_EXPECT(unwind_unregister_table(g_table) != 0);
    unwind_clear_modules();
}

static const mt_case_t cases[] = {
    { "deleted_table_is_no_longer_searched", deleted_table_is_no_longer_searched },
    { "leaf_frame_without_module_pops_return_address", leaf_frame_without_module_pops_return_address },
    { "handler_type_selects_matching_handler", handler_type_selects_matching_handler },
    { "end_address_is_not_masked", end_address_is_not_masked },
    { "sized_module_finds_function", sized_module_finds_function },
    { "zero_size_module_finds_function", zero_size_module_finds_function },
    { "zero_size_module_misses_outside_table", zero_size_module_misses_outside_table },
    { "address_below_base_is_not_found", address_below_base_is_not_found },
};

int main(void)
{
    return mt_run("unwind", cases, sizeof(cases) / sizeof(cases[0]));
}
