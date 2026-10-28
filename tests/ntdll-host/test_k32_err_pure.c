/*
 * test_k32_err_pure.c - host tests for src/ntdll/k32_err_pure.h: the message table, and the FormatMessage engine on
 * inserts, printf specifications, line breaking and the special sequences.
 */
#include "minitest.h"
#include "k32_err_pure.h"

#include <string.h>

static size_t widen(const char *s, WCHAR *w)
{
    size_t n = 0;
    while (s[n]) {
        w[n] = (WCHAR)(unsigned char)s[n];
        ++n;
    }
    w[n] = 0;
    return n;
}

static void narrow(const k32e_out *o, char *out)
{
    size_t n = o->n < o->cap ? o->n : o->cap;
    for (size_t i = 0; i < n; ++i)
        out[i] = (char)o->buf[i];
    out[n] = 0;
}

static DWORD run(const char *fmt, DWORD flags, const ULONG_PTR *args, size_t nargs, int narrow_inserts, char *out, int *stopped)
{
    WCHAR f[512], buf[1024];
    k32e_out o;
    DWORD err;
    widen(fmt, f);
    o.buf = buf;
    o.cap = 1024;
    o.n = 0;
    o.overflow = 0;
    o.line = 0;
    err = k32e_format(&o, f, flags, args, nargs, narrow_inserts, stopped);
    narrow(&o, out);
    return err;
}

static void table_is_sorted_and_found(void)
{
    for (size_t i = 1; i < K32E_TABLE_COUNT; ++i)
        MT_ASSERT(k32e_table[i - 1].code < k32e_table[i].code);
    MT_EXPECT(k32e_lookup(2) && strcmp(k32e_lookup(2), "The system cannot find the file specified.") == 0);
    MT_EXPECT(strcmp(k32e_lookup(5), "Access is denied.") == 0);
    MT_EXPECT(strcmp(k32e_lookup(87), "The parameter is incorrect.") == 0);
    MT_EXPECT(k32e_lookup(0xDEAD) == NULL);
}

static void plain_text_and_inserts(void)
{
    char out[1024];
    int stopped;
    ULONG_PTR args[3];
    MT_EXPECT_EQ((long)run("Hello world", 0, NULL, 0, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "Hello world") == 0 && !stopped);
    args[0] = (ULONG_PTR)"Bob";
    args[1] = (ULONG_PTR)"Alice";
    MT_EXPECT_EQ((long)run("%1 meets %2 and %1 again", 0, args, 2, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "Bob meets Alice and Bob again") == 0);
    MT_EXPECT_EQ((long)run("%3", 0, args, 2, 1, out, &stopped), 87); /* no third insert */
    args[0] = 42;
    args[1] = 255;
    MT_EXPECT_EQ((long)run("%1!d! is %2!x! and %2!04X!", 0, args, 2, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "42 is ff and 00FF") == 0);
    args[0] = (ULONG_PTR)"txt";
    MT_EXPECT_EQ((long)run("[%1!10s!][%1!-6s!]", 0, args, 1, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "[       txt][txt   ]") == 0);
    /* a null string insert */
    args[0] = 0;
    MT_EXPECT_EQ((long)run("<%1>", 0, args, 1, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "<(null)>") == 0);
    /* a wide insert from the wide function */
    {
        static const WCHAR ws[] = {'w', 'i', 'd', 'e', 0};
        args[0] = (ULONG_PTR)ws;
        MT_EXPECT_EQ((long)run("%1", 0, args, 1, 0, out, &stopped), 0);
        MT_EXPECT(strcmp(out, "wide") == 0);
        MT_EXPECT_EQ((long)run("%1!s!", 0, args, 1, 0, out, &stopped), 0);
        MT_EXPECT(strcmp(out, "wide") == 0);
    }
}

static void special_sequences(void)
{
    char out[1024];
    int stopped;
    MT_EXPECT_EQ((long)run("a%nb%tc%rd%%e%!f%.g%h", 0, NULL, 0, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "a\r\nb\tc\rd%e!f.ghh") != 0); /* %h is not special: the letter */
    MT_EXPECT(strncmp(out, "a\r\nb\tc\rd%e!f.g", 14) == 0);
    MT_EXPECT_EQ((long)run("done%0 and never seen", 0, NULL, 0, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "done") == 0 && stopped == 1);
    /* IGNORE_INSERTS leaves the sequences as they are */
    MT_EXPECT_EQ((long)run("keep %1 and %2!d!", K32E_FORMAT_MESSAGE_IGNORE_INSERTS, NULL, 0, 1, out, &stopped), 0);
    MT_EXPECT(strncmp(out, "keep %1 and %2", 14) == 0);
}

static void line_breaks_and_width(void)
{
    char out[1024];
    int stopped;
    /* width 0: the source's breaks become spaces */
    MT_EXPECT_EQ((long)run("one\r\ntwo\nthree", 0, NULL, 0, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "one two three") == 0);
    /* 255: kept */
    MT_EXPECT_EQ((long)run("one\r\ntwo", 0xFF, NULL, 0, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "one\r\ntwo") == 0);
    /* a width of 10 breaks at the first space after the line reaches it */
    MT_EXPECT_EQ((long)run("aaaa bbbb cccc dddd", 10, NULL, 0, 1, out, &stopped), 0);
    MT_EXPECT(strcmp(out, "aaaa bbbb\r\ncccc dddd") == 0);
}

static void output_limit(void)
{
    WCHAR f[64], buf[4];
    k32e_out o;
    int stopped;
    widen("abcdefgh", f);
    o.buf = buf;
    o.cap = 4;
    o.n = 0;
    o.overflow = 0;
    o.line = 0;
    MT_EXPECT_EQ((long)k32e_format(&o, f, 0, NULL, 0, 1, &stopped), 0);
    MT_EXPECT(o.overflow && o.n == 8); /* the length needed is known */
}

static const mt_case_t cases[] = {
    MT_CASE(table_is_sorted_and_found), MT_CASE(plain_text_and_inserts), MT_CASE(special_sequences), MT_CASE(line_breaks_and_width), MT_CASE(output_limit),
};

int main(void)
{
    return MT_RUN("k32_err_pure", cases);
}
