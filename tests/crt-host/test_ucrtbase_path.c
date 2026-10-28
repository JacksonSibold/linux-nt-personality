/*
 * test_ucrtbase_path.c - tests for src/ntdll/ucrtbase_path_t.inc: _splitpath and _makepath with their _s forms, for char and
 * wide characters, on the cases of the Microsoft documentation and the corner cases (dots in directories, drive-relative
 * names, dotfiles, buffer sizes).
 */
#include "minitest.h"
#include <stdio.h>
#include <string.h>
#include "ucrt_runtime.h"

#define UCT_T char
#define UCT_P w__
#include "../../src/ntdll/ucrtbase_path_t.inc"
#undef UCT_T
#undef UCT_P
#define UCT_T ucrt_wchar
#define UCT_P w__w
#include "../../src/ntdll/ucrtbase_path_t.inc"
#undef UCT_T
#undef UCT_P

static void setup(void)
{
    g_ucrt_state.invalid_handler = NULL;
    g_ucrt_state.invalid_count = 0;
    ucrt_set_errno(0);
}

static void widen(const char *s, ucrt_wchar *w)
{
    while ((*w++ = (unsigned char)*s++) != 0)
        ;
}

static int wide_equals(const ucrt_wchar *w, const char *s)
{
    for (; *w && *s; ++w, ++s)
        if (*w != (unsigned char)*s)
            return 0;
    return !*w && !*s;
}

static void check_split(const char *path, const char *drive, const char *dir, const char *fname, const char *ext)
{
    char d[16], r[64], f[64], e[32];
    ucrt_wchar wp[128], wd[16], wr[64], wf[64], we[32];
    w__splitpath(path, d, r, f, e);
    if (strcmp(d, drive) || strcmp(r, dir) || strcmp(f, fname) || strcmp(e, ext))
        mt_fail(__FILE__, __LINE__, path);
    widen(path, wp);
    w__wsplitpath(wp, wd, wr, wf, we);
    if (!wide_equals(wd, drive) || !wide_equals(wr, dir) || !wide_equals(wf, fname) || !wide_equals(we, ext))
        mt_fail(__FILE__, __LINE__, path);
}

static void split_cases(void)
{
    setup();
    check_split("C:\\dir\\sub\\file.tar.gz", "C:", "\\dir\\sub\\", "file.tar", ".gz");
    check_split("/usr/lib/x.so", "", "/usr/lib/", "x", ".so");
    check_split("file", "", "", "file", "");
    check_split(".bashrc", "", "", "", ".bashrc");
    check_split("dir.d\\file", "", "dir.d\\", "file", "");
    check_split("C:file.txt", "C:", "", "file", ".txt");
    check_split("a\\b\\", "", "a\\b\\", "", "");
    check_split("name.", "", "", "name", ".");
    check_split("", "", "", "", "");
    check_split("C:\\", "C:", "\\", "", "");
    check_split("\\\\server\\share\\f.txt", "", "\\\\server\\share\\", "f", ".txt");
}

static void split_allows_null_outputs(void)
{
    char f[16];
    setup();
    w__splitpath("C:\\a\\b.txt", NULL, NULL, f, NULL);
    MT_EXPECT(strcmp(f, "b") == 0);
}

static void split_s_checks_sizes(void)
{
    char d[3], r[8], f[8], e[8];
    setup();
    MT_EXPECT_EQ(w__splitpath_s("C:\\ab\\cd.txt", d, 3, r, 8, f, 8, e, 8), 0);
    MT_EXPECT(strcmp(d, "C:") == 0 && strcmp(r, "\\ab\\") == 0 && strcmp(f, "cd") == 0 && strcmp(e, ".txt") == 0);
    /* a piece that does not fit empties every output and reports ERANGE */
    MT_EXPECT_EQ(w__splitpath_s("C:\\abcdefgh\\cd.txt", d, 3, r, 8, f, 8, e, 8), UCRT_ERANGE);
    MT_EXPECT(d[0] == 0 && r[0] == 0 && f[0] == 0 && e[0] == 0);
    /* a buffer without a size is a bad argument */
    MT_EXPECT_EQ(w__splitpath_s("a", d, 0, r, 8, f, 8, e, 8), UCRT_EINVAL);
    MT_EXPECT_EQ(w__splitpath_s(NULL, d, 3, r, 8, f, 8, e, 8), UCRT_EINVAL);
    /* every output may be left out */
    MT_EXPECT_EQ(w__splitpath_s("x.y", NULL, 0, NULL, 0, f, 8, NULL, 0), 0);
    MT_EXPECT(strcmp(f, "x") == 0);
}

static void make_cases(void)
{
    char p[64];
    ucrt_wchar wp[64], wd[4], wdir[16], wf[16], we[8];
    setup();
    w__makepath(p, "C", "\\dir", "file", "txt");
    MT_EXPECT(strcmp(p, "C:\\dir\\file.txt") == 0);
    w__makepath(p, "C:", "/dir/", "file", ".txt");
    MT_EXPECT(strcmp(p, "C:/dir/file.txt") == 0);
    w__makepath(p, NULL, NULL, "file", NULL);
    MT_EXPECT(strcmp(p, "file") == 0);
    w__makepath(p, "", "dir", "", "");
    MT_EXPECT(strcmp(p, "dir\\") == 0);
    widen("D", wd);
    widen("a\\b", wdir);
    widen("n", wf);
    widen("e", we);
    w__wmakepath(wp, wd, wdir, wf, we);
    MT_EXPECT(wide_equals(wp, "D:a\\b\\n.e"));
}

static void make_s_checks_size(void)
{
    char p[10];
    setup();
    MT_EXPECT_EQ(w__makepath_s(p, sizeof p, "C", "d", "f", "x"), 0); /* C:d\f.x = 7 */
    MT_EXPECT(strcmp(p, "C:d\\f.x") == 0);
    MT_EXPECT_EQ(w__makepath_s(p, 7, "C", "d", "f", "x"), UCRT_ERANGE); /* needs 8 with the terminator */
    MT_EXPECT_EQ(p[0], 0);
    MT_EXPECT_EQ(w__makepath_s(NULL, 10, "C", "d", "f", "x"), UCRT_EINVAL);
    MT_EXPECT_EQ(w__makepath_s(p, 0, "C", "d", "f", "x"), UCRT_EINVAL);
}

static void split_then_make_round_trips(void)
{
    static const char *paths[] = {"C:\\a\\b\\c.txt", "/x/y.z", "name.ext", "D:\\dir\\sub\\"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        char d[8], r[64], f[64], e[16], back[128];
        w__splitpath(paths[i], d, r, f, e);
        w__makepath(back, d, r, f, e);
        MT_EXPECT(strcmp(back, paths[i]) == 0);
    }
}

static const mt_case_t cases[] = {
    MT_CASE(split_cases), MT_CASE(split_allows_null_outputs), MT_CASE(split_s_checks_sizes), MT_CASE(make_cases),
    MT_CASE(make_s_checks_size), MT_CASE(split_then_make_round_trips),
};

int main(void)
{
    return MT_RUN("ucrtbase_path", cases);
}
