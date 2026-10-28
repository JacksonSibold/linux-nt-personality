/*
 * test_k32_y_pure.c - tests for src/ntdll/k32_y_pure.h: pointer encoding, VerifyVersionInfo condition logic and the
 * logical processor information records.
 */
#include "minitest.h"
#include "k32_y_pure.h"

static void pointer_encoding_round_trips(void)
{
    static const uint64_t cookies[] = {0, 1, 0x123456789ABCDEF0ull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000040ull};
    static const uint64_t ptrs[] = {0, 1, 0x7FFF12345678ull, 0xFFFFFFFFFFFFFFFFull, 0x140001000ull};
    for (size_t i = 0; i < sizeof cookies / sizeof cookies[0]; ++i)
        for (size_t j = 0; j < sizeof ptrs / sizeof ptrs[0]; ++j) {
            uint64_t e = k32y_encode_pointer(ptrs[j], cookies[i]);
            MT_EXPECT(k32y_decode_pointer(e, cookies[i]) == ptrs[j]);
            if (cookies[i] && ptrs[j])
                MT_EXPECT(e != ptrs[j] || (cookies[i] & 63) == 0);
        }
    MT_EXPECT(k32y_encode_pointer(0x1000, 0x1234) != k32y_encode_pointer(0x1000, 0x1235));
}

static void condition_mask_packs_three_bits_per_field(void)
{
    uint64_t m = 0;
    m = k32y_set_condition(m, K32Y_VER_MAJORVERSION, K32Y_VER_GREATER_EQUAL);
    MT_EXPECT_EQ((long)m, 3L << 3);
    m = k32y_set_condition(m, K32Y_VER_MINORVERSION | K32Y_VER_BUILDNUMBER, K32Y_VER_EQUAL);
    MT_EXPECT_EQ((long)k32y_condition_of(m, 0), 1);
    MT_EXPECT_EQ((long)k32y_condition_of(m, 1), 3);
    MT_EXPECT_EQ((long)k32y_condition_of(m, 2), 1);
    m = k32y_set_condition(m, K32Y_VER_MAJORVERSION, K32Y_VER_LESS); /* a later call replaces the field's condition */
    MT_EXPECT_EQ((long)k32y_condition_of(m, 1), 4);
    MT_EXPECT(k32y_set_condition(m, K32Y_VER_BUILDNUMBER, 0) == m);
}

static const k32y_version win10 = {10, 0, 26200, 2, 0, 0, 0, 1};

static int verify(uint32_t type, uint64_t mask, k32y_version w)
{
    return k32y_verify(&win10, &w, type, mask);
}

static void version_checks_follow_the_hierarchy(void)
{
    k32y_version w = {6, 1, 0, 0, 0, 0, 0, 0};
    uint64_t ge = k32y_set_condition(k32y_set_condition(0, K32Y_VER_MAJORVERSION, K32Y_VER_GREATER_EQUAL), K32Y_VER_MINORVERSION,
                                     K32Y_VER_GREATER_EQUAL);
    /* "Windows 7 or later": 10.0 is later than 6.1 although 0 < 1 */
    MT_EXPECT_EQ(verify(K32Y_VER_MAJORVERSION | K32Y_VER_MINORVERSION, ge, w), 1);
    w.major = 10;
    w.minor = 1; /* 10.1: the major ties, so the minor decides */
    MT_EXPECT_EQ(verify(K32Y_VER_MAJORVERSION | K32Y_VER_MINORVERSION, ge, w), 0);
    w.minor = 0;
    MT_EXPECT_EQ(verify(K32Y_VER_MAJORVERSION | K32Y_VER_MINORVERSION, ge, w), 1);
    w.major = 11;
    MT_EXPECT_EQ(verify(K32Y_VER_MAJORVERSION | K32Y_VER_MINORVERSION, ge, w), 0);
    /* equality on the major demands exactly it */
    w.major = 10;
    MT_EXPECT_EQ(verify(K32Y_VER_MAJORVERSION, k32y_set_condition(0, K32Y_VER_MAJORVERSION, K32Y_VER_EQUAL), w), 1);
    w.major = 6;
    MT_EXPECT_EQ(verify(K32Y_VER_MAJORVERSION, k32y_set_condition(0, K32Y_VER_MAJORVERSION, K32Y_VER_EQUAL), w), 0);
}

static void version_checks_build_platform_suite_product(void)
{
    k32y_version w = {0};
    uint64_t m;
    w.build = 19045;
    m = k32y_set_condition(0, K32Y_VER_BUILDNUMBER, K32Y_VER_GREATER_EQUAL);
    MT_EXPECT_EQ(verify(K32Y_VER_BUILDNUMBER, m, w), 1);
    w.build = 30000;
    MT_EXPECT_EQ(verify(K32Y_VER_BUILDNUMBER, m, w), 0);
    w.platform = 2;
    m = k32y_set_condition(0, K32Y_VER_PLATFORMID, K32Y_VER_EQUAL);
    MT_EXPECT_EQ(verify(K32Y_VER_PLATFORMID, m, w), 1);
    w.product_type = 1;
    m = k32y_set_condition(0, K32Y_VER_PRODUCT_TYPE, K32Y_VER_EQUAL);
    MT_EXPECT_EQ(verify(K32Y_VER_PRODUCT_TYPE, m, w), 1);
    w.product_type = 3;
    MT_EXPECT_EQ(verify(K32Y_VER_PRODUCT_TYPE, m, w), 0);
    {
        k32y_version have = win10;
        have.suite = 0x0110;
        w.suite = 0x0010;
        m = k32y_set_condition(0, K32Y_VER_SUITENAME, K32Y_VER_AND);
        MT_EXPECT_EQ(k32y_verify(&have, &w, K32Y_VER_SUITENAME, m), 1);
        w.suite = 0x0210;
        MT_EXPECT_EQ(k32y_verify(&have, &w, K32Y_VER_SUITENAME, m), 0);
        m = k32y_set_condition(0, K32Y_VER_SUITENAME, K32Y_VER_OR);
        MT_EXPECT_EQ(k32y_verify(&have, &w, K32Y_VER_SUITENAME, m), 1);
    }
    /* a condition that makes no sense for the field is an error */
    MT_EXPECT_EQ(verify(K32Y_VER_MAJORVERSION, k32y_set_condition(0, K32Y_VER_MAJORVERSION, K32Y_VER_AND), w), -1);
    MT_EXPECT_EQ(verify(K32Y_VER_MAJORVERSION, 0, w), -1);
}

static const k32y_cache caches[] = {
    {1, 8, 64, 32768, K32Y_CACHE_DATA, 2},
    {1, 8, 64, 32768, K32Y_CACHE_INSTRUCTION, 2},
    {2, 8, 64, 262144, K32Y_CACHE_UNIFIED, 2},
    {3, 16, 64, 8388608, K32Y_CACHE_UNIFIED, 8},
};

static void legacy_records_describe_cores_caches_node_package(void)
{
    k32y_topology t = {8, 2, caches, 4};
    size_t n = k32y_build_legacy(&t, NULL, 0);
    k32y_slpi recs[64];
    size_t cores = 0, l1 = 0, l2 = 0, l3 = 0, nodes = 0, pkgs = 0;
    MT_EXPECT_EQ((long)sizeof(k32y_slpi), 32L);
    MT_ASSERT(n <= 64);
    MT_EXPECT(k32y_build_legacy(&t, recs, 64) == n);
    for (size_t i = 0; i < n; ++i) {
        switch (recs[i].relationship) {
        case K32Y_RELATION_PROCESSOR_CORE:
            ++cores;
            MT_EXPECT_EQ((long)recs[i].u.core.flags, 1L);
            MT_EXPECT(__builtin_popcountll(recs[i].mask) == 2);
            break;
        case K32Y_RELATION_CACHE:
            if (recs[i].u.cache.level == 1) ++l1;
            if (recs[i].u.cache.level == 2) ++l2;
            if (recs[i].u.cache.level == 3) {
                ++l3;
                MT_EXPECT_EQ((long)recs[i].mask, 0xFFL); /* shared by all 8 logical processors */
            }
            break;
        case K32Y_RELATION_NUMA_NODE: ++nodes; MT_EXPECT_EQ((long)recs[i].mask, 0xFFL); break;
        case K32Y_RELATION_PROCESSOR_PACKAGE: ++pkgs; break;
        }
    }
    MT_EXPECT_EQ((long)cores, 4L);
    MT_EXPECT_EQ((long)l1, 8L);
    MT_EXPECT_EQ((long)l2, 4L);
    MT_EXPECT_EQ((long)l3, 1L);
    MT_EXPECT_EQ((long)nodes, 1L);
    MT_EXPECT_EQ((long)pkgs, 1L);
}

static void legacy_count_does_not_overrun_a_small_buffer(void)
{
    k32y_topology t = {4, 1, caches, 4};
    k32y_slpi recs[3];
    size_t n = k32y_build_legacy(&t, recs, 3);
    MT_EXPECT(n > 3);
    MT_EXPECT_EQ((long)recs[0].relationship, 0L);
}

static void extended_records_have_the_documented_sizes(void)
{
    k32y_topology t = {4, 2, caches, 4};
    uint8_t buf[2048];
    unsigned nrec = 0;
    size_t need = k32y_build_ex(&t, K32Y_RELATION_ALL, NULL, 0, &nrec);
    size_t off = 0;
    unsigned seen = 0, cores = 0, groups = 0;
    MT_ASSERT(need <= sizeof buf);
    MT_EXPECT(k32y_build_ex(&t, K32Y_RELATION_ALL, buf, sizeof buf, &nrec) == need);
    while (off < need) {
        uint32_t rel, size;
        memcpy(&rel, buf + off, 4);
        memcpy(&size, buf + off + 4, 4);
        MT_ASSERT(size >= 8 && off + size <= need);
        if (rel == K32Y_RELATION_PROCESSOR_CORE) {
            uint64_t mask;
            ++cores;
            MT_EXPECT_EQ((long)size, (long)K32Y_EX_PROCESSOR_SIZE);
            memcpy(&mask, buf + off + 8 + 24, 8);
            MT_EXPECT(__builtin_popcountll(mask) == 2);
        }
        if (rel == K32Y_RELATION_GROUP) {
            ++groups;
            MT_EXPECT_EQ((long)buf[off + 8 + 25], 4L);
        }
        off += size;
        ++seen;
    }
    MT_EXPECT_EQ((long)seen, (long)nrec);
    MT_EXPECT_EQ((long)cores, 2L);
    MT_EXPECT_EQ((long)groups, 1L);
    /* one relationship only */
    MT_EXPECT(k32y_build_ex(&t, K32Y_RELATION_NUMA_NODE, NULL, 0, &nrec) == K32Y_EX_NUMA_SIZE && nrec == 1);
}

static const mt_case_t cases[] = {
    MT_CASE(pointer_encoding_round_trips), MT_CASE(condition_mask_packs_three_bits_per_field),
    MT_CASE(version_checks_follow_the_hierarchy), MT_CASE(version_checks_build_platform_suite_product),
    MT_CASE(legacy_records_describe_cores_caches_node_package), MT_CASE(legacy_count_does_not_overrun_a_small_buffer),
    MT_CASE(extended_records_have_the_documented_sizes),
};

int main(void)
{
    return MT_RUN("k32_y_pure", cases);
}
