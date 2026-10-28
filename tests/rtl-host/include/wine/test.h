/*
 * wine/test.h - a small stand-in for Wine's test framework header, so that the unit tests of Wine's dlls/ntdll/tests
 * (LGPL-2.1-or-later, kept in tests/rtl-host/wine/) build and run on a Linux host against the Rtl sources of
 * src/ntdll/rtl. Same macros as the original: ok, skip, win_skip, trace, todo_if, todo_wine, flaky, START_TEST,
 * broken. The platform is reported as Wine, so a todo_wine block is a known divergence from Windows: the check may fail,
 * and the run is flagged if it starts to pass. That keeps the port at the level of Wine's own test results.
 *
 * The counters and the printing are in host_support.c. A test passes when no ok() failed.
 */
#ifndef RTL_HOST_WINE_TEST_H
#define RTL_HOST_WINE_TEST_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"

extern int winetest_debug;
extern int winetest_platform_is_wine;
extern long winetest_successes, winetest_failures, winetest_skipped, winetest_todo_successes, winetest_todo_failures;
extern const char *winetest_cur_file;
extern int winetest_cur_line;
extern unsigned int winetest_todo_level, winetest_flaky_level;

#define START_TEST(name) void func_##name(void)

#define subtest_(file, line) (winetest_set_location(file, line), 0) ? (void)0 : winetest_subtest
#define ok_(file, line) (winetest_set_location(file, line), 0) ? (void)0 : winetest_ok
#define skip_(file, line) (winetest_set_location(file, line), 0) ? (void)0 : winetest_skip
#define win_skip_(file, line) (winetest_set_location(file, line), 0) ? (void)0 : winetest_win_skip
#define trace_(file, line) (winetest_set_location(file, line), 0) ? (void)0 : winetest_trace

#define subtest subtest_(__FILE__, __LINE__)
#define ok ok_(__FILE__, __LINE__)
#define skip skip_(__FILE__, __LINE__)
#define win_skip win_skip_(__FILE__, __LINE__)
#define trace trace_(__FILE__, __LINE__)

#define flaky_if(is_flaky) for (winetest_start_flaky(is_flaky); winetest_loop_flaky(); winetest_end_flaky())
#define flaky flaky_if(TRUE)
#define flaky_wine flaky_if(winetest_platform_is_wine)
#define flaky_wine_if(is_flaky) flaky_if((is_flaky) && winetest_platform_is_wine)

#define todo_if(is_todo) for (winetest_start_todo(is_todo); winetest_loop_todo(); winetest_end_todo())
#define todo_wine todo_if(winetest_platform_is_wine)
#define todo_wine_if(is_todo) todo_if((is_todo) && winetest_platform_is_wine)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define WINETEST_PRINTF(a, b) __attribute__((format(printf, a, b)))

void winetest_set_location(const char *file, int line);
void winetest_ok(int condition, const char *msg, ...) WINETEST_PRINTF(2, 3);
void winetest_skip(const char *msg, ...) WINETEST_PRINTF(1, 2);
void winetest_win_skip(const char *msg, ...) WINETEST_PRINTF(1, 2);
void winetest_trace(const char *msg, ...) WINETEST_PRINTF(1, 2);
void winetest_subtest(const char *name);
void winetest_push_context(const char *fmt, ...) WINETEST_PRINTF(1, 2);
void winetest_pop_context(void);
void winetest_start_todo(int is_todo);
int winetest_loop_todo(void);
void winetest_end_todo(void);
void winetest_start_flaky(int is_flaky);
int winetest_loop_flaky(void);
void winetest_end_flaky(void);

static inline int broken(int condition) { return !winetest_platform_is_wine && condition; }

#endif
