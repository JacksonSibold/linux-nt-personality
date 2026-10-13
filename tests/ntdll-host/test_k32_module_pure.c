/*
 * test_k32_module_pure.c - host tests for the pure module-API helpers in
 * src/ntdll/k32_module_pure.h: ANSI and wide conversion, output copy and
 * GetModuleFileName truncation, GetModuleHandleEx flag validation, GetProcAddress
 * name and ordinal arguments, module-name normalisation (adds .dll), LoadLibraryEx
 * flag mapping, the NTSTATUS to Win32 error table for module calls, the
 * address-in-module test, and the DisableThreadLibraryCalls flag.
 *
 * ntstatus_to_dos.c is included so the cross-check against RtlNtStatusToDosError
 * runs on the same table the kernel32 shims use.
 */
#include "minitest.h"
#include "ntstatus_to_dos.c"
#include "k32_module_pure.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* ANSI <-> wide conversion                                                 */
/* ------------------------------------------------------------------------ */

static void ansi_to_wide_ascii(void)
{
    WCHAR out[16];
    size_t n = k32_wide_from_ansi("kernel32", out, 16);
    MT_ASSERT(n == 8);
    MT_EXPECT(out[0] == 'k' && out[7] == '2' && out[8] == 0);
}

static void ansi_to_wide_high_byte_is_latin1(void)
{
    WCHAR out[4];
    size_t n = k32_wide_from_ansi("\xE9", out, 4);
    MT_ASSERT(n == 1);
    MT_EXPECT(out[0] == 0x00E9);
}

static void ansi_to_wide_capacity_includes_nul(void)
{
    WCHAR out[16];
    /* 8 characters need 9 slots including the terminator. */
    MT_EXPECT(k32_wide_from_ansi("kernel32", out, 8) == (size_t)-1);
    MT_EXPECT(k32_wide_from_ansi("kernel32", out, 9) == 8);
    MT_EXPECT(k32_wide_from_ansi(NULL, out, 16) == (size_t)-1);
}

static void wide_to_ansi_ascii(void)
{
    static const WCHAR in[] = { 'a', 'b', 'c', 0 };
    char out[8];
    size_t n = k32_ansi_from_wide(in, out, 8);
    MT_ASSERT(n == 3);
    MT_EXPECT(strcmp(out, "abc") == 0);
}

static void wide_to_ansi_unmappable_becomes_question_mark(void)
{
    static const WCHAR in[] = { 0x4E2D, 0 };   /* not in the single-byte set */
    char out[4];
    size_t n = k32_ansi_from_wide(in, out, 4);
    MT_ASSERT(n == 1);
    MT_EXPECT(out[0] == '?');
}

static void wide_to_ansi_capacity_includes_nul(void)
{
    static const WCHAR in[] = { 'a', 'b', 0 };
    char out[4];
    MT_EXPECT(k32_ansi_from_wide(in, out, 2) == (size_t)-1);
    MT_EXPECT(k32_ansi_from_wide(in, out, 3) == 2);
    MT_EXPECT(k32_ansi_from_wide(NULL, out, 4) == (size_t)-1);
}

/* ------------------------------------------------------------------------ */
/* GetModuleFileName result and truncation                                  */
/* ------------------------------------------------------------------------ */

static void filename_fits_returns_length(void)
{
    DWORD err = 0;
    MT_EXPECT_EQ(k32_filename_result(9, 10, &err), 9);
    MT_EXPECT_EQ(err, 0);
}

static void filename_truncated_returns_size_and_insufficient_buffer(void)
{
    DWORD err = 0;
    /* The NUL needs one slot, so 10 characters into a 10-slot buffer truncate. */
    MT_EXPECT_EQ(k32_filename_result(10, 10, &err), 10);
    MT_EXPECT_EQ(err, 122);
    err = 0;
    MT_EXPECT_EQ(k32_filename_result(50, 10, &err), 10);
    MT_EXPECT_EQ(err, 122);
}

static void filename_copy_length_leaves_room_for_nul(void)
{
    MT_EXPECT_EQ(k32_filename_copy_len(9, 10), 9);
    MT_EXPECT_EQ(k32_filename_copy_len(10, 10), 9);
    MT_EXPECT_EQ(k32_filename_copy_len(50, 10), 9);
    MT_EXPECT_EQ(k32_filename_copy_len(5, 0), 0);
}

static void filename_zero_buffer_fails(void)
{
    DWORD err = 0;
    MT_EXPECT_EQ(k32_filename_result(5, 0, &err), 0);
    MT_EXPECT_EQ(err, 122);
}

/* ------------------------------------------------------------------------ */
/* GetModuleHandleEx flags and arguments                                    */
/* ------------------------------------------------------------------------ */

static void module_flags_known_values_valid(void)
{
    MT_EXPECT(k32_module_flags_valid(0));
    MT_EXPECT(k32_module_flags_valid(0x1));          /* PIN */
    MT_EXPECT(k32_module_flags_valid(0x2));          /* UNCHANGED_REFCOUNT */
    MT_EXPECT(k32_module_flags_valid(0x4));          /* FROM_ADDRESS */
    MT_EXPECT(k32_module_flags_valid(0x5));          /* PIN | FROM_ADDRESS */
}

static void module_flags_reject_unknown_and_pin_with_unchanged(void)
{
    MT_EXPECT(!k32_module_flags_valid(0x8));
    MT_EXPECT(!k32_module_flags_valid(0x80000000u));
    MT_EXPECT(!k32_module_flags_valid(0x3));         /* PIN | UNCHANGED_REFCOUNT */
}

static void getmodulehandleex_argument_errors(void)
{
    void *out = NULL;
    static int marker;
    MT_EXPECT_EQ(k32_getmodulehandleex_check(0x8, NULL, &out), 87);
    MT_EXPECT_EQ(k32_getmodulehandleex_check(0, NULL, NULL), 87);
    /* Name NULL without FROM_ADDRESS means the main module: valid. */
    MT_EXPECT_EQ(k32_getmodulehandleex_check(0, NULL, &out), 0);
    /* FROM_ADDRESS with a NULL address is not inside any module. */
    MT_EXPECT_EQ(k32_getmodulehandleex_check(0x4, NULL, &out), 126);
    MT_EXPECT_EQ(k32_getmodulehandleex_check(0x4, &marker, &out), 0);
}

/* ------------------------------------------------------------------------ */
/* GetProcAddress name and ordinal                                          */
/* ------------------------------------------------------------------------ */

static void getprocaddress_name_pointer(void)
{
    WORD ord = 99;
    int by_ord = 99;
    MT_EXPECT_EQ(k32_getprocaddress_check("CreateFileW", &ord, &by_ord), 0);
    MT_EXPECT_EQ(by_ord, 0);
}

static void getprocaddress_ordinal_value(void)
{
    WORD ord = 0;
    int by_ord = 0;
    const void *p = (const void *)(uintptr_t)3;
    MT_EXPECT_EQ(k32_getprocaddress_check(p, &ord, &by_ord), 0);
    MT_EXPECT_EQ(by_ord, 1);
    MT_EXPECT_EQ(ord, 3);
}

static void getprocaddress_ordinal_zero_and_null(void)
{
    WORD ord = 0;
    int by_ord = 0;
    /* Ordinal 0 is the NULL pointer, so it is rejected as an argument. */
    MT_EXPECT_EQ(k32_getprocaddress_check(NULL, &ord, &by_ord), 87);
    MT_EXPECT_EQ(k32_getprocaddress_check((const void *)(uintptr_t)0, &ord, &by_ord), 87);
}

static void getprocaddress_largest_ordinal_and_first_pointer(void)
{
    WORD ord = 0;
    int by_ord = 0;
    const void *max_ord = (const void *)(uintptr_t)0xFFFF;
    const void *first_ptr = (const void *)(uintptr_t)0x10000;
    MT_EXPECT_EQ(k32_getprocaddress_check(max_ord, &ord, &by_ord), 0);
    MT_EXPECT_EQ(ord, 0xFFFF);
    MT_EXPECT_EQ(by_ord, 1);
    MT_EXPECT_EQ(k32_getprocaddress_check(first_ptr, &ord, &by_ord), 0);
    MT_EXPECT_EQ(by_ord, 0);
}

/* ------------------------------------------------------------------------ */
/* Module name normalisation (adds .dll, drops directories)                 */
/* ------------------------------------------------------------------------ */

static size_t norm(const char *ascii, WCHAR *out, size_t cap)
{
    WCHAR in[260];
    size_t n = k32_wide_from_ansi(ascii, in, sizeof(in) / sizeof(in[0]));
    if (n == (size_t)-1) return 0;
    return k32_module_name_normalise(in, n, out, cap);
}

static int wide_equals(const WCHAR *w, const char *a)
{
    while (*a) {
        if (*w != (WCHAR)(unsigned char)*a) return 0;
        ++w; ++a;
    }
    return *w == 0;
}

static void normalise_adds_dll_when_no_extension(void)
{
    WCHAR out[64];
    MT_EXPECT_EQ(norm("kernel32", out, 64), 12);
    MT_EXPECT(wide_equals(out, "kernel32.dll"));
}

static void normalise_keeps_existing_extension(void)
{
    WCHAR out[64];
    MT_EXPECT_EQ(norm("foo.dll", out, 64), 7);
    MT_EXPECT(wide_equals(out, "foo.dll"));
    MT_EXPECT_EQ(norm("foo.exe", out, 64), 7);
    MT_EXPECT(wide_equals(out, "foo.exe"));
}

static void normalise_drops_directories(void)
{
    WCHAR out[64];
    /* A dot in a directory name does not count as an extension. */
    MT_EXPECT_EQ(norm("C:\\a.b\\bar", out, 64), 7);
    MT_EXPECT(wide_equals(out, "bar.dll"));
    MT_EXPECT_EQ(norm("dir/x\\baz.dll", out, 64), 7);
    MT_EXPECT(wide_equals(out, "baz.dll"));
}

static void normalise_rejects_empty_and_small_buffer(void)
{
    WCHAR out[16];
    MT_EXPECT_EQ(norm("", out, 16), 0);
    MT_EXPECT_EQ(norm("C:\\", out, 16), 0);       /* no base name */
    MT_EXPECT_EQ(norm("kernel32", out, 12), 0);   /* 12 chars + NUL needs 13 */
    MT_EXPECT_EQ(norm("kernel32", out, 13), 12);
    MT_EXPECT_EQ(k32_module_name_normalise(NULL, 0, out, 16), 0);
}

static void name_dir_length(void)
{
    static const WCHAR a[] = { 'C', ':', '\\', 'a', '\\', 'b', '.', 'd', 'l', 'l', 0 };
    static const WCHAR b[] = { 'b', '.', 'd', 'l', 'l', 0 };
    static const WCHAR c[] = { '/', 'x', '/', 'y', 0 };
    MT_EXPECT_EQ(k32_name_dir_len(a, 10), 5);     /* "C:\a\" */
    MT_EXPECT_EQ(k32_name_dir_len(b, 5), 0);
    MT_EXPECT_EQ(k32_name_dir_len(c, 4), 3);      /* "/x/" */
}

/* ------------------------------------------------------------------------ */
/* LoadLibraryEx flags and name length                                      */
/* ------------------------------------------------------------------------ */

static void loadlibraryex_flag_mapping(void)
{
    ULONG ldr = 0xdead;
    MT_EXPECT_EQ(k32_loadlibraryex_flags(0, &ldr), 0);
    MT_EXPECT_EQ(ldr, 0);
    MT_EXPECT_EQ(k32_loadlibraryex_flags(0x2, &ldr), 0);      /* AS_DATAFILE */
    MT_EXPECT_EQ(ldr, K32_LDR_DLL_LOAD_AS_DATAFILE);
    MT_EXPECT_EQ(k32_loadlibraryex_flags(0x40, &ldr), 0);     /* AS_DATAFILE_EXCLUSIVE */
    MT_EXPECT_EQ(ldr, K32_LDR_DLL_LOAD_AS_DATAFILE);
}

static void loadlibraryex_unsupported_and_unknown_flags(void)
{
    ULONG ldr = 0;
    /* DONT_RESOLVE_DLL_REFERENCES: the loader always runs DllMain, so refuse it. */
    MT_EXPECT_EQ(k32_loadlibraryex_flags(0x1, &ldr), 120);
    MT_EXPECT_EQ(k32_loadlibraryex_flags(0x10000, &ldr), 87);
    MT_EXPECT_EQ(k32_loadlibraryex_flags(0, NULL), 87);
}

static void loadlibrary_name_errors(void)
{
    MT_EXPECT_EQ(k32_loadlibrary_name_error(NULL, 0), 87);
    MT_EXPECT_EQ(k32_loadlibrary_name_error(NULL, 5), 87);
    MT_EXPECT_EQ(k32_loadlibrary_name_error("x", 0), 126);
    MT_EXPECT_EQ(k32_loadlibrary_name_error("x", 1), 0);
    MT_EXPECT_EQ(k32_loadlibrary_name_error("x", K32_MODULE_NAME_MAX), 206);
}

/* ------------------------------------------------------------------------ */
/* NTSTATUS to Win32 error for module calls                                 */
/* ------------------------------------------------------------------------ */

static void module_status_golden_values(void)
{
    MT_EXPECT_EQ(k32_module_status_to_error(STATUS_DLL_NOT_FOUND), 126);
    MT_EXPECT_EQ(k32_module_status_to_error(STATUS_ENTRYPOINT_NOT_FOUND), 127);
    MT_EXPECT_EQ(k32_module_status_to_error(STATUS_PROCEDURE_NOT_FOUND), 127);
    MT_EXPECT_EQ(k32_module_status_to_error(STATUS_INVALID_IMAGE_FORMAT), 193);
    MT_EXPECT_EQ(k32_module_status_to_error(STATUS_DLL_INIT_FAILED), 1114);
    MT_EXPECT_EQ(k32_module_status_to_error(STATUS_INVALID_PARAMETER), 87);
    MT_EXPECT_EQ(k32_module_status_to_error(STATUS_NO_MEMORY), 8);
}

static void module_status_unknown_and_success_fall_through(void)
{
    /* 0 tells the caller to use RtlNtStatusToDosError. */
    MT_EXPECT_EQ(k32_module_status_to_error(STATUS_SUCCESS), 0);
    MT_EXPECT_EQ(k32_module_status_to_error((NTSTATUS)0xC0000001L), 0);
}

static void module_status_agrees_with_ntstatus_table(void)
{
    /* STATUS_DLL_INIT_FAILED is left out: ntstatus_to_dos has no entry for it
     * (falls back to 317). The module list gives 1114 (K32-18). */
    static const NTSTATUS list[] = {
        STATUS_DLL_NOT_FOUND, STATUS_ENTRYPOINT_NOT_FOUND, STATUS_PROCEDURE_NOT_FOUND,
        STATUS_INVALID_IMAGE_FORMAT,
        STATUS_INVALID_PARAMETER, STATUS_NO_MEMORY,
    };
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); ++i) {
        ULONG table = RtlNtStatusToDosError(list[i]);
        MT_EXPECT_EQ(k32_module_status_to_error(list[i]), (long)table);
    }
}

/* ------------------------------------------------------------------------ */
/* Address in module, DisableThreadLibraryCalls                             */
/* ------------------------------------------------------------------------ */

static void address_in_module_bounds(void)
{
    MT_EXPECT(k32_addr_in_module(0x1000, 0x1000, 0x100));
    MT_EXPECT(k32_addr_in_module(0x10ff, 0x1000, 0x100));
    MT_EXPECT(!k32_addr_in_module(0x1100, 0x1000, 0x100));
    MT_EXPECT(!k32_addr_in_module(0x0fff, 0x1000, 0x100));
    MT_EXPECT(!k32_addr_in_module(0x1000, 0, 0x100));
}

static void address_in_module_no_overflow(void)
{
    /* base + size wraps past zero; the address is still inside. */
    MT_EXPECT(k32_addr_in_module(UINTPTR_MAX - 1, UINTPTR_MAX - 0x10, 0x100));
    /* Address below the base never matches, even with a wrapping size. */
    MT_EXPECT(!k32_addr_in_module(0x10, UINTPTR_MAX - 0x10, 0x100));
}

static void disable_thread_calls_flag_and_argument(void)
{
    MT_EXPECT_EQ(k32_disable_thread_calls_flags(0), 0x00040000);
    MT_EXPECT_EQ(k32_disable_thread_calls_flags(0x00040000), 0x00040000);
    static int marker;
    MT_EXPECT_EQ(k32_disable_thread_calls_check(NULL), 87);
    MT_EXPECT_EQ(k32_disable_thread_calls_check(&marker), 0);
}

static const mt_case_t cases[] = {
    { "ansi_to_wide_ascii", ansi_to_wide_ascii },
    { "ansi_to_wide_high_byte_is_latin1", ansi_to_wide_high_byte_is_latin1 },
    { "ansi_to_wide_capacity_includes_nul", ansi_to_wide_capacity_includes_nul },
    { "wide_to_ansi_ascii", wide_to_ansi_ascii },
    { "wide_to_ansi_unmappable_becomes_question_mark", wide_to_ansi_unmappable_becomes_question_mark },
    { "wide_to_ansi_capacity_includes_nul", wide_to_ansi_capacity_includes_nul },
    { "filename_fits_returns_length", filename_fits_returns_length },
    { "filename_truncated_returns_size_and_insufficient_buffer", filename_truncated_returns_size_and_insufficient_buffer },
    { "filename_copy_length_leaves_room_for_nul", filename_copy_length_leaves_room_for_nul },
    { "filename_zero_buffer_fails", filename_zero_buffer_fails },
    { "module_flags_known_values_valid", module_flags_known_values_valid },
    { "module_flags_reject_unknown_and_pin_with_unchanged", module_flags_reject_unknown_and_pin_with_unchanged },
    { "getmodulehandleex_argument_errors", getmodulehandleex_argument_errors },
    { "getprocaddress_name_pointer", getprocaddress_name_pointer },
    { "getprocaddress_ordinal_value", getprocaddress_ordinal_value },
    { "getprocaddress_ordinal_zero_and_null", getprocaddress_ordinal_zero_and_null },
    { "getprocaddress_largest_ordinal_and_first_pointer", getprocaddress_largest_ordinal_and_first_pointer },
    { "normalise_adds_dll_when_no_extension", normalise_adds_dll_when_no_extension },
    { "normalise_keeps_existing_extension", normalise_keeps_existing_extension },
    { "normalise_drops_directories", normalise_drops_directories },
    { "normalise_rejects_empty_and_small_buffer", normalise_rejects_empty_and_small_buffer },
    { "name_dir_length", name_dir_length },
    { "loadlibraryex_flag_mapping", loadlibraryex_flag_mapping },
    { "loadlibraryex_unsupported_and_unknown_flags", loadlibraryex_unsupported_and_unknown_flags },
    { "loadlibrary_name_errors", loadlibrary_name_errors },
    { "module_status_golden_values", module_status_golden_values },
    { "module_status_unknown_and_success_fall_through", module_status_unknown_and_success_fall_through },
    { "module_status_agrees_with_ntstatus_table", module_status_agrees_with_ntstatus_table },
    { "address_in_module_bounds", address_in_module_bounds },
    { "address_in_module_no_overflow", address_in_module_no_overflow },
    { "disable_thread_calls_flag_and_argument", disable_thread_calls_flag_and_argument },
};

int main(void)
{
    return MT_RUN("k32_module_pure", cases);
}
