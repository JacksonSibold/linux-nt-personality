/* minitest.h - minimal assert-based runner for host-side C tests.
 *
 * A test is a void function. MT_ASSERT() records a failure and returns from
 * the test; MT_EXPECT_EQ() records a failure and keeps going. Tests that
 * allocate should use MT_EXPECT* after the allocation so that cleanup code
 * always runs.
 */
#ifndef MINITEST_H
#define MINITEST_H

#include <stddef.h>
#include <stdio.h>

typedef void (*mt_fn)(void);

typedef struct mt_case {
    const char *name;
    mt_fn fn;
} mt_case_t;

static int mt_case_failures;

/* Failures go to stdout so that a Makefile can send stderr (diagnostics the
 * code under test prints) to a log file without hiding them. */
static inline void mt_fail(const char *file, int line, const char *what)
{
    ++mt_case_failures;
    printf("    %s:%d: check failed: %s\n", file, line, what);
}

static inline void mt_fail_eq(const char *file, int line, const char *expr,
                              long got, long want)
{
    ++mt_case_failures;
    printf("    %s:%d: %s: got %ld, want %ld\n", file, line, expr, got, want);
}

static inline int mt_run(const char *suite, const mt_case_t *cases, size_t n)
{
    size_t failed = 0;
    for (size_t i = 0; i < n; ++i) {
        mt_case_failures = 0;
        cases[i].fn();
        printf("[%s] %s ... %s\n", suite, cases[i].name,
               mt_case_failures ? "FAIL" : "ok");
        if (mt_case_failures)
            ++failed;
    }
    printf("%s: %zu run, %zu failed\n", suite, n, failed);
    return failed ? 1 : 0;
}

#define MT_ASSERT(cond) do { \
    if (!(cond)) { mt_fail(__FILE__, __LINE__, #cond); return; } \
} while (0)

#define MT_EXPECT(cond) do { \
    if (!(cond)) mt_fail(__FILE__, __LINE__, #cond); \
} while (0)

#define MT_EXPECT_EQ(actual, expected) do { \
    long mt_a_ = (long)(actual), mt_e_ = (long)(expected); \
    if (mt_a_ != mt_e_) mt_fail_eq(__FILE__, __LINE__, #actual, mt_a_, mt_e_); \
} while (0)

#define MT_CASE(fn) { #fn, fn }

#define MT_RUN(suite, cases) \
    mt_run((suite), (cases), sizeof(cases) / sizeof((cases)[0]))

#endif /* MINITEST_H */
