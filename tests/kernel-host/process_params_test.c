/* process_params_test.c - host tests for the pure RTL_USER_PROCESS_PARAMETERS
 * blob builder and validator in src/kernel/nt/process_params.h (prerequisite P7,
 * milestone M1 of docs/architecture/process-creation.md).
 *
 * Covers: the builder's normalization (empty environment becomes the canonical
 * double NUL, a current directory gets a trailing backslash), the two-pass size
 * query, every rejection the builder makes, and the validator's rejection of
 * malformed blobs: truncated, oversized, bad header fields, misaligned or
 * overlapping ranges, offsets that would overflow, and bad string contents.
 * Truncation and single-byte corruption sweeps check that nothing crashes.
 *
 * Built and run by tests/kernel-host/Makefile (local test, no kernel objects).
 */

#include "process_params.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { printf("  RUN %s\n", #name); test_##name(); } while (0)
#define PASS() do { g_pass++; printf("  PASS %s\n", __func__); } while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; return; \
    } \
} while (0)

#define ASSERT_STATUS(expr, expect) do { \
    NTSTATUS _st = (expr); \
    if (_st != (expect)) { \
        fprintf(stderr, "FAIL %s:%d: expected 0x%08lx got 0x%08lx for %s\n", \
                __FILE__, __LINE__, (unsigned long)(expect), (unsigned long)_st, #expr); \
        g_fail++; return; \
    } \
} while (0)

/* Converts an ASCII literal to UTF-16 code units in a fixed buffer. */
#define W_MAX 512
static WCHAR g_wbuf[8][W_MAX];
static int g_wslot;
static const WCHAR *w(const char *s, ULONG *chars)
{
    WCHAR *dst = g_wbuf[g_wslot++ & 7];
    ULONG n = 0;
    while (*s && n < W_MAX - 2) dst[n++] = (WCHAR)(unsigned char)*s++;
    dst[n] = 0;
    if (chars) *chars = n;
    return dst;
}

static struct pp_str_in S(const char *s)
{
    struct pp_str_in r;
    r.buf = w(s, &r.chars);
    return r;
}

static struct pp_str_in NONE(void)
{
    struct pp_str_in r = { NULL, 0 };
    return r;
}

/* A fully populated input used by several tests. */
static struct pp_params_in full_in(void)
{
    struct pp_params_in in;
    in.image = S("C:\\tests\\app.exe");
    in.command_line = S("app.exe --flag \"quoted arg\"");
    in.current_dir = S("C:\\tests");
    /* Environment: "A=1\0B=2\0\0" */
    static const WCHAR env[] = { 'A', '=', '1', 0, 'B', '=', '2', 0, 0, 0 };
    in.environment.buf = env;
    in.environment.chars = 10;
    return in;
}

/* Builds a blob into a heap buffer; returns NULL on failure. */
static unsigned char *build_blob(const struct pp_params_in *in, ULONG *len_out)
{
    ULONG need = 0;
    NTSTATUS st = PpBuildParamsBlob(in, NULL, 0, &need);
    if (st != STATUS_BUFFER_TOO_SMALL || need == 0) return NULL;
    unsigned char *buf = (unsigned char *)malloc(need);
    if (!buf) return NULL;
    ULONG got = 0;
    st = PpBuildParamsBlob(in, buf, need, &got);
    if (st != STATUS_SUCCESS || got != need) { free(buf); return NULL; }
    *len_out = got;
    return buf;
}

static void put32(unsigned char *b, size_t off, ULONG v)
{
    b[off] = (unsigned char)v; b[off + 1] = (unsigned char)(v >> 8);
    b[off + 2] = (unsigned char)(v >> 16); b[off + 3] = (unsigned char)(v >> 24);
}

static ULONG get32(const unsigned char *b, size_t off)
{
    return (ULONG)b[off] | ((ULONG)b[off + 1] << 8) |
           ((ULONG)b[off + 2] << 16) | ((ULONG)b[off + 3] << 24);
}

/* Header field offsets, from the layout in process_params.h. */
#define F_MAGIC     0
#define F_VERSION   4
#define F_TOTAL     8
#define F_FLAGS     12
#define F_IMG_OFF   16
#define F_IMG_LEN   20
#define F_CMD_OFF   24
#define F_CMD_LEN   28
#define F_CWD_OFF   32
#define F_CWD_LEN   36
#define F_ENV_OFF   40
#define F_ENV_LEN   44
#define F_RESERVED  48

static bool wcs_eq(const WCHAR *a, ULONG chars, const char *b)
{
    ULONG n = (ULONG)strlen(b);
    if (n != chars) return false;
    for (ULONG i = 0; i < n; i++) if (a[i] != (WCHAR)(unsigned char)b[i]) return false;
    return true;
}

/* -------------------------------------------------------------------------- */
/* 1. round trip of a fully populated block                                    */
/* -------------------------------------------------------------------------- */
TEST(round_trip_full)
{
    struct pp_params_in in = full_in();
    ULONG len = 0;
    unsigned char *blob = build_blob(&in, &len);
    ASSERT(blob != NULL);
    ASSERT(len >= PP_HEADER_SIZE);
    ASSERT(len % 2 == 0);
    ASSERT(get32(blob, F_TOTAL) == len);

    struct pp_params_view v;
    ASSERT_STATUS(PpValidateParamsBlob(blob, len, &v), STATUS_SUCCESS);
    ASSERT(wcs_eq(v.image, v.image_chars, "C:\\tests\\app.exe"));
    ASSERT(wcs_eq(v.command_line, v.command_line_chars, "app.exe --flag \"quoted arg\""));
    ASSERT(wcs_eq(v.current_dir, v.current_dir_chars, "C:\\tests\\"));
    ASSERT(v.environment_chars == 10);
    ASSERT(v.environment[0] == 'A' && v.environment[8] == 0 && v.environment[9] == 0);
    free(blob);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 2. normalization: empty environment, trailing backslash, empty optionals    */
/* -------------------------------------------------------------------------- */
TEST(normalization)
{
    struct pp_params_in in;
    in.image = S("C:\\x.exe");
    in.command_line = NONE();
    in.current_dir = S("C:");
    in.environment = NONE();
    ULONG len = 0;
    unsigned char *blob = build_blob(&in, &len);
    ASSERT(blob != NULL);

    struct pp_params_view v;
    ASSERT_STATUS(PpValidateParamsBlob(blob, len, &v), STATUS_SUCCESS);
    ASSERT(v.command_line_chars == 0);
    ASSERT(wcs_eq(v.current_dir, v.current_dir_chars, "C:\\"));  /* backslash added */
    /* An absent environment is stored as the canonical empty block "\0\0". */
    ASSERT(v.environment_chars == 2);
    ASSERT(v.environment[0] == 0 && v.environment[1] == 0);
    free(blob);

    /* A current directory that already ends in a backslash is not extended. */
    in.current_dir = S("D:\\dir\\");
    blob = build_blob(&in, &len);
    ASSERT(blob != NULL);
    ASSERT_STATUS(PpValidateParamsBlob(blob, len, &v), STATUS_SUCCESS);
    ASSERT(wcs_eq(v.current_dir, v.current_dir_chars, "D:\\dir\\"));
    free(blob);

    /* An empty current directory stays empty. */
    in.current_dir = NONE();
    blob = build_blob(&in, &len);
    ASSERT(blob != NULL);
    ASSERT_STATUS(PpValidateParamsBlob(blob, len, &v), STATUS_SUCCESS);
    ASSERT(v.current_dir_chars == 0);
    free(blob);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 3. two-pass size query and capacity                                         */
/* -------------------------------------------------------------------------- */
TEST(size_query_and_capacity)
{
    struct pp_params_in in = full_in();
    ULONG need = 0;
    ASSERT_STATUS(PpBuildParamsBlob(&in, NULL, 0, &need), STATUS_BUFFER_TOO_SMALL);
    ASSERT(need > PP_HEADER_SIZE);

    unsigned char *small = (unsigned char *)malloc(need - 1);
    ASSERT(small != NULL);
    ULONG got = 0;
    ASSERT_STATUS(PpBuildParamsBlob(&in, small, need - 1, &got), STATUS_BUFFER_TOO_SMALL);
    ASSERT(got == need);  /* the required size is reported even on failure */
    free(small);

    unsigned char *exact = (unsigned char *)malloc(need);
    ASSERT(exact != NULL);
    ASSERT_STATUS(PpBuildParamsBlob(&in, exact, need, &got), STATUS_SUCCESS);
    ASSERT(got == need);
    free(exact);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 4. builder rejects bad input                                                */
/* -------------------------------------------------------------------------- */
TEST(builder_rejects_bad_input)
{
    ULONG need = 0;
    struct pp_params_in in = full_in();

    /* empty image */
    in.image = NONE();
    ASSERT_STATUS(PpBuildParamsBlob(&in, NULL, 0, &need), STATUS_INVALID_PARAMETER);

    /* NUL inside the image path */
    in = full_in();
    static const WCHAR nul_img[] = { 'C', ':', 0, 'x' };
    in.image.buf = nul_img;
    in.image.chars = 4;
    ASSERT_STATUS(PpBuildParamsBlob(&in, NULL, 0, &need), STATUS_INVALID_PARAMETER);

    /* image one character over the limit */
    in = full_in();
    static WCHAR big[PP_MAX_CHARS + 2];
    for (ULONG i = 0; i < PP_MAX_CHARS + 1; i++) big[i] = 'a';
    in.image.buf = big;
    in.image.chars = PP_MAX_CHARS + 1;
    ASSERT_STATUS(PpBuildParamsBlob(&in, NULL, 0, &need), STATUS_NAME_TOO_LONG);

    /* environment without the double NUL terminator */
    in = full_in();
    static const WCHAR env_bad[] = { 'A', '=', '1', 0, 'B', '=', '2' };
    in.environment.buf = env_bad;
    in.environment.chars = 7;
    ASSERT_STATUS(PpBuildParamsBlob(&in, NULL, 0, &need), STATUS_INVALID_PARAMETER);

    /* environment over the byte limit */
    in = full_in();
    static WCHAR env_huge[PP_MAX_ENV_CHARS + 2];
    env_huge[PP_MAX_ENV_CHARS] = 0;
    env_huge[PP_MAX_ENV_CHARS + 1] = 0;
    in.environment.buf = env_huge;
    in.environment.chars = PP_MAX_ENV_CHARS + 2;
    ASSERT_STATUS(PpBuildParamsBlob(&in, NULL, 0, &need), STATUS_NAME_TOO_LONG);

    /* null output with zero-length input pointer is not a valid string */
    in = full_in();
    in.command_line.buf = NULL;
    in.command_line.chars = 5;
    ASSERT_STATUS(PpBuildParamsBlob(&in, NULL, 0, &need), STATUS_INVALID_PARAMETER);

    /* a NULL input block */
    ASSERT_STATUS(PpBuildParamsBlob(NULL, NULL, 0, &need), STATUS_INVALID_PARAMETER);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 5. validator: size and header fields                                        */
/* -------------------------------------------------------------------------- */
TEST(validator_header_fields)
{
    struct pp_params_in in = full_in();
    ULONG len = 0;
    unsigned char *good = build_blob(&in, &len);
    ASSERT(good != NULL);
    unsigned char *b = (unsigned char *)malloc(len);
    ASSERT(b != NULL);
    struct pp_params_view v;

    ASSERT_STATUS(PpValidateParamsBlob(NULL, len, &v), STATUS_INVALID_PARAMETER);
    ASSERT_STATUS(PpValidateParamsBlob(good, 0, &v), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(PpValidateParamsBlob(good, PP_HEADER_SIZE - 8, &v), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(PpValidateParamsBlob(good, len - 8, &v), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(PpValidateParamsBlob(good, len + 8, &v), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(PpValidateParamsBlob(good, len + 1, &v), STATUS_INFO_LENGTH_MISMATCH);
    ASSERT_STATUS(PpValidateParamsBlob(good, PP_MAX_BLOB_SIZE + 8, &v), STATUS_INFO_LENGTH_MISMATCH);

#define MUTATE(off, val, expect) do { \
        memcpy(b, good, len); put32(b, (off), (val)); \
        ASSERT_STATUS(PpValidateParamsBlob(b, len, &v), (expect)); \
    } while (0)

    MUTATE(F_MAGIC, 0x12345678UL, STATUS_INVALID_PARAMETER);
    MUTATE(F_VERSION, 2, STATUS_INVALID_PARAMETER);
    MUTATE(F_VERSION, 0, STATUS_INVALID_PARAMETER);
    MUTATE(F_FLAGS, 1, STATUS_INVALID_PARAMETER);
    MUTATE(F_TOTAL, len - 8, STATUS_INFO_LENGTH_MISMATCH);
    MUTATE(F_TOTAL, len + 8, STATUS_INFO_LENGTH_MISMATCH);
    MUTATE(F_RESERVED, 1, STATUS_INVALID_PARAMETER);
    MUTATE(F_RESERVED + 12, 0x80000000UL, STATUS_INVALID_PARAMETER);

    free(b);
    free(good);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 6. validator: offsets, lengths, alignment, overlap, overflow                */
/* -------------------------------------------------------------------------- */
TEST(validator_ranges)
{
    struct pp_params_in in = full_in();
    ULONG len = 0;
    unsigned char *good = build_blob(&in, &len);
    ASSERT(good != NULL);
    ULONG img_off = get32(good, F_IMG_OFF);
    ULONG img_len = get32(good, F_IMG_LEN);
    ULONG env_off = get32(good, F_ENV_OFF);
    ULONG env_len = get32(good, F_ENV_LEN);
    unsigned char *b = (unsigned char *)malloc(len);
    ASSERT(b != NULL);
    struct pp_params_view v;

#define MUT2(off, val, expect) do { \
        memcpy(b, good, len); put32(b, (off), (val)); \
        ASSERT_STATUS(PpValidateParamsBlob(b, len, &v), (expect)); \
    } while (0)

    /* offset inside the header */
    MUT2(F_IMG_OFF, 8, STATUS_INVALID_PARAMETER);
    /* odd offset */
    MUT2(F_IMG_OFF, img_off + 1, STATUS_INVALID_PARAMETER);
    /* odd length */
    MUT2(F_IMG_LEN, img_len + 1, STATUS_INVALID_PARAMETER);
    /* length beyond the blob */
    MUT2(F_CMD_OFF, len, STATUS_INVALID_PARAMETER);
    /* offset near 2^32 so that offset + length wraps in 32 bits */
    MUT2(F_CMD_OFF, 0xFFFFFFF0UL, STATUS_INVALID_PARAMETER);
    MUT2(F_CMD_LEN, 0xFFFFFFF0UL, STATUS_INVALID_PARAMETER);
    /* a length over the 0xFFFE byte UNICODE_STRING limit */
    MUT2(F_CMD_LEN, 0x10000UL, STATUS_INVALID_PARAMETER);
    /* image overlapping the environment: point the image at the env block */
    MUT2(F_IMG_OFF, env_off, STATUS_INVALID_PARAMETER);
    /* environment overlapping the image */
    MUT2(F_ENV_OFF, img_off, STATUS_INVALID_PARAMETER);
    MUT2(F_ENV_LEN, env_len + 2, STATUS_INVALID_PARAMETER);
    /* environment shorter than the double NUL */
    MUT2(F_ENV_LEN, 2, STATUS_INVALID_PARAMETER);
    /* empty image */
    MUT2(F_IMG_LEN, 0, STATUS_INVALID_PARAMETER);

    free(b);
    free(good);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 7. validator: string contents                                               */
/* -------------------------------------------------------------------------- */
TEST(validator_string_contents)
{
    struct pp_params_in in = full_in();
    ULONG len = 0;
    unsigned char *good = build_blob(&in, &len);
    ASSERT(good != NULL);
    ULONG cwd_off = get32(good, F_CWD_OFF);
    ULONG cwd_len = get32(good, F_CWD_LEN);
    ULONG img_off = get32(good, F_IMG_OFF);
    unsigned char *b = (unsigned char *)malloc(len);
    ASSERT(b != NULL);
    struct pp_params_view v;

    /* current directory without the trailing backslash */
    memcpy(b, good, len);
    b[cwd_off + cwd_len - 2] = 'x';  /* final char is L'\\' at cwd_len-2 */
    ASSERT_STATUS(PpValidateParamsBlob(b, len, &v), STATUS_INVALID_PARAMETER);

    /* NUL inside the image path */
    memcpy(b, good, len);
    b[img_off + 2] = 0; b[img_off + 3] = 0;  /* WCHAR at index 1 becomes 0 */
    ASSERT_STATUS(PpValidateParamsBlob(b, len, &v), STATUS_INVALID_PARAMETER);

    /* the untouched blob still validates */
    ASSERT_STATUS(PpValidateParamsBlob(good, len, &v), STATUS_SUCCESS);
    free(b);
    free(good);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 8. truncation and corruption sweeps: never crash, never accept a truncation */
/* -------------------------------------------------------------------------- */
TEST(truncation_and_corruption_sweep)
{
    struct pp_params_in in = full_in();
    ULONG len = 0;
    unsigned char *good = build_blob(&in, &len);
    ASSERT(good != NULL);
    struct pp_params_view v;

    /* Every proper prefix must be rejected. */
    unsigned char *tmp = (unsigned char *)malloc(len);
    ASSERT(tmp != NULL);
    for (ULONG n = 0; n < len; n++) {
        memcpy(tmp, good, n);
        NTSTATUS st = PpValidateParamsBlob(tmp, n, &v);
        if (NT_SUCCESS(st)) {
            fprintf(stderr, "FAIL: prefix of %lu bytes accepted\n", (unsigned long)n);
            g_fail++;
            free(tmp); free(good);
            return;
        }
    }

    /* Flipping any single byte must not crash; the result is checked only
     * against the validator's own invariants (a success must be consistent). */
    for (ULONG i = 0; i < len; i++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(tmp, good, len);
            tmp[i] ^= (unsigned char)(1u << bit);
            NTSTATUS st = PpValidateParamsBlob(tmp, len, &v);
            if (NT_SUCCESS(st)) {
                ASSERT(v.image_chars >= 1);
                ASSERT(v.environment_chars >= 2);
                ASSERT(v.environment[v.environment_chars - 1] == 0);
                ASSERT(v.environment[v.environment_chars - 2] == 0);
            }
        }
    }
    free(tmp);
    free(good);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 9. blob size limit                                                          */
/* -------------------------------------------------------------------------- */
TEST(maximum_size_accepted)
{
    /* The largest legal blob: every string at its character limit. */
    static WCHAR big[PP_MAX_CHARS];
    for (ULONG i = 0; i < PP_MAX_CHARS; i++) big[i] = 'a';
    big[PP_MAX_CHARS - 1] = '\\';
    static WCHAR env[PP_MAX_ENV_CHARS];
    memset(env, 0, sizeof(env));
    env[0] = 'A'; env[1] = '=';
    struct pp_params_in in;
    in.image.buf = big;   in.image.chars = PP_MAX_CHARS;
    in.command_line.buf = big; in.command_line.chars = PP_MAX_CHARS;
    in.current_dir.buf = big;  in.current_dir.chars = PP_MAX_CHARS - 1;
    in.environment.buf = env;  in.environment.chars = PP_MAX_ENV_CHARS;
    ULONG len = 0;
    unsigned char *blob = build_blob(&in, &len);
    ASSERT(blob != NULL);
    ASSERT(len <= PP_MAX_BLOB_SIZE);
    struct pp_params_view v;
    ASSERT_STATUS(PpValidateParamsBlob(blob, len, &v), STATUS_SUCCESS);
    ASSERT(v.image_chars == PP_MAX_CHARS);
    free(blob);
    PASS();
}

int main(void)
{
    printf("process params tests starting\n");
    RUN(round_trip_full);
    RUN(normalization);
    RUN(size_query_and_capacity);
    RUN(builder_rejects_bad_input);
    RUN(validator_header_fields);
    RUN(validator_ranges);
    RUN(validator_string_contents);
    RUN(truncation_and_corruption_sweep);
    RUN(maximum_size_accepted);
    printf("process params tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    return 0;
}
