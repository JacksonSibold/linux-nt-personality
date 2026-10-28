/*
 * test_ucrtbase_fp.c - tests for src/ntdll/ucrtbase_fp.inc: the Microsoft control word on MXCSR (this runs against the
 * host's real MXCSR), the C99 fenv functions, and the classification functions against the C library.
 */
#include "minitest.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ucrt_runtime.h"

#include "../../src/ntdll/ucrtbase_fp.inc"

static unsigned saved_mxcsr;

static void setup(void)
{
    g_ucrt_state.invalid_handler = NULL;
    g_ucrt_state.invalid_count = 0;
    saved_mxcsr = uct_read_mxcsr();
}

static void teardown(void) { uct_write_mxcsr(saved_mxcsr); }

static void default_control_word(void)
{
    setup();
    uct_write_mxcsr(0x1F80u); /* the state of a new thread: everything masked, round to nearest */
    /* all six exceptions masked, nearest, denormals kept */
    MT_EXPECT_EQ((long)w__control87(0, 0), (long)(UCT_MCW_EM));
    teardown();
}

static void rounding_mode_changes_arithmetic(void)
{
    volatile double one = 1.0, three = 3.0, r;
    setup();
    uct_write_mxcsr(0x1F80u);
    MT_EXPECT_EQ((long)(w__control87(0x100, UCT_MCW_RC) & UCT_MCW_RC), 0x100); /* _RC_DOWN */
    MT_EXPECT_EQ(w_fegetround(), 0x100);
    r = one / three;
    {
        double down = r;
        w__control87(0x200, UCT_MCW_RC); /* _RC_UP */
        r = one / three;
        MT_EXPECT(r > down);
        w__control87(0x300, UCT_MCW_RC); /* toward zero */
        r = one / three;
        MT_EXPECT(r == down);
    }
    MT_EXPECT_EQ(w_fesetround(0), 0);
    MT_EXPECT_EQ(w_fegetround(), 0);
    MT_EXPECT_EQ(w_fesetround(0x400), 1); /* not a rounding mode */
    teardown();
}

static void exception_masks(void)
{
    unsigned cur = 0;
    setup();
    uct_write_mxcsr(0x1F80u);
    /* unmask divide-by-zero: the mask bit is cleared */
    w__control87(0, UCT_EM_ZERODIVIDE);
    MT_EXPECT((uct_read_mxcsr() & UCT_MX_ZM) == 0);
    MT_EXPECT((w__control87(0, 0) & UCT_EM_ZERODIVIDE) == 0);
    w__control87(UCT_EM_ZERODIVIDE, UCT_EM_ZERODIVIDE);
    MT_EXPECT((uct_read_mxcsr() & UCT_MX_ZM) != 0);
    /* _controlfp_s refuses the x87-only controls */
    MT_EXPECT_EQ(w__controlfp_s(&cur, 0, UCT_MCW_PC), UCRT_EINVAL);
    MT_EXPECT_EQ(w__controlfp_s(&cur, 0, UCT_MCW_IC), UCRT_EINVAL);
    MT_EXPECT_EQ(w__controlfp_s(&cur, UCT_EM_INVALID, UCT_MCW_EM), 0);
    MT_EXPECT((cur & UCT_EM_INVALID) != 0);
    teardown();
}

static void denormal_modes(void)
{
    setup();
    uct_write_mxcsr(0x1F80u);
    w__control87(0x01000000, UCT_MCW_DN);
    MT_EXPECT((uct_read_mxcsr() & (UCT_MX_FZ | UCT_MX_DAZ)) == (UCT_MX_FZ | UCT_MX_DAZ));
    MT_EXPECT_EQ((long)(w__control87(0, 0) & UCT_MCW_DN), 0x01000000);
    w__control87(0x02000000, UCT_MCW_DN);
    MT_EXPECT((uct_read_mxcsr() & (UCT_MX_FZ | UCT_MX_DAZ)) == UCT_MX_DAZ);
    w__control87(0x03000000, UCT_MCW_DN);
    MT_EXPECT((uct_read_mxcsr() & (UCT_MX_FZ | UCT_MX_DAZ)) == UCT_MX_FZ);
    w__control87(0, UCT_MCW_DN);
    MT_EXPECT((uct_read_mxcsr() & (UCT_MX_FZ | UCT_MX_DAZ)) == 0);
    teardown();
}

static void status_flags(void)
{
    volatile double one = 1.0, three = 3.0, big = 1e308, zero = 0.0, r;
    setup();
    uct_write_mxcsr(0x1F80u);
    w__clearfp();
    r = one / three;
    MT_EXPECT((w__statusfp() & UCT_EM_INEXACT) != 0);
    r = big * big;
    MT_EXPECT((w__statusfp() & UCT_EM_OVERFLOW) != 0);
    r = one / zero;
    MT_EXPECT((w__statusfp() & UCT_EM_ZERODIVIDE) != 0);
    MT_EXPECT((w_fetestexcept(0x1F) & 0x08) != 0);
    {
        unsigned old = w__clearfp(); /* returns what was set, and clears it */
        MT_EXPECT((old & (UCT_EM_INEXACT | UCT_EM_OVERFLOW | UCT_EM_ZERODIVIDE)) == (UCT_EM_INEXACT | UCT_EM_OVERFLOW | UCT_EM_ZERODIVIDE));
        MT_EXPECT_EQ((long)w__statusfp(), 0);
    }
    w_feraiseexcept(0x01);
    MT_EXPECT_EQ(w_fetestexcept(0x1F), 0x01);
    w_feclearexcept(0x01);
    MT_EXPECT_EQ(w_fetestexcept(0x1F), 0);
    (void)r;
    teardown();
}

static void environment_save_and_restore(void)
{
    uct_fenv e, held;
    setup();
    uct_write_mxcsr(0x1F80u);
    w__control87(0x200, UCT_MCW_RC);
    w_fegetenv(&e);
    w__control87(0x100, UCT_MCW_RC);
    w_feraiseexcept(0x04);
    MT_EXPECT_EQ(w_feholdexcept(&held), 0); /* saves, clears the flags and masks everything */
    MT_EXPECT_EQ(w_fetestexcept(0x1F), 0);
    MT_EXPECT_EQ(w_fesetenv(&e), 0);
    MT_EXPECT_EQ(w_fegetround(), 0x200);
    teardown();
}

static void classification_against_libm(void)
{
    static const double vals[] = {0.0, -0.0, 1.0, -2.5, 5e-324, -5e-324, 2.2250738585072014e-308, 1e308, INFINITY, -INFINITY};
    setup();
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; ++i) {
        double d = vals[i];
        int cls = w__fpclass(d);
        short dc = w__dclass(d);
        MT_EXPECT_EQ(dc, fpclassify(d) == FP_NAN ? 2 : fpclassify(d) == FP_INFINITE ? 1 : fpclassify(d) == FP_ZERO ? 0 : fpclassify(d) == FP_SUBNORMAL ? -2 : -1);
        MT_EXPECT_EQ(w__dsign(d) != 0, signbit(d) != 0);
        MT_EXPECT(__builtin_popcount((unsigned)cls) == 1);
    }
    MT_EXPECT_EQ(w__fpclass(INFINITY), UCT_FPCLASS_PINF);
    MT_EXPECT_EQ(w__fpclass(-INFINITY), UCT_FPCLASS_NINF);
    MT_EXPECT_EQ(w__fpclass(-0.0), UCT_FPCLASS_NZ);
    MT_EXPECT_EQ(w__fpclass(5e-324), UCT_FPCLASS_PD);
    MT_EXPECT_EQ(w__fpclass(-3.0), UCT_FPCLASS_NN);
    MT_EXPECT_EQ(w__fpclass(NAN), UCT_FPCLASS_QNAN);
    {
        uint64_t snan = 0x7FF0000000000001ull;
        double d;
        memcpy(&d, &snan, 8);
        MT_EXPECT_EQ(w__fpclass(d), UCT_FPCLASS_SNAN);
    }
    MT_EXPECT_EQ(w__fdclass(1.5f), -1);
    MT_EXPECT_EQ(w__fdclass(1e-45f), -2);
    MT_EXPECT_EQ(w__fdclass(INFINITY), 1);
    MT_EXPECT_EQ(w__fdclass(NAN), 2);
    MT_EXPECT(w__fdsign(-1.0f) != 0 && w__fdsign(1.0f) == 0);
}

static const mt_case_t cases[] = {
    MT_CASE(default_control_word), MT_CASE(rounding_mode_changes_arithmetic), MT_CASE(exception_masks), MT_CASE(denormal_modes),
    MT_CASE(status_flags), MT_CASE(environment_save_and_restore), MT_CASE(classification_against_libm),
};

int main(void)
{
    return MT_RUN("ucrtbase_fp", cases);
}
