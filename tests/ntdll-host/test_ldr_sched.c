/*
 * test_ldr_sched.c - host tests for the pure loader helpers in
 * src/ntdll/ldr_sched.h: DLL name normalisation, the search directory order,
 * the API-set prefix test, reference counts, the forwarder depth limit, the
 * attach/detach order with cycles, and the attach result rule (decision 0031).
 *
 * These run on the host with no NT types and no I/O. ldr.c calls the same
 * helpers on the MinGW target.
 */
#include "minitest.h"
#include "ldr_sched.h"
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Init order (DFS post-order, cycles broken at the back edge)              */
/* ------------------------------------------------------------------------ */

static void attach_order_chain(void)
{
    /* 0 imports 1, 1 imports 2. Dependencies attach first. */
    unsigned long long deps[3] = { 1ULL << 1, 1ULL << 2, 0 };
    size_t order[3] = { 0 }, count = 0;

    MT_ASSERT(ldr_sched_attach_order(3, deps, 0, order, 3, &count) == 0);
    MT_EXPECT_EQ(count, 3);
    MT_EXPECT(order[0] == 2 && order[1] == 1 && order[2] == 0);
}

static void attach_order_diamond_shares_dependency(void)
{
    /* 0 -> {1, 2}, 1 -> 3, 2 -> 3. Module 3 attaches once, before 1 and 2. */
    unsigned long long deps[4] = { (1ULL << 1) | (1ULL << 2), 1ULL << 3, 1ULL << 3, 0 };
    size_t order[4] = { 0 }, count = 0;

    MT_ASSERT(ldr_sched_attach_order(4, deps, 0, order, 4, &count) == 0);
    MT_EXPECT_EQ(count, 4);
    MT_EXPECT(order[0] == 3 && order[1] == 1 && order[2] == 2 && order[3] == 0);
}

static void attach_order_cycle_terminates(void)
{
    /* 0 -> 1, 1 -> 0 (cycle), 1 -> 2. The back edge 1 -> 0 is skipped, the
     * same way ldr_process_attach skips a module already on the init stack. */
    unsigned long long deps[3] = { 1ULL << 1, (1ULL << 0) | (1ULL << 2), 0 };
    size_t order[3] = { 0 }, count = 0;

    MT_ASSERT(ldr_sched_attach_order(3, deps, 0, order, 3, &count) == 0);
    MT_EXPECT_EQ(count, 3);
    MT_EXPECT(order[0] == 2 && order[1] == 1 && order[2] == 0);
}

static void attach_order_self_import(void)
{
    unsigned long long deps[1] = { 1ULL << 0 };
    size_t order[1] = { 9 }, count = 0;

    MT_ASSERT(ldr_sched_attach_order(1, deps, 0, order, 1, &count) == 0);
    MT_EXPECT_EQ(count, 1);
    MT_EXPECT_EQ(order[0], 0);
}

static void attach_order_rejects_bad_input(void)
{
    unsigned long long deps[2] = { 0, 0 };
    size_t order[2] = { 0 }, count = 0;

    MT_EXPECT(ldr_sched_attach_order(2, deps, 2, order, 2, &count) == -1); /* root out of range */
    MT_EXPECT(ldr_sched_attach_order(LDR_SCHED_MAX_MODULES + 1, deps, 0, order, 2, &count) == -1);
    /* cap too small: root 0 imports 1, so the order needs two slots. */
    unsigned long long chain[2] = { 1ULL << 1, 0 };
    MT_EXPECT(ldr_sched_attach_order(2, chain, 0, order, 1, &count) == -1);
}

static void detach_order_is_reverse_of_attach(void)
{
    size_t order[3] = { 2, 1, 0 };
    ldr_sched_reverse(order, 3);
    MT_EXPECT(order[0] == 0 && order[1] == 1 && order[2] == 2);
}

/* ------------------------------------------------------------------------ */
/* Reference counts (LoadCount, LdrLoadDll / LdrUnloadDll)                   */
/* ------------------------------------------------------------------------ */

static void refcount_inc_saturates(void)
{
    unsigned short c = 0;
    MT_EXPECT(ldr_ref_inc(&c) == 1);
    MT_EXPECT_EQ(c, 1);

    c = 0xFFFF;
    MT_EXPECT(ldr_ref_inc(&c) == 0);
    MT_EXPECT_EQ(c, 0xFFFF);
}

static void refcount_dec_reports_last_release(void)
{
    unsigned short c = 2;
    MT_EXPECT(ldr_ref_dec(&c) == LDR_REF_HELD);
    MT_EXPECT_EQ(c, 1);
    MT_EXPECT(ldr_ref_dec(&c) == LDR_REF_FREE);
    MT_EXPECT_EQ(c, 0);
}

static void refcount_dec_underflow_changes_nothing(void)
{
    unsigned short c = 0;
    MT_EXPECT(ldr_ref_dec(&c) == LDR_REF_UNDERFLOW);
    MT_EXPECT_EQ(c, 0);
}

/* ------------------------------------------------------------------------ */
/* Forwarder depth (LdrGetProcedureAddress)                                  */
/* ------------------------------------------------------------------------ */

static void forward_depth_limit(void)
{
    MT_EXPECT(ldr_forward_depth_ok(0) == 1);
    MT_EXPECT(ldr_forward_depth_ok(LDR_MAX_FORWARD_DEPTH - 1) == 1);
    MT_EXPECT(ldr_forward_depth_ok(LDR_MAX_FORWARD_DEPTH) == 0);
    MT_EXPECT(ldr_forward_depth_ok(0xFFFFFFFFu) == 0);
}

/* ------------------------------------------------------------------------ */
/* DLL names (case-insensitive, .dll appended, base name only)               */
/* ------------------------------------------------------------------------ */

static void normalize_appends_dll_and_lowercases(void)
{
    char out[64];
    MT_EXPECT(ldr_dll_normalize("KERNEL32", out, sizeof(out)) == 1);
    MT_EXPECT(strcmp(out, "kernel32.dll") == 0);

    MT_EXPECT(ldr_dll_normalize("kernel32.DLL", out, sizeof(out)) == 1);
    MT_EXPECT(strcmp(out, "kernel32.dll") == 0);
}

static void normalize_rejects_paths_empty_and_long(void)
{
    char out[16];
    MT_EXPECT(ldr_dll_normalize("", out, sizeof(out)) == 0);
    MT_EXPECT(ldr_dll_normalize("a/b.dll", out, sizeof(out)) == 0);
    MT_EXPECT(ldr_dll_normalize("c:\\x.dll", out, sizeof(out)) == 0);
    /* "ntdll-long-name" + ".dll" needs 19 bytes with the NUL; cap is 16. */
    MT_EXPECT(ldr_dll_normalize("ntdll-long-name", out, sizeof(out)) == 0);
}

static void name_equal_ignores_ascii_case(void)
{
    MT_EXPECT(ldr_dll_name_equal("NTDLL.dll", "ntdll.DLL") == 1);
    MT_EXPECT(ldr_dll_name_equal("ntdll", "ntdll.dll") == 0);
    MT_EXPECT(ldr_dll_name_equal("ntdll.dll", "ntdll.dl") == 0);
}

/* ------------------------------------------------------------------------ */
/* Search directory order                                                    */
/* ------------------------------------------------------------------------ */

static void search_order_full(void)
{
    /* Order: search path override, System32 (known DLL), application dir,
     * System32 (already listed, skipped), PATH entries, then "." last. */
    const char *path_dirs[] = { "C:/p1" };
    const char *out[8] = { 0 };
    size_t n = ldr_search_dirs("C:/x", 1, "C:/app", "C:/sys",
                               path_dirs, 1, out, 8);
    MT_ASSERT(n == 5);
    MT_EXPECT(strcmp(out[0], "C:/x") == 0);
    MT_EXPECT(strcmp(out[1], "C:/sys") == 0);
    MT_EXPECT(strcmp(out[2], "C:/app") == 0);
    MT_EXPECT(strcmp(out[3], "C:/p1") == 0);
    MT_EXPECT(strcmp(out[4], ".") == 0);
}

static void search_order_not_known_puts_system32_after_app(void)
{
    const char *out[8] = { 0 };
    size_t n = ldr_search_dirs(NULL, 0, "C:/app", "C:/sys", NULL, 0, out, 8);
    MT_ASSERT(n == 3);
    MT_EXPECT(strcmp(out[0], "C:/app") == 0);
    MT_EXPECT(strcmp(out[1], "C:/sys") == 0);
    MT_EXPECT(strcmp(out[2], ".") == 0);
}

static void search_order_respects_capacity(void)
{
    const char *out[2] = { 0 };
    size_t n = ldr_search_dirs(NULL, 0, "C:/app", "C:/sys", NULL, 0, out, 2);
    MT_EXPECT_EQ(n, 2);
    MT_EXPECT(strcmp(out[1], "C:/sys") == 0);
}

/* ------------------------------------------------------------------------ */
/* API set names                                                             */
/* ------------------------------------------------------------------------ */

static void apiset_prefix_is_case_insensitive(void)
{
    MT_EXPECT(ldr_apiset_is_virtual("api-ms-win-core-file-l1-1-0.dll") == 1);
    MT_EXPECT(ldr_apiset_is_virtual("API-MS-WIN-CORE-X") == 1);
    MT_EXPECT(ldr_apiset_is_virtual("ext-ms-win-foo-l1-1-0.dll") == 1);
    MT_EXPECT(ldr_apiset_is_virtual("kernel32.dll") == 0);
    MT_EXPECT(ldr_apiset_is_virtual("api-ms") == 0);
}

/* ------------------------------------------------------------------------ */
/* DllMain result (decision 0031: FALSE on attach fails the load)            */
/* ------------------------------------------------------------------------ */

static void attach_false_fails_load(void)
{
    MT_EXPECT(ldr_attach_result_ok(1 /* DLL_PROCESS_ATTACH */, 0) == 0);
    MT_EXPECT(ldr_attach_result_ok(1 /* DLL_PROCESS_ATTACH */, 1) == 1);
}

static void detach_result_is_ignored(void)
{
    MT_EXPECT(ldr_attach_result_ok(0 /* DLL_PROCESS_DETACH */, 0) == 1);
}

/* ------------------------------------------------------------------------ */
/* API-set schema lines and ordinals (replaces sscanf/strtoul, which the     */
/* freestanding ntdll runtime does not provide)                              */
/* ------------------------------------------------------------------------ */

static void apiset_line_parses_pair_and_strips_comment(void)
{
    char virt[64], host[64];
    MT_EXPECT(ldr_apiset_parse_line("api-ms-win-core-x-l1-1-0 kernelbase.dll # note\n",
                                    virt, sizeof(virt), host, sizeof(host)) == 1);
    MT_EXPECT(strcmp(virt, "api-ms-win-core-x-l1-1-0") == 0);
    MT_EXPECT(strcmp(host, "kernelbase.dll") == 0);
}

static void apiset_line_accepts_crlf_and_tabs(void)
{
    char virt[64], host[64];
    MT_EXPECT(ldr_apiset_parse_line("api-ms-win-a\tntdll.dll\r\n",
                                    virt, sizeof(virt), host, sizeof(host)) == 1);
    MT_EXPECT(strcmp(virt, "api-ms-win-a") == 0);
    MT_EXPECT(strcmp(host, "ntdll.dll") == 0);
}

static void apiset_line_rejects_comment_blank_and_single_token(void)
{
    char virt[64], host[64];
    MT_EXPECT(ldr_apiset_parse_line("# only a comment", virt, sizeof(virt), host, sizeof(host)) == 0);
    MT_EXPECT(ldr_apiset_parse_line("   \n", virt, sizeof(virt), host, sizeof(host)) == 0);
    MT_EXPECT(ldr_apiset_parse_line("onlyone\n", virt, sizeof(virt), host, sizeof(host)) == 0);
}

static void apiset_line_rejects_overlong_token(void)
{
    char virt[8], host[64];
    MT_EXPECT(ldr_apiset_parse_line("api-ms-win-core-x kernelbase.dll",
                                    virt, sizeof(virt), host, sizeof(host)) == 0);
}

static void parse_decimal_stops_at_first_non_digit(void)
{
    MT_EXPECT_EQ(ldr_parse_decimal("15"), 15);
    MT_EXPECT_EQ(ldr_parse_decimal("7x"), 7);
    MT_EXPECT_EQ(ldr_parse_decimal("0010"), 10);
    MT_EXPECT_EQ(ldr_parse_decimal(""), 0);
}

static const mt_case_t cases[] = {
    { "attach_order_chain",                       attach_order_chain },
    { "attach_order_diamond_shares_dependency",   attach_order_diamond_shares_dependency },
    { "attach_order_cycle_terminates",            attach_order_cycle_terminates },
    { "attach_order_self_import",                 attach_order_self_import },
    { "attach_order_rejects_bad_input",           attach_order_rejects_bad_input },
    { "detach_order_is_reverse_of_attach",        detach_order_is_reverse_of_attach },
    { "refcount_inc_saturates",                   refcount_inc_saturates },
    { "refcount_dec_reports_last_release",        refcount_dec_reports_last_release },
    { "refcount_dec_underflow_changes_nothing",   refcount_dec_underflow_changes_nothing },
    { "forward_depth_limit",                      forward_depth_limit },
    { "normalize_appends_dll_and_lowercases",     normalize_appends_dll_and_lowercases },
    { "normalize_rejects_paths_empty_and_long",   normalize_rejects_paths_empty_and_long },
    { "name_equal_ignores_ascii_case",            name_equal_ignores_ascii_case },
    { "search_order_full",                        search_order_full },
    { "search_order_not_known_puts_system32_after_app", search_order_not_known_puts_system32_after_app },
    { "search_order_respects_capacity",           search_order_respects_capacity },
    { "apiset_prefix_is_case_insensitive",        apiset_prefix_is_case_insensitive },
    { "attach_false_fails_load",                  attach_false_fails_load },
    { "detach_result_is_ignored",                 detach_result_is_ignored },
    { "apiset_line_parses_pair_and_strips_comment", apiset_line_parses_pair_and_strips_comment },
    { "apiset_line_accepts_crlf_and_tabs",        apiset_line_accepts_crlf_and_tabs },
    { "apiset_line_rejects_comment_blank_and_single_token", apiset_line_rejects_comment_blank_and_single_token },
    { "apiset_line_rejects_overlong_token",       apiset_line_rejects_overlong_token },
    { "parse_decimal_stops_at_first_non_digit",   parse_decimal_stops_at_first_non_digit },
};

int main(void)
{
    return MT_RUN("ldr_sched", cases);
}
