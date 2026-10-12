/*
 * test_runtime.c - host tests for the freestanding allocator in
 * ntdll_runtime.c.
 *
 * ntdll_runtime.c replaces printf and friends with stubs that print nothing,
 * so this file must not use minitest.h. It reports through fputs instead.
 * Malloc and calloc come from ntdll_runtime.c because that object is linked
 * first; the test does not depend on the MinGW libc allocator.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void *malloc(size_t sz);
void *calloc(size_t n, size_t sz);

static int g_failures;

static void report_fail(int line, const char *what)
{
    char num[16];
    int i = 0;
    int v = line;
    char rev[16];
    int r = 0;
    do {
        rev[r++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (r > 0)
        num[i++] = rev[--r];
    num[i] = '\0';

    ++g_failures;
    fputs("    test_runtime.c:", stdout);
    fputs(num, stdout);
    fputs(": check failed: ", stdout);
    fputs(what, stdout);
    fputs("\n", stdout);
}

#define CHECK(cond) do { if (!(cond)) report_fail(__LINE__, #cond); } while (0)

/* calloc(n, sz) must fail when n * sz does not fit in size_t. */
/* volatile keeps gcc from folding the sizes into a constant-argument warning. */
static void calloc_rejects_multiplication_overflow(void)
{
    volatile size_t huge = (size_t)1 << (sizeof(size_t) * 8 - 1);
    volatile size_t two = 2;
    CHECK(calloc(huge, two) == NULL);
}

/* malloc must fail when the rounded size wraps around. */
static void malloc_rejects_huge_request(void)
{
    volatile size_t request = (size_t)-8;
    CHECK(malloc(request) == NULL);
}

static void malloc_returns_usable_block(void)
{
    unsigned char *p = malloc(32);
    CHECK(p != NULL);
    if (p) {
        memset(p, 0x5a, 32);
        CHECK(p[0] == 0x5a && p[31] == 0x5a);
    }
}

static void calloc_zeroes_block(void)
{
    unsigned char *p = calloc(4, 8);
    CHECK(p != NULL);
    if (p) {
        int zero = 1;
        for (size_t i = 0; i < 32; ++i)
            zero = zero && p[i] == 0;
        CHECK(zero);
    }
}

typedef void (*test_fn)(void);

static const test_fn tests[] = {
    calloc_rejects_multiplication_overflow,
    malloc_rejects_huge_request,
    malloc_returns_usable_block,
    calloc_zeroes_block,
};

int main(void)
{
    size_t n = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < n; ++i) {
        int before = g_failures;
        tests[i]();
        fputs(g_failures == before ? "[runtime] ok\n" : "[runtime] FAIL\n", stdout);
    }
    fputs(g_failures ? "runtime: failures present\n" : "runtime: all passed\n", stdout);
    return g_failures ? 1 : 0;
}
