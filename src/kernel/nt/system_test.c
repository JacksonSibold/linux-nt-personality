/* system_test.c - system and misc NT services (userspace shim, ASan+UBSan).
 * Covers NtSetTimerResolution / NtQueryTimerResolution (timer resolution in
 * 100 ns units; range 0.5 ms .. 15.625 ms, default 15.625 ms) and the
 * NtRaiseHardError refusal. Handlers live in nt_services.c. */
#include "object.h"
#include "handle.h"
#include "sync.h"
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"
#include "kshim.h"
#include "ntstatus_kernel.h"

NTSTATUS nt_svc_NtSetTimerResolution(ULONG DesiredTime, BOOLEAN SetResolution, PULONG ActualTime);
NTSTATUS nt_svc_NtQueryTimerResolution(PULONG MaximumTime, PULONG MinimumTime, PULONG CurrentTime);
NTSTATUS nt_svc_NtRaiseHardError(NTSTATUS ErrorStatus, ULONG NumberOfParameters, ULONG UnicodeStringParameterMask, ULONG_PTR *Parameters, ULONG ValidResponseOptions, PULONG Response);

#include <stdio.h>

static int g_pass=0, g_fail=0;
#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  RUN %s\n", #name); test_##name(); } while(0)
#define ASSERT(cond) do { if (!(cond)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); g_fail++; return; } } while(0)
#define ASSERT_STATUS(expr, expect) do { NTSTATUS _s=(expr); if (_s != (expect)) { fprintf(stderr,"FAIL %s:%d: expect 0x%lx got 0x%lx for %s\n",__FILE__,__LINE__,(unsigned long)(expect),(unsigned long)_s,#expr); g_fail++; return; } } while(0)
#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while(0)

/* Timer resolution constants, 100 ns units. */
#define TR_MAX 156250UL   /* 15.625 ms: coarsest, and the default */
#define TR_MIN 5000UL     /* 0.5 ms: finest */

TEST(timer_resolution_query) {
    ULONG mx=0, mn=0, cur=0;
    ASSERT_STATUS(nt_svc_NtQueryTimerResolution(NULL, &mn, &cur), STATUS_ACCESS_VIOLATION);
    ASSERT_STATUS(nt_svc_NtQueryTimerResolution(&mx, NULL, &cur), STATUS_ACCESS_VIOLATION);
    ASSERT_STATUS(nt_svc_NtQueryTimerResolution(&mx, &mn, NULL), STATUS_ACCESS_VIOLATION);
    ASSERT_STATUS(nt_svc_NtQueryTimerResolution(&mx, &mn, &cur), STATUS_SUCCESS);
    ASSERT(mx == TR_MAX);
    ASSERT(mn == TR_MIN);
    ASSERT(cur == TR_MAX);
    PASS();
}

TEST(timer_resolution_set_and_release) {
    ULONG act=0, mx=0, mn=0, cur=0;
    ASSERT_STATUS(nt_svc_NtSetTimerResolution(10000, 1, NULL), STATUS_ACCESS_VIOLATION);
    ASSERT_STATUS(nt_svc_NtSetTimerResolution(10000, 1, &act), STATUS_SUCCESS);
    ASSERT(act == 10000);
    ASSERT_STATUS(nt_svc_NtQueryTimerResolution(&mx, &mn, &cur), STATUS_SUCCESS);
    ASSERT(cur == 10000);
    ASSERT_STATUS(nt_svc_NtSetTimerResolution(10000, 0, &act), STATUS_SUCCESS);
    ASSERT(act == TR_MAX);
    ASSERT_STATUS(nt_svc_NtQueryTimerResolution(&mx, &mn, &cur), STATUS_SUCCESS);
    ASSERT(cur == TR_MAX);
    PASS();
}

TEST(timer_resolution_clamps) {
    ULONG act=0;
    ASSERT_STATUS(nt_svc_NtSetTimerResolution(1, 1, &act), STATUS_SUCCESS);
    ASSERT(act == TR_MIN);
    ASSERT_STATUS(nt_svc_NtSetTimerResolution(0xFFFFFFFFUL, 1, &act), STATUS_SUCCESS);
    ASSERT(act == TR_MAX);
    ASSERT_STATUS(nt_svc_NtSetTimerResolution(TR_MAX, 0, &act), STATUS_SUCCESS);
    ASSERT(act == TR_MAX);
    PASS();
}

TEST(raise_hard_error_refused) {
    ULONG_PTR params[4] = {0, 0, 0, 0};
    ULONG resp = 0;
    /* More than MAXIMUM_HARDERROR_PARAMETERS (4) is an invalid parameter. */
    ASSERT_STATUS(nt_svc_NtRaiseHardError(0xC0000022L, 5, 0, params, 0, &resp), STATUS_INVALID_PARAMETER);
    /* A valid request is refused: no error box and no response is written. */
    ASSERT_STATUS(nt_svc_NtRaiseHardError(0xC0000022L, 4, 0, params, 0, &resp), STATUS_NOT_SUPPORTED);
    ASSERT(resp == 0);
    PASS();
}

int main(void) {
    printf("System tests starting\n");
    RUN(timer_resolution_query);
    RUN(timer_resolution_set_and_release);
    RUN(timer_resolution_clamps);
    RUN(raise_hard_error_refused);
    printf("System tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
