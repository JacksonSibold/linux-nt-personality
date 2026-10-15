/* exc_deliver_test.c - the pure part of user-mode exception delivery
 * (src/kernel/nt/exc_frame_pure.h): trap and access-type mapping, the dispatch
 * frame layout on the user stack, and the EFLAGS merge used by NtContinue.
 *
 * The kernel branch of proc_thread.c (the copy to and from the trap frame) is not
 * compiled on the host. Its layout inputs are checked here, and the kernel build
 * plus a NONVM boot of the probes check the rest.
 */
#include "exc_frame_pure.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void test_trap_mapping(void)
{
    CHECK(exc_pure_status_for_trap(EXC_PURE_TRAP_DE) == 0xC0000094u);
    CHECK(exc_pure_status_for_trap(EXC_PURE_TRAP_OF) == 0xC0000095u);
    CHECK(exc_pure_status_for_trap(EXC_PURE_TRAP_BP) == 0x80000003u);
    CHECK(exc_pure_status_for_trap(EXC_PURE_TRAP_UD) == 0xC000001Du);
    /* Traps Windows does not turn into an exception record here stay with the
     * Linux signal: bounds (5), alignment check (17), GP (13), PF (14). */
    CHECK(exc_pure_status_for_trap(5) == 0);
    CHECK(exc_pure_status_for_trap(13) == 0);
    CHECK(exc_pure_status_for_trap(14) == 0);
    CHECK(exc_pure_status_for_trap(17) == 0);
    g_pass++;
}

static void test_access_type(void)
{
    CHECK(exc_pure_access_type(0) == 0);                       /* read */
    CHECK(exc_pure_access_type(EXC_PURE_PF_WRITE) == 1);       /* write */
    CHECK(exc_pure_access_type(EXC_PURE_PF_INSTR) == 8);       /* execute */
    CHECK(exc_pure_access_type(EXC_PURE_PF_WRITE | EXC_PURE_PF_INSTR) == 8);
    CHECK(exc_pure_access_type(0x4) == 0);                     /* user bit alone */
    g_pass++;
}

static void test_windows_sizes(void)
{
    /* The kernel copies these structures to user memory, so their layout must be
     * the Windows x64 one. */
    CHECK(sizeof(CONTEXT) == 0x4D0);
    CHECK(sizeof(EXCEPTION_RECORD) == 0x98);
    g_pass++;
}

/* Frame layout: for many stack pointers the record and context are 16-byte
 * aligned, lie below the red zone, do not overlap each other or the return
 * slot, and new_sp is 8 mod 16 (the ABI entry alignment). */
static void test_frame_layout(void)
{
    const uint64_t rec_size = sizeof(EXCEPTION_RECORD);
    const uint64_t ctx_size = sizeof(CONTEXT);
    uint64_t sp;
    unsigned long n = 0;
    srand(12345);
    for (n = 0; n < 200000; n++) {
        struct exc_pure_frame f;
        /* Mix small offsets from a page boundary with random 48-bit values. */
        if (n & 1)
            sp = 0x7ffff7ffe000ull - (uint64_t)(rand() % 0x4000) * 8u;
        else
            sp = (((uint64_t)rand() << 32) ^ (uint64_t)rand()) & 0x00007ffffffffff0ull;
        if (sp < 0x100000ull) continue;
        if (exc_pure_frame_layout(sp, rec_size, ctx_size, &f) != 0) {
            /* Only tiny stacks may be refused, and this sp is large. */
            CHECK(sp < 0x1000);
            continue;
        }
        CHECK((f.ctx_va & 0xF) == 0);
        CHECK((f.rec_va & 0xF) == 0);
        CHECK((f.new_sp & 0xF) == 8);
        CHECK(f.new_sp + 8 + EXC_PURE_SHADOW <= f.rec_va);       /* return slot and shadow below rec */
        CHECK(f.rec_va + rec_size <= f.ctx_va);                  /* record ends before context */
        CHECK(f.ctx_va + ctx_size <= sp - EXC_PURE_RED_ZONE);    /* context below the red zone */
        if (g_fail > 20) break;
    }
    g_pass++;
}

static void test_frame_too_small(void)
{
    struct exc_pure_frame f;
    CHECK(exc_pure_frame_layout(0x100, sizeof(EXCEPTION_RECORD), sizeof(CONTEXT), &f) == -1);
    CHECK(exc_pure_frame_layout(0, sizeof(EXCEPTION_RECORD), sizeof(CONTEXT), &f) == -1);
    g_pass++;
}

static void test_eflags(void)
{
    /* IF is forced on, reserved bit 1 is set, IOPL (bits 12-13) and NT/RF/VM/AC/ID
     * keep the thread's value, and the user-changeable bits come from the context. */
    uint64_t cur = 0x3202ull;                 /* IOPL=3 (0x3000), IF, reserved */
    uint64_t merged = exc_pure_eflags_merge(0x0000ull, cur);
    CHECK((merged & 0x200) != 0);             /* IF forced on */
    CHECK((merged & 0x2) != 0);               /* reserved bit set */
    CHECK((merged & 0x3000) == 0x3000);       /* IOPL kept from the thread */
    merged = exc_pure_eflags_merge(0x0041ull, 0x4000ull); /* ZF in the context, NT in the thread */
    CHECK((merged & 0x40) != 0);
    CHECK((merged & 0x4000) != 0);            /* NT kept */
    CHECK((merged & 0x200) != 0);
    g_pass++;
}

int main(void)
{
    test_trap_mapping();
    test_access_type();
    test_windows_sizes();
    test_frame_layout();
    test_frame_too_small();
    test_eflags();
    printf("exc_deliver_test: %d groups passed, %d checks failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
