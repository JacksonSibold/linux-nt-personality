/* dh_test.h - minimal check macros for the driver host tests.  A failed check
 * prints its location and marks the run failed; the program exits nonzero if any
 * check failed.  No check is ever relaxed to make a run pass.
 */
#ifndef DH_TEST_H
#define DH_TEST_H

#include <stdio.h>

static int dh_checks_run = 0;
static int dh_checks_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        dh_checks_run++;                                                         \
        if (!(cond)) {                                                           \
            dh_checks_failed++;                                                  \
            fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_STATUS(expr, want)                                                 \
    do {                                                                         \
        dh_checks_run++;                                                         \
        long long got_ = (long long)(int)(expr);                                 \
        long long want_ = (long long)(int)(want);                                \
        if (got_ != want_) {                                                     \
            dh_checks_failed++;                                                  \
            fprintf(stderr, "CHECK FAILED %s:%d: %s = 0x%08llx, want 0x%08llx\n", \
                    __FILE__, __LINE__, #expr, (unsigned long long)(unsigned)got_, \
                    (unsigned long long)(unsigned)want_);                        \
        }                                                                        \
    } while (0)

#define DH_TEST_SUMMARY(name)                                                    \
    do {                                                                         \
        printf("%s: %d checks, %d failed\n", (name), dh_checks_run, dh_checks_failed); \
        return dh_checks_failed ? 1 : 0;                                         \
    } while (0)

#endif /* DH_TEST_H */
