/*
 * test_k32_w_pure.c - host tests for src/ntdll/k32_w_pure.h: the wildcard match with DOS wildcards, search path
 * splitting, the GetFullPathName and GetCurrentDirectory result conventions, MoveFileEx flag checks.
 */
#include "minitest.h"
#include "k32_w_pure.h"

#include <string.h>

static size_t wide(const char *s, WCHAR *w)
{
    size_t n = 0;
    while (s[n]) {
        w[n] = (WCHAR)(unsigned char)s[n];
        ++n;
    }
    w[n] = 0;
    return n;
}

static int match(const char *pat, const char *name)
{
    WCHAR p[300], n[300];
    size_t pn = wide(pat, p), nn = wide(name, n);
    return k32w_wild_match(p, pn, n, nn);
}

static void star_and_question(void)
{
    MT_EXPECT(match("*", "") && match("*", "x.y") && match("a*", "a") && match("a*c", "abbbc"));
    MT_EXPECT(match("a?c", "abc") && !match("a?c", "ac") && !match("a?c", "abbc"));
    MT_EXPECT(match("*.txt", "notes.txt") && !match("*.txt", "notes.doc") && !match("*.txt", "txt"));
    MT_EXPECT(match("abc", "ABC") && match("ABC", "abc") && !match("abc", "abcd") && !match("abcd", "abc"));
    MT_EXPECT(match("*a*b", "xaxxab") && !match("*a*b", "xaxxa") && match("a*a*a", "aaaa"));
    MT_EXPECT(match("*?*", "x") && !match("*?*", ""));
}

static void unicode_case(void)
{
    WCHAR p[2] = {0x00E9, 0}, n[2] = {0x00C9, 0};
    MT_EXPECT(k32w_wild_match(p, 1, n, 1)); /* é matches É */
    p[0] = 0x0436;
    n[0] = 0x0416; /* Cyrillic zhe */
    MT_EXPECT(k32w_wild_match(p, 1, n, 1));
    n[0] = 'x';
    MT_EXPECT(!k32w_wild_match(p, 1, n, 1));
}

static void dos_wildcards(void)
{
    /* '<' is a star that does not take the last dot: "<.txt" is the DOS "*.txt" */
    MT_EXPECT(match("<.txt", "a.txt") && match("<.txt", "a.b.txt") && !match("<.txt", "a.txtx"));
    MT_EXPECT(match("<", "abc") && match("<", ""));
    /* '>' is one non-dot character or nothing */
    MT_EXPECT(match("a>>.txt", "a.txt") && match("a>>.txt", "ab.txt") && match("a>>.txt", "abc.txt") && !match("a>>.txt", "abcd.txt"));
    /* '"' is a dot or the end */
    MT_EXPECT(match("abc\"", "abc") && match("abc\"", "abc.") && !match("abc\"", "abcd"));
}

static void pattern_normalise(void)
{
    WCHAR in[8], out[8];
    size_t n = wide("*.*", in);
    MT_EXPECT_EQ((long)k32w_pattern_normalise(in, n, out, 8), 1);
    MT_EXPECT(out[0] == '*');
    n = wide("*.c", in);
    MT_EXPECT_EQ((long)k32w_pattern_normalise(in, n, out, 8), 3);
    MT_EXPECT(out[1] == '.' && out[2] == 'c');
}

static void split_and_wildcard_detection(void)
{
    WCHAR w[64];
    size_t n = wide("C:\\tmp\\*.txt", w);
    MT_EXPECT_EQ((long)k32w_dir_len(w, n), 7);
    n = wide("C:/a/b", w);
    MT_EXPECT_EQ((long)k32w_dir_len(w, n), 5);
    n = wide("*.txt", w);
    MT_EXPECT_EQ((long)k32w_dir_len(w, n), 0);
    n = wide("C:*.txt", w);
    MT_EXPECT_EQ((long)k32w_dir_len(w, n), 2);
    n = wide("C:\\x\\y", w);
    MT_EXPECT(!k32w_has_wildcard(w, n));
    n = wide("C:\\x\\y?", w);
    MT_EXPECT(k32w_has_wildcard(w, n));
    MT_EXPECT(k32w_is_dot_entry(w, 0) == 0);
    n = wide("..", w);
    MT_EXPECT(k32w_is_dot_entry(w, n));
    n = wide("...", w);
    MT_EXPECT(!k32w_is_dot_entry(w, n));
}

static void full_path_result(void)
{
    WCHAR full[64], buf[64];
    WCHAR *part = (WCHAR *)1;
    size_t n = wide("C:\\dir\\file.txt", full);
    MT_EXPECT_EQ((long)k32w_full_path_result(full, n, buf, 64, &part), 15);
    MT_EXPECT(part == buf + 7 && part[0] == 'f');
    /* too small: the size needed including the NUL, nothing written */
    buf[0] = 'Z';
    MT_EXPECT_EQ((long)k32w_full_path_result(full, n, buf, 15, &part), 16);
    MT_EXPECT(buf[0] == 'Z' && part == NULL);
    MT_EXPECT_EQ((long)k32w_full_path_result(full, n, buf, 16, &part), 15);
    /* a trailing separator and a bare root have no file part */
    n = wide("C:\\dir\\", full);
    k32w_full_path_result(full, n, buf, 64, &part);
    MT_EXPECT(part == NULL);
    n = wide("C:\\", full);
    k32w_full_path_result(full, n, buf, 64, &part);
    MT_EXPECT(part == NULL);
    n = wide("C:\\name", full);
    k32w_full_path_result(full, n, buf, 64, &part);
    MT_EXPECT(part == buf + 3);
    /* UNC: the share is part of the root */
    n = wide("\\\\srv\\share\\f", full);
    k32w_full_path_result(full, n, buf, 64, &part);
    MT_EXPECT(part == buf + 12);
    n = wide("\\\\srv\\share", full);
    k32w_full_path_result(full, n, buf, 64, &part);
    MT_EXPECT(part == NULL);
}

static void current_directory_forms(void)
{
    WCHAR stored[64], out[64], buf[64];
    size_t n = wide("C:\\work\\", stored);
    MT_EXPECT_EQ((long)k32w_curdir_reported_len(stored, n), 7);
    MT_EXPECT_EQ((long)k32w_curdir_result(stored, n, buf, 64), 7);
    MT_EXPECT(buf[6] == 'k' && buf[7] == 0);
    MT_EXPECT_EQ((long)k32w_curdir_result(stored, n, buf, 7), 8); /* needs 7 + NUL */
    n = wide("C:\\", stored);
    MT_EXPECT_EQ((long)k32w_curdir_reported_len(stored, n), 3); /* a root keeps its backslash */
    n = wide("C:\\a", stored);
    MT_EXPECT_EQ((long)k32w_curdir_stored(stored, n, out, 64), 5);
    MT_EXPECT(out[4] == '\\' && out[5] == 0);
    n = wide("C:\\a\\", stored);
    MT_EXPECT_EQ((long)k32w_curdir_stored(stored, n, out, 64), 5);
    MT_EXPECT_EQ((long)k32w_curdir_stored(stored, n, out, 5), 0);
}

static void move_flags_and_misc(void)
{
    MT_EXPECT_EQ((long)k32w_move_flags_check(0), 0);
    MT_EXPECT_EQ((long)k32w_move_flags_check(K32W_MOVEFILE_REPLACE_EXISTING | K32W_MOVEFILE_COPY_ALLOWED), 0);
    MT_EXPECT_EQ((long)k32w_move_flags_check(0x100), 87);
    MT_EXPECT_EQ((long)k32w_move_flags_check(K32W_MOVEFILE_DELAY_UNTIL_REBOOT | K32W_MOVEFILE_COPY_ALLOWED), 87);
    {
        DWORD lo, hi;
        k32w_split64(0x0000000500000007ull, &lo, &hi);
        MT_EXPECT(lo == 7 && hi == 5);
    }
    MT_EXPECT_EQ((long)k32w_drive_bit('C'), 4);
    MT_EXPECT_EQ((long)k32w_drive_bit('a'), 1);
    MT_EXPECT_EQ((long)k32w_drive_bit('1'), 0);
    {
        WCHAR root[4], p[16];
        size_t n = wide("d:\\x\\y", p);
        MT_EXPECT(k32w_drive_root(p, n, root) && root[0] == 'D' && root[2] == '\\' && root[3] == 0);
        n = wide("\\\\srv\\s", p);
        MT_EXPECT(!k32w_drive_root(p, n, root));
    }
}

static const mt_case_t cases[] = {
    MT_CASE(star_and_question), MT_CASE(unicode_case), MT_CASE(dos_wildcards), MT_CASE(pattern_normalise),
    MT_CASE(split_and_wildcard_detection), MT_CASE(full_path_result), MT_CASE(current_directory_forms), MT_CASE(move_flags_and_misc),
};

int main(void)
{
    return MT_RUN("k32_w_pure", cases);
}
