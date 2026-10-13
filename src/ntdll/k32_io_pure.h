/*
 * k32_io_pure.h - pure helpers for the kernel32 handle, console and I/O shims.
 *
 * GetStdHandle and SetStdHandle, the ReadFile and WriteFile status handling,
 * the console calls (WriteConsole, ReadConsole, Get/SetConsoleMode) and the
 * file-type classification used by GetFileType. kernel32.c reads the PEB and
 * the NT services and passes the values here. Nothing in this header touches
 * the TEB, the PEB or the NT syscalls, so tests/ntdll-host can compile it on
 * the host (test_k32_io_pure.c).
 *
 * Error values are Win32 error codes (the last-error value). The standard
 * handles are the process parameters' StandardInput, StandardOutput and
 * StandardError slots (RTL_USER_PROCESS_PARAMETERS, nt_types.h).
 */
#ifndef NTDLL_K32_IO_PURE_H
#define NTDLL_K32_IO_PURE_H

#include "nt_types.h"
#include "ntstatus.h"
#include <stddef.h>
#include <stdint.h>

#define K32_ERROR_INVALID_HANDLE  6
#define K32_ERROR_HANDLE_EOF      38

/* nStdHandle values for GetStdHandle and SetStdHandle. */
#define K32_STD_INPUT_HANDLE   ((DWORD)-10)
#define K32_STD_OUTPUT_HANDLE  ((DWORD)-11)
#define K32_STD_ERROR_HANDLE   ((DWORD)-12)

/* Index of a standard handle slot: 0 StandardInput, 1 StandardOutput,
 * 2 StandardError. -1 for any other value. */
static inline int k32_std_index(DWORD which)
{
    switch (which) {
    case K32_STD_INPUT_HANDLE:  return 0;
    case K32_STD_OUTPUT_HANDLE: return 1;
    case K32_STD_ERROR_HANDLE:  return 2;
    default:                    return -1;
    }
}

/* Last-error value for a GetStdHandle or SetStdHandle call: 0 for a valid
 * index, ERROR_INVALID_HANDLE otherwise. An unset standard handle (NULL) is
 * not an error. */
static inline DWORD k32_std_handle_error(DWORD which)
{
    return k32_std_index(which) < 0 ? K32_ERROR_INVALID_HANDLE : 0;
}

/* How a ReadFile or WriteFile status is handled.
 *   DONE  the transfer finished; the count is k32_io_count.
 *   WAIT  the request is pending; the caller waits on the handle and then
 *         reads the final status (synchronous handles should not report this,
 *         but a wait keeps the result correct if they do).
 *   FAIL  the call fails; the status goes through RtlNtStatusToDosError.
 * A read that reaches end of file is DONE with zero bytes (Win32 ReadFile on a
 * synchronous handle returns TRUE and a zero count there). */
typedef enum {
    K32_IO_DONE = 0,
    K32_IO_WAIT,
    K32_IO_FAIL
} k32_io_outcome;

static inline k32_io_outcome k32_io_classify(NTSTATUS s, int is_read)
{
    if (s == STATUS_PENDING) return K32_IO_WAIT;
    if (NT_SUCCESS(s)) return K32_IO_DONE;
    if (is_read && s == STATUS_END_OF_FILE) return K32_IO_DONE;
    return K32_IO_FAIL;
}

/* Last-error value for a status that is end of file: an overlapped read past
 * the end fails with ERROR_HANDLE_EOF; a synchronous read returns TRUE with
 * zero bytes (k32_io_classify). 0 means no error override. */
static inline DWORD k32_io_eof_error(NTSTATUS s, int is_read, int overlapped)
{
    return (is_read && overlapped && s == STATUS_END_OF_FILE) ? K32_ERROR_HANDLE_EOF : 0;
}

/* Bytes transferred for a DONE status. End of file transfers none, whatever
 * the information field holds. */
static inline DWORD k32_io_count(NTSTATUS s, ULONG_PTR info)
{
    if (s == STATUS_END_OF_FILE) return 0;
    return (DWORD)info;
}

/* OVERLAPPED Offset (low DWORD) and OffsetHigh (high DWORD) as the byte offset
 * passed to NtReadFile or NtWriteFile as ByteOffset. */
static inline LONGLONG k32_offset_from_parts(DWORD lo, DWORD hi)
{
    return (LONGLONG)(((uint64_t)hi << 32) | (uint64_t)lo);
}

/* GetConsoleMode and SetConsoleMode accept only the console device. The
 * device type comes from FileFsDeviceInformation (NtQueryVolumeInformationFile). */
#define K32_FILE_DEVICE_DISK               0x00000007u
#define K32_FILE_DEVICE_NETWORK_FILE_SYS   0x00000014u
#define K32_FILE_DEVICE_NAMED_PIPE         0x00000011u
#define K32_FILE_DEVICE_CONSOLE            0x00000050u

static inline int k32_device_is_console(ULONG device_type)
{
    return device_type == K32_FILE_DEVICE_CONSOLE;
}

/* GetFileType results: FILE_TYPE_UNKNOWN 0, FILE_TYPE_DISK 1,
 * FILE_TYPE_CHAR 2, FILE_TYPE_PIPE 3. Only the device types above are
 * classified; any other device is UNKNOWN. */
static inline DWORD k32_file_type_from_device(ULONG device_type)
{
    switch (device_type) {
    case K32_FILE_DEVICE_CONSOLE:          return 2;
    case K32_FILE_DEVICE_NAMED_PIPE:       return 3;
    case K32_FILE_DEVICE_DISK:
    case K32_FILE_DEVICE_NETWORK_FILE_SYS: return 1;
    default:                               return 0;
    }
}

/* Console mode flags. Input: ENABLE_PROCESSED_INPUT 0x1, LINE_INPUT 0x2,
 * ECHO_INPUT 0x4, WINDOW_INPUT 0x8, MOUSE_INPUT 0x10, INSERT_MODE 0x20,
 * QUICK_EDIT_MODE 0x40, EXTENDED_FLAGS 0x80, AUTO_POSITION 0x100,
 * VIRTUAL_TERMINAL_INPUT 0x200. Output: PROCESSED_OUTPUT 0x1,
 * WRAP_AT_EOL_OUTPUT 0x2, VIRTUAL_TERMINAL_PROCESSING 0x4,
 * DISABLE_NEWLINE_AUTO_RETURN 0x8, LVB_GRID_WORLDWIDE 0x10. */
#define K32_CONSOLE_IN_MASK      0x000003FFu
#define K32_CONSOLE_OUT_MASK     0x0000001Fu
#define K32_CONSOLE_IN_DEFAULT   0x00000007u /* PROCESSED | LINE | ECHO */
#define K32_CONSOLE_OUT_DEFAULT  0x00000003u /* PROCESSED | WRAP_AT_EOL */

/* 1 when mode has no bit outside the input (output == 0) or output set. */
static inline int k32_console_mode_valid(DWORD mode, int output)
{
    DWORD mask = output ? K32_CONSOLE_OUT_MASK : K32_CONSOLE_IN_MASK;
    return (mode & ~mask) == 0;
}

/* WriteConsoleW and ReadConsoleW move text in chunks of at most this many
 * characters, so the shim needs only a fixed stack buffer. */
#define K32_CONSOLE_CHUNK 256u

static inline size_t k32_console_chunk(size_t remaining)
{
    return remaining < K32_CONSOLE_CHUNK ? remaining : K32_CONSOLE_CHUNK;
}

/* Counted narrowing for WriteConsoleW. The input is not NUL-terminated; the
 * policy matches k32_ansi_from_wide (characters above 0xFF become '?'). out
 * must hold n bytes. */
static inline void k32_narrow_counted(const WCHAR *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; ++i)
        out[i] = in[i] <= 0xFF ? (char)(unsigned char)in[i] : '?';
}

/* Counted widening for ReadConsoleW: one byte to one WCHAR (Latin-1, as in
 * k32_wide_from_ansi). out must hold n WCHARs. */
static inline void k32_widen_counted(const char *in, size_t n, WCHAR *out)
{
    for (size_t i = 0; i < n; ++i)
        out[i] = (WCHAR)(unsigned char)in[i];
}

#endif /* NTDLL_K32_IO_PURE_H */
