/*
 * test_k32_thread_pure.c - host tests for src/ntdll/k32_thread_pure.h: the
 * CreateThread flag check and mapping, the exit code for a running object,
 * and the suspend count result.
 */
#include "minitest.h"
#include "k32_thread_pure.h"

/* Only CREATE_SUSPENDED and STACK_SIZE_PARAM_IS_A_RESERVATION are defined;
 * CREATE_SUSPENDED maps to the NT suspended flag. */
static void create_flags_mapping(void)
{
    ULONG nt = 0xFFFF;
    MT_EXPECT(k32t_create_flags(0, &nt) == 1);
    MT_EXPECT_EQ(nt, 0);
    MT_EXPECT(k32t_create_flags(K32T_CREATE_SUSPENDED, &nt) == 1);
    MT_EXPECT_EQ(nt, K32T_NT_CREATE_SUSPENDED);
    MT_EXPECT(k32t_create_flags(K32T_STACK_SIZE_PARAM_IS_A_RESERVATION, &nt) == 1);
    MT_EXPECT_EQ(nt, 0);
    MT_EXPECT(k32t_create_flags(K32T_CREATE_SUSPENDED | K32T_STACK_SIZE_PARAM_IS_A_RESERVATION, &nt) == 1);
    MT_EXPECT_EQ(nt, K32T_NT_CREATE_SUSPENDED);
}

/* An unknown creation bit is an invalid parameter. */
static void create_flags_reject_unknown(void)
{
    ULONG nt = 0;
    MT_EXPECT(k32t_create_flags(0x00000008u, &nt) == 0);
    MT_EXPECT(k32t_create_flags(0x80000000u, &nt) == 0);
    MT_EXPECT(k32t_create_flags(K32T_CREATE_SUSPENDED | 0x1u, &nt) == 0);
}

/* STATUS_PENDING in ExitStatus means still running; any other status is the
 * exit code. */
static void exit_code_pending_and_done(void)
{
    MT_EXPECT_EQ(k32t_exit_code(STATUS_PENDING), K32T_STILL_ACTIVE);
    MT_EXPECT_EQ(k32t_exit_code(0), 0);
    MT_EXPECT_EQ(k32t_exit_code(3), 3);
    MT_EXPECT_EQ(k32t_exit_code((NTSTATUS)0xC0000005L), 0xC0000005u);
}

/* A failed suspend or resume is (DWORD)-1; success is the previous count. */
static void suspend_result_mapping(void)
{
    MT_EXPECT_EQ(k32t_suspend_result(STATUS_SUCCESS, 0), 0);
    MT_EXPECT_EQ(k32t_suspend_result(STATUS_SUCCESS, 2), 2);
    MT_EXPECT_EQ(k32t_suspend_result((NTSTATUS)0xC0000008L, 0), K32T_SUSPEND_FAILED);
}

static const mt_case_t cases[] = {
    { "create_flags_mapping", create_flags_mapping },
    { "create_flags_reject_unknown", create_flags_reject_unknown },
    { "exit_code_pending_and_done", exit_code_pending_and_done },
    { "suspend_result_mapping", suspend_result_mapping },
};

int main(void)
{
    return MT_RUN("k32_thread_pure", cases);
}
