/*
 * process_params.h - pure builder and validator for the process parameter
 * block (prerequisite P7, milestone M1 of docs/architecture/process-creation.md).
 *
 * The block carries the four strings of RTL_USER_PROCESS_PARAMETERS that a
 * new process needs (ImagePathName, CommandLine, CurrentDirectory and
 * Environment) in a relocatable form. Every string is stored as UTF-16 bytes at
 * an offset from the start of the block, never as a pointer, so the block can be
 * copied into another address space and used there unchanged.
 *
 * Layout (all fields little-endian ULONG, offsets from the block start):
 *
 *   0x00 Magic            PP_MAGIC ("PPR1")
 *   0x04 Version          PP_VERSION (1)
 *   0x08 TotalSize        length of the whole block in bytes
 *   0x0C Flags            must be 0 in version 1
 *   0x10 ImagePathOffset  0x14 ImagePathLength (bytes)
 *   0x18 CommandLineOffset 0x1C CommandLineLength (bytes)
 *   0x20 CurrentDirOffset 0x24 CurrentDirLength (bytes)
 *   0x28 EnvironmentOffset 0x2C EnvironmentLength (bytes)
 *   0x30 Reserved[4]      must be 0
 *   0x40 string data, packed in the order above, each string 2-byte aligned
 *
 * Normalization done by the builder:
 *   - an absent environment is stored as the canonical empty block "\0\0";
 *   - a non-empty current directory without a trailing backslash gets one.
 * The validator accepts only blocks the builder could have produced in shape,
 * and rejects anything else: bad sizes, misaligned or overlapping ranges, offsets
 * that would overflow, strings over the UNICODE_STRING limit, and bad contents.
 *
 * Nothing here touches memory outside the buffers passed in, takes a lock, or
 * allocates. It needs no libc. The caller copies user memory into a kernel
 * buffer first; these functions never see user pointers.
 */

#ifndef NT_PROCESS_PARAMS_H
#define NT_PROCESS_PARAMS_H

#ifndef __KERNEL__
#include <stdbool.h>
#endif
#include "../../ntdll/nt_types.h"
#include "../../ntdll/ntstatus.h"

/* -------------------------------------------------------------------------- */
/* Format constants                                                           */
/* -------------------------------------------------------------------------- */

#define PP_MAGIC             0x31525050UL      /* "PPR1" */
#define PP_VERSION           1UL
#define PP_HEADER_SIZE       0x40UL
#define PP_MAX_CHARS         0x7FFFUL          /* 0xFFFE bytes: UNICODE_STRING limit */
#define PP_MAX_STRING_BYTES  (PP_MAX_CHARS * 2UL)
#define PP_MAX_ENV_CHARS     0x7FFFUL
#define PP_MAX_ENV_BYTES     (PP_MAX_ENV_CHARS * 2UL)
#define PP_MAX_BLOB_SIZE     0x50000UL         /* 320 KiB, above the sum of the maxima */

#define PP_OFF_MAGIC         0x00
#define PP_OFF_VERSION       0x04
#define PP_OFF_TOTAL         0x08
#define PP_OFF_FLAGS         0x0C
#define PP_OFF_IMG           0x10              /* offset, then length */
#define PP_OFF_CMD           0x18
#define PP_OFF_CWD           0x20
#define PP_OFF_ENV           0x28
#define PP_OFF_RESERVED      0x30

/* -------------------------------------------------------------------------- */
/* Builder input and validator output                                         */
/* -------------------------------------------------------------------------- */

/* A UTF-16 string as counted code units. buf may be NULL only when chars is 0. */
struct pp_str_in {
    const WCHAR *buf;
    ULONG        chars;
};

struct pp_params_in {
    struct pp_str_in image;          /* required, 1..PP_MAX_CHARS, no NUL */
    struct pp_str_in command_line;   /* optional, 0..PP_MAX_CHARS, no NUL */
    struct pp_str_in current_dir;    /* optional, 0..PP_MAX_CHARS, no NUL */
    struct pp_str_in environment;    /* optional; if set, ends in two NULs */
};

/* Points into a validated block. Valid only while the block is unchanged. */
struct pp_params_view {
    const WCHAR *image;
    ULONG        image_chars;
    const WCHAR *command_line;
    ULONG        command_line_chars;
    const WCHAR *current_dir;
    ULONG        current_dir_chars;
    const WCHAR *environment;        /* includes the final two NULs */
    ULONG        environment_chars;
};

/* -------------------------------------------------------------------------- */
/* Internal helpers                                                           */
/* -------------------------------------------------------------------------- */

static inline void pp_put32(unsigned char *b, ULONG off, ULONG v)
{
    b[off]     = (unsigned char)(v);
    b[off + 1] = (unsigned char)(v >> 8);
    b[off + 2] = (unsigned char)(v >> 16);
    b[off + 3] = (unsigned char)(v >> 24);
}

static inline ULONG pp_get32(const unsigned char *b, ULONG off)
{
    return (ULONG)b[off] | ((ULONG)b[off + 1] << 8) |
           ((ULONG)b[off + 2] << 16) | ((ULONG)b[off + 3] << 24);
}

static inline void pp_put16(unsigned char *b, ULONG off, WCHAR c)
{
    b[off]     = (unsigned char)(c);
    b[off + 1] = (unsigned char)(c >> 8);
}

static inline WCHAR pp_get16(const unsigned char *b, ULONG off)
{
    return (WCHAR)((WCHAR)b[off] | ((WCHAR)b[off + 1] << 8));
}

/* Checks one caller string. An empty string is allowed only when min_chars is 0. */
static inline NTSTATUS pp_check_string(struct pp_str_in s, ULONG min_chars,
                                       ULONG max_chars, bool nul_ok)
{
    if (s.chars == 0)
        return min_chars == 0 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    if (!s.buf) return STATUS_INVALID_PARAMETER;
    if (s.chars < min_chars) return STATUS_INVALID_PARAMETER;
    if (s.chars > max_chars) return STATUS_NAME_TOO_LONG;
    if (!nul_ok) {
        for (ULONG i = 0; i < s.chars; i++)
            if (s.buf[i] == 0) return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Builder                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * Builds a block for in into out. *out_len always receives the block size the
 * input needs, so a caller can pass out = NULL, out_cap = 0 to query it.
 * Returns STATUS_BUFFER_TOO_SMALL when out_cap is smaller than that size.
 * Returns STATUS_INVALID_PARAMETER for a malformed input and
 * STATUS_NAME_TOO_LONG for a string or block over its limit.
 */
static inline NTSTATUS PpBuildParamsBlob(const struct pp_params_in *in,
                                         void *out, ULONG out_cap, ULONG *out_len)
{
    ULONG img_c, cmd_c, cwd_c, env_c;
    bool  add_slash = false;
    ULONG total;
    unsigned char *b = (unsigned char *)out;
    NTSTATUS st;

    if (!in || !out_len) return STATUS_INVALID_PARAMETER;

    st = pp_check_string(in->image, 1, PP_MAX_CHARS, false);
    if (!NT_SUCCESS(st)) return st;
    st = pp_check_string(in->command_line, 0, PP_MAX_CHARS, false);
    if (!NT_SUCCESS(st)) return st;
    st = pp_check_string(in->current_dir, 0, PP_MAX_CHARS, false);
    if (!NT_SUCCESS(st)) return st;

    img_c = in->image.chars;
    cmd_c = in->command_line.chars;
    cwd_c = in->current_dir.chars;
    if (cwd_c != 0 && in->current_dir.buf[cwd_c - 1] != (WCHAR)'\\') {
        if (cwd_c + 1 > PP_MAX_CHARS) return STATUS_NAME_TOO_LONG;
        add_slash = true;
        cwd_c++;
    }

    if (in->environment.chars == 0) {
        env_c = 2;                              /* canonical empty block */
    } else {
        st = pp_check_string(in->environment, 2, PP_MAX_ENV_CHARS, true);
        if (!NT_SUCCESS(st)) return st;
        if (in->environment.buf[in->environment.chars - 1] != 0 ||
            in->environment.buf[in->environment.chars - 2] != 0)
            return STATUS_INVALID_PARAMETER;
        env_c = in->environment.chars;
    }

    total = (ULONG)(PP_HEADER_SIZE + 2UL * ((ULONG)img_c + cmd_c + cwd_c + env_c));
    /* Each term is at most PP_MAX_ENV_CHARS, so this sum cannot wrap a ULONG. */
    if (total > PP_MAX_BLOB_SIZE) return STATUS_NAME_TOO_LONG;
    *out_len = total;

    if (!out || out_cap < total) return STATUS_BUFFER_TOO_SMALL;

    {
        ULONG cur = (ULONG)PP_HEADER_SIZE;
        ULONG img_off = cur;  cur += img_c * 2;
        ULONG cmd_off = cur;  cur += cmd_c * 2;
        ULONG cwd_off = cur;  cur += cwd_c * 2;
        ULONG env_off = cur;

        pp_put32(b, PP_OFF_MAGIC, PP_MAGIC);
        pp_put32(b, PP_OFF_VERSION, PP_VERSION);
        pp_put32(b, PP_OFF_TOTAL, total);
        pp_put32(b, PP_OFF_FLAGS, 0);
        pp_put32(b, PP_OFF_IMG, img_off);
        pp_put32(b, PP_OFF_IMG + 4, img_c * 2);
        pp_put32(b, PP_OFF_CMD, cmd_off);
        pp_put32(b, PP_OFF_CMD + 4, cmd_c * 2);
        pp_put32(b, PP_OFF_CWD, cwd_off);
        pp_put32(b, PP_OFF_CWD + 4, cwd_c * 2);
        pp_put32(b, PP_OFF_ENV, env_off);
        pp_put32(b, PP_OFF_ENV + 4, env_c * 2);
        for (ULONG i = 0; i < 4; i++) pp_put32(b, PP_OFF_RESERVED + 4 * i, 0);

        for (ULONG i = 0; i < img_c; i++)
            pp_put16(b, img_off + 2 * i, in->image.buf[i]);
        for (ULONG i = 0; i < cmd_c; i++)
            pp_put16(b, cmd_off + 2 * i, in->command_line.buf ? in->command_line.buf[i] : 0);
        for (ULONG i = 0; i < in->current_dir.chars; i++)
            pp_put16(b, cwd_off + 2 * i, in->current_dir.buf[i]);
        if (add_slash)
            pp_put16(b, cwd_off + 2 * (cwd_c - 1), (WCHAR)'\\');
        if (in->environment.chars == 0) {
            pp_put16(b, env_off, 0);
            pp_put16(b, env_off + 2, 0);
        } else {
            for (ULONG i = 0; i < env_c; i++)
                pp_put16(b, env_off + 2 * i, in->environment.buf[i]);
        }
    }
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Validator                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * Checks a block of len bytes and, on success, fills view. Never reads outside
 * [blob, blob + len). A block whose len is odd, or whose size
 * differs from TotalSize, is STATUS_INFO_LENGTH_MISMATCH. Every other defect is
 * STATUS_INVALID_PARAMETER.
 */
static inline NTSTATUS PpValidateParamsBlob(const void *blob, ULONG len,
                                            struct pp_params_view *view)
{
    const unsigned char *b = (const unsigned char *)blob;
    ULONG off[4], sz[4], i, j;
    static const ULONG hdr_off[4] = { PP_OFF_IMG, PP_OFF_CMD, PP_OFF_CWD, PP_OFF_ENV };

    if (!blob || !view) return STATUS_INVALID_PARAMETER;
    if (len < PP_HEADER_SIZE || len > PP_MAX_BLOB_SIZE || (len & 1))
        return STATUS_INFO_LENGTH_MISMATCH;
    /* WCHAR views need 2-byte alignment; a block at an odd address is malformed. */
    if ((((ULONG_PTR)blob) & 1U) != 0) return STATUS_INVALID_PARAMETER;

    if (pp_get32(b, PP_OFF_MAGIC) != PP_MAGIC) return STATUS_INVALID_PARAMETER;
    if (pp_get32(b, PP_OFF_VERSION) != PP_VERSION) return STATUS_INVALID_PARAMETER;
    if (pp_get32(b, PP_OFF_TOTAL) != len) return STATUS_INFO_LENGTH_MISMATCH;
    if (pp_get32(b, PP_OFF_FLAGS) != 0) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < 4; i++)
        if (pp_get32(b, PP_OFF_RESERVED + 4 * i) != 0) return STATUS_INVALID_PARAMETER;

    for (i = 0; i < 4; i++) {
        ULONG max_bytes = (i == 3) ? PP_MAX_ENV_BYTES : PP_MAX_STRING_BYTES;
        off[i] = pp_get32(b, hdr_off[i]);
        sz[i]  = pp_get32(b, hdr_off[i] + 4);
        if ((off[i] & 1) || (sz[i] & 1)) return STATUS_INVALID_PARAMETER;
        if (sz[i] > max_bytes) return STATUS_INVALID_PARAMETER;
        if (off[i] < PP_HEADER_SIZE || off[i] > len) return STATUS_INVALID_PARAMETER;
        /* Both values are at most 2^32, so this sum is exact in 64 bits. */
        if ((unsigned long long)off[i] + sz[i] > len) return STATUS_INVALID_PARAMETER;
    }

    /* Ranges must not overlap. Empty ranges occupy nothing. */
    for (i = 0; i < 4; i++) {
        for (j = i + 1; j < 4; j++) {
            if (sz[i] == 0 || sz[j] == 0) continue;
            if ((unsigned long long)off[i] < (unsigned long long)off[j] + sz[j] &&
                (unsigned long long)off[j] < (unsigned long long)off[i] + sz[i])
                return STATUS_INVALID_PARAMETER;
        }
    }

    /* Image: non-empty, no NUL. */
    if (sz[0] < 2) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < sz[0] / 2; i++)
        if (pp_get16(b, off[0] + 2 * i) == 0) return STATUS_INVALID_PARAMETER;
    /* Command line: no NUL. */
    for (i = 0; i < sz[1] / 2; i++)
        if (pp_get16(b, off[1] + 2 * i) == 0) return STATUS_INVALID_PARAMETER;
    /* Current directory: no NUL, and a trailing backslash when set. */
    for (i = 0; i < sz[2] / 2; i++)
        if (pp_get16(b, off[2] + 2 * i) == 0) return STATUS_INVALID_PARAMETER;
    if (sz[2] != 0 && pp_get16(b, off[2] + sz[2] - 2) != (WCHAR)'\\')
        return STATUS_INVALID_PARAMETER;
    /* Environment: at least the double NUL terminator. */
    if (sz[3] < 4) return STATUS_INVALID_PARAMETER;
    if (pp_get16(b, off[3] + sz[3] - 2) != 0 || pp_get16(b, off[3] + sz[3] - 4) != 0)
        return STATUS_INVALID_PARAMETER;

    view->image               = (const WCHAR *)(b + off[0]);
    view->image_chars         = sz[0] / 2;
    view->command_line        = (const WCHAR *)(b + off[1]);
    view->command_line_chars  = sz[1] / 2;
    view->current_dir         = (const WCHAR *)(b + off[2]);
    view->current_dir_chars   = sz[2] / 2;
    view->environment         = (const WCHAR *)(b + off[3]);
    view->environment_chars   = sz[3] / 2;
    return STATUS_SUCCESS;
}

#endif /* NT_PROCESS_PARAMS_H */
