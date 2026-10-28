/*
 * test_k32_obj_pure.c - host tests for src/ntdll/k32_obj_pure.h: object names, wait results, timeouts, timer and
 * completion port argument rules.
 */
#include "minitest.h"
#include "k32_obj_pure.h"

#include <string.h>

static DWORD name_of(const char *s, char *out, size_t *outn)
{
    WCHAR w[300], r[400];
    size_t n = 0, rn = 0;
    DWORD err;
    while (s[n]) {
        w[n] = (WCHAR)(unsigned char)s[n];
        ++n;
    }
    err = k32o_object_name(w, n, r, 400, &rn);
    if (!err) {
        for (size_t i = 0; i <= rn; ++i)
            out[i] = (char)r[i];
    }
    if (outn)
        *outn = rn;
    return err;
}

static void object_names(void)
{
    char out[400];
    size_t n;
    MT_EXPECT_EQ((long)name_of("MyEvent", out, &n), 0);
    MT_EXPECT(strcmp(out, "\\BaseNamedObjects\\MyEvent") == 0 && n == 25);
    MT_EXPECT_EQ((long)name_of("Global\\Shared", out, &n), 0);
    MT_EXPECT(strcmp(out, "\\BaseNamedObjects\\Shared") == 0);
    MT_EXPECT_EQ((long)name_of("local\\Mine", out, &n), 0);
    MT_EXPECT(strcmp(out, "\\BaseNamedObjects\\Mine") == 0);
    MT_EXPECT_EQ((long)name_of("Session\\2\\X", out, &n), 0);
    MT_EXPECT(strcmp(out, "\\BaseNamedObjects\\X") == 0);
    MT_EXPECT_EQ((long)name_of("Globalx", out, &n), 0); /* not a prefix: no backslash after it */
    MT_EXPECT(strcmp(out, "\\BaseNamedObjects\\Globalx") == 0);
    MT_EXPECT_EQ((long)name_of("a\\b", out, &n), 3);     /* a backslash inside the name */
    MT_EXPECT_EQ((long)name_of("Global\\", out, &n), 87); /* nothing after the prefix */
    MT_EXPECT_EQ((long)name_of("", out, &n), 87);
}

static void wait_results(void)
{
    MT_EXPECT_EQ((long)k32o_wait_result(STATUS_WAIT_0), 0);
    MT_EXPECT_EQ((long)k32o_wait_result(STATUS_WAIT_0 + 5), 5);
    MT_EXPECT_EQ((long)k32o_wait_result(STATUS_ABANDONED_WAIT_0 + 2), 0x82);
    MT_EXPECT_EQ((long)k32o_wait_result(STATUS_TIMEOUT), 258);
    MT_EXPECT_EQ((long)k32o_wait_result(K32O_STATUS_USER_APC), 192);
    MT_EXPECT_EQ((long)k32o_wait_result((NTSTATUS)0xC0000008L), (long)0xFFFFFFFFu);
}

static void timeouts_and_timers(void)
{
    LARGE_INTEGER t;
    MT_EXPECT(k32o_timeout(K32O_INFINITE, &t) == NULL);
    MT_EXPECT(k32o_timeout(5, &t) == &t && t.QuadPart == -50000);
    MT_EXPECT(k32o_timeout(0, &t) == &t && t.QuadPart == 0);
    t.QuadPart = -10;
    MT_EXPECT_EQ((long)k32o_timer_args_check(&t, 100), 0);
    MT_EXPECT_EQ((long)k32o_timer_args_check(&t, -1), 87);
    MT_EXPECT_EQ((long)k32o_timer_args_check(NULL, 0), 87);
}

static void iocp_modes(void)
{
    HANDLE inv = (HANDLE)(intptr_t)-1, port = (HANDLE)0x40, file = (HANDLE)0x44;
    MT_EXPECT_EQ(k32o_iocp_mode(inv, NULL), 1);
    MT_EXPECT_EQ(k32o_iocp_mode(inv, port), 0);
    MT_EXPECT_EQ(k32o_iocp_mode(file, port), 2);
    MT_EXPECT_EQ(k32o_iocp_mode(file, NULL), 0);
    MT_EXPECT_EQ(k32o_iocp_mode(NULL, port), 0);
}

static const mt_case_t cases[] = {
    MT_CASE(object_names), MT_CASE(wait_results), MT_CASE(timeouts_and_timers), MT_CASE(iocp_modes),
};

int main(void)
{
    return MT_RUN("k32_obj_pure", cases);
}
