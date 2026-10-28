/*
 * test_k32_fiber_pure.c - tests for src/ntdll/k32_fiber_pure.h on the host: switching between a main context and fibers
 * on their own stacks, saved registers, MXCSR per fiber, deep calls on a fiber stack and several fibers in turn.
 */
#include "minitest.h"
#include "k32_fiber_pure.h"

#include <stdlib.h>
#include <xmmintrin.h>

#define STACK 65536

typedef struct {
    uint64_t sp;
    uint8_t *mem;
} fib;

static fib main_ctx;
static fib f1, f2;
static int trace[64];
static int ntrace;

static void log_(int v) { if (ntrace < 64) trace[ntrace++] = v; }

static void new_fiber(fib *f, void (*entry)(void *), void *arg)
{
    f->mem = (uint8_t *)aligned_alloc(16, STACK);
    memset(f->mem, 0xCC, STACK);
    f->sp = k32_fiber_prepare(f->mem + STACK, entry, arg);
}

static void free_fiber(fib *f) { free(f->mem); f->mem = NULL; }

/* Back to the main context from a fiber; resumable later. */
static void yield_to_main(fib *self) { k32_fiber_swap(&self->sp, main_ctx.sp); }
static void resume(fib *f) { k32_fiber_swap(&main_ctx.sp, f->sp); }

static void counter_fiber(void *arg)
{
    int base = (int)(intptr_t)arg;
    for (int i = 0; i < 3; ++i) {
        log_(base + i);
        yield_to_main(&f1);
    }
    log_(base + 100);
    for (;;)
        yield_to_main(&f1);
}

static void first_switch_runs_the_entry_and_yields_resume(void)
{
    ntrace = 0;
    new_fiber(&f1, counter_fiber, (void *)(intptr_t)10);
    resume(&f1);
    log_(-1);
    resume(&f1);
    log_(-2);
    resume(&f1);
    resume(&f1);
    MT_EXPECT_EQ(ntrace, 6);
    MT_EXPECT(trace[0] == 10 && trace[1] == -1 && trace[2] == 11 && trace[3] == -2 && trace[4] == 12 && trace[5] == 110);
    free_fiber(&f1);
}

static void register_fiber(void *arg)
{
    (void)arg;
    __asm__ volatile("movq $0xBBBB, %%r12\n\tmovq $0xB1B1, %%rbx" ::: "r12", "rbx");
    yield_to_main(&f1);
    {
        uint64_t r12, rbx;
        __asm__ volatile("movq %%r12, %0\n\tmovq %%rbx, %1" : "=r"(r12), "=r"(rbx));
        log_(r12 == 0xBBBB && rbx == 0xB1B1);
    }
    for (;;)
        yield_to_main(&f1);
}

static void callee_saved_registers_are_per_context(void)
{
    uint64_t r12, rbx;
    ntrace = 0;
    new_fiber(&f1, register_fiber, NULL);
    __asm__ volatile("movq $0xAAAA, %%r12\n\tmovq $0xA1A1, %%rbx" ::: "r12", "rbx");
    resume(&f1);
    __asm__ volatile("movq %%r12, %0\n\tmovq %%rbx, %1" : "=r"(r12), "=r"(rbx));
    MT_EXPECT(r12 == 0xAAAA && rbx == 0xA1A1); /* the fiber's changes did not leak here */
    resume(&f1);
    MT_EXPECT(ntrace == 1 && trace[0] == 1); /* and ours did not leak there */
    free_fiber(&f1);
}

static void mxcsr_fiber(void *arg)
{
    (void)arg;
    _mm_setcsr(_mm_getcsr() | 0x6000u); /* round toward zero */
    yield_to_main(&f1);
    log_((_mm_getcsr() & 0x6000u) == 0x6000u);
    for (;;)
        yield_to_main(&f1);
}

static void floating_point_control_is_saved_per_fiber(void)
{
    unsigned main_csr = _mm_getcsr();
    ntrace = 0;
    new_fiber(&f1, mxcsr_fiber, NULL);
    resume(&f1);
    MT_EXPECT((_mm_getcsr() & 0x6000u) == (main_csr & 0x6000u)); /* the fiber's rounding mode stayed with it */
    resume(&f1);
    MT_EXPECT(ntrace == 1 && trace[0] == 1);
    free_fiber(&f1);
}

static int recurse(int n)
{
    volatile char pad[256];
    pad[0] = (char)n;
    return n == 0 ? 0 : 1 + recurse(n - 1) + (pad[0] - (char)n);
}

static void deep_fiber(void *arg)
{
    (void)arg;
    log_(recurse(100));
    for (;;)
        yield_to_main(&f1);
}

static void deep_calls_use_the_fiber_stack(void)
{
    ntrace = 0;
    new_fiber(&f1, deep_fiber, NULL);
    resume(&f1);
    MT_EXPECT(ntrace == 1 && trace[0] == 100);
    free_fiber(&f1);
}

static void ping_fiber(void *arg)
{
    int id = (int)(intptr_t)arg;
    fib *self = id == 1 ? &f1 : &f2;
    for (int i = 0; i < 4; ++i) {
        log_(id * 10 + i);
        k32_fiber_swap(&self->sp, (id == 1 ? &f2 : &f1)->sp); /* hand over to the other fiber */
    }
    for (;;)
        yield_to_main(self);
}

static void fibers_switch_directly_between_each_other(void)
{
    ntrace = 0;
    new_fiber(&f1, ping_fiber, (void *)(intptr_t)1);
    new_fiber(&f2, ping_fiber, (void *)(intptr_t)2);
    resume(&f1);
    /* 10, 20, 11, 21, ... until one reaches its end and yields to main */
    MT_EXPECT(ntrace >= 8);
    MT_EXPECT(trace[0] == 10 && trace[1] == 20 && trace[2] == 11 && trace[3] == 21 && trace[4] == 12 && trace[5] == 22);
    free_fiber(&f1);
    free_fiber(&f2);
}

static const mt_case_t cases[] = {
    MT_CASE(first_switch_runs_the_entry_and_yields_resume), MT_CASE(callee_saved_registers_are_per_context),
    MT_CASE(floating_point_control_is_saved_per_fiber), MT_CASE(deep_calls_use_the_fiber_stack),
    MT_CASE(fibers_switch_directly_between_each_other),
};

int main(void)
{
    return MT_RUN("k32_fiber_pure", cases);
}
