/*
 * test_stdlib.c - tests for src/crt/ucrt_stdlib.h: the Microsoft rand sequence, qsort and bsearch against
 * the properties of a sort (sorted, a permutation of the input) on many shapes of input, qsort_s with context,
 * the exit tables, div and abs, byte swapping.
 */
#include "minitest.h"
#include "ucrt_stdlib.h"

#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0x5851F42D4C957F2Dull;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void rand_sequence_matches_msvc(void)
{
    static const int expected[10] = {41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464};
    int i;
    ucrt_srand(1); /* the initial state */
    for (i = 0; i < 10; ++i)
        MT_EXPECT_EQ(ucrt_rand(), expected[i]);
    /* A different seed gives a different, reproducible sequence. */
    ucrt_srand(12345);
    {
        int a = ucrt_rand(), b = ucrt_rand();
        ucrt_srand(12345);
        MT_EXPECT_EQ(ucrt_rand(), a);
        MT_EXPECT_EQ(ucrt_rand(), b);
    }
    /* Every value is below RAND_MAX + 1. */
    ucrt_srand(7);
    for (i = 0; i < 100000; ++i)
        MT_ASSERT((unsigned)ucrt_rand() <= UCRT_RAND_MAX);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static int cmp_int_ctx(void *ctx, const void *a, const void *b)
{
    int dir = *(int *)ctx;
    return dir * cmp_int(a, b);
}

static int is_sorted(const int *v, size_t n)
{
    for (size_t i = 1; i < n; ++i)
        if (v[i - 1] > v[i])
            return 0;
    return 1;
}

static void qsort_many_shapes(void)
{
    enum { MAXN = 700 };
    int a[MAXN], b[MAXN];
    int shape, trial;
    for (trial = 0; trial < 3000; ++trial) {
        size_t n = (size_t)(rnd64() % MAXN), i;
        shape = (int)(rnd64() % 8);
        for (i = 0; i < n; ++i) {
            switch (shape) {
            case 0: a[i] = (int)rnd64(); break;                       /* random */
            case 1: a[i] = (int)i; break;                              /* already sorted */
            case 2: a[i] = (int)(n - i); break;                        /* reversed */
            case 3: a[i] = (int)(rnd64() % 4); break;                  /* many duplicates */
            case 4: a[i] = 7; break;                                   /* all equal */
            case 5: a[i] = (int)((i % 2) ? i : n - i); break;          /* organ pipe-ish */
            case 6: a[i] = (int)(i < n / 2 ? i : n - i); break;        /* mountain */
            default: a[i] = (int)(i ^ (i >> 3)) * 7919 % 1000; break;
            }
        }
        memcpy(b, a, n * sizeof(int));
        ucrt_qsort(a, n, sizeof(int), cmp_int);
        qsort(b, n, sizeof(int), cmp_int);
        if (!is_sorted(a, n) || memcmp(a, b, n * sizeof(int)) != 0) { /* sorted and the same multiset as glibc's result */
            MT_EXPECT(0);
            return;
        }
    }
}

typedef struct rec {
    int key;
    char payload[11];
} rec;

static int cmp_rec(const void *a, const void *b)
{
    return ((const rec *)a)->key - ((const rec *)b)->key;
}

static void qsort_moves_whole_elements(void)
{
    enum { N = 500 };
    rec r[N];
    int i;
    for (i = 0; i < N; ++i) {
        r[i].key = (int)(rnd64() % 100000);
        memset(r[i].payload, (char)(r[i].key & 0x7F), sizeof(r[i].payload));
    }
    ucrt_qsort(r, N, sizeof(rec), cmp_rec);
    for (i = 0; i < N; ++i) {
        MT_ASSERT(i == 0 || r[i - 1].key <= r[i].key);
        for (size_t k = 0; k < sizeof(r[i].payload); ++k)
            MT_ASSERT(r[i].payload[k] == (char)(r[i].key & 0x7F)); /* the payload travelled with its key */
    }
}

static void qsort_s_with_context(void)
{
    int a[200], dir = -1, i;
    for (i = 0; i < 200; ++i)
        a[i] = (int)(rnd64() % 1000);
    ucrt_qsort_s(a, 200, sizeof(int), cmp_int_ctx, &dir);
    for (i = 1; i < 200; ++i)
        MT_ASSERT(a[i - 1] >= a[i]); /* descending */
    /* Bad arguments are invalid parameters, not crashes. */
    g_ucrt_state.invalid_count = 0;
    ucrt_qsort_s(NULL, 5, 4, cmp_int_ctx, &dir);
    ucrt_qsort_s(a, 5, 0, cmp_int_ctx, &dir);
    ucrt_qsort_s(a, 5, 4, NULL, &dir);
    MT_EXPECT_EQ(g_ucrt_state.invalid_count, 3);
    ucrt_qsort_s(NULL, 0, 4, NULL, &dir); /* nothing to sort is fine */
    MT_EXPECT_EQ(g_ucrt_state.invalid_count, 3);
}

/* A comparison that is a total order but adversarial for naive pivots: the sort must stay fast. */
static void qsort_worst_case_is_bounded(void)
{
    enum { N = 60000 };
    static int a[N];
    int i;
    for (i = 0; i < N; ++i)
        a[i] = (i % 2) ? i : N - i; /* the classic median-of-three killer shape */
    ucrt_qsort(a, N, sizeof(int), cmp_int);
    MT_EXPECT(is_sorted(a, N));
}

static void bsearch_finds_and_misses(void)
{
    int v[1000], i;
    for (i = 0; i < 1000; ++i)
        v[i] = i * 3;
    for (i = 0; i < 1000; ++i) {
        int key = i * 3;
        int *p = (int *)ucrt_bsearch(&key, v, 1000, sizeof(int), cmp_int);
        MT_ASSERT(p == &v[i]);
    }
    for (i = -5; i < 3010; ++i) {
        int *p = (int *)ucrt_bsearch(&i, v, 1000, sizeof(int), cmp_int);
        int hit = (i >= 0 && i % 3 == 0 && i < 3000);
        MT_ASSERT((p != NULL) == hit);
    }
    MT_EXPECT(ucrt_bsearch(&i, v, 0, sizeof(int), cmp_int) == NULL);
    {
        int dir = 1, key = 30;
        MT_EXPECT(ucrt_bsearch_s(&key, v, 1000, sizeof(int), cmp_int_ctx, &dir) == &v[10]);
    }
}

static void linear_searches(void)
{
    int v[8] = {5, 6, 7, 0, 0, 0, 0, 0};
    unsigned count = 3;
    int key = 6;
    MT_EXPECT(ucrt_lfind(&key, v, &count, sizeof(int), cmp_int) == &v[1]);
    key = 9;
    MT_EXPECT(ucrt_lfind(&key, v, &count, sizeof(int), cmp_int) == NULL);
    MT_EXPECT_EQ(count, 3);
    MT_EXPECT(ucrt_lsearch(&key, v, &count, sizeof(int), cmp_int) == &v[3]); /* appended */
    MT_EXPECT_EQ(count, 4);
    MT_EXPECT_EQ(v[3], 9);
    MT_EXPECT(ucrt_lsearch(&key, v, &count, sizeof(int), cmp_int) == &v[3]); /* found now */
    MT_EXPECT_EQ(count, 4);
}

static int order[32], order_n;
static ucrt_onexit_table table;

static void h1(void) { order[order_n++] = 1; }
static void h2(void) { order[order_n++] = 2; }
static void h3(void) { order[order_n++] = 3; }
static void h_registers_more(void)
{
    order[order_n++] = 9;
    ucrt_register_onexit(&table, h1, realloc);
}

static void onexit_runs_in_reverse(void)
{
    order_n = 0;
    MT_EXPECT_EQ(ucrt_initialize_onexit_table(&table), 0);
    MT_EXPECT_EQ(ucrt_register_onexit(&table, h1, realloc), 0);
    MT_EXPECT_EQ(ucrt_register_onexit(&table, h2, realloc), 0);
    MT_EXPECT_EQ(ucrt_register_onexit(&table, h3, realloc), 0);
    for (int i = 0; i < 20; ++i) /* growth beyond the first allocation */
        ucrt_register_onexit(&table, h1, realloc);
    MT_EXPECT_EQ(ucrt_execute_onexit(&table, free), 0);
    MT_EXPECT_EQ(order_n, 23);
    MT_EXPECT(order[0] == 1 && order[19] == 1 && order[20] == 3 && order[21] == 2 && order[22] == 1);
    MT_EXPECT(table.first == NULL);
    /* A handler that registers another: the new one runs too, first. */
    order_n = 0;
    ucrt_register_onexit(&table, h2, realloc);
    ucrt_register_onexit(&table, h_registers_more, realloc);
    ucrt_execute_onexit(&table, free);
    MT_EXPECT_EQ(order_n, 3);
    MT_EXPECT(order[0] == 9 && order[1] == 1 && order[2] == 2);
    MT_EXPECT_EQ(ucrt_initialize_onexit_table(NULL), -1);
    MT_EXPECT_EQ(ucrt_register_onexit(NULL, h1, realloc), -1);
}

static void div_and_abs(void)
{
    ucrt_div_t d = ucrt_div(-7, 2);
    ucrt_ldiv_t l = ucrt_ldiv(7, -2);
    ucrt_lldiv_t ll = ucrt_lldiv(-9000000000ll, 7);
    MT_EXPECT(d.quot == -3 && d.rem == -1); /* truncation toward zero */
    MT_EXPECT(l.quot == -3 && l.rem == 1);
    MT_EXPECT(ll.quot == -1285714285 && ll.rem == -5);
    MT_EXPECT(ucrt_abs(-5) == 5 && ucrt_abs(5) == 5 && ucrt_labs(-70000) == 70000 && ucrt_llabs(-5000000000ll) == 5000000000ll);
}

static void byte_swaps(void)
{
    char out[8];
    MT_EXPECT_EQ(ucrt_byteswap_ushort(0x1234), 0x3412);
    MT_EXPECT_EQ(ucrt_byteswap_ulong(0x12345678u), 0x78563412u);
    MT_EXPECT(ucrt_byteswap_uint64(0x0102030405060708ull) == 0x0807060504030201ull);
    memset(out, 'x', sizeof(out));
    ucrt_swab("abcdef", out, 5); /* an odd count drops the last byte */
    MT_EXPECT(memcmp(out, "badcxxxx", 8) == 0);
    ucrt_swab("abcdef", out, -3);
    MT_EXPECT(memcmp(out, "badcxxxx", 8) == 0);
}

static const mt_case_t cases[] = {
    MT_CASE(rand_sequence_matches_msvc),
    MT_CASE(qsort_many_shapes),
    MT_CASE(qsort_moves_whole_elements),
    MT_CASE(qsort_s_with_context),
    MT_CASE(qsort_worst_case_is_bounded),
    MT_CASE(bsearch_finds_and_misses),
    MT_CASE(linear_searches),
    MT_CASE(onexit_runs_in_reverse),
    MT_CASE(div_and_abs),
    MT_CASE(byte_swaps),
};

int main(void)
{
    return MT_RUN("crt_stdlib", cases);
}
