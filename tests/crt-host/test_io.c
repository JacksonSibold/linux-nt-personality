/*
 * test_io.c - tests for src/crt/ucrt_io.h on a POSIX backend: descriptor allocation, text-mode translation in
 * both directions (including a CR at the end of a read), the Ctrl-Z end of file, positioning, errno mapping.
 */
#include "minitest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "posix_os.h"
#include "ucrt_io.h"

#define MT_FAIL(...) do { char fb_[256]; snprintf(fb_, sizeof fb_, __VA_ARGS__); mt_fail(__FILE__, __LINE__, fb_); } while (0)

static char tmpdir[256];

static const char *path_of(const char *name)
{
    static char b[8][512];
    static int i;
    char *p = b[i++ & 7];
    snprintf(p, 512, "%s/%s", tmpdir, name);
    return p;
}

static void setup(void)
{
    g_ucrt_os = &posix_os;
    if (!tmpdir[0]) {
        snprintf(tmpdir, sizeof tmpdir, "/tmp/crt_io_test_%d", (int)getpid());
        mkdir(tmpdir, 0755);
    }
    g_ucrt_io.initialised = 0;
    for (int i = 3; i < UCRT_NFILE; ++i)
        g_ucrt_io.fds[i].flags = 0;
    g_ucrt_io.fmode = UCRT_O_TEXT;
}

static void put_raw(const char *name, const void *data, size_t n)
{
    FILE *f = fopen(path_of(name), "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}

static size_t get_raw(const char *name, void *data, size_t cap)
{
    FILE *f = fopen(path_of(name), "rb");
    size_t n;
    if (!f)
        return (size_t)-1;
    n = fread(data, 1, cap, f);
    fclose(f);
    return n;
}

static void binary_roundtrip(void)
{
    char buf[100];
    int fd;
    setup();
    fd = ucrt_open(path_of("bin"), UCRT_O_CREAT | UCRT_O_TRUNC | UCRT_O_RDWR | UCRT_O_BINARY, UCRT_S_IWRITE);
    MT_ASSERT(fd >= 3);
    MT_EXPECT_EQ(ucrt_write(fd, "a\r\nb\n\x1a" "c", 7), 7);
    MT_EXPECT_EQ(ucrt_lseek(fd, 0, 0), 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 100), 7);
    MT_EXPECT(memcmp(buf, "a\r\nb\n\x1a" "c", 7) == 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 100), 0);
    MT_EXPECT_EQ(ucrt_eof(fd), 1);
    MT_EXPECT_EQ(ucrt_filelength(fd), 7);
    MT_EXPECT_EQ(ucrt_tell(fd), 7);
    MT_EXPECT_EQ(ucrt_close(fd), 0);
    MT_EXPECT_EQ(ucrt_close(fd), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EBADF);
}

static void text_write_translates_lf(void)
{
    char raw[100];
    int fd;
    size_t n;
    setup();
    fd = ucrt_open(path_of("txt"), UCRT_O_CREAT | UCRT_O_TRUNC | UCRT_O_WRONLY | UCRT_O_TEXT, UCRT_S_IWRITE);
    MT_ASSERT(fd >= 3);
    MT_EXPECT_EQ(ucrt_write(fd, "one\ntwo\n\nthree", 14), 14); /* the count is of the bytes the caller gave */
    ucrt_close(fd);
    n = get_raw("txt", raw, sizeof raw);
    MT_EXPECT_EQ((long)n, 17);
    MT_EXPECT(memcmp(raw, "one\r\ntwo\r\n\r\nthree", 17) == 0);
    /* a large write exercises the chunked translation */
    {
        char *big = malloc(100000), *back = malloc(200000);
        for (int i = 0; i < 100000; ++i)
            big[i] = (i % 7 == 0) ? '\n' : 'x';
        fd = ucrt_open(path_of("big"), UCRT_O_CREAT | UCRT_O_TRUNC | UCRT_O_WRONLY | UCRT_O_TEXT, UCRT_S_IWRITE);
        MT_EXPECT_EQ(ucrt_write(fd, big, 100000), 100000);
        ucrt_close(fd);
        n = get_raw("big", back, 200000);
        MT_EXPECT_EQ((long)n, 100000 + 14286);
        {
            size_t j = 0;
            for (int i = 0; i < 100000; ++i) {
                if (big[i] == '\n')
                    MT_ASSERT(back[j++] == '\r');
                MT_ASSERT(back[j++] == big[i]);
            }
        }
        free(big);
        free(back);
    }
}

static void text_read_translates_crlf(void)
{
    char buf[100];
    int fd;
    setup();
    put_raw("crlf", "ab\r\ncd\r\n\r\nef\rgh", 16);
    fd = ucrt_open(path_of("crlf"), UCRT_O_RDONLY | UCRT_O_TEXT, 0);
    MT_ASSERT(fd >= 3);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 100), 13);
    MT_EXPECT(memcmp(buf, "ab\ncd\n\nef\rgh", 13) == 0); /* a lone CR stays */
    MT_EXPECT_EQ(ucrt_read(fd, buf, 100), 0);
    ucrt_close(fd);
    /* a CR at the very end of a read: its LF is fetched, and a non-LF byte is pushed back */
    fd = ucrt_open(path_of("crlf"), UCRT_O_RDONLY | UCRT_O_TEXT, 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 3), 3); /* "ab\r" + "\n" */
    MT_EXPECT(memcmp(buf, "ab\n", 3) == 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 3), 3); /* "cd\r" + "\n" */
    MT_EXPECT(memcmp(buf, "cd\n", 3) == 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 2), 1); /* "\r\n" becomes one byte: a read can return less than asked */
    ucrt_close(fd);
    put_raw("cr", "ab\rcd", 5);
    fd = ucrt_open(path_of("cr"), UCRT_O_RDONLY | UCRT_O_TEXT, 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 3), 3); /* "ab\r": the next byte is not LF, so the CR stays and 'c' is read again */
    MT_EXPECT(memcmp(buf, "ab\r", 3) == 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 10), 2);
    MT_EXPECT(memcmp(buf, "cd", 2) == 0);
    ucrt_close(fd);
}

static void ctrl_z_ends_text_file(void)
{
    char buf[100];
    int fd;
    setup();
    put_raw("z", "abc\x1a" "def", 7);
    fd = ucrt_open(path_of("z"), UCRT_O_RDONLY | UCRT_O_TEXT, 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 100), 3);
    MT_EXPECT(memcmp(buf, "abc", 3) == 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 100), 0); /* stays at the end ... */
    MT_EXPECT_EQ(ucrt_lseek(fd, 0, 0), 0);    /* ... until the file is repositioned */
    MT_EXPECT_EQ(ucrt_read(fd, buf, 100), 3);
    ucrt_close(fd);
    /* binary mode reads it all */
    fd = ucrt_open(path_of("z"), UCRT_O_RDONLY | UCRT_O_BINARY, 0);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 100), 7);
    ucrt_close(fd);
    /* an appended text file loses a trailing Ctrl-Z first */
    put_raw("z2", "abc\x1a", 4);
    fd = ucrt_open(path_of("z2"), UCRT_O_RDWR | UCRT_O_APPEND | UCRT_O_TEXT, 0);
    MT_EXPECT_EQ(ucrt_write(fd, "d", 1), 1);
    ucrt_close(fd);
    {
        char raw[16];
        size_t n = get_raw("z2", raw, sizeof raw);
        MT_EXPECT_EQ((long)n, 4);
        MT_EXPECT(memcmp(raw, "abcd", 4) == 0);
    }
}

static void positioning_and_errors(void)
{
    char buf[16];
    int fd;
    setup();
    put_raw("pos", "0123456789", 10);
    fd = ucrt_open(path_of("pos"), UCRT_O_RDWR | UCRT_O_BINARY, 0);
    MT_EXPECT_EQ(ucrt_lseek(fd, 4, 0), 4);
    MT_EXPECT_EQ(ucrt_read(fd, buf, 2), 2);
    MT_EXPECT(memcmp(buf, "45", 2) == 0);
    MT_EXPECT_EQ(ucrt_lseek(fd, -3, 2), 7);
    MT_EXPECT_EQ((long)ucrt_lseeki64(fd, 2, 1), 9);
    MT_EXPECT_EQ(ucrt_lseek(fd, -100, 0), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EINVAL);
    MT_EXPECT_EQ(ucrt_eof(fd), 0);
    ucrt_lseek(fd, 0, 2);
    MT_EXPECT_EQ(ucrt_eof(fd), 1);
    MT_EXPECT_EQ(ucrt_write(99, "x", 1), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EBADF);
    MT_EXPECT_EQ(ucrt_read(-1, buf, 1), -1);
    MT_EXPECT_EQ(ucrt_lseek(fd, 0, 7), -1);
    ucrt_close(fd);
    /* errno mapping */
    MT_EXPECT_EQ(ucrt_open(path_of("missing"), UCRT_O_RDONLY, 0), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ENOENT);
    MT_EXPECT_EQ((long)g_ucrt_state.doserrno_value, 2);
    MT_EXPECT_EQ(ucrt_open(path_of("pos"), UCRT_O_CREAT | UCRT_O_EXCL | UCRT_O_RDWR, UCRT_S_IWRITE), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EEXIST);
    MT_EXPECT_EQ(ucrt_open(NULL, UCRT_O_RDONLY, 0), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EINVAL);
}

static void descriptor_table(void)
{
    int fds[UCRT_NFILE], n = 0, fd;
    setup();
    put_raw("d", "x", 1);
    while ((fd = ucrt_open(path_of("d"), UCRT_O_RDONLY, 0)) >= 0 && n < UCRT_NFILE)
        fds[n++] = fd;
    MT_EXPECT_EQ(n, UCRT_NFILE - 3); /* 0, 1 and 2 are the standard streams */
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EMFILE);
    MT_EXPECT_EQ(fds[0], 3);
    ucrt_close(fds[5]);
    MT_EXPECT_EQ(ucrt_open(path_of("d"), UCRT_O_RDONLY, 0), fds[5]); /* the lowest free number is reused */
    for (int i = 0; i < n; ++i)
        ucrt_close(fds[i]);
}

static void dup_and_dup2_share_position(void)
{
    char buf[8];
    int a, b, c;
    setup();
    put_raw("dd", "abcdef", 6);
    a = ucrt_open(path_of("dd"), UCRT_O_RDONLY | UCRT_O_BINARY, 0);
    b = ucrt_dup(a);
    MT_ASSERT(b >= 0 && b != a);
    MT_EXPECT_EQ(ucrt_read(a, buf, 2), 2);
    MT_EXPECT_EQ(ucrt_read(b, buf, 2), 2); /* a duplicate shares the file position */
    MT_EXPECT(memcmp(buf, "cd", 2) == 0);
    c = ucrt_open(path_of("dd"), UCRT_O_RDONLY | UCRT_O_BINARY, 0);
    MT_EXPECT_EQ(ucrt_dup2(a, c), 0);
    MT_EXPECT_EQ(ucrt_read(c, buf, 2), 2);
    MT_EXPECT(memcmp(buf, "ef", 2) == 0);
    MT_EXPECT_EQ(ucrt_dup2(a, a), 0);
    MT_EXPECT_EQ(ucrt_dup(77), -1);
    ucrt_close(a);
    ucrt_close(b);
    ucrt_close(c);
}

static void modes_chsize_unlink_rename(void)
{
    char raw[16];
    int fd;
    setup();
    fd = ucrt_open(path_of("m"), UCRT_O_CREAT | UCRT_O_TRUNC | UCRT_O_RDWR | UCRT_O_BINARY, UCRT_S_IWRITE);
    MT_EXPECT_EQ(ucrt_setmode(fd, UCRT_O_TEXT), UCRT_O_BINARY);
    MT_EXPECT_EQ(ucrt_setmode(fd, UCRT_O_BINARY), UCRT_O_TEXT);
    MT_EXPECT_EQ(ucrt_setmode(fd, 12345), -1);
    ucrt_write(fd, "abcdef", 6);
    MT_EXPECT_EQ(ucrt_chsize(fd, 3), 0);
    MT_EXPECT_EQ(ucrt_filelength(fd), 3);
    MT_EXPECT_EQ(ucrt_chsize(fd, 8), 0);
    MT_EXPECT_EQ(ucrt_filelength(fd), 8);
    MT_EXPECT_EQ(ucrt_commit(fd), 0);
    MT_EXPECT_EQ(ucrt_isatty(fd), 0);
    ucrt_close(fd);
    MT_EXPECT_EQ((long)get_raw("m", raw, sizeof raw), 8);
    MT_EXPECT(memcmp(raw, "abc\0\0\0\0\0", 8) == 0);
    MT_EXPECT_EQ(ucrt_rename(path_of("m"), path_of("m2")), 0);
    MT_EXPECT_EQ((long)get_raw("m", raw, sizeof raw), -1);
    MT_EXPECT_EQ(ucrt_unlink(path_of("m2")), 0);
    MT_EXPECT_EQ(ucrt_unlink(path_of("m2")), -1);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ENOENT);
    MT_EXPECT_EQ(ucrt_rename(path_of("nothing"), path_of("else")), -1);
    /* the default translation mode applies when the flags say neither */
    g_ucrt_io.fmode = UCRT_O_BINARY;
    fd = ucrt_open(path_of("f"), UCRT_O_CREAT | UCRT_O_TRUNC | UCRT_O_WRONLY, UCRT_S_IWRITE);
    ucrt_write(fd, "a\n", 2);
    ucrt_close(fd);
    MT_EXPECT_EQ((long)get_raw("f", raw, sizeof raw), 2);
    g_ucrt_io.fmode = UCRT_O_TEXT;
    ucrt_unlink(path_of("f"));
}

static void standard_descriptors(void)
{
    setup();
    ucrt_io_init();
    MT_EXPECT(ucrt_fd_get(0) && ucrt_fd_get(1) && ucrt_fd_get(2));
    MT_EXPECT(ucrt_get_osfhandle(1) == posix_handle(1));
    MT_EXPECT(ucrt_get_osfhandle(50) == (void *)(intptr_t)-1);
    {
        int fd = ucrt_open_osfhandle(posix_handle(dup(1)), UCRT_O_BINARY);
        MT_EXPECT(fd >= 3);
        MT_EXPECT_EQ(ucrt_close(fd), 0);
    }
}

static const mt_case_t cases[] = {
    MT_CASE(binary_roundtrip), MT_CASE(text_write_translates_lf), MT_CASE(text_read_translates_crlf),
    MT_CASE(ctrl_z_ends_text_file), MT_CASE(positioning_and_errors), MT_CASE(descriptor_table),
    MT_CASE(dup_and_dup2_share_position), MT_CASE(modes_chsize_unlink_rename), MT_CASE(standard_descriptors),
};

int main(void)
{
    int rc = MT_RUN("crt_io", cases);
    if (tmpdir[0]) {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -rf %s", tmpdir);
        (void)!system(cmd);
    }
    return rc;
}
