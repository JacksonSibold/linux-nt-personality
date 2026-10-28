/*
 * ucrt_stdlib.h - <stdlib.h> functions that need no operating system: abs and div, rand, qsort and bsearch
 * (and their _s forms), the exit-handler tables, byte swapping, and swab. Host tests: tests/crt-host/test_stdlib.c.
 */
#ifndef UCRT_STDLIB_H
#define UCRT_STDLIB_H

#include "ucrt_state.h"
#include "ucrt_string.h"

static inline int ucrt_abs(int v) { return v < 0 ? -v : v; }
static inline int32_t ucrt_labs(int32_t v) { return v < 0 ? -v : v; } /* long is 32 bits on Windows */
static inline int64_t ucrt_llabs(int64_t v) { return v < 0 ? -v : v; }

typedef struct ucrt_div_t { int quot, rem; } ucrt_div_t;
typedef struct ucrt_ldiv_t { int32_t quot, rem; } ucrt_ldiv_t;
typedef struct ucrt_lldiv_t { int64_t quot, rem; } ucrt_lldiv_t;

static inline ucrt_div_t ucrt_div(int n, int d)
{
    ucrt_div_t r;
    r.quot = n / d;
    r.rem = n % d;
    return r;
}

static inline ucrt_ldiv_t ucrt_ldiv(int32_t n, int32_t d)
{
    ucrt_ldiv_t r;
    r.quot = n / d;
    r.rem = n % d;
    return r;
}

static inline ucrt_lldiv_t ucrt_lldiv(int64_t n, int64_t d)
{
    ucrt_lldiv_t r;
    r.quot = n / d;
    r.rem = n % d;
    return r;
}

/* The Microsoft rand: a linear congruential generator, 15 bits per call. srand(1) is the initial state. */
#define UCRT_RAND_MAX 0x7FFF

static inline void ucrt_srand(unsigned seed)
{
    g_ucrt_state.rand_seed = seed;
}

static inline int ucrt_rand(void)
{
    g_ucrt_state.rand_seed = g_ucrt_state.rand_seed * 214013u + 2531011u;
    return (int)((g_ucrt_state.rand_seed >> 16) & UCRT_RAND_MAX);
}

/* ---- Byte swapping --------------------------------------------------------------------------------- */

static inline uint16_t ucrt_byteswap_ushort(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t ucrt_byteswap_ulong(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t ucrt_byteswap_uint64(uint64_t v) { return __builtin_bswap64(v); }

/* _swab: copies n bytes (rounded down to an even number), swapping each pair. */
static inline void ucrt_swab(const char *src, char *dst, int n)
{
    if (n < 0)
        n = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        char a = src[i], b = src[i + 1];
        dst[i] = b;
        dst[i + 1] = a;
    }
}

/* ---- Sorting and searching ------------------------------------------------------------------------- */

/* A comparison function taking a context, as qsort_s and bsearch_s do. qsort and bsearch wrap theirs. */
typedef int (*ucrt_cmp_s_fn)(void *ctx, const void *a, const void *b);
typedef int (*ucrt_cmp_fn)(const void *a, const void *b);

typedef struct ucrt_cmp_adapter {
    ucrt_cmp_fn cmp;
} ucrt_cmp_adapter;

static inline int ucrt_cmp_adapt(void *ctx, const void *a, const void *b)
{
    return ((ucrt_cmp_adapter *)ctx)->cmp(a, b);
}

static inline void ucrt_swap_bytes(uint8_t *a, uint8_t *b, size_t size)
{
    while (size >= 8) {
        uint64_t x, y;
        __builtin_memcpy(&x, a, 8);
        __builtin_memcpy(&y, b, 8);
        __builtin_memcpy(a, &y, 8);
        __builtin_memcpy(b, &x, 8);
        a += 8;
        b += 8;
        size -= 8;
    }
    while (size--) {
        uint8_t t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Insertion sort for short runs. */
static inline void ucrt_insertion_sort(uint8_t *base, size_t n, size_t size, ucrt_cmp_s_fn cmp, void *ctx)
{
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && cmp(ctx, base + (j - 1) * size, base + j * size) > 0; --j)
            ucrt_swap_bytes(base + (j - 1) * size, base + j * size, size);
    }
}

static inline void ucrt_sift_down(uint8_t *base, size_t start, size_t end, size_t size, ucrt_cmp_s_fn cmp, void *ctx)
{
    size_t root = start;
    while (2 * root + 1 < end) {
        size_t child = 2 * root + 1;
        if (child + 1 < end && cmp(ctx, base + child * size, base + (child + 1) * size) < 0)
            ++child;
        if (cmp(ctx, base + root * size, base + child * size) >= 0)
            return;
        ucrt_swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

static inline void ucrt_heap_sort(uint8_t *base, size_t n, size_t size, ucrt_cmp_s_fn cmp, void *ctx)
{
    for (size_t i = n / 2; i-- > 0;)
        ucrt_sift_down(base, i, n, size, cmp, ctx);
    for (size_t end = n; end-- > 1;) {
        ucrt_swap_bytes(base, base + end * size, size);
        ucrt_sift_down(base, 0, end, size, cmp, ctx);
    }
}

/* Introsort: quicksort with a median of three, insertion sort for short runs, and heapsort when the
 * recursion gets too deep, so the worst case stays O(n log n). The sort is not stable. */
static inline void ucrt_intro_sort(uint8_t *base, size_t n, size_t size, ucrt_cmp_s_fn cmp, void *ctx, int depth)
{
    while (n > 16) {
        size_t lo = 0, hi = n - 1, mid = n / 2, i, j;
        uint8_t *pivot;
        if (depth-- == 0) {
            ucrt_heap_sort(base, n, size, cmp, ctx);
            return;
        }
        /* median of three into position 0 */
        if (cmp(ctx, base + mid * size, base + lo * size) < 0)
            ucrt_swap_bytes(base + mid * size, base + lo * size, size);
        if (cmp(ctx, base + hi * size, base + lo * size) < 0)
            ucrt_swap_bytes(base + hi * size, base + lo * size, size);
        if (cmp(ctx, base + hi * size, base + mid * size) < 0)
            ucrt_swap_bytes(base + hi * size, base + mid * size, size);
        ucrt_swap_bytes(base + mid * size, base + (hi - 1) * size, size); /* the pivot sits at hi - 1 */
        pivot = base + (hi - 1) * size;
        i = lo;
        j = hi - 1;
        for (;;) {
            while (cmp(ctx, base + ++i * size, pivot) < 0)
                ;
            while (cmp(ctx, base + --j * size, pivot) > 0 && j > lo)
                ;
            if (i >= j)
                break;
            ucrt_swap_bytes(base + i * size, base + j * size, size);
        }
        ucrt_swap_bytes(base + i * size, base + (hi - 1) * size, size);
        /* recurse into the smaller side, loop on the larger */
        if (i < n - i - 1) {
            ucrt_intro_sort(base, i, size, cmp, ctx, depth);
            base += (i + 1) * size;
            n -= i + 1;
        } else {
            ucrt_intro_sort(base + (i + 1) * size, n - i - 1, size, cmp, ctx, depth);
            n = i;
        }
    }
    ucrt_insertion_sort(base, n, size, cmp, ctx);
}

static inline void ucrt_qsort_s_impl(void *base, size_t n, size_t size, ucrt_cmp_s_fn cmp, void *ctx)
{
    int depth = 0;
    size_t m = n;
    if (n < 2 || size == 0)
        return;
    while (m) {
        ++depth;
        m >>= 1;
    }
    ucrt_intro_sort((uint8_t *)base, n, size, cmp, ctx, 2 * depth);
}

/* qsort_s validates its arguments (an invalid parameter when n != 0 and base, cmp NULL or size 0). */
static inline void ucrt_qsort_s(void *base, size_t n, size_t size, ucrt_cmp_s_fn cmp, void *ctx)
{
    if (n == 0)
        return;
    if (!base || size == 0 || !cmp) {
        ucrt_invalid(UCRT_EINVAL);
        return;
    }
    ucrt_qsort_s_impl(base, n, size, cmp, ctx);
}

static inline void ucrt_qsort(void *base, size_t n, size_t size, ucrt_cmp_fn cmp)
{
    ucrt_cmp_adapter a;
    a.cmp = cmp;
    if (n == 0)
        return;
    if (!base || size == 0 || !cmp) {
        ucrt_invalid(UCRT_EINVAL);
        return;
    }
    ucrt_qsort_s_impl(base, n, size, ucrt_cmp_adapt, &a);
}

static inline void *ucrt_bsearch_s(const void *key, const void *base, size_t n, size_t size, ucrt_cmp_s_fn cmp, void *ctx)
{
    size_t lo = 0, hi = n;
    if (n == 0)
        return NULL;
    if (!key || !base || size == 0 || !cmp) {
        ucrt_invalid(UCRT_EINVAL);
        return NULL;
    }
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const uint8_t *p = (const uint8_t *)base + mid * size;
        int c = cmp(ctx, key, p);
        if (c == 0)
            return (void *)(uintptr_t)p;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static inline void *ucrt_bsearch(const void *key, const void *base, size_t n, size_t size, ucrt_cmp_fn cmp)
{
    ucrt_cmp_adapter a;
    a.cmp = cmp;
    return ucrt_bsearch_s(key, base, n, size, ucrt_cmp_adapt, &a);
}

/* _lfind and _lsearch: linear searches; _lsearch appends the key when absent. */
static inline void *ucrt_lfind(const void *key, const void *base, unsigned *count, unsigned size, ucrt_cmp_fn cmp)
{
    const uint8_t *p = (const uint8_t *)base;
    for (unsigned i = 0; i < *count; ++i, p += size)
        if (cmp(key, p) == 0)
            return (void *)(uintptr_t)p;
    return NULL;
}

static inline void *ucrt_lsearch(const void *key, void *base, unsigned *count, unsigned size, ucrt_cmp_fn cmp)
{
    void *found = ucrt_lfind(key, base, count, size, cmp);
    if (found)
        return found;
    found = (uint8_t *)base + (size_t)*count * size;
    ucrt_memmove(found, key, size);
    ++*count;
    return found;
}

/* ---- Exit handler tables (_onexit_table_t) ---------------------------------------------------------------- */

/* The tables of the universal runtime: each module has one, and the functions registered on it run in
 * reverse order of registration. The storage comes from the caller's allocator (realloc and free). */
typedef void (*ucrt_onexit_fn)(void);

typedef struct ucrt_onexit_table {
    ucrt_onexit_fn *first;
    ucrt_onexit_fn *last; /* one past the last registered function */
    ucrt_onexit_fn *end;  /* one past the allocation */
} ucrt_onexit_table;

static inline int ucrt_initialize_onexit_table(ucrt_onexit_table *t)
{
    if (!t)
        return -1;
    t->first = t->last = t->end = NULL;
    return 0;
}

static inline int ucrt_register_onexit(ucrt_onexit_table *t, ucrt_onexit_fn fn, void *(*realloc_fn)(void *, size_t))
{
    if (!t)
        return -1;
    if (t->last == t->end) {
        size_t used = (size_t)(t->last - t->first);
        size_t cap = used ? used * 2 : 8;
        ucrt_onexit_fn *p = (ucrt_onexit_fn *)realloc_fn(t->first, cap * sizeof(*p));
        if (!p)
            return -1;
        t->first = p;
        t->last = p + used;
        t->end = p + cap;
    }
    *t->last++ = fn;
    return 0;
}

/* Runs the registered functions, last first, and empties the table. A function may register another; it
 * runs before the table finishes. */
static inline int ucrt_execute_onexit(ucrt_onexit_table *t, void (*free_fn)(void *))
{
    if (!t)
        return -1;
    while (t->first && t->last > t->first) {
        ucrt_onexit_fn fn = *--t->last;
        if (fn)
            fn();
    }
    if (t->first)
        free_fn(t->first);
    t->first = t->last = t->end = NULL;
    return 0;
}

#endif /* UCRT_STDLIB_H */
