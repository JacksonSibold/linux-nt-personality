/*
 * test_k32_find_pure.c - host tests for src/ntdll/k32_find_pure.h: the FindFirstFile
 * wildcard match, the directory split, the name length limit, the temp path and
 * temp file name formats, and the environment name match.
 */
#include "minitest.h"
#include "k32_find_pure.h"

#include <string.h>

static int match(const char *pat, const char *name)
{
    WCHAR w[300];
    size_t n = 0;
    while (name[n] != '\0') { w[n] = (WCHAR)(unsigned char)name[n]; ++n; }
    return k32f_wild_match(pat, w, n);
}

/* '*' matches any run, including empty; '?' matches one character. */
static void wild_star_and_question(void)
{
    MT_EXPECT(match("*", "") == 1);
    MT_EXPECT(match("*", "anything") == 1);
    MT_EXPECT(match("a*", "a") == 1);
    MT_EXPECT(match("a*c", "abbbc") == 1);
    MT_EXPECT(match("a?c", "abc") == 1);
    MT_EXPECT(match("a?c", "ac") == 0);
    MT_EXPECT(match("*.txt", "notes.txt") == 1);
    MT_EXPECT(match("*.txt", "notes.doc") == 0);
    MT_EXPECT(match("abc", "abc") == 1);
    MT_EXPECT(match("abc", "abcd") == 0);
    MT_EXPECT(match("abcd", "abc") == 0);
}

/* The backtrack case: the first '*' must give back characters. */
static void wild_backtracks(void)
{
    MT_EXPECT(match("*a*b", "xaxxab") == 1);
    MT_EXPECT(match("*a*b", "xaxxa") == 0);
    MT_EXPECT(match("a*a*a", "aaaa") == 1);
    MT_EXPECT(match("*?*", "x") == 1);
    MT_EXPECT(match("*?*", "") == 0);
}

/* ASCII letters fold; a wide character above 0x7F never equals a letter. */
static void wild_case_insensitive(void)
{
    MT_EXPECT(match("HELLO.TXT", "hello.txt") == 1);
    MT_EXPECT(match("h*O.t?T", "hello.TXT") == 1);
    WCHAR w[2] = { 0x00E9, 0 };
    MT_EXPECT(k32f_wild_match("e", w, 1) == 0);
    MT_EXPECT(k32f_wild_match("?", w, 1) == 1);
}

/* The directory is everything before the last separator. */
static void dir_split(void)
{
    MT_EXPECT_EQ(k32f_dir_len("C:\\tmp\\*"), 6);
    MT_EXPECT_EQ(k32f_dir_len("C:/tmp/a/b.txt"), 8);
    MT_EXPECT_EQ(k32f_dir_len("*.txt"), 0);
    MT_EXPECT_EQ(k32f_dir_len("C:\\tmp\\"), 6);
}

/* cFileName holds 259 characters and the NUL. */
static void name_length_limit(void)
{
    MT_EXPECT(k32f_name_fits(259) == 1);
    MT_EXPECT(k32f_name_fits(260) == 0);
}

/* TMP is used with a trailing separator. An unset or empty value falls back to
 * C:\tmp\. A buffer too small for the path with its NUL gives 0. */
static void temp_path_format(void)
{
    char buf[64];
    MT_EXPECT_EQ(k32f_temp_path("C:\\Users\\me\\Temp", buf, sizeof(buf)), 17);
    MT_EXPECT(strcmp(buf, "C:\\Users\\me\\Temp\\") == 0);
    MT_EXPECT_EQ(k32f_temp_path("D:\\t\\", buf, sizeof(buf)), 5);
    MT_EXPECT(strcmp(buf, "D:\\t\\") == 0);
    MT_EXPECT_EQ(k32f_temp_path(NULL, buf, sizeof(buf)), 7);
    MT_EXPECT(strcmp(buf, "C:\\tmp\\") == 0);
    MT_EXPECT_EQ(k32f_temp_path("", buf, sizeof(buf)), 7);
    char small[4];
    MT_EXPECT_EQ(k32f_temp_path("C:\\tmp", small, sizeof(small)), 0);
}

/* GetTempFileName: directory, three prefix characters, four hex digits of the
 * unique number, and .tmp. */
static void temp_file_name_format(void)
{
    char buf[64];
    MT_EXPECT_EQ(k32f_temp_file_name("C:\\tmp\\", "hdl", 0x756A, buf, sizeof(buf)), 18);
    MT_EXPECT(strcmp(buf, "C:\\tmp\\hdl756A.tmp") == 0);
    MT_EXPECT_EQ(k32f_temp_file_name("C:\\tmp", "ntpers", 1, buf, sizeof(buf)), 18);
    MT_EXPECT(strcmp(buf, "C:\\tmp\\ntp0001.tmp") == 0);
    char small[8];
    MT_EXPECT_EQ(k32f_temp_file_name("C:\\tmp\\", "x", 1, small, sizeof(small)), 0);
}

/* A name in NAME=value form matches NAME case-insensitively; the value starts
 * after the '='. A prefix of the name does not match. */
static void env_name_match(void)
{
    const char *entry = "TMP=C:\\x";
    WCHAR w[16];
    size_t n = 0;
    while (entry[n] != '\0') { w[n] = (WCHAR)(unsigned char)entry[n]; ++n; }
    size_t off = 0;
    MT_EXPECT(k32f_env_name_match(w, n, "TMP", &off) == 1);
    MT_EXPECT_EQ(off, 4);
    MT_EXPECT(k32f_env_name_match(w, n, "tmp", &off) == 1);
    MT_EXPECT(k32f_env_name_match(w, n, "TM", &off) == 0);
    MT_EXPECT(k32f_env_name_match(w, n, "TEMP", &off) == 0);
    MT_EXPECT(k32f_env_name_match(w, n, "TMP=", &off) == 0);
}

static const mt_case_t cases[] = {
    { "wild_star_and_question", wild_star_and_question },
    { "wild_backtracks", wild_backtracks },
    { "wild_case_insensitive", wild_case_insensitive },
    { "dir_split", dir_split },
    { "name_length_limit", name_length_limit },
    { "temp_path_format", temp_path_format },
    { "temp_file_name_format", temp_file_name_format },
    { "env_name_match", env_name_match },
};

int main(void)
{
    return MT_RUN("k32_find_pure", cases);
}
