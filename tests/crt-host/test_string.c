/*
 * test_string.c - differential tests for src/crt/ucrt_string.h against the system C library: the narrow
 * string functions on random inputs, the wide ones through 32-bit wchar_t, memory functions with overlap,
 * the secure _s functions against the documented error behaviour, and strerror.
 */
#include "minitest.h"
#include "ucrt_string.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

static uint64_t rng = 0x243F6A8885A308D3ull;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* A random string over a small alphabet so that matches and prefixes are common. */
static void rand_str(char *out, size_t max_len, const char *alphabet)
{
    size_t n = rnd() % (max_len + 1), i, a = strlen(alphabet);
    for (i = 0; i < n; ++i)
        out[i] = alphabet[rnd() % a];
    out[n] = 0;
}

static int sign(int v)
{
    return v < 0 ? -1 : v > 0 ? 1 : 0;
}

static void narrow_basics(void)
{
    char a[64], b[64], c[128], d[128];
    int i;
    for (i = 0; i < 20000; ++i) {
        size_t n;
        rand_str(a, 12, "abAB \xE9\xC3");
        rand_str(b, 12, "abAB \xE9\xC3");
        MT_EXPECT_EQ(ucrt_strlen(a), strlen(a));
        n = rnd() % 14;
        MT_EXPECT_EQ(ucrt_strnlen(a, n), strnlen(a, n));
        MT_EXPECT_EQ(sign(ucrt_strcmp(a, b)), sign(strcmp(a, b)));
        MT_EXPECT_EQ(sign(ucrt_strncmp(a, b, n)), sign(strncmp(a, b, n)));
        MT_EXPECT_EQ(sign(ucrt_stricmp(a, b)), sign(strcasecmp(a, b)));
        MT_EXPECT_EQ(sign(ucrt_strnicmp(a, b, n)), sign(strncasecmp(a, b, n)));
        /* strcpy, strncpy (with padding), strcat, strncat. */
        memset(c, 'x', sizeof(c));
        memset(d, 'x', sizeof(d));
        ucrt_strncpy(c, a, n);
        strncpy(d, a, n);
        MT_EXPECT(memcmp(c, d, sizeof(c)) == 0);
        strcpy(c, a);
        ucrt_strcpy(d, a);
        MT_EXPECT(strcmp(c, d) == 0);
        strcat(c, b);
        ucrt_strcat(d, b);
        MT_EXPECT(strcmp(c, d) == 0);
        strcpy(c, a);
        ucrt_strcpy(d, a);
        strncat(c, b, n);
        ucrt_strncat(d, b, n);
        MT_EXPECT(strcmp(c, d) == 0);
    }
}

static void narrow_searching(void)
{
    char a[64], b[16];
    int i;
    for (i = 0; i < 20000; ++i) {
        int ch = "abc\0"[rnd() % 4];
        rand_str(a, 20, "abc");
        rand_str(b, 3, "abc");
        MT_EXPECT(ucrt_strchr(a, ch) == strchr(a, ch));
        MT_EXPECT(ucrt_strrchr(a, ch) == strrchr(a, ch));
        MT_EXPECT(ucrt_strstr(a, b) == strstr(a, b));
        MT_EXPECT_EQ(ucrt_strspn(a, b), strspn(a, b));
        MT_EXPECT_EQ(ucrt_strcspn(a, b), strcspn(a, b));
        MT_EXPECT(ucrt_strpbrk(a, b) == strpbrk(a, b));
    }
    /* The terminator is found by strchr and strrchr. */
    MT_EXPECT(ucrt_strchr("abc", 0) != NULL && *ucrt_strchr("abc", 0) == 0);
    MT_EXPECT(ucrt_strstr("abc", "") != NULL);
}

static void narrow_tokenising(void)
{
    char a[80], b[80], *ta, *tb, *ca = NULL, *cb = NULL;
    int i;
    for (i = 0; i < 5000; ++i) {
        char delim[8];
        rand_str(a, 30, "ab,; ");
        strcpy(b, a);
        rand_str(delim, 3, ",; ");
        ta = ucrt_strtok_r(a, delim, &ca);
        tb = strtok_r(b, delim, &cb);
        for (;;) {
            if (!ta || !tb) {
                MT_EXPECT(ta == NULL && tb == NULL);
                break;
            }
            MT_EXPECT(strcmp(ta, tb) == 0);
            ta = ucrt_strtok_r(NULL, delim, &ca);
            tb = strtok_r(NULL, delim, &cb);
        }
    }
}

static void narrow_case_and_reverse(void)
{
    char a[64], b[64];
    int i;
    for (i = 0; i < 5000; ++i) {
        size_t j, n;
        rand_str(a, 20, "abXY1 _\xE9");
        strcpy(b, a);
        ucrt_strlwr(a);
        for (j = 0; b[j]; ++j)
            b[j] = (b[j] >= 'A' && b[j] <= 'Z') ? (char)(b[j] + 32) : b[j];
        MT_EXPECT(strcmp(a, b) == 0);
        ucrt_strupr(a);
        for (j = 0; b[j]; ++j)
            b[j] = (b[j] >= 'a' && b[j] <= 'z') ? (char)(b[j] - 32) : b[j];
        MT_EXPECT(strcmp(a, b) == 0);
        n = strlen(a);
        ucrt_strrev(a);
        for (j = 0; j < n; ++j)
            MT_EXPECT(a[j] == b[n - 1 - j]);
        ucrt_strset(a, 'q');
        for (j = 0; j < n; ++j)
            MT_EXPECT(a[j] == 'q');
        MT_EXPECT_EQ(a[n], 0);
    }
}

static void memory_functions(void)
{
    uint8_t a[300], b[300], ref[300];
    int i;
    for (i = 0; i < 30000; ++i) {
        size_t n = rnd() % 140, off1 = rnd() % 40, off2 = rnd() % 40, j;
        for (j = 0; j < sizeof(a); ++j)
            a[j] = (uint8_t)rnd();
        memcpy(b, a, sizeof(a));
        memcpy(ref, a, sizeof(a));
        /* Overlapping and disjoint copies behave like memmove. */
        ucrt_memmove(a + off1, a + off2, n);
        memmove(ref + off1, ref + off2, n);
        MT_EXPECT(memcmp(a, ref, sizeof(a)) == 0);
        ucrt_memcpy(b + off1, b + off2, n);
        MT_EXPECT(memcmp(b, ref, sizeof(b)) == 0);
        /* memset */
        ucrt_memset(a + off1, (int)(rnd() & 0x1FF), n);
        MT_EXPECT(ucrt_memcmp(a, a, sizeof(a)) == 0);
        /* memcmp sign and memchr */
        b[off1 + (n ? rnd() % n : 0)] ^= (uint8_t)(1u << (rnd() % 8));
        MT_EXPECT_EQ(sign(ucrt_memcmp(a, b, sizeof(a))), sign(memcmp(a, b, sizeof(a))));
        {
            int c = (int)(rnd() & 0xFF);
            MT_EXPECT(ucrt_memchr(a, c, n + off1) == memchr(a, c, n + off1));
        }
    }
    /* memcmp returns the byte difference, as the Microsoft runtime does. */
    MT_EXPECT_EQ(ucrt_memcmp("a", "c", 1), -2);
    MT_EXPECT_EQ(ucrt_memcmp("\xff", "\x01", 1), 254);
    MT_EXPECT_EQ(ucrt_memcmp("abc", "abd", 0), 0);
    MT_EXPECT_EQ(ucrt_memicmp("HELLO", "hellp", 5), -1);
    MT_EXPECT_EQ(ucrt_memicmp("HeLLo", "hello", 5), 0);
    {
        char d[8] = "zzzzzzz";
        MT_EXPECT(ucrt_memccpy(d, "abcdef", 'd', 6) == d + 4);
        MT_EXPECT(memcmp(d, "abcdzzz", 7) == 0);
        MT_EXPECT(ucrt_memccpy(d, "xyz", 'q', 3) == NULL);
    }
}

static void to_wide32(const ucrt_wchar *s, wchar_t *out)
{
    size_t i = 0;
    for (; s[i]; ++i)
        out[i] = (wchar_t)s[i];
    out[i] = 0;
}

static void rand_wstr(ucrt_wchar *out, size_t max_len)
{
    static const ucrt_wchar alpha[] = {'a', 'b', 'A', 'B', ' ', 0xE9, 0x20AC, 0xFFFF, 0x0100};
    size_t n = rnd() % (max_len + 1), i;
    for (i = 0; i < n; ++i)
        out[i] = alpha[rnd() % (sizeof(alpha) / sizeof(alpha[0]))];
    out[n] = 0;
}

static void wide_functions(void)
{
    ucrt_wchar a[40], b[40], c[80];
    wchar_t wa[40], wb[40], wc[80];
    int i;
    for (i = 0; i < 20000; ++i) {
        size_t n = rnd() % 14;
        rand_wstr(a, 10);
        rand_wstr(b, 10);
        to_wide32(a, wa);
        to_wide32(b, wb);
        MT_EXPECT_EQ(ucrt_wcslen(a), wcslen(wa));
        MT_EXPECT_EQ(sign(ucrt_wcscmp(a, b)), sign(wcscmp(wa, wb)));
        MT_EXPECT_EQ(sign(ucrt_wcsncmp(a, b, n)), sign(wcsncmp(wa, wb, n)));
        MT_EXPECT_EQ(ucrt_wcsspn(a, b), wcsspn(wa, wb));
        MT_EXPECT_EQ(ucrt_wcscspn(a, b), wcscspn(wa, wb));
        {
            ucrt_wchar *p = ucrt_wcsstr(a, b);
            wchar_t *q = wcsstr(wa, wb);
            MT_EXPECT((p == NULL) == (q == NULL));
            if (p && q)
                MT_EXPECT((size_t)(p - a) == (size_t)(q - wa));
        }
        {
            int ch = "ab\xE9"[rnd() % 3] == 0 ? 0 : (int)(wa[0] ? wa[0] : 'a');
            ucrt_wchar *p = ucrt_wcschr(a, (ucrt_wchar)ch);
            wchar_t *q = wcschr(wa, (wchar_t)ch);
            MT_EXPECT((p == NULL) == (q == NULL));
            if (p && q)
                MT_EXPECT((size_t)(p - a) == (size_t)(q - wa));
            p = ucrt_wcsrchr(a, (ucrt_wchar)ch);
            q = wcsrchr(wa, (wchar_t)ch);
            MT_EXPECT((p == NULL) == (q == NULL));
            if (p && q)
                MT_EXPECT((size_t)(p - a) == (size_t)(q - wa));
        }
        ucrt_wcscpy(c, a);
        ucrt_wcscat(c, b);
        wcscpy(wc, wa);
        wcscat(wc, wb);
        {
            size_t k, l = wcslen(wc);
            MT_EXPECT_EQ(ucrt_wcslen(c), l);
            for (k = 0; k <= l; ++k)
                MT_EXPECT_EQ(c[k], (ucrt_wchar)wc[k]);
        }
    }
    /* Case-insensitive compare folds ASCII only. */
    {
        static const ucrt_wchar x[] = {'A', 'b', 0xC9, 0}, y[] = {'a', 'B', 0xC9, 0}, z[] = {'a', 'B', 0xE9, 0};
        MT_EXPECT_EQ(ucrt_wcsicmp(x, y), 0);
        MT_EXPECT(ucrt_wcsicmp(x, z) != 0);
        MT_EXPECT_EQ(ucrt_wcsnicmp(x, z, 2), 0);
    }
    /* wcsncpy pads with zeros. */
    {
        static const ucrt_wchar src[] = {'h', 'i', 0};
        ucrt_wchar d[6] = {1, 1, 1, 1, 1, 1};
        ucrt_wcsncpy(d, src, 5);
        MT_EXPECT(d[0] == 'h' && d[1] == 'i' && d[2] == 0 && d[3] == 0 && d[4] == 0 && d[5] == 1);
    }
}

static void secure_string_functions(void)
{
    char d[8];
    ucrt_errno_t r;
    memset(&g_ucrt_state, 0, sizeof(g_ucrt_state));
    /* strcpy_s */
    MT_EXPECT_EQ(ucrt_strcpy_s(d, sizeof(d), "abc"), 0);
    MT_EXPECT(strcmp(d, "abc") == 0);
    MT_EXPECT_EQ(ucrt_strcpy_s(d, sizeof(d), "toolongstring"), UCRT_ERANGE);
    MT_EXPECT_EQ(d[0], 0); /* the destination is emptied */
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ERANGE);
    MT_EXPECT_EQ(ucrt_strcpy_s(d, sizeof(d), NULL), UCRT_EINVAL);
    MT_EXPECT_EQ(ucrt_strcpy_s(NULL, 8, "x"), UCRT_EINVAL);
    MT_EXPECT_EQ(ucrt_strcpy_s(d, 0, "x"), UCRT_EINVAL);
    MT_EXPECT_EQ(ucrt_strcpy_s(d, 4, "abc"), 0); /* exactly fits with the terminator */
    MT_EXPECT_EQ(ucrt_strcpy_s(d, 3, "abc"), UCRT_ERANGE);
    MT_EXPECT_EQ(g_ucrt_state.invalid_count, 5);
    /* strcat_s */
    strcpy(d, "ab");
    MT_EXPECT_EQ(ucrt_strcat_s(d, sizeof(d), "cd"), 0);
    MT_EXPECT(strcmp(d, "abcd") == 0);
    MT_EXPECT_EQ(ucrt_strcat_s(d, sizeof(d), "efgh"), UCRT_ERANGE);
    MT_EXPECT_EQ(d[0], 0);
    {
        char u[4] = {'a', 'b', 'c', 'd'}; /* not terminated within its size */
        MT_EXPECT_EQ(ucrt_strcat_s(u, sizeof(u), "x"), UCRT_EINVAL);
    }
    /* strncpy_s */
    MT_EXPECT_EQ(ucrt_strncpy_s(d, sizeof(d), "abcdef", 3), 0);
    MT_EXPECT(strcmp(d, "abc") == 0);
    MT_EXPECT_EQ(ucrt_strncpy_s(d, sizeof(d), "abcdefghij", (size_t)-1), 80); /* _TRUNCATE: STRUNCATE */
    MT_EXPECT(strcmp(d, "abcdefg") == 0);
    MT_EXPECT_EQ(ucrt_strncpy_s(d, sizeof(d), "ab", (size_t)-1), 0);
    MT_EXPECT_EQ(ucrt_strncpy_s(d, sizeof(d), "abcdefghij", 9), UCRT_ERANGE);
    MT_EXPECT_EQ(ucrt_strncpy_s(d, sizeof(d), "x", 0), 0);
    MT_EXPECT_EQ(d[0], 0);
    /* strncat_s */
    strcpy(d, "ab");
    MT_EXPECT_EQ(ucrt_strncat_s(d, sizeof(d), "cdef", 2), 0);
    MT_EXPECT(strcmp(d, "abcd") == 0);
    MT_EXPECT_EQ(ucrt_strncat_s(d, sizeof(d), "123456", (size_t)-1), 80);
    MT_EXPECT(strcmp(d, "abcd123") == 0);
    /* memcpy_s and memmove_s */
    {
        char dst[4] = "zzz", src[] = "abcdef";
        r = ucrt_memcpy_s(dst, sizeof(dst), src, 3);
        MT_EXPECT_EQ(r, 0);
        MT_EXPECT(memcmp(dst, "abc", 3) == 0);
        r = ucrt_memcpy_s(dst, sizeof(dst), src, 6);
        MT_EXPECT_EQ(r, UCRT_ERANGE);
        MT_EXPECT(memcmp(dst, "\0\0\0\0", 4) == 0); /* memcpy_s clears the destination on a range error */
        MT_EXPECT_EQ(ucrt_memcpy_s(dst, sizeof(dst), NULL, 2), UCRT_EINVAL);
        MT_EXPECT_EQ(ucrt_memcpy_s(NULL, 4, src, 2), UCRT_EINVAL);
        MT_EXPECT_EQ(ucrt_memcpy_s(NULL, 0, NULL, 0), 0);
        memcpy(dst, "wxyz", 4);
        MT_EXPECT_EQ(ucrt_memmove_s(dst, 4, dst + 1, 3), 0);
        MT_EXPECT(memcmp(dst, "xyzz", 4) == 0);
        MT_EXPECT_EQ(ucrt_memmove_s(dst, 2, src, 3), UCRT_ERANGE);
    }
    /* The wide secure functions behave alike. */
    {
        static const ucrt_wchar src[] = {'a', 'b', 'c', 0};
        ucrt_wchar d2[3];
        MT_EXPECT_EQ(ucrt_wcscpy_s(d2, 3, src), UCRT_ERANGE);
        MT_EXPECT_EQ(d2[0], 0);
        MT_EXPECT_EQ(ucrt_wcscpy_s(NULL, 3, src), UCRT_EINVAL);
    }
}

static int handler_calls;

static void count_calls(void)
{
    ++handler_calls;
}

static void invalid_parameter_handler_is_called(void)
{
    char d[2];
    handler_calls = 0;
    memset(&g_ucrt_state, 0, sizeof(g_ucrt_state));
    g_ucrt_state.invalid_handler = count_calls;
    ucrt_strcpy_s(d, sizeof(d), "toolong");
    ucrt_strcpy_s(NULL, 4, "x");
    MT_EXPECT_EQ(handler_calls, 2);
    g_ucrt_state.invalid_handler = NULL;
}

static void strerror_table(void)
{
    MT_EXPECT(strcmp(ucrt_strerror(0), "No error") == 0);
    MT_EXPECT(strcmp(ucrt_strerror(UCRT_ENOENT), "No such file or directory") == 0);
    MT_EXPECT(strcmp(ucrt_strerror(UCRT_EINVAL), "Invalid argument") == 0);
    MT_EXPECT(strcmp(ucrt_strerror(UCRT_ERANGE), "Result too large") == 0);
    MT_EXPECT(strcmp(ucrt_strerror(UCRT_EDOM), "Domain error") == 0);
    MT_EXPECT(strcmp(ucrt_strerror(UCRT_ENOMEM), "Not enough space") == 0);
    MT_EXPECT(strcmp(ucrt_strerror(UCRT_EILSEQ), "Illegal byte sequence") == 0);
    MT_EXPECT(strcmp(ucrt_strerror(-1), "Unknown error") == 0);
    MT_EXPECT(strcmp(ucrt_strerror(200), "Unknown error") == 0);
    MT_EXPECT(strcmp(ucrt_strerror(26), "Unknown error") == 0);
}

static const mt_case_t cases[] = {
    MT_CASE(narrow_basics),
    MT_CASE(narrow_searching),
    MT_CASE(narrow_tokenising),
    MT_CASE(narrow_case_and_reverse),
    MT_CASE(memory_functions),
    MT_CASE(wide_functions),
    MT_CASE(secure_string_functions),
    MT_CASE(invalid_parameter_handler_is_called),
    MT_CASE(strerror_table),
};

int main(void)
{
    return MT_RUN("crt_string", cases);
}
