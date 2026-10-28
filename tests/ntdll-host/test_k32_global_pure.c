/*
 * test_k32_global_pure.c - host tests for src/ntdll/k32_global_pure.h, the handle table
 * behind GlobalAlloc/LocalAlloc and the lock, size, flags, realloc and discard rules.
 * The allocator is malloc with a counter, so leaks and double frees show up.
 */
#include "minitest.h"
#include "k32_global_pure.h"

#include <stdlib.h>
#include <string.h>

static int g_allocs;
static int g_releases;
static int g_fail_next;

static void *test_alloc(void *ctx, size_t n)
{
    (void)ctx;
    if (g_fail_next) {
        g_fail_next = 0;
        return NULL;
    }
    g_allocs++;
    return malloc(n);
}

static void test_release(void *ctx, void *p)
{
    (void)ctx;
    g_releases++;
    free(p);
}

static k32g_ops test_ops(void)
{
    k32g_ops o;
    o.alloc = test_alloc;
    o.release = test_release;
    o.ctx = NULL;
    return o;
}

static k32g_table T;

static void setup(void)
{
    g_allocs = 0;
    g_releases = 0;
    g_fail_next = 0;
    k32g_init(&T, test_ops());
}

static void teardown(void)
{
    k32g_reset(&T);
    MT_EXPECT_EQ(g_allocs, g_releases);
}

/* ---- Flag constants ----------------------------------------------------------- */

static void flag_constants(void)
{
    MT_EXPECT_EQ(K32G_GHND, 0x42);
    MT_EXPECT_EQ(K32G_GPTR, 0x40);
    MT_EXPECT_EQ(K32G_LHND, 0x42);
    MT_EXPECT_EQ(K32G_LPTR, 0x40);
    MT_EXPECT_EQ(K32G_GMEM_VALID_FLAGS, 0x7F72);
    MT_EXPECT_EQ(K32G_LMEM_DISCARDABLE, 0x0F00);
    MT_EXPECT_EQ(K32G_GMEM_DISCARDABLE, 0x0100);
}

static void local_flags_map(void)
{
    MT_EXPECT_EQ(k32g_local_to_global_flags(K32G_LHND), K32G_GHND);
    MT_EXPECT_EQ(k32g_local_to_global_flags(K32G_LMEM_DISCARDABLE), K32G_GMEM_DISCARDABLE);
    MT_EXPECT_EQ(k32g_local_to_global_flags(K32G_LMEM_FIXED | K32G_LMEM_ZEROINIT), K32G_GMEM_ZEROINIT);
}

/* ---- Allocation and lock ------------------------------------------------------ */

static void alloc_moveable_round_trip(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 16);
    MT_ASSERT(h != 0);
    MT_EXPECT_EQ(k32g_size(&T, h), 16);
    unsigned char *p = (unsigned char *)k32g_lock(&T, h);
    MT_ASSERT(p != NULL);
    memcpy(p, "clipboard-data!!", 16);
    MT_EXPECT_EQ(k32g_flags(&T, h), 1);
    MT_EXPECT_EQ(k32g_unlock(&T, h), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_SUCCESS);
    MT_EXPECT_EQ(k32g_flags(&T, h), 0);
    teardown();
}

static void zeroinit_ghnd(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GHND, 64);
    MT_ASSERT(h != 0);
    unsigned char *p = (unsigned char *)k32g_lock(&T, h);
    MT_ASSERT(p != NULL);
    int nonzero = 0;
    for (int i = 0; i < 64; ++i)
        nonzero |= p[i] != 0;
    MT_EXPECT_EQ(nonzero, 0);
    k32g_unlock(&T, h);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    teardown();
}

static void invalid_alloc_flags(void)
{
    setup();
    MT_EXPECT_EQ(k32g_alloc(&T, K32G_GMEM_MODIFY, 8), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_INVALID_PARAMETER);
    MT_EXPECT_EQ(k32g_alloc(&T, 0x8000u, 8), 0);
    MT_EXPECT_EQ(k32g_alloc(&T, 0x0001u, 8), 0);
    MT_EXPECT_EQ(g_allocs, 0);
    teardown();
}

static void oversize_alloc_fails(void)
{
    setup();
    MT_EXPECT_EQ(k32g_alloc(&T, K32G_GMEM_MOVEABLE, K32G_MAX_BYTES + 1), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_NOT_ENOUGH_MEMORY);
    teardown();
}

static void allocator_failure(void)
{
    setup();
    g_fail_next = 1;
    MT_EXPECT_EQ(k32g_alloc(&T, K32G_GMEM_MOVEABLE, 32), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_NOT_ENOUGH_MEMORY);
    MT_EXPECT_EQ(k32g_live_count(&T), 0);
    teardown();
}

static void zero_size_alloc(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 0);
    MT_ASSERT(h != 0);
    MT_EXPECT_EQ(k32g_size(&T, h), 0);
    MT_EXPECT(k32g_lock(&T, h) != NULL);
    MT_EXPECT_EQ(k32g_unlock(&T, h), 0);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    teardown();
}

static void lock_count_limit(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 4);
    MT_ASSERT(h != 0);
    for (unsigned i = 0; i < K32G_LOCK_MAX; ++i)
        MT_ASSERT(k32g_lock(&T, h) != NULL);
    MT_EXPECT_EQ(k32g_flags(&T, h), K32G_LOCK_MAX);
    MT_EXPECT(k32g_lock(&T, h) == NULL);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_NOT_ENOUGH_MEMORY);
    for (unsigned i = 0; i < K32G_LOCK_MAX - 1; ++i)
        MT_EXPECT_EQ(k32g_unlock(&T, h), 1);
    MT_EXPECT_EQ(k32g_unlock(&T, h), 0);
    MT_EXPECT_EQ(k32g_unlock(&T, h), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_NOT_LOCKED);
    teardown();
}

static void nested_lock_unlock(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 4);
    MT_ASSERT(h != 0);
    void *a = k32g_lock(&T, h);
    void *b = k32g_lock(&T, h);
    MT_EXPECT(a == b);
    MT_EXPECT_EQ(k32g_flags(&T, h), 2);
    MT_EXPECT_EQ(k32g_unlock(&T, h), 1);
    MT_EXPECT_EQ(k32g_unlock(&T, h), 0);
    teardown();
}

/* ---- Free, stale handles, and fixed blocks ------------------------------------ */

static void free_locked_refused(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 8);
    MT_ASSERT(h != 0);
    MT_ASSERT(k32g_lock(&T, h) != NULL);
    MT_EXPECT_EQ(k32g_free(&T, h), h);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_LOCK_FAILED);
    MT_EXPECT_EQ(k32g_size(&T, h), 8);
    k32g_unlock(&T, h);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    teardown();
}

static void stale_handle_rejected(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 8);
    MT_ASSERT(h != 0);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    MT_EXPECT_EQ(k32g_size(&T, h), 0);
    MT_EXPECT_EQ(k32g_flags(&T, h), K32G_GMEM_INVALID_HANDLE);
    MT_EXPECT(k32g_lock(&T, h) == NULL);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_INVALID_HANDLE);
    /* A new block in the same slot has a different handle. */
    uintptr_t h2 = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 8);
    MT_ASSERT(h2 != 0);
    MT_EXPECT(h2 != h);
    MT_EXPECT_EQ(k32g_size(&T, h), 0);
    MT_EXPECT_EQ(k32g_size(&T, h2), 8);
    MT_EXPECT_EQ(k32g_free(&T, h2), 0);
    teardown();
}

static void bogus_handles(void)
{
    setup();
    MT_EXPECT_EQ(k32g_size(&T, 0), 0);
    MT_EXPECT_EQ(k32g_free(&T, 0), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_INVALID_HANDLE);
    MT_EXPECT(k32g_lock(&T, 0x12345670u) == NULL);
    MT_EXPECT_EQ(k32g_unlock(&T, 0x12345670u), 0);
    MT_EXPECT_EQ(k32g_handle_valid(&T, 0x12345670u), 0);
    teardown();
}

static void fixed_blocks_use_pointer(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GPTR, 24);
    MT_ASSERT(h != 0);
    void *p = (void *)h;
    MT_EXPECT_EQ(k32g_size(&T, h), 24);
    MT_EXPECT((void *)k32g_lock(&T, h) == p);
    MT_EXPECT_EQ(k32g_flags(&T, h), 0);
    MT_EXPECT_EQ(k32g_handle_of_ptr(&T, p), h);
    MT_EXPECT_EQ(k32g_unlock(&T, h), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_NOT_LOCKED);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    MT_EXPECT_EQ(k32g_size(&T, h), 0);
    teardown();
}

static void handle_of_moveable_pointer(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 40);
    MT_ASSERT(h != 0);
    void *p = k32g_lock(&T, h);
    MT_ASSERT(p != NULL);
    MT_EXPECT_EQ(k32g_handle_of_ptr(&T, p), h);
    MT_EXPECT_EQ(k32g_handle_of_ptr(&T, (char *)p + 1), 0);
    k32g_unlock(&T, h);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    teardown();
}

/* ---- Realloc, modify, discard ------------------------------------------------- */

static void realloc_keeps_handle_and_contents(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 4);
    MT_ASSERT(h != 0);
    unsigned char *p = k32g_lock(&T, h);
    MT_ASSERT(p != NULL);
    memcpy(p, "abcd", 4);
    k32g_unlock(&T, h);
    uintptr_t h2 = k32g_realloc(&T, h, 10, K32G_GMEM_MOVEABLE | K32G_GMEM_ZEROINIT);
    MT_EXPECT_EQ(h2, h);
    MT_EXPECT_EQ(k32g_size(&T, h), 10);
    unsigned char *q = k32g_lock(&T, h);
    MT_ASSERT(q != NULL);
    MT_EXPECT(memcmp(q, "abcd", 4) == 0);
    MT_EXPECT_EQ(q[4], 0);
    MT_EXPECT_EQ(q[9], 0);
    k32g_unlock(&T, h);
    /* Shrinking keeps the prefix. */
    MT_EXPECT_EQ(k32g_realloc(&T, h, 2, K32G_GMEM_MOVEABLE), h);
    q = k32g_lock(&T, h);
    MT_EXPECT(memcmp(q, "ab", 2) == 0);
    k32g_unlock(&T, h);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    teardown();
}

static void realloc_locked_moveable_refused(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 4);
    MT_ASSERT(h != 0);
    MT_ASSERT(k32g_lock(&T, h) != NULL);
    MT_EXPECT_EQ(k32g_realloc(&T, h, 8, 0), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_LOCK_FAILED);
    k32g_unlock(&T, h);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    teardown();
}

static void realloc_fixed_new_pointer(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GPTR, 8);
    MT_ASSERT(h != 0);
    memcpy((void *)h, "12345678", 8);
    uintptr_t h2 = k32g_realloc(&T, h, 16, K32G_GMEM_ZEROINIT);
    MT_ASSERT(h2 != 0);
    MT_EXPECT(memcmp((void *)h2, "12345678", 8) == 0);
    MT_EXPECT_EQ(((unsigned char *)h2)[15], 0);
    MT_EXPECT_EQ(k32g_size(&T, h2), 16);
    MT_EXPECT_EQ(k32g_free(&T, h2), 0);
    teardown();
}

static void modify_changes_flags_only(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 12);
    MT_ASSERT(h != 0);
    MT_EXPECT_EQ(k32g_realloc(&T, h, 0, K32G_GMEM_MODIFY | K32G_GMEM_DISCARDABLE), h);
    MT_EXPECT_EQ(k32g_size(&T, h), 12);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    teardown();
}

static void discard_and_reuse(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, K32G_GMEM_MOVEABLE | K32G_GMEM_DISCARDABLE, 32);
    MT_ASSERT(h != 0);
    MT_EXPECT_EQ(k32g_discard(&T, h), 32);
    MT_EXPECT_EQ(k32g_size(&T, h), 0);
    MT_EXPECT(k32g_flags(&T, h) & K32G_GMEM_DISCARDED);
    MT_EXPECT(k32g_lock(&T, h) == NULL);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_NOT_ENOUGH_MEMORY);
    /* Realloc of a discarded block gives it storage again; the old bytes are gone. */
    MT_EXPECT_EQ(k32g_realloc(&T, h, 16, K32G_GMEM_MOVEABLE), h);
    MT_EXPECT_EQ(k32g_size(&T, h), 16);
    MT_EXPECT_EQ(k32g_flags(&T, h) & K32G_GMEM_DISCARDED, 0);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    teardown();
}

static void discard_rules(void)
{
    setup();
    uintptr_t plain = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 8);
    uintptr_t disc = k32g_alloc(&T, K32G_GMEM_MOVEABLE | K32G_GMEM_DISCARDABLE, 8);
    MT_ASSERT(plain != 0 && disc != 0);
    MT_EXPECT_EQ(k32g_discard(&T, plain), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_NOT_LOCKED);
    MT_ASSERT(k32g_lock(&T, disc) != NULL);
    MT_EXPECT_EQ(k32g_discard(&T, disc), 0);
    k32g_unlock(&T, disc);
    MT_EXPECT_EQ(k32g_free(&T, plain), 0);
    MT_EXPECT_EQ(k32g_free(&T, disc), 0);
    teardown();
}

static void table_capacity(void)
{
    setup();
    uintptr_t hs[K32G_MAX_BLOCKS];
    for (unsigned i = 0; i < K32G_MAX_BLOCKS; ++i) {
        hs[i] = k32g_alloc(&T, K32G_GMEM_MOVEABLE, 1);
        MT_ASSERT(hs[i] != 0);
    }
    MT_EXPECT_EQ(k32g_live_count(&T), K32G_MAX_BLOCKS);
    MT_EXPECT_EQ(k32g_alloc(&T, K32G_GMEM_MOVEABLE, 1), 0);
    MT_EXPECT_EQ(k32g_get_last_error(&T), K32G_ERROR_NOT_ENOUGH_MEMORY);
    /* All handles are distinct. */
    for (unsigned i = 0; i < K32G_MAX_BLOCKS; ++i)
        for (unsigned j = i + 1; j < K32G_MAX_BLOCKS; ++j)
            MT_ASSERT(hs[i] != hs[j]);
    for (unsigned i = 0; i < K32G_MAX_BLOCKS; ++i)
        MT_EXPECT_EQ(k32g_free(&T, hs[i]), 0);
    MT_EXPECT_EQ(k32g_live_count(&T), 0);
    teardown();
}

static void reset_releases_everything(void)
{
    setup();
    k32g_alloc(&T, K32G_GMEM_MOVEABLE, 8);
    k32g_alloc(&T, K32G_GPTR, 8);
    k32g_alloc(&T, K32G_GMEM_MOVEABLE, 8);
    MT_EXPECT_EQ(g_allocs, 3);
    k32g_reset(&T);
    MT_EXPECT_EQ(g_releases, 3);
    MT_EXPECT_EQ(k32g_live_count(&T), 0);
    g_allocs = 0;
    g_releases = 0;
}

static void local_api_uses_same_table(void)
{
    setup();
    uintptr_t h = k32g_alloc(&T, k32g_local_to_global_flags(K32G_LHND), 20);
    MT_ASSERT(h != 0);
    MT_EXPECT_EQ(k32g_size(&T, h), 20);
    MT_EXPECT_EQ(k32g_free(&T, h), 0);
    teardown();
}

static const mt_case_t cases[] = {
    MT_CASE(flag_constants),
    MT_CASE(local_flags_map),
    MT_CASE(alloc_moveable_round_trip),
    MT_CASE(zeroinit_ghnd),
    MT_CASE(invalid_alloc_flags),
    MT_CASE(oversize_alloc_fails),
    MT_CASE(allocator_failure),
    MT_CASE(zero_size_alloc),
    MT_CASE(lock_count_limit),
    MT_CASE(nested_lock_unlock),
    MT_CASE(free_locked_refused),
    MT_CASE(stale_handle_rejected),
    MT_CASE(bogus_handles),
    MT_CASE(fixed_blocks_use_pointer),
    MT_CASE(handle_of_moveable_pointer),
    MT_CASE(realloc_keeps_handle_and_contents),
    MT_CASE(realloc_locked_moveable_refused),
    MT_CASE(realloc_fixed_new_pointer),
    MT_CASE(modify_changes_flags_only),
    MT_CASE(discard_and_reuse),
    MT_CASE(discard_rules),
    MT_CASE(table_capacity),
    MT_CASE(reset_releases_everything),
    MT_CASE(local_api_uses_same_table),
};

int main(void)
{
    return MT_RUN("k32_global_pure", cases);
}
