/*
 * test_k32_io_pure.c - host tests for src/ntdll/k32_io_pure.h: standard
 * handle indexes and bad-handle errors, ReadFile/WriteFile status
 * classification (EOF, pending), byte counts, overlapped offsets, the
 * console-device test and console mode validation, and the counted
 * narrow/widen used by WriteConsoleW and ReadConsoleW.
 */
#include "minitest.h"
#include "k32_io_pure.h"

#include <stdint.h>

/* GetStdHandle maps the three Win32 values to the PEB slots 0, 1, 2. Any other
 * value is not a standard handle. */
static void std_index_maps_win32_values(void)
{
    MT_EXPECT_EQ(k32_std_index(K32_STD_INPUT_HANDLE), 0);
    MT_EXPECT_EQ(k32_std_index(K32_STD_OUTPUT_HANDLE), 1);
    MT_EXPECT_EQ(k32_std_index(K32_STD_ERROR_HANDLE), 2);
    MT_EXPECT_EQ(k32_std_index(0), -1);
    MT_EXPECT_EQ(k32_std_index(5), -1);
    MT_EXPECT_EQ(k32_std_index((DWORD)-13), -1);
}

/* An unknown nStdHandle fails with ERROR_INVALID_HANDLE (6); a valid one sets
 * no error. */
static void std_handle_error_for_unknown_values(void)
{
    MT_EXPECT_EQ(k32_std_handle_error(K32_STD_OUTPUT_HANDLE), 0);
    MT_EXPECT_EQ(k32_std_handle_error((DWORD)-13), 6);
    MT_EXPECT_EQ(k32_std_handle_error(0), 6);
}

/* Status classification: success and a read at end of file are done; a
 * write at end of file is a failure; STATUS_PENDING asks for a wait. */
static void io_classify_success_pending_and_eof(void)
{
    MT_EXPECT(k32_io_classify(STATUS_SUCCESS, 0) == K32_IO_DONE);
    MT_EXPECT(k32_io_classify(STATUS_PENDING, 1) == K32_IO_WAIT);
    MT_EXPECT(k32_io_classify(STATUS_END_OF_FILE, 1) == K32_IO_DONE);
    MT_EXPECT(k32_io_classify(STATUS_END_OF_FILE, 0) == K32_IO_FAIL);
}

static void io_classify_errors_fail(void)
{
    MT_EXPECT(k32_io_classify((NTSTATUS)0xC0000008L, 1) == K32_IO_FAIL); /* STATUS_INVALID_HANDLE */
    MT_EXPECT(k32_io_classify((NTSTATUS)0xC0000005L, 0) == K32_IO_FAIL); /* STATUS_ACCESS_VIOLATION */
}

/* Win32 ReadFile on a synchronous handle returns TRUE at end of file, but an
 * overlapped read past end of file fails with ERROR_HANDLE_EOF (38). */
static void io_eof_error_only_for_overlapped_reads(void)
{
    MT_EXPECT_EQ(k32_io_eof_error(STATUS_END_OF_FILE, 1, 1), 38);
    MT_EXPECT_EQ(k32_io_eof_error(STATUS_END_OF_FILE, 1, 0), 0);
    MT_EXPECT_EQ(k32_io_eof_error(STATUS_END_OF_FILE, 0, 1), 0);
    MT_EXPECT_EQ(k32_io_eof_error(STATUS_SUCCESS, 1, 1), 0);
}

/* A read at end of file transfers no bytes, whatever the information field
 * holds. A successful transfer reports its count. */
static void io_count_is_zero_at_eof(void)
{
    MT_EXPECT_EQ(k32_io_count(STATUS_END_OF_FILE, 0), 0);
    MT_EXPECT_EQ(k32_io_count(STATUS_SUCCESS, 512), 512);
    MT_EXPECT_EQ(k32_io_count(STATUS_SUCCESS, 0), 0);
}

/* OVERLAPPED Offset is the low DWORD and OffsetHigh the high DWORD. */
static void offset_from_overlapped_parts(void)
{
    MT_EXPECT(k32_offset_from_parts(0, 0) == 0);
    MT_EXPECT(k32_offset_from_parts(0x10, 1) == 0x100000010LL);
    MT_EXPECT(k32_offset_from_parts(0xFFFFFFFFu, 0xFFFFFFFFu) == -1);
}

/* GetConsoleMode only succeeds for the console device (FILE_DEVICE_CONSOLE
 * 0x50). Disks, pipes and other devices are not consoles. */
static void console_device_detection(void)
{
    MT_EXPECT(k32_device_is_console(0x00000050u));
    MT_EXPECT(!k32_device_is_console(0x00000011u)); /* FILE_DEVICE_NAMED_PIPE */
    MT_EXPECT(!k32_device_is_console(0x00000007u)); /* FILE_DEVICE_DISK */
}

/* GetFileType: console is CHAR (2), a named pipe is PIPE (3), disk is DISK (1),
 * a device type not in the table is UNKNOWN (0). */
static void file_type_from_device(void)
{
    MT_EXPECT_EQ(k32_file_type_from_device(0x00000050u), 2);
    MT_EXPECT_EQ(k32_file_type_from_device(0x00000011u), 3);
    MT_EXPECT_EQ(k32_file_type_from_device(0x00000007u), 1);
    MT_EXPECT_EQ(k32_file_type_from_device(0x00000014u), 1); /* FILE_DEVICE_NETWORK_FILE_SYSTEM */
    MT_EXPECT_EQ(k32_file_type_from_device(0x00000099u), 0);
}

/* Console modes: unknown bits are ERROR_INVALID_PARAMETER. The input and
 * output flag sets differ (0x20 is an input flag only). */
static void console_mode_validation(void)
{
    MT_EXPECT(k32_console_mode_valid(K32_CONSOLE_IN_DEFAULT, 0));
    MT_EXPECT(k32_console_mode_valid(K32_CONSOLE_OUT_DEFAULT, 1));
    MT_EXPECT(k32_console_mode_valid(0x20u, 0));  /* ENABLE_INSERT_MODE, input */
    MT_EXPECT(!k32_console_mode_valid(0x20u, 1)); /* not an output flag */
    MT_EXPECT(!k32_console_mode_valid(0x400u, 0));
    MT_EXPECT(k32_console_mode_valid(0x4u, 1));   /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
}

static void console_default_modes_are_win32_values(void)
{
    MT_EXPECT_EQ(K32_CONSOLE_IN_DEFAULT, 0x7u);
    MT_EXPECT_EQ(K32_CONSOLE_OUT_DEFAULT, 0x3u);
}

/* WriteConsoleW narrows counted UTF-16 (no NUL) with the same '?' policy as
 * the ANSI conversion in k32_module_pure.h. */
static void narrow_counted_wide_text(void)
{
    static const WCHAR in[] = { 'a', 'b', 0x00E9, 0x20AC, '!' };
    char out[5];
    k32_narrow_counted(in, 5, out);
    MT_EXPECT(out[0] == 'a');
    MT_EXPECT(out[1] == 'b');
    MT_EXPECT((unsigned char)out[2] == 0xE9);
    MT_EXPECT(out[3] == '?');
    MT_EXPECT(out[4] == '!');
}

/* ReadConsoleW widens each byte to one WCHAR (Latin-1, as in the module
 * helpers). */
static void widen_counted_bytes(void)
{
    static const char in[] = { 'x', (char)0xE9 };
    WCHAR out[2];
    k32_widen_counted(in, 2, out);
    MT_EXPECT_EQ(out[0], 'x');
    MT_EXPECT_EQ(out[1], 0x00E9);
}

/* Console I/O is done in chunks so the shim needs only a fixed stack buffer. */
static void console_chunk_is_bounded(void)
{
    MT_EXPECT_EQ(k32_console_chunk(0), 0);
    MT_EXPECT_EQ(k32_console_chunk(10), 10);
    MT_EXPECT_EQ(k32_console_chunk(K32_CONSOLE_CHUNK), K32_CONSOLE_CHUNK);
    MT_EXPECT_EQ(k32_console_chunk(100000), K32_CONSOLE_CHUNK);
}

static const mt_case_t cases[] = {
    { "std_index_maps_win32_values", std_index_maps_win32_values },
    { "std_handle_error_for_unknown_values", std_handle_error_for_unknown_values },
    { "io_classify_success_pending_and_eof", io_classify_success_pending_and_eof },
    { "io_classify_errors_fail", io_classify_errors_fail },
    { "io_eof_error_only_for_overlapped_reads", io_eof_error_only_for_overlapped_reads },
    { "io_count_is_zero_at_eof", io_count_is_zero_at_eof },
    { "offset_from_overlapped_parts", offset_from_overlapped_parts },
    { "console_device_detection", console_device_detection },
    { "file_type_from_device", file_type_from_device },
    { "console_mode_validation", console_mode_validation },
    { "console_default_modes_are_win32_values", console_default_modes_are_win32_values },
    { "narrow_counted_wide_text", narrow_counted_wide_text },
    { "widen_counted_bytes", widen_counted_bytes },
    { "console_chunk_is_bounded", console_chunk_is_bounded },
};

int main(void)
{
    return MT_RUN("k32_io_pure", cases);
}
