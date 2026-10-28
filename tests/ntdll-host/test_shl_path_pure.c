/*
 * test_shl_path_pure.c - tests for src/ntdll/shl_path_pure.h. The expected values are Windows behaviour, including its quirks
 * (they are the results documented by the shlwapi conformance tests that were checked against Windows).
 */
#include "minitest.h"
#include "shl_path_pure.h"

#include <stdio.h>
#include <string.h>

#define BUF 1024

static void W(const char *s, shl_wchar *w)
{
    size_t i = 0;
    for (; s[i]; ++i)
        w[i] = (unsigned char)s[i];
    w[i] = 0;
}

static const char *N(const shl_wchar *w)
{
    static char ring[8][BUF];
    static int k;
    char *s = ring[k++ & 7];
    size_t i = 0;
    for (; w[i] && i < BUF - 1; ++i)
        s[i] = w[i] < 0x80 ? (char)w[i] : '?';
    s[i] = 0;
    return s;
}

#define STR_EQ(w, want) (strcmp(N(w), (want)) == 0)

static void canon(const char *in, const char *want)
{
    shl_wchar src[BUF], dst[BUF];
    W(in, src);
    MT_EXPECT(shl_canonicalize(dst, src));
    if (strcmp(N(dst), want) != 0)
        printf("    canonicalize('%s') = '%s', want '%s'\n", in, N(dst), want);
    MT_EXPECT(STR_EQ(dst, want));
}

static void canonicalizing(void)
{
    shl_wchar d[BUF];
    canon("", "\\");
    canon("C:\\", "C:\\");
    canon(" ", " ");
    canon("one\\two", "one\\two");
    canon("C:\\one\\.\\..\\two\\three\\..", "C:\\two");
    canon("C:\\one/two/three\\four/five\\six", "C:\\one/two/three\\four/five\\six"); /* a forward slash is part of a name */
    canon("C:\\one/.\\two", "C:\\one/.\\two");
    canon("../../one/two/", "../../one/two/");
    canon("../../one/two/\\*", "../../one/two/\\*");
    canon("C:\\one/.\\..\\two/three\\..\\four/.five", "C:\\four/.five");
    canon("C:", "C:\\"); /* a bare drive gets its root */
    canon("C:\\a\\b\\..\\..\\..", "C:\\");
    {
        /* ".." never removes the server name of a UNC path */
        shl_wchar src[BUF], dst[BUF];
        W("\\\\server\\share\\..\\..\\..\\x", src);
        MT_EXPECT(shl_canonicalize(dst, src));
        MT_EXPECT(strncmp(N(dst), "\\\\server", 8) == 0);
    }
    canon("a\\.\\b", "a\\b");
    MT_EXPECT(!shl_canonicalize(d, NULL));
    MT_EXPECT_EQ(d[0], 0);
    MT_EXPECT(!shl_canonicalize(NULL, d));
}

static void combining(void)
{
    shl_wchar d[BUF], a[BUF], b[BUF], big[BUF];
    size_t i;
    W("C:\\one", a);
    W("two\\three", b);
    MT_EXPECT(shl_combine(d, a, b) == d && STR_EQ(d, "C:\\one\\two\\three"));
    W("\\two", b); /* rooted in the directory's root */
    MT_EXPECT(shl_combine(d, a, b) == d && STR_EQ(d, "C:\\two"));
    W("D:\\x", b); /* absolute replaces */
    MT_EXPECT(shl_combine(d, a, b) == d && STR_EQ(d, "D:\\x"));
    W("\\\\srv\\sh", b); /* UNC replaces */
    MT_EXPECT(shl_combine(d, a, b) == d && STR_EQ(d, "\\\\srv\\sh"));
    W("", b);
    MT_EXPECT(shl_combine(d, a, b) == d && STR_EQ(d, "C:\\one")); /* empty file: just dir */
    W("..\\x", b);
    MT_EXPECT(shl_combine(d, a, b) == d && STR_EQ(d, "C:\\x"));
    MT_EXPECT(shl_combine(d, NULL, NULL) == NULL && d[0] == 0);
    MT_EXPECT(shl_combine(NULL, a, b) == NULL);
    /* in place */
    W("C:\\", a);
    W("AA", b);
    MT_EXPECT(shl_combine(a, a, b) == a && STR_EQ(a, "C:\\AA"));
    /* too long */
    W("C:\\", a);
    for (i = 3; i < 130; ++i)
        a[i] = 'A';
    a[130] = 0;
    for (i = 0; i < 131; ++i)
        big[i] = 'B';
    big[131] = 0;
    MT_EXPECT(shl_combine(d, a, big) == NULL && d[0] == 0);
}

static void appending(void)
{
    shl_wchar p[BUF], m[BUF];
    W("C:\\one", p);
    W("", m);
    MT_EXPECT(shl_append(p, m) && STR_EQ(p, "C:\\one"));
    MT_EXPECT(!shl_append(p, NULL) && STR_EQ(p, "C:\\one"));
    MT_EXPECT(!shl_append(NULL, m));
    p[0] = 0;
    W("two\\three", m);
    MT_EXPECT(shl_append(p, m) && STR_EQ(p, "two\\three"));
    p[0] = 0;
    W("", m);
    MT_EXPECT(shl_append(p, m) && STR_EQ(p, "\\"));
    W("C:\\one\\", p);
    W("two\\three", m);
    MT_EXPECT(shl_append(p, m) && STR_EQ(p, "C:\\one\\two\\three"));
    W("C:\\one", p);
    W("\\two\\three", m); /* leading backslash dropped */
    MT_EXPECT(shl_append(p, m) && STR_EQ(p, "C:\\one\\two\\three"));
    W("C:\\one", p);
    W("two\\three\\", m);
    MT_EXPECT(shl_append(p, m) && STR_EQ(p, "C:\\one\\two\\three\\"));
    W("C: \\ one ", p);
    W("two\\three", m);
    MT_EXPECT(shl_append(p, m) && STR_EQ(p, "C: \\ one \\two\\three"));
    W("C:\\one", p);
    W(" two \\ three ", m);
    MT_EXPECT(shl_append(p, m) && STR_EQ(p, "C:\\one\\ two \\ three "));
}

static void backslashes(void)
{
    shl_wchar p[BUF];
    shl_wchar *end;
    p[0] = 0;
    end = shl_add_backslash(p);
    MT_EXPECT(end == p + 0 && p[0] == 0);
    W("one\\two", p);
    end = shl_add_backslash(p);
    MT_EXPECT(end == p + shl_len(p) && STR_EQ(p, "one\\two\\"));
    W("C:\\one\\", p);
    end = shl_add_backslash(p);
    MT_EXPECT(end == p + shl_len(p) && STR_EQ(p, "C:\\one\\"));
    W(" ", p);
    shl_add_backslash(p);
    MT_EXPECT(STR_EQ(p, " \\"));
    MT_EXPECT(shl_add_backslash(NULL) == NULL);
    {
        shl_wchar big[BUF];
        size_t i;
        for (i = 0; i < 400; ++i)
            big[i] = 'a';
        big[400] = 0;
        MT_EXPECT(shl_add_backslash(big) == NULL);
    }
    /* removing: not from a root */
    W("C:\\one\\", p);
    shl_remove_backslash(p);
    MT_EXPECT(STR_EQ(p, "C:\\one"));
    W("C:\\", p);
    shl_remove_backslash(p);
    MT_EXPECT(STR_EQ(p, "C:\\"));
    W("\\", p);
    shl_remove_backslash(p);
    MT_EXPECT(STR_EQ(p, "\\"));
    W("\\\\server\\share\\", p);
    shl_remove_backslash(p);
    MT_EXPECT(STR_EQ(p, "\\\\server\\share"));
}

static void parts_of_a_path(void)
{
    shl_wchar p[BUF], e[BUF];
    W("file.txt.doc", p);
    MT_EXPECT(STR_EQ(shl_find_extension(p), ".doc"));
    W("file", p);
    MT_EXPECT(shl_find_extension(p) == p + 4);
    p[0] = 0;
    MT_EXPECT(shl_find_extension(p) == p);
    W("a.b\\c", p);
    MT_EXPECT(shl_find_extension(p) == p + 5); /* the dot is before the last backslash */
    W("foo.txt bar", p);
    MT_EXPECT(shl_find_extension(p) == p + 11); /* a space restarts the search */
    W("foo bar.txt", p);
    MT_EXPECT(STR_EQ(shl_find_extension(p), ".txt"));
    MT_EXPECT(shl_find_extension(NULL) == NULL);

    W("C:\\dir\\name.ext", p);
    MT_EXPECT(STR_EQ(shl_find_file_name(p), "name.ext"));
    W("C:/dir/name.ext", p);
    MT_EXPECT(STR_EQ(shl_find_file_name(p), "name.ext"));
    W("C:name", p);
    MT_EXPECT(STR_EQ(shl_find_file_name(p), "name"));
    W("name", p);
    MT_EXPECT(shl_find_file_name(p) == p);
    W("C:\\dir\\", p);
    MT_EXPECT(STR_EQ(shl_find_file_name(p), "dir\\"));

    W("C:\\dir\\name.ext", p);
    shl_strip_path(p);
    MT_EXPECT(STR_EQ(p, "name.ext"));

    W("a b.exe /x /y", p);
    MT_EXPECT(STR_EQ(shl_get_args(p), "b.exe /x /y")); /* the first unquoted space */
    W("\"a b.exe\" /x", p);
    MT_EXPECT(STR_EQ(shl_get_args(p), "/x"));
    W("noargs", p);
    MT_EXPECT(shl_get_args(p) == p + 6);
    W("\"a b.exe\" /x", p);
    shl_remove_args(p);
    MT_EXPECT(STR_EQ(p, "\"a b.exe\""));
    W("prog ", p);
    shl_remove_args(p);
    MT_EXPECT(STR_EQ(p, "prog"));

    W("a\\b\\c", p);
    MT_EXPECT(STR_EQ(shl_find_next_component(p), "b\\c"));
    W("c", p);
    MT_EXPECT(shl_find_next_component(p) == p + 1);
    W("\\\\srv\\x", p);
    MT_EXPECT(STR_EQ(shl_find_next_component(p), "srv\\x"));
    p[0] = 0;
    MT_EXPECT(shl_find_next_component(p) == NULL);

    W("C:\\a", p);
    MT_EXPECT(STR_EQ(shl_skip_root(p), "a"));
    W("\\\\srv\\share\\dir", p);
    MT_EXPECT(STR_EQ(shl_skip_root(p), "dir"));
    W("relative", p);
    MT_EXPECT(shl_skip_root(p) == NULL);
    (void)e;
}

static void edits(void)
{
    shl_wchar p[BUF], x[BUF];
    W("name", p);
    W(".txt", x);
    MT_EXPECT(shl_add_extension(p, x) && STR_EQ(p, "name.txt"));
    MT_EXPECT(!shl_add_extension(p, x)); /* already has one */
    W("name.old", p);
    W(".new", x);
    MT_EXPECT(shl_rename_extension(p, x) && STR_EQ(p, "name.new"));
    shl_remove_extension(p);
    MT_EXPECT(STR_EQ(p, "name"));
    W("C:\\dir\\file.ext", p);
    MT_EXPECT(shl_remove_file_spec(p) && STR_EQ(p, "C:\\dir"));
    MT_EXPECT(shl_remove_file_spec(p) && STR_EQ(p, "C:\\"));
    MT_EXPECT(!shl_remove_file_spec(p) && STR_EQ(p, "C:\\")); /* nothing left to remove */
    W("C:\\file", p);
    MT_EXPECT(shl_remove_file_spec(p) && STR_EQ(p, "C:\\"));
    W("file", p);
    MT_EXPECT(shl_remove_file_spec(p) && STR_EQ(p, ""));
    W("\\\\srv\\share\\dir\\f", p);
    MT_EXPECT(shl_remove_file_spec(p) && STR_EQ(p, "\\\\srv\\share\\dir"));
    p[0] = 0;
    MT_EXPECT(!shl_remove_file_spec(p));

    W("C:\\a\\b\\c", p);
    MT_EXPECT(shl_strip_to_root(p) && STR_EQ(p, "C:\\"));
    W("\\\\srv\\share\\a", p);
    MT_EXPECT(shl_strip_to_root(p) && STR_EQ(p, "\\\\srv\\share"));

    {
        static const struct {
            const char *in, *out;
        } unq[] = {{"abcdef", "abcdef"}, {"\"abcdef\"", "abcdef"}, {"\"abcdef", "\"abcdef"}, {"abcdef\"", "abcdef\""},
                   {"\"\"abcdef\"\"", "\"abcdef\""}, {"abc\"def", "abc\"def"}, {"\"abc\"def", "\"abc\"def"},
                   {"\"abc\"def\"", "abc\"def"}, {"\'abcdef\'", "\'abcdef\'"}, {"\"\"", ""}, {"\"", ""}};
        size_t i;
        for (i = 0; i < sizeof unq / sizeof unq[0]; ++i) {
            W(unq[i].in, p);
            shl_unquote_spaces(p);
            if (!STR_EQ(p, unq[i].out))
                printf("    unquote('%s') = '%s', want '%s'\n", unq[i].in, N(p), unq[i].out);
            MT_EXPECT(STR_EQ(p, unq[i].out));
        }
    }
    W("a b", p);
    shl_quote_spaces(p);
    MT_EXPECT(STR_EQ(p, "\"a b\""));
    W("ab", p);
    shl_quote_spaces(p);
    MT_EXPECT(STR_EQ(p, "ab"));
    W("  ab c  ", p);
    shl_remove_blanks(p);
    MT_EXPECT(STR_EQ(p, "ab c"));
    W("   ", p);
    shl_remove_blanks(p);
    MT_EXPECT(STR_EQ(p, ""));
}

static void tests_of_a_path(void)
{
    shl_wchar p[BUF], q[BUF];
    int i;
    W("relative\\x", p);
    MT_EXPECT(shl_is_relative(p));
    W("\\x", p);
    MT_EXPECT(!shl_is_relative(p));
    W("C:x", p);
    MT_EXPECT(!shl_is_relative(p));
    p[0] = 0;
    MT_EXPECT(shl_is_relative(p));
    W("\\\\srv\\x", p);
    MT_EXPECT(shl_is_unc(p) && !shl_is_relative(p));
    W("C:\\", p);
    MT_EXPECT(shl_is_root(p));
    W("C:\\a", p);
    MT_EXPECT(!shl_is_root(p));
    W("\\", p);
    MT_EXPECT(shl_is_root(p));
    W("\\\\srv\\share", p);
    MT_EXPECT(shl_is_root(p));
    W("\\\\srv\\share\\x", p);
    MT_EXPECT(!shl_is_root(p));
    W("\\\\srv", p);
    MT_EXPECT(shl_is_unc_server(p) && !shl_is_unc_server_share(p));
    W("\\\\srv\\share", p);
    MT_EXPECT(!shl_is_unc_server(p) && shl_is_unc_server_share(p));
    W("name.ext", p);
    MT_EXPECT(shl_is_file_spec(p));
    W("a\\b", p);
    MT_EXPECT(!shl_is_file_spec(p));
    W("C:x", p);
    MT_EXPECT(!shl_is_file_spec(p));

    W("C:\\x", p);
    MT_EXPECT_EQ(shl_drive_number(p), 2);
    W("a:", p);
    MT_EXPECT_EQ(shl_drive_number(p), 0);
    W("\\\\?\\D:\\x", p);
    MT_EXPECT_EQ(shl_drive_number(p), 3);
    W("1:\\x", p);
    MT_EXPECT_EQ(shl_drive_number(p), -1);
    W("x", p);
    MT_EXPECT_EQ(shl_drive_number(p), -1);

    for (i = 0; i < 26; ++i) {
        shl_build_root(p, i);
        q[0] = (shl_wchar)('A' + i);
        q[1] = ':';
        q[2] = '\\';
        q[3] = 0;
        MT_EXPECT(shl_str_cmp(p, q, 0) == 0);
    }
    W("aaaaaaaaa", p);
    shl_build_root(p, -1);
    MT_EXPECT(p[0] == 0);
    W("aaaaaaaaa", p);
    shl_build_root(p, 26);
    MT_EXPECT(p[0] == 0);
    MT_EXPECT(shl_build_root(NULL, 0) == NULL);
}

static void common_prefix(void)
{
    shl_wchar a[BUF], b[BUF], out[BUF];
    W("C:\\", a);
    W("C:\\", b);
    MT_EXPECT_EQ(shl_common_prefix(a, b, out), 3);
    MT_EXPECT(STR_EQ(out, "C:\\"));
    W("C:\\one\\two", a);
    W("C:\\one\\three", b);
    MT_EXPECT_EQ(shl_common_prefix(a, b, out), 6);
    MT_EXPECT(STR_EQ(out, "C:\\one"));
    W("one\\.two", a);
    W("one\\.three", b);
    MT_EXPECT_EQ(shl_common_prefix(a, b, out), 3);
    MT_EXPECT(STR_EQ(out, "one"));
    W("one\\.\\two", a);
    W("one\\.\\three", b);
    MT_EXPECT_EQ(shl_common_prefix(a, b, out), 5);
    MT_EXPECT(STR_EQ(out, "one\\."));
    W("one\\...\\two", a);
    W("one\\...\\three", b);
    MT_EXPECT_EQ(shl_common_prefix(a, b, out), 7);
    W("same", a);
    W("same", b);
    MT_EXPECT_EQ(shl_common_prefix(a, b, out), 4);
    out[0] = 'x';
    a[0] = 0;
    W("C:\\", b);
    MT_EXPECT_EQ(shl_common_prefix(a, b, out), 0);
    MT_EXPECT(out[0] == 0);
    MT_EXPECT_EQ(shl_common_prefix(NULL, b, out), 0);
    W("\\\\srv\\a", a);
    W("C:\\a", b);
    MT_EXPECT_EQ(shl_common_prefix(a, b, out), 0); /* UNC against drive */
    W("c:\\ONE\\x", a);
    W("C:\\one\\y", b);
    MT_EXPECT_EQ(shl_common_prefix(a, b, out), 6); /* case-insensitive; the text comes from the first */
    MT_EXPECT(STR_EQ(out, "c:\\ONE"));

    W("C:\\one\\two", a);
    W("C:\\one", b);
    MT_EXPECT(shl_is_prefix(b, a));
    W("C:\\on", b);
    MT_EXPECT(!shl_is_prefix(b, a));
    W("C:\\a\\x", a);
    W("C:\\b\\y", b);
    MT_EXPECT(shl_is_same_root(a, b));
    W("D:\\a", b);
    MT_EXPECT(!shl_is_same_root(a, b));
}

static void match_spec(void)
{
    shl_wchar f[BUF], m[BUF];
    static const struct {
        const char *mask;
        int want;
    } t[] = {{".ext", 0},      {"*.ext", 1},       {"*.ext ", 0},   {"  *.ext", 1}, {"* . ext", 0},  {"*.e?t", 1},
             {"filename.ext", 0}, {"*bar\\filename.ext", 1}, {" foo; *.ext", 1}, {"*.ext;*.bar", 1}, {"*bar*", 1},
             {"*.*", 1},        {"*.txt", 0},      {"", 0},          {"*", 1}};
    size_t i;
    W("c:\\foo\\bar\\filename.ext", f);
    for (i = 0; i < sizeof t / sizeof t[0]; ++i) {
        W(t[i].mask, m);
        if (shl_match_spec(f, m) != t[i].want)
            printf("    match '%s' -> %d, want %d\n", t[i].mask, shl_match_spec(f, m), t[i].want);
        MT_EXPECT_EQ(shl_match_spec(f, m), t[i].want);
    }
    W("noext", f);
    W("*.*", m);
    MT_EXPECT(shl_match_spec(f, m)); /* "*.*" is special: everything */
}

static void char_types(void)
{
    MT_EXPECT_EQ(shl_char_type('a'), SHL_GCT_SHORTCHAR | SHL_GCT_LFNCHAR);
    MT_EXPECT_EQ(shl_char_type('?'), SHL_GCT_WILD);
    MT_EXPECT_EQ(shl_char_type('*'), SHL_GCT_WILD);
    MT_EXPECT_EQ(shl_char_type('\\'), SHL_GCT_SEPARATOR);
    MT_EXPECT_EQ(shl_char_type(':'), SHL_GCT_SEPARATOR);
    MT_EXPECT_EQ(shl_char_type('<'), SHL_GCT_INVALID);
    MT_EXPECT_EQ(shl_char_type('/'), SHL_GCT_INVALID);
    MT_EXPECT_EQ(shl_char_type(7), SHL_GCT_INVALID);
    MT_EXPECT_EQ(shl_char_type(' '), SHL_GCT_LFNCHAR);
    MT_EXPECT_EQ(shl_char_type(0x4E2D), SHL_GCT_SHORTCHAR | SHL_GCT_LFNCHAR);
}

static void strings(void)
{
    shl_wchar a[BUF], b[BUF];
    int64_t v;
    W("Hello World", a);
    W("WORLD", b);
    MT_EXPECT(shl_str_str(a, b, 0) == NULL);
    MT_EXPECT(shl_str_str(a, b, 1) == a + 6);
    W("o", b);
    MT_EXPECT(shl_str_chr_i(a, 'O') == a + 4);
    MT_EXPECT(shl_str_rchr(a, NULL, 'o') == a + 7);
    MT_EXPECT(shl_str_rchr(a, a + 6, 'o') == a + 4); /* the end bound is exclusive */
    MT_EXPECT(shl_str_rchr_i(a, NULL, 'L') == a + 9);
    W("o w", b);
    MT_EXPECT(shl_str_rstr_i(a, b) == a + 4);
    MT_EXPECT_EQ(shl_str_cmp(a, a, 0), 0);
    W("hello world", b);
    MT_EXPECT(shl_str_cmp(a, b, 0) < 0);
    MT_EXPECT_EQ(shl_str_cmp(a, b, 1), 0);
    MT_EXPECT_EQ(shl_str_cmp_n(a, b, 5, 1), 0);
    MT_EXPECT(shl_str_cmp_n(a, b, 1, 0) < 0);
    W("abc123", a);
    W("abc", b);
    MT_EXPECT_EQ(shl_str_spn(a, b), 3);
    W("123", b);
    MT_EXPECT_EQ(shl_str_cspn(a, b, 0), 3);
    MT_EXPECT(shl_str_pbrk(a, b) == a + 3);

    W("  -42abc", a);
    MT_EXPECT(shl_str_to_int64(a, 0, &v) && v == -42);
    W("+7", a);
    MT_EXPECT(shl_str_to_int64(a, 0, &v) && v == 7);
    W("0x1F", a);
    MT_EXPECT(shl_str_to_int64(a, 1, &v) && v == 31);
    MT_EXPECT(shl_str_to_int64(a, 0, &v) && v == 0); /* decimal stops at the x */
    W("abc", a);
    MT_EXPECT(!shl_str_to_int64(a, 0, &v));
    W("-", a);
    MT_EXPECT(!shl_str_to_int64(a, 0, &v));
    W("9223372036854775807", a);
    MT_EXPECT(shl_str_to_int64(a, 0, &v) && v == INT64_MAX);

    W("xx hello xx", a);
    W("x ", b);
    MT_EXPECT(shl_str_trim(a, b) && STR_EQ(a, "hello"));
    MT_EXPECT(!shl_str_trim(a, b));
    W("xxxx", a);
    W("x", b);
    MT_EXPECT(shl_str_trim(a, b) && STR_EQ(a, ""));
    MT_EXPECT_EQ(shl_upper(0xE9), 0xC9);
    MT_EXPECT_EQ(shl_upper(0x3B1), 0x391);
    MT_EXPECT_EQ(shl_upper(0x44F), 0x42F);
    MT_EXPECT_EQ(shl_lower(0x42F), 0x44F);
}

static const mt_case_t cases[] = {
    MT_CASE(canonicalizing), MT_CASE(combining), MT_CASE(appending), MT_CASE(backslashes), MT_CASE(parts_of_a_path),
    MT_CASE(edits), MT_CASE(tests_of_a_path), MT_CASE(common_prefix), MT_CASE(match_spec), MT_CASE(char_types), MT_CASE(strings),
};

int main(void)
{
    return MT_RUN("shl_path_pure", cases);
}
