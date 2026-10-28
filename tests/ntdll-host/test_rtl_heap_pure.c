/*
 * test_rtl_heap_pure.c - host tests for src/ntdll/rtl_heap_pure.h: allocation, free and
 * coalescing, alignment, HeapSize, realloc in place and by moving, zeroing, large dedicated
 * segments, fixed-size heaps, double free and bad pointers, validation, walking, and a
 * randomised stress run checked against a shadow of every live block.
 */
#include "minitest.h"
#include "rtl_heap_pure.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

static int seg_allocs, seg_frees;
static size_t seg_limit; /* 0: no limit on the bytes the fake OS hands out */
static size_t seg_out;

static void *os_alloc(size_t size)
{
    void *p;
    size = rh_round_up(size, 4096);
    if (seg_limit && seg_out + size > seg_limit)
        return NULL;
    p = aligned_alloc(4096, size);
    if (p) {
        ++seg_allocs;
        seg_out += size;
        memset(p, 0xEE, size); /* not zeroed: the heap must not assume fresh pages */
    }
    return p;
}

static void os_free(void *base, size_t size)
{
    ++seg_frees;
    seg_out -= rh_round_up(size, 4096);
    free(base);
}

static void os_lock(volatile long *w)
{
    while (__sync_lock_test_and_set(w, 1))
        sched_yield();
}

static void os_unlock(volatile long *w)
{
    __sync_lock_release(w);
}

static const rh_os OS = {os_alloc, os_free, os_lock, os_unlock};

static void reset_os(void)
{
    seg_allocs = seg_frees = 0;
    seg_limit = 0;
    seg_out = 0;
}

static rh_heap *make(uint32_t options, size_t size, size_t max)
{
    reset_os();
    return rh_create(&OS, options, size, max);
}

static void alloc_basics(void)
{
    rh_heap *h = make(0, 0, 0);
    void *a, *b, *c;
    MT_ASSERT(h != NULL);
    a = rh_alloc(h, 1, 0);
    b = rh_alloc(h, 100, 0);
    c = rh_alloc(h, 4096, 0);
    MT_ASSERT(a && b && c);
    /* 16-byte alignment, distinct, non-overlapping. */
    MT_EXPECT_EQ((uintptr_t)a % 16, 0);
    MT_EXPECT_EQ((uintptr_t)b % 16, 0);
    MT_EXPECT_EQ((uintptr_t)c % 16, 0);
    MT_EXPECT(a != b && b != c);
    memset(a, 1, 1);
    memset(b, 2, 100);
    memset(c, 3, 4096);
    MT_EXPECT_EQ(rh_size(h, a), 1);
    MT_EXPECT_EQ(rh_size(h, b), 100);
    MT_EXPECT_EQ(rh_size(h, c), 4096);
    MT_EXPECT(rh_validate(h, NULL));
    MT_EXPECT(rh_free(h, b));
    MT_EXPECT(rh_free(h, a));
    MT_EXPECT(rh_free(h, c));
    MT_EXPECT(rh_validate(h, NULL));
    MT_EXPECT_EQ(h->in_use, 0);
    MT_EXPECT_EQ(h->alloc_count, 0);
    rh_destroy(h);
    MT_EXPECT_EQ(seg_allocs, seg_frees);
    MT_EXPECT_EQ(seg_out, 0);
}

static void zero_size_and_zero_memory(void)
{
    rh_heap *h = make(0, 0, 0);
    uint8_t *p, *q;
    size_t i;
    MT_ASSERT(h != NULL);
    p = rh_alloc(h, 0, 0); /* a zero-byte block is a real, distinct block */
    q = rh_alloc(h, 0, 0);
    MT_ASSERT(p && q && p != q);
    MT_EXPECT_EQ(rh_size(h, p), 0);
    rh_free(h, p);
    rh_free(h, q);
    /* HEAP_ZERO_MEMORY clears reused chunks too. */
    p = rh_alloc(h, 500, 0);
    memset(p, 0xAB, 500);
    rh_free(h, p);
    q = rh_alloc(h, 500, RH_ZERO_MEMORY);
    MT_ASSERT(q != NULL);
    for (i = 0; i < 500; ++i)
        if (q[i]) {
            MT_EXPECT_EQ(q[i], 0);
            break;
        }
    rh_destroy(h);
}

static void coalescing_restores_one_chunk(void)
{
    rh_heap *h = make(0, 0, 0);
    void *p[64];
    size_t free_bytes_before, free_bytes_after;
    int i;
    MT_ASSERT(h != NULL);
    free_bytes_before = h->committed - h->in_use;
    for (i = 0; i < 64; ++i)
        p[i] = rh_alloc(h, 24 + (size_t)i * 8, 0);
    /* Free in an order that exercises coalescing with the next, the previous and both neighbours. */
    for (i = 0; i < 64; i += 2)
        rh_free(h, p[i]);
    MT_EXPECT(rh_validate(h, NULL));
    for (i = 1; i < 64; i += 2)
        rh_free(h, p[i]);
    MT_EXPECT(rh_validate(h, NULL));
    free_bytes_after = h->committed - h->in_use;
    MT_EXPECT_EQ(free_bytes_after, free_bytes_before);
    /* The heap is one free chunk again: a block of nearly the whole segment fits. */
    p[0] = rh_alloc(h, RH_LARGE - 4096, 0);
    MT_EXPECT(p[0] != NULL);
    MT_EXPECT_EQ(h->seg_count, 1);
    rh_destroy(h);
}

static void reuse_after_free(void)
{
    rh_heap *h = make(0, 0, 0);
    void *a, *b;
    MT_ASSERT(h != NULL);
    a = rh_alloc(h, 200, 0);
    rh_alloc(h, 200, 0); /* keeps a from coalescing with the rest of the segment */
    rh_free(h, a);
    b = rh_alloc(h, 200, 0);
    MT_EXPECT(a == b); /* the exact-size bin gives the freed chunk back */
    rh_destroy(h);
}

static void double_free_and_bad_pointers(void)
{
    rh_heap *h = make(0, 0, 0);
    void *a, *other;
    int on_stack = 0;
    MT_ASSERT(h != NULL);
    a = rh_alloc(h, 64, 0);
    rh_alloc(h, 64, 0);
    MT_EXPECT(rh_free(h, a));
    MT_EXPECT(!rh_free(h, a));          /* double free */
    MT_EXPECT(!rh_free(h, NULL));
    MT_EXPECT(!rh_free(h, &on_stack));  /* not in the heap */
    MT_EXPECT(!rh_free(h, (uint8_t *)h + 8));
    other = rh_alloc(h, 64, 0);
    MT_EXPECT(!rh_free(h, (uint8_t *)other + 16)); /* inside a block */
    MT_EXPECT_EQ(rh_size(h, &on_stack), (size_t)-1);
    MT_EXPECT_EQ(rh_size(h, NULL), (size_t)-1);
    MT_EXPECT(rh_realloc(h, &on_stack, 10, 0) == NULL);
    MT_EXPECT(rh_validate(h, NULL));
    MT_EXPECT(!rh_validate(h, &on_stack));
    MT_EXPECT(rh_validate(h, other));
    rh_destroy(h);
}

static void realloc_in_place_and_moving(void)
{
    rh_heap *h = make(0, 0, 0);
    uint8_t *a, *b, *c;
    size_t i;
    MT_ASSERT(h != NULL);
    a = rh_alloc(h, 100, 0);
    memset(a, 0x5A, 100);
    /* Growing into the free tail of the segment stays in place. */
    b = rh_realloc(h, a, 5000, 0);
    MT_ASSERT(b != NULL);
    MT_EXPECT(b == a);
    MT_EXPECT_EQ(rh_size(h, b), 5000);
    for (i = 0; i < 100; ++i)
        if (b[i] != 0x5A) {
            MT_EXPECT_EQ(b[i], 0x5A);
            break;
        }
    /* Shrinking keeps the pointer and gives the tail back. */
    c = rh_realloc(h, b, 40, 0);
    MT_EXPECT(c == b);
    MT_EXPECT_EQ(rh_size(h, c), 40);
    MT_EXPECT(rh_validate(h, NULL));
    /* A block with a live neighbour must move; the contents move with it. */
    {
        uint8_t *x = rh_alloc(h, 64, 0), *y = rh_alloc(h, 64, 0), *z;
        memset(x, 0x11, 64);
        z = rh_realloc(h, x, 4000, 0);
        MT_ASSERT(z != NULL);
        MT_EXPECT(z != x);
        for (i = 0; i < 64; ++i)
            if (z[i] != 0x11) {
                MT_EXPECT_EQ(z[i], 0x11);
                break;
            }
        MT_EXPECT_EQ(rh_size(h, z), 4000);
        MT_EXPECT(!rh_free(h, x)); /* the old pointer is gone */
        (void)y;
        /* HEAP_REALLOC_IN_PLACE_ONLY refuses a move and leaves the block alone. */
        MT_EXPECT(rh_realloc(h, y, 100000, RH_REALLOC_IN_PLACE_ONLY) == NULL);
        MT_EXPECT_EQ(rh_size(h, y), 64);
    }
    MT_EXPECT(rh_validate(h, NULL));
    rh_destroy(h);
}

static void realloc_zero_memory(void)
{
    rh_heap *h = make(0, 0, 0);
    uint8_t *a, *b;
    size_t i;
    MT_ASSERT(h != NULL);
    a = rh_alloc(h, 20, 0);
    memset(a, 0xCC, 20);
    b = rh_realloc(h, a, 300, RH_ZERO_MEMORY); /* in place into the free tail */
    MT_ASSERT(b != NULL);
    for (i = 0; i < 20; ++i)
        MT_EXPECT_EQ(b[i], 0xCC);
    for (i = 20; i < 300; ++i)
        if (b[i]) {
            MT_EXPECT_EQ(b[i], 0);
            break;
        }
    /* And when the block has to move. */
    {
        uint8_t *x = rh_alloc(h, 32, 0), *y;
        memset(x, 0xDD, 32);
        rh_alloc(h, 32, 0);
        y = rh_realloc(h, x, 600, RH_ZERO_MEMORY);
        MT_ASSERT(y != NULL);
        for (i = 0; i < 32; ++i)
            MT_EXPECT_EQ(y[i], 0xDD);
        for (i = 32; i < 600; ++i)
            if (y[i]) {
                MT_EXPECT_EQ(y[i], 0);
                break;
            }
    }
    rh_destroy(h);
}

static void large_blocks_get_dedicated_segments(void)
{
    rh_heap *h = make(0, 0, 0);
    uint8_t *big, *big2;
    int allocs_before;
    MT_ASSERT(h != NULL);
    allocs_before = seg_allocs;
    big = rh_alloc(h, 3 * 1024 * 1024, 0);
    MT_ASSERT(big != NULL);
    MT_EXPECT_EQ(seg_allocs, allocs_before + 1);
    MT_EXPECT_EQ(h->seg_count, 2);
    memset(big, 7, 3 * 1024 * 1024);
    big2 = rh_alloc(h, RH_LARGE, 0);
    MT_ASSERT(big2 != NULL);
    MT_EXPECT_EQ(h->seg_count, 3);
    MT_EXPECT(rh_validate(h, NULL));
    /* Freeing a large block returns its segment. */
    MT_EXPECT(rh_free(h, big));
    MT_EXPECT_EQ(seg_frees, 1);
    MT_EXPECT_EQ(h->seg_count, 2);
    MT_EXPECT(rh_free(h, big2));
    MT_EXPECT_EQ(seg_frees, 2);
    MT_EXPECT_EQ(h->seg_count, 1);
    MT_EXPECT(rh_validate(h, NULL));
    /* A large block can be grown and shrunk through realloc. */
    big = rh_alloc(h, 600 * 1024, 0);
    memset(big, 9, 600 * 1024);
    big2 = rh_realloc(h, big, 2 * 1024 * 1024, 0);
    MT_ASSERT(big2 != NULL);
    MT_EXPECT_EQ(big2[600 * 1024 - 1], 9);
    MT_EXPECT_EQ(rh_size(h, big2), 2 * 1024 * 1024);
    big = rh_realloc(h, big2, 100, 0);
    MT_ASSERT(big != NULL);
    MT_EXPECT_EQ(big[99], 9);
    MT_EXPECT(rh_validate(h, NULL));
    rh_destroy(h);
    MT_EXPECT_EQ(seg_allocs, seg_frees);
}

static void heap_grows_by_segments(void)
{
    rh_heap *h = make(0, 0, 0);
    void *p[40];
    int i;
    MT_ASSERT(h != NULL);
    for (i = 0; i < 40; ++i) {
        p[i] = rh_alloc(h, 100 * 1024, 0); /* 4 MB in total: several segments */
        MT_ASSERT(p[i] != NULL);
    }
    MT_EXPECT(h->seg_count > 1);
    MT_EXPECT(rh_validate(h, NULL));
    for (i = 0; i < 40; ++i)
        MT_EXPECT(rh_free(h, p[i]));
    MT_EXPECT(rh_validate(h, NULL));
    MT_EXPECT_EQ(h->in_use, 0);
    rh_destroy(h);
    MT_EXPECT_EQ(seg_allocs, seg_frees);
}

static void fixed_size_heap_refuses_to_grow(void)
{
    rh_heap *h = make(0, 64 * 1024, 64 * 1024);
    void *a, *b;
    MT_ASSERT(h != NULL);
    a = rh_alloc(h, 20000, 0);
    MT_ASSERT(a != NULL);
    b = rh_alloc(h, 60000, 0); /* does not fit and the heap may not grow */
    MT_EXPECT(b == NULL);
    MT_EXPECT_EQ(h->seg_count, 1);
    MT_EXPECT(rh_alloc(h, 30000, 0) != NULL);
    MT_EXPECT(rh_validate(h, NULL));
    rh_destroy(h);
    /* A heap whose first segment is too small to hold even its header is refused. */
    reset_os();
    MT_EXPECT(rh_create(&OS, 0, 0, 100) == NULL);
}

static void os_refusal_returns_null(void)
{
    rh_heap *h = make(0, 0, 0);
    MT_ASSERT(h != NULL);
    seg_limit = seg_out; /* the OS now refuses every new segment */
    MT_EXPECT(rh_alloc(h, 2 * RH_GROW, 0) == NULL);
    MT_EXPECT(rh_alloc(h, 64, 0) != NULL); /* the first segment still serves small requests */
    MT_EXPECT(rh_alloc(h, (size_t)1 << 40, 0) == NULL);  /* beyond the maximum request */
    MT_EXPECT(rh_alloc(h, (size_t)-1, 0) == NULL);
    MT_EXPECT(rh_alloc(h, (size_t)-16, 0) == NULL);
    MT_EXPECT(rh_validate(h, NULL));
    rh_destroy(h);
}

static void walk_sees_every_block(void)
{
    rh_heap *h = make(0, 0, 0);
    void *p[10];
    rh_walk w;
    int busy = 0, freeb = 0, i;
    size_t busy_bytes = 0;
    MT_ASSERT(h != NULL);
    for (i = 0; i < 10; ++i)
        p[i] = rh_alloc(h, 50 + (size_t)i * 10, 0);
    rh_free(h, p[3]);
    rh_free(h, p[7]);
    rh_walk_init(&w);
    while (rh_walk_next(h, &w)) {
        if (w.busy) {
            ++busy;
            busy_bytes += w.size;
            MT_EXPECT(w.data != NULL);
        } else {
            ++freeb;
        }
    }
    MT_EXPECT_EQ(busy, 8);
    MT_EXPECT_EQ(freeb, 3); /* two holes and the tail of the segment */
    {
        size_t want = 0;
        for (i = 0; i < 10; ++i)
            if (i != 3 && i != 7)
                want += 50 + (size_t)i * 10;
        MT_EXPECT_EQ(busy_bytes, want);
    }
    rh_destroy(h);
}

static void validate_catches_corruption(void)
{
    rh_heap *h = make(0, 0, 0);
    uint8_t *a, *b;
    MT_ASSERT(h != NULL);
    a = rh_alloc(h, 64, 0);
    b = rh_alloc(h, 64, 0);
    MT_EXPECT(rh_validate(h, NULL));
    /* A buffer overrun into the next header is caught by validation and by free. */
    memset(a, 0xFF, 64 + 16);
    MT_EXPECT(!rh_validate(h, NULL));
    MT_EXPECT(!rh_free(h, b));
    rh_destroy(h);
}

static uint64_t rng_state = 88172645463325252ull;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 11);
}

typedef struct live {
    uint8_t *p;
    size_t size;
    uint8_t tag;
} live;

static void fill(live *l)
{
    memset(l->p, l->tag, l->size);
}

static int check(const live *l)
{
    size_t i;
    for (i = 0; i < l->size; ++i)
        if (l->p[i] != l->tag)
            return 0;
    return 1;
}

static void random_stress(void)
{
    enum { N = 400, ROUNDS = 60000 };
    rh_heap *h = make(0, 0, 0);
    live blocks[N];
    int round, i;
    MT_ASSERT(h != NULL);
    memset(blocks, 0, sizeof(blocks));
    for (round = 0; round < ROUNDS; ++round) {
        int k = (int)(rnd() % N);
        live *l = &blocks[k];
        uint32_t op = rnd() % 10;
        size_t size;
        uint32_t r = rnd() % 100;
        size = r < 70 ? rnd() % 200 : r < 95 ? rnd() % 5000 : rnd() % 150000;
        if (!l->p) {
            l->p = rh_alloc(h, size, op == 0 ? RH_ZERO_MEMORY : 0);
            MT_ASSERT(l->p != NULL);
            l->size = size;
            l->tag = (uint8_t)(rnd() | 1);
            fill(l);
        } else if (op < 4) {
            if (!check(l)) {
                MT_EXPECT(check(l));
                return;
            }
            MT_ASSERT(rh_free(h, l->p));
            l->p = NULL;
        } else {
            uint8_t *np;
            size_t keep = size < l->size ? size : l->size, j;
            if (!check(l)) {
                MT_EXPECT(check(l));
                return;
            }
            np = rh_realloc(h, l->p, size, 0);
            MT_ASSERT(np != NULL);
            for (j = 0; j < keep; ++j)
                if (np[j] != l->tag) {
                    MT_EXPECT_EQ(np[j], l->tag);
                    return;
                }
            l->p = np;
            l->size = size;
            fill(l);
        }
        MT_ASSERT(rh_size(h, l->p ? l->p : NULL) == (l->p ? l->size : (size_t)-1));
        if (round % 5000 == 0)
            MT_ASSERT(rh_validate(h, NULL));
    }
    for (i = 0; i < N; ++i) {
        if (blocks[i].p) {
            MT_EXPECT(check(&blocks[i]));
            MT_EXPECT(rh_free(h, blocks[i].p));
        }
    }
    MT_EXPECT(rh_validate(h, NULL));
    MT_EXPECT_EQ(h->in_use, 0);
    rh_destroy(h);
    MT_EXPECT_EQ(seg_allocs, seg_frees);
}

typedef struct tctx {
    rh_heap *h;
    int id;
    int failures;
} tctx;

static void *worker(void *arg)
{
    tctx *t = (tctx *)arg;
    void *held[32];
    int i, round;
    unsigned s = (unsigned)t->id * 7919u + 1;
    memset(held, 0, sizeof(held));
    for (round = 0; round < 20000; ++round) {
        s = s * 1103515245u + 12345u;
        i = (int)((s >> 16) % 32);
        if (held[i]) {
            uint8_t *b = (uint8_t *)held[i];
            if (b[0] != (uint8_t)t->id || !rh_free(t->h, held[i]))
                ++t->failures;
            held[i] = NULL;
        } else {
            size_t size = 1 + ((s >> 8) % 3000);
            held[i] = rh_alloc(t->h, size, 0);
            if (!held[i]) {
                ++t->failures;
                continue;
            }
            memset(held[i], t->id, size);
        }
    }
    for (i = 0; i < 32; ++i)
        if (held[i] && !rh_free(t->h, held[i]))
            ++t->failures;
    return NULL;
}

static void threads_share_a_heap(void)
{
    enum { T = 4 };
    rh_heap *h = make(0, 0, 0);
    pthread_t th[T];
    tctx ctx[T];
    int i, total = 0;
    MT_ASSERT(h != NULL);
    for (i = 0; i < T; ++i) {
        ctx[i].h = h;
        ctx[i].id = i + 1;
        ctx[i].failures = 0;
        pthread_create(&th[i], NULL, worker, &ctx[i]);
    }
    for (i = 0; i < T; ++i) {
        pthread_join(th[i], NULL);
        total += ctx[i].failures;
    }
    MT_EXPECT_EQ(total, 0);
    MT_EXPECT(rh_validate(h, NULL));
    MT_EXPECT_EQ(h->in_use, 0);
    rh_destroy(h);
}

static void no_serialize_skips_the_lock(void)
{
    rh_heap *h = make(RH_NO_SERIALIZE, 0, 0);
    void *a;
    MT_ASSERT(h != NULL);
    a = rh_alloc(h, 64, 0);
    MT_EXPECT(a != NULL);
    MT_EXPECT_EQ(h->lock, 0);
    MT_EXPECT(rh_free(h, a));
    rh_destroy(h);
}

static void bin_arithmetic(void)
{
    unsigned b;
    MT_EXPECT_EQ(rh_bin_of(32), 2);
    MT_EXPECT_EQ(rh_bin_of(1008), 63);
    MT_EXPECT_EQ(rh_bin_of(1024), 64);
    MT_EXPECT_EQ(rh_bin_of(1280), 65);
    MT_EXPECT_EQ(rh_bin_of(2048), 68);
    /* Every size maps to a bin and the mapping never decreases. */
    {
        unsigned prev = 0;
        size_t s;
        for (s = 32; s < ((size_t)1 << 30); s += s < 4096 ? 16 : s / 7) {
            b = rh_bin_of(s);
            MT_ASSERT(b < RH_BINS);
            MT_ASSERT(b >= prev);
            prev = b;
        }
    }
    MT_EXPECT_EQ(rh_bin_of((size_t)1 << 40), RH_BINS - 1 < 64 + 30 * 4 + 3 ? RH_BINS - 1 : 64 + 30 * 4);
}

static const mt_case_t cases[] = {
    MT_CASE(alloc_basics),
    MT_CASE(zero_size_and_zero_memory),
    MT_CASE(coalescing_restores_one_chunk),
    MT_CASE(reuse_after_free),
    MT_CASE(double_free_and_bad_pointers),
    MT_CASE(realloc_in_place_and_moving),
    MT_CASE(realloc_zero_memory),
    MT_CASE(large_blocks_get_dedicated_segments),
    MT_CASE(heap_grows_by_segments),
    MT_CASE(fixed_size_heap_refuses_to_grow),
    MT_CASE(os_refusal_returns_null),
    MT_CASE(walk_sees_every_block),
    MT_CASE(validate_catches_corruption),
    MT_CASE(random_stress),
    MT_CASE(threads_share_a_heap),
    MT_CASE(no_serialize_skips_the_lock),
    MT_CASE(bin_arithmetic),
};

int main(void)
{
    return MT_RUN("rtl_heap_pure", cases);
}
