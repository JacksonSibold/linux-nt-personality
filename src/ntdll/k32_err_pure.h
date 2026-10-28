/*
 * k32_err_pure.h - FormatMessage's engine and the system error message table (US English), for kernel32_x.c.
 *
 * The formatter handles FORMAT_MESSAGE_FROM_STRING and FROM_SYSTEM with insert sequences (%1 .. %99, printf-style
 * "%1!d!" forms, %0 to end without a line break, %n, %r, %t, %%, "%space", "%.", "%!") and the line-width flags, over
 * ASCII or wide text. Host tests: tests/ntdll-host/test_k32_err_pure.c.
 */
#ifndef NTDLL_K32_ERR_PURE_H
#define NTDLL_K32_ERR_PURE_H

#include "nt_types.h"
#include <stddef.h>
#include <stdint.h>

#include "../crt/ucrt_printf.h"

#define K32E_FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100u
#define K32E_FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200u
#define K32E_FORMAT_MESSAGE_FROM_STRING 0x00000400u
#define K32E_FORMAT_MESSAGE_FROM_HMODULE 0x00000800u
#define K32E_FORMAT_MESSAGE_FROM_SYSTEM 0x00001000u
#define K32E_FORMAT_MESSAGE_ARGUMENT_ARRAY 0x00002000u
#define K32E_FORMAT_MESSAGE_MAX_WIDTH_MASK 0x000000FFu

typedef struct {
    DWORD code;
    const char *text; /* without the trailing line break */
} k32e_msg;

static const k32e_msg k32e_table[] = {
    {0, "The operation completed successfully."},
    {1, "Incorrect function."},
    {2, "The system cannot find the file specified."},
    {3, "The system cannot find the path specified."},
    {4, "The system cannot open the file."},
    {5, "Access is denied."},
    {6, "The handle is invalid."},
    {7, "The storage control blocks were destroyed."},
    {8, "Not enough memory resources are available to process this command."},
    {9, "The storage control block address is invalid."},
    {10, "The environment is incorrect."},
    {11, "An attempt was made to load a program with an incorrect format."},
    {12, "The access code is invalid."},
    {13, "The data is invalid."},
    {14, "Not enough memory resources are available to complete this operation."},
    {15, "The system cannot find the drive specified."},
    {16, "The directory cannot be removed."},
    {17, "The system cannot move the file to a different disk drive."},
    {18, "There are no more files."},
    {19, "The media is write protected."},
    {20, "The system cannot find the device specified."},
    {21, "The device is not ready."},
    {22, "The device does not recognize the command."},
    {23, "Data error (cyclic redundancy check)."},
    {24, "The program issued a command but the command length is incorrect."},
    {25, "The drive cannot locate a specific area or track on the disk."},
    {26, "The specified disk or diskette cannot be accessed."},
    {27, "The drive cannot find the sector requested."},
    {28, "The printer is out of paper."},
    {29, "The system cannot write to the specified device."},
    {30, "The system cannot read from the specified device."},
    {31, "A device attached to the system is not functioning."},
    {32, "The process cannot access the file because it is being used by another process."},
    {33, "The process cannot access the file because another process has locked a portion of the file."},
    {34, "The wrong diskette is in the drive. Insert %2 (Volume Serial Number: %3) into drive %1."},
    {36, "Too many files opened for sharing."},
    {38, "Reached the end of the file."},
    {39, "The disk is full."},
    {50, "The request is not supported."},
    {51, "Windows cannot find the network path. Verify that the network path is correct and the destination computer is not busy or turned off. If Windows still cannot find the network path, contact your network administrator."},
    {52, "You were not connected because a duplicate name exists on the network. If joining a domain, go to System in Control Panel to change the computer name and try again. If joining a workgroup, choose another workgroup name."},
    {53, "The network path was not found."},
    {54, "The network is busy."},
    {55, "The specified network resource or device is no longer available."},
    {56, "The network BIOS command limit has been reached."},
    {57, "A network adapter hardware error occurred."},
    {58, "The specified server cannot perform the requested operation."},
    {59, "An unexpected network error occurred."},
    {65, "Network access is denied."},
    {66, "The network resource type is not correct."},
    {67, "The network name cannot be found."},
    {80, "The file exists."},
    {82, "The directory or file cannot be created."},
    {83, "Fail on INT 24."},
    {84, "Storage to process this request is not available."},
    {85, "The local device name is already in use."},
    {86, "The specified network password is not correct."},
    {87, "The parameter is incorrect."},
    {88, "A write fault occurred on the network."},
    {89, "The system cannot start another process at this time."},
    {100, "Cannot create another system semaphore."},
    {101, "The exclusive semaphore is owned by another process."},
    {102, "The semaphore is set and cannot be closed."},
    {103, "The semaphore cannot be set again."},
    {104, "Cannot request exclusive semaphores at interrupt time."},
    {105, "The previous ownership of this semaphore has ended."},
    {107, "The program stopped because an alternate diskette was not inserted."},
    {108, "The disk is in use or locked by another process."},
    {109, "The pipe has been ended."},
    {110, "The system cannot open the device or file specified."},
    {111, "The file name is too long."},
    {112, "There is not enough space on the disk."},
    {113, "No more internal file identifiers available."},
    {114, "The target internal file identifier is incorrect."},
    {117, "The IOCTL call made by the application program is not correct."},
    {118, "The verify-on-write switch parameter value is not correct."},
    {119, "The system does not support the command requested."},
    {120, "This function is not supported on this system."},
    {121, "The semaphore timeout period has expired."},
    {122, "The data area passed to a system call is too small."},
    {123, "The filename, directory name, or volume label syntax is incorrect."},
    {124, "The system call level is not correct."},
    {125, "The disk has no volume label."},
    {126, "The specified module could not be found."},
    {127, "The specified procedure could not be found."},
    {128, "There are no child processes to wait for."},
    {129, "The %1 application cannot be run in Win32 mode."},
    {130, "Attempt to use a file handle to an open disk partition for an operation other than raw disk I/O."},
    {131, "An attempt was made to move the file pointer before the beginning of the file."},
    {132, "The file pointer cannot be set on the specified device or file."},
    {133, "A JOIN or SUBST command cannot be used for a drive that contains previously joined drives."},
    {145, "The directory is not empty."},
    {158, "The segment is already unlocked."},
    {161, "The specified path is invalid."},
    {164, "No more threads can be created in the system."},
    {170, "The requested resource is in use."},
    {183, "Cannot create a file when that file already exists."},
    {193, "%1 is not a valid Win32 application."},
    {206, "The filename or extension is too long."},
    {232, "The pipe is being closed."},
    {233, "No process is on the other end of the pipe."},
    {234, "More data is available."},
    {258, "The wait operation timed out."},
    {259, "No more data is available."},
    {267, "The directory name is invalid."},
    {298, "Too many posts were made to a semaphore."},
    {299, "Only part of a ReadProcessMemory or WriteProcessMemory request was completed."},
    {487, "Attempt to access invalid address."},
    {534, "Arithmetic result exceeded 32 bits."},
    {535, "There is a process on other end of the pipe."},
    {536, "Waiting for a process to open the other end of the pipe."},
    {995, "The I/O operation has been aborted because of either a thread exit or an application request."},
    {996, "Overlapped I/O event is not in a signaled state."},
    {997, "Overlapped I/O operation is in progress."},
    {998, "Invalid access to memory location."},
    {1004, "Invalid flags."},
    {1008, "An attempt was made to reference a token that does not exist."},
    {1058, "The service cannot be started, either because it is disabled or because it has no enabled devices associated with it."},
    {1167, "The device is not connected."},
    {1168, "Element not found."},
    {1223, "The operation was canceled by the user."},
    {1314, "A required privilege is not held by the client."},
    {1450, "Insufficient system resources exist to complete the requested service."},
    {1460, "This operation returned because the timeout period expired."},
    {1722, "The RPC server is unavailable."},
    {1813, "The specified resource type cannot be found in the image file."},
    {1814, "The specified resource name cannot be found in the image file."},
    {1815, "The specified resource language ID cannot be found in the image file."},
    {5023, "The cluster resource is not found."},
    {6118, "The list of servers for this workgroup is not currently available"},
};

#define K32E_TABLE_COUNT (sizeof(k32e_table) / sizeof(k32e_table[0]))

static inline const char *k32e_lookup(DWORD code)
{
    size_t lo = 0, hi = K32E_TABLE_COUNT;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (k32e_table[mid].code == code)
            return k32e_table[mid].text;
        if (k32e_table[mid].code < code)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* An output accumulator for wide characters; narrow output converts at the end. */
typedef struct {
    WCHAR *buf;
    size_t cap, n;
    int overflow;
    int line; /* characters since the last line break, for the width flag */
} k32e_out;

static inline void k32e_put(k32e_out *o, WCHAR c)
{
    if (o->n < o->cap)
        o->buf[o->n] = c;
    else
        o->overflow = 1;
    ++o->n;
    o->line = c == '\n' ? 0 : o->line + 1;
}

/* Formats `fmt` (wide) into o. args holds the insert values by number minus one (for strings, pointers; for numbers, the
 * value). narrow_inserts says %s takes narrow strings (the A function). flags are the FORMAT_MESSAGE_ ones; their low byte
 * is the line width (0 = the source's line breaks become spaces, 255 = kept as they are, otherwise breaks are made at
 * spaces once a line reaches that width). *stopped is set to 1 when "%0" ended the message (no trailing line break is
 * added then). Returns 0, or a Win32 error (ERROR_INVALID_PARAMETER for a bad insert reference). */
static inline DWORD k32e_format(k32e_out *o, const WCHAR *fmt, DWORD flags, const ULONG_PTR *args, size_t nargs, int narrow_inserts, int *stopped)
{
    DWORD width = flags & K32E_FORMAT_MESSAGE_MAX_WIDTH_MASK;
    int ignore = (flags & K32E_FORMAT_MESSAGE_IGNORE_INSERTS) != 0;
    *stopped = 0;
    for (const WCHAR *p = fmt; *p; ++p) {
        if (*p == '\r' && p[1] == '\n') { /* a line break in the source */
            ++p;
            if (width == 0xFF) {
                k32e_put(o, '\r');
                k32e_put(o, '\n');
            } else if (width == 0) {
                k32e_put(o, ' ');
            } else {
                k32e_put(o, ' ');
            }
            continue;
        }
        if (*p == '\n') {
            if (width == 0xFF)
                k32e_put(o, '\n');
            else
                k32e_put(o, ' ');
            continue;
        }
        if (*p != '%') {
            if (width && width != 0xFF && *p == ' ') {
                /* greedy wrapping: break at this space if the next word would carry the line past the width */
                size_t wl = 0;
                while (p[1 + wl] && p[1 + wl] != ' ' && p[1 + wl] != '%' && p[1 + wl] != '\r' && p[1 + wl] != '\n')
                    ++wl;
                if ((size_t)o->line + 1 + wl > width) {
                    k32e_put(o, '\r');
                    k32e_put(o, '\n');
                    continue;
                }
            }
            k32e_put(o, *p);
            continue;
        }
        ++p;
        if (*p == 0)
            break;
        if (*p >= '1' && *p <= '9' && !ignore) {
            DWORD idx = 0;
            ULONG_PTR a;
            while (*p >= '0' && *p <= '9') {
                idx = idx * 10 + (DWORD)(*p - '0');
                ++p;
            }
            if (idx == 0 || idx > 99 || idx > nargs)
                return 87u;
            a = args[idx - 1];
            if (*p == '!') {
                char spec[32], tmp[512], last;
                size_t sl = 0;
                int len;
                ++p;
                spec[sl++] = '%';
                while (*p && *p != '!' && sl < sizeof(spec) - 4)
                    spec[sl++] = (char)*p++;
                spec[sl] = 0;
                if (*p != '!')
                    return 87u;
                last = sl > 1 ? spec[sl - 1] : 0;
                if (last == 's' || last == 'S') {
                    /* %s is a string of the function's own width, %S the other */
                    int wide = narrow_inserts ? (last == 'S') : (last == 's');
                    if (wide) {
                        spec[sl - 1] = 'l';
                        spec[sl] = 's';
                        spec[sl + 1] = 0;
                    } else {
                        spec[sl - 1] = 's';
                    }
                    len = ucrt_snprintf(tmp, sizeof(tmp), spec, (void *)a);
                } else if (last == 'd' || last == 'i' || last == 'u' || last == 'x' || last == 'X' || last == 'c' || last == 'o') {
                    len = ucrt_snprintf(tmp, sizeof(tmp), spec, (int)a);
                } else {
                    len = ucrt_snprintf(tmp, sizeof(tmp), spec, (void *)a);
                }
                for (int i = 0; i < len && i < (int)sizeof(tmp) - 1; ++i)
                    k32e_put(o, (WCHAR)(unsigned char)tmp[i]);
            } else {
                --p;
                if (a) {
                    if (narrow_inserts) {
                        for (const char *s = (const char *)a; *s; ++s)
                            k32e_put(o, (WCHAR)(unsigned char)*s);
                    } else {
                        for (const WCHAR *s = (const WCHAR *)a; *s; ++s)
                            k32e_put(o, *s);
                    }
                } else {
                    for (const char *nul = "(null)"; *nul; ++nul)
                        k32e_put(o, (WCHAR)*nul);
                }
            }
            continue;
        }
        if (*p >= '1' && *p <= '9') { /* IGNORE_INSERTS: the sequence stays as written */
            k32e_put(o, '%');
            k32e_put(o, *p);
            continue;
        }
        switch (*p) {
        case '0':
            *stopped = 1;
            return 0;
        case 'n': k32e_put(o, '\r'); k32e_put(o, '\n'); break;
        case 'r': k32e_put(o, '\r'); break;
        case 't': k32e_put(o, '\t'); break;
        case '%': k32e_put(o, '%'); break;
        case ' ': k32e_put(o, ' '); break;
        case '.': k32e_put(o, '.'); break;
        case '!': k32e_put(o, '!'); break;
        default: k32e_put(o, *p); break;
        }
    }
    return 0;
}

#endif /* NTDLL_K32_ERR_PURE_H */
