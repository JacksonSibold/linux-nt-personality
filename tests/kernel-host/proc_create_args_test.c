/* proc_create_args_test.c - host tests for the NtCreateUserProcess argument
 * copy-in in src/kernel/nt/proc_create_args.h (milestone M5 of
 * docs/architecture/process-creation.md; argument validation only).
 *
 * The copy function stands in for copy_from_user, so a test can make a read
 * fault, and can change the user memory between two reads to model a double
 * fetch. The tests cover:
 *
 *   - PS_CREATE_INFO: exact Size (0x58), PsCreateInitialState, and a fault
 *   - PS_ATTRIBUTE_LIST: NULL means empty; TotalLength must be at least the
 *     header, a whole number of entries, and at most the maximum, checked
 *     before any arithmetic so a huge value cannot wrap
 *   - a TotalLength that changes between the header read and the full read is
 *     rejected, so the validated bytes are one snapshot
 *   - a fault during either read is STATUS_ACCESS_VIOLATION
 *   - the combined helper validates CreateInfo first and reads nothing else
 *     when that fails
 *
 * Built and run by tests/kernel-host/Makefile (local test, no kernel objects).
 */

#include "proc_create_args.h"

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

/* -------------------------------------------------------------------------- */
/* Fake user copy with fault injection and a hook that runs before each read.  */
/* -------------------------------------------------------------------------- */
static int g_calls;
static int g_fault_on_call;          /* 1-based call to fail, 0 = never */
static void (*g_hook)(int call);     /* runs before the copy of each call */

static int fake_copy(void *to, const void *from, size_t n)
{
    g_calls++;
    if (g_fault_on_call && g_calls == g_fault_on_call) return -1;
    if (g_hook) g_hook(g_calls);
    memcpy(to, from, n);
    return 0;
}

static void reset_copy(void)
{
    g_calls = 0;
    g_fault_on_call = 0;
    g_hook = NULL;
}

static void put64(unsigned char *b, size_t off, unsigned long long v)
{
    for (int i = 0; i < 8; i++) b[off + i] = (unsigned char)(v >> (8 * i));
}

static void put32(unsigned char *b, size_t off, unsigned long v)
{
    for (int i = 0; i < 4; i++) b[off + i] = (unsigned char)(v >> (8 * i));
}

/* A user PS_CREATE_INFO: Size at 0, State at 8, filler after. */
static void make_create_info(unsigned char *b, unsigned long long size, unsigned long state)
{
    memset(b, 0xA5, PCA_CREATE_INFO_SIZE);
    put64(b, 0, size);
    put32(b, 8, state);
}

/* A user PS_ATTRIBUTE_LIST with n entries and the given TotalLength. */
static void make_attr_list(unsigned char *b, unsigned long long total, unsigned int n)
{
    memset(b, 0, PCA_ATTR_LIST_MAX_BYTES);
    put64(b, 0, total);
    for (unsigned int i = 0; i < n; i++) {
        size_t base = PCA_ATTRIBUTE_LIST_HDR + i * PCA_ATTRIBUTE_SIZE;
        put64(b, base + 0, 0x0000000100000000ULL + i);   /* Attribute, above 32 bits */
        put64(b, base + 8, 8 + i);                       /* Size */
        put64(b, base + 16, 0x1122334455660000ULL + i);  /* Value */
        put64(b, base + 24, 0);                          /* ReturnLength */
    }
}

/* -------------------------------------------------------------------------- */
/* 1. PS_CREATE_INFO                                                           */
/* -------------------------------------------------------------------------- */
TEST(create_info_valid_copied_whole)
{
    reset_copy();
    unsigned char user[PCA_CREATE_INFO_SIZE];
    make_create_info(user, PCA_CREATE_INFO_SIZE, PCA_PS_CREATE_INITIAL);
    struct pca_create_info ci;
    ASSERT_STATUS(PcaCopyInCreateInfo(fake_copy, user, &ci), STATUS_SUCCESS);
    ASSERT(memcmp(ci.raw, user, PCA_CREATE_INFO_SIZE) == 0);
    ASSERT(g_calls == 1);
    PASS();
}

TEST(create_info_bad_size_and_state)
{
    reset_copy();
    unsigned char user[PCA_CREATE_INFO_SIZE];
    struct pca_create_info ci;

    make_create_info(user, PCA_CREATE_INFO_SIZE - 8, PCA_PS_CREATE_INITIAL);
    ASSERT_STATUS(PcaCopyInCreateInfo(fake_copy, user, &ci), STATUS_INFO_LENGTH_MISMATCH);
    make_create_info(user, PCA_CREATE_INFO_SIZE + 8, PCA_PS_CREATE_INITIAL);
    ASSERT_STATUS(PcaCopyInCreateInfo(fake_copy, user, &ci), STATUS_INFO_LENGTH_MISMATCH);
    make_create_info(user, 0, PCA_PS_CREATE_INITIAL);
    ASSERT_STATUS(PcaCopyInCreateInfo(fake_copy, user, &ci), STATUS_INFO_LENGTH_MISMATCH);
    make_create_info(user, ~0ULL, PCA_PS_CREATE_INITIAL);
    ASSERT_STATUS(PcaCopyInCreateInfo(fake_copy, user, &ci), STATUS_INFO_LENGTH_MISMATCH);

    make_create_info(user, PCA_CREATE_INFO_SIZE, 1);   /* not PsCreateInitialState */
    ASSERT_STATUS(PcaCopyInCreateInfo(fake_copy, user, &ci), STATUS_INVALID_PARAMETER);
    make_create_info(user, PCA_CREATE_INFO_SIZE, 3);   /* a result state, not an input */
    ASSERT_STATUS(PcaCopyInCreateInfo(fake_copy, user, &ci), STATUS_INVALID_PARAMETER);
    PASS();
}

TEST(create_info_fault_and_null)
{
    reset_copy();
    unsigned char user[PCA_CREATE_INFO_SIZE];
    make_create_info(user, PCA_CREATE_INFO_SIZE, PCA_PS_CREATE_INITIAL);
    struct pca_create_info ci;

    g_fault_on_call = 1;
    ASSERT_STATUS(PcaCopyInCreateInfo(fake_copy, user, &ci), STATUS_ACCESS_VIOLATION);
    reset_copy();
    ASSERT_STATUS(PcaCopyInCreateInfo(fake_copy, NULL, &ci), STATUS_INVALID_PARAMETER);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 2. PS_ATTRIBUTE_LIST                                                        */
/* -------------------------------------------------------------------------- */
TEST(attr_list_null_is_empty)
{
    reset_copy();
    struct pca_attr_list al;
    al.count = 99;
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, NULL, &al), STATUS_SUCCESS);
    ASSERT(al.count == 0);
    ASSERT(g_calls == 0);   /* nothing is read */
    PASS();
}

TEST(attr_list_valid_three_entries)
{
    reset_copy();
    unsigned char user[PCA_ATTR_LIST_MAX_BYTES];
    make_attr_list(user, PCA_ATTRIBUTE_LIST_HDR + 3 * PCA_ATTRIBUTE_SIZE, 3);
    struct pca_attr_list al;
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_SUCCESS);
    ASSERT(al.count == 3);
    ASSERT(g_calls == 2);   /* header, then the whole list */

    for (unsigned int i = 0; i < 3; i++) {
        struct pca_attribute a;
        ASSERT_STATUS(PcaAttributeAt(&al, i, &a), STATUS_SUCCESS);
        ASSERT(a.attribute == 0x0000000100000000ULL + i);
        ASSERT(a.size == 8 + i);
        ASSERT(a.value == 0x1122334455660000ULL + i);
        ASSERT(a.return_length == 0);
    }
    struct pca_attribute none;
    ASSERT_STATUS(PcaAttributeAt(&al, 3, &none), STATUS_INVALID_PARAMETER);
    PASS();
}

TEST(attr_list_header_only)
{
    reset_copy();
    unsigned char user[PCA_ATTR_LIST_MAX_BYTES];
    make_attr_list(user, PCA_ATTRIBUTE_LIST_HDR, 0);
    struct pca_attr_list al;
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_SUCCESS);
    ASSERT(al.count == 0);
    PASS();
}

TEST(attr_list_total_length_rules)
{
    reset_copy();
    unsigned char user[PCA_ATTR_LIST_MAX_BYTES];
    struct pca_attr_list al;

    /* shorter than the header */
    make_attr_list(user, 0, 0);
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_INFO_LENGTH_MISMATCH);
    make_attr_list(user, 7, 0);
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_INFO_LENGTH_MISMATCH);
    /* not a whole number of entries */
    make_attr_list(user, PCA_ATTRIBUTE_LIST_HDR + 33, 1);
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_INFO_LENGTH_MISMATCH);
    /* 33 entries: over the maximum */
    make_attr_list(user, PCA_ATTRIBUTE_LIST_HDR + 33 * PCA_ATTRIBUTE_SIZE, 0);
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_INVALID_PARAMETER);
    /* values near 2^64 that would wrap if multiplied first */
    make_attr_list(user, ~0ULL, 0);
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_INVALID_PARAMETER);
    make_attr_list(user, 0xFFFFFFFFFFFFFFE0ULL, 0);
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_INVALID_PARAMETER);
    PASS();
}

/* The header is read once and then the whole list. The hook rewrites the user
 * total before the second read, so the snapshot disagrees with itself. */
static unsigned char g_dfetch_user[PCA_ATTR_LIST_MAX_BYTES];

static void dfetch_hook(int call)
{
    if (call == 2)
        put64(g_dfetch_user, 0, PCA_ATTRIBUTE_LIST_HDR + 2 * PCA_ATTRIBUTE_SIZE);
}

TEST(attr_list_double_fetch_rejected)
{
    reset_copy();
    make_attr_list(g_dfetch_user, PCA_ATTRIBUTE_LIST_HDR + PCA_ATTRIBUTE_SIZE, 1);
    g_hook = dfetch_hook;
    struct pca_attr_list al;
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, g_dfetch_user, &al), STATUS_INVALID_PARAMETER);
    ASSERT(g_calls == 2);
    PASS();
}

TEST(attr_list_fault_on_either_read)
{
    reset_copy();
    unsigned char user[PCA_ATTR_LIST_MAX_BYTES];
    make_attr_list(user, PCA_ATTRIBUTE_LIST_HDR + PCA_ATTRIBUTE_SIZE, 1);
    struct pca_attr_list al;

    g_fault_on_call = 1;
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_ACCESS_VIOLATION);
    reset_copy();
    g_fault_on_call = 2;
    ASSERT_STATUS(PcaCopyInAttributeList(fake_copy, user, &al), STATUS_ACCESS_VIOLATION);
    PASS();
}

/* -------------------------------------------------------------------------- */
/* 3. the combined helper used by NtCreateUserProcess                          */
/* -------------------------------------------------------------------------- */
TEST(user_args_create_info_first)
{
    reset_copy();
    unsigned char ci_user[PCA_CREATE_INFO_SIZE];
    unsigned char al_user[PCA_ATTR_LIST_MAX_BYTES];
    make_create_info(ci_user, PCA_CREATE_INFO_SIZE, PCA_PS_CREATE_INITIAL);
    make_attr_list(al_user, PCA_ATTRIBUTE_LIST_HDR + 2 * PCA_ATTRIBUTE_SIZE, 2);
    struct pca_create_info ci;
    struct pca_attr_list al;

    ASSERT_STATUS(PcaCopyInUserProcessArgs(fake_copy, ci_user, al_user, &ci, &al), STATUS_SUCCESS);
    ASSERT(al.count == 2);

    /* a bad CreateInfo stops before the attribute list is read */
    reset_copy();
    make_create_info(ci_user, 0x40, PCA_PS_CREATE_INITIAL);
    ASSERT_STATUS(PcaCopyInUserProcessArgs(fake_copy, ci_user, al_user, &ci, &al),
                  STATUS_INFO_LENGTH_MISMATCH);
    ASSERT(g_calls == 1);

    /* CreateInfo is required */
    reset_copy();
    ASSERT_STATUS(PcaCopyInUserProcessArgs(fake_copy, NULL, al_user, &ci, &al),
                  STATUS_INVALID_PARAMETER);
    ASSERT(g_calls == 0);

    /* AttributeList is optional */
    reset_copy();
    make_create_info(ci_user, PCA_CREATE_INFO_SIZE, PCA_PS_CREATE_INITIAL);
    ASSERT_STATUS(PcaCopyInUserProcessArgs(fake_copy, ci_user, NULL, &ci, &al), STATUS_SUCCESS);
    ASSERT(al.count == 0);
    PASS();
}

int main(void)
{
    printf("proc create args tests starting\n");
    RUN(create_info_valid_copied_whole);
    RUN(create_info_bad_size_and_state);
    RUN(create_info_fault_and_null);
    RUN(attr_list_null_is_empty);
    RUN(attr_list_valid_three_entries);
    RUN(attr_list_header_only);
    RUN(attr_list_total_length_rules);
    RUN(attr_list_double_fetch_rejected);
    RUN(attr_list_fault_on_either_read);
    RUN(user_args_create_info_first);
    printf("proc create args tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    return 0;
}
