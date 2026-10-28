/*
 * test_stdio.c - tests for src/crt/ucrt_stdio.h on the POSIX backend: a differential test of random stream
 * operations against glibc's FILE in binary mode, then text mode, buffering, modes, wide streams.
 */
#include "minitest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "posix_os.h"
#include "ucrt_stdio.h"

#define MT_FAIL(...) do { char fb_[256]; snprintf(fb_, sizeof fb_, __VA_ARGS__); mt_fail(__FILE__, __LINE__, fb_); } while (0)

static char tmpdir[256];
static uint64_t rng = 0x2545F4914F6CDD1Dull;

static uint64_t rnd64(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

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
    g_ucrt_malloc = malloc;
    g_ucrt_free = free;
    if (!tmpdir[0]) {
        snprintf(tmpdir, sizeof tmpdir, "/tmp/crt_stdio_test_%d", (int)getpid());
        mkdir(tmpdir, 0755);
    }
    ucrt_fcloseall();
    g_ucrt_io.initialised = 0;
    for (int i = 3; i < UCRT_NFILE; ++i)
        g_ucrt_io.fds[i].flags = 0;
    g_ucrt_io.fmode = UCRT_O_TEXT;
    g_ucrt_stdio.initialised = 0;
    ucrt_stdio_init();
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

static void basic_write_read(void)
{
    ucrt_file *f;
    char buf[64], raw[64];
    setup();
    f = ucrt_fopen(path_of("a"), "wb");
    MT_ASSERT(f != NULL);
    MT_EXPECT_EQ(ucrt_fileno(f), 3);
    MT_EXPECT_EQ((long)ucrt_fwrite("hello ", 1, 6, f), 6);
    MT_EXPECT_EQ(ucrt_fputs("world", f), 0);
    MT_EXPECT_EQ(ucrt_fputc('\n', f), '\n');
    MT_EXPECT_EQ(ucrt_fprintf(f, "%d-%s-%5.2f", 42, "x", 3.14159), 10);
    MT_EXPECT_EQ((long)get_raw("a", raw, sizeof raw), 0); /* still in the buffer */
    MT_EXPECT_EQ(ucrt_fflush(f), 0);
    MT_EXPECT_EQ((long)get_raw("a", raw, sizeof raw), 22);
    MT_EXPECT_EQ(ucrt_fclose(f), 0);
    f = ucrt_fopen(path_of("a"), "rb");
    MT_ASSERT(f != NULL);
    MT_EXPECT(ucrt_fgets(buf, sizeof buf, f) == buf);
    MT_EXPECT(strcmp(buf, "hello world\n") == 0);
    MT_EXPECT(ucrt_fgets(buf, sizeof buf, f) == buf);
    MT_EXPECT(strcmp(buf, "42-x- 3.14") == 0);
    MT_EXPECT(ucrt_fgets(buf, sizeof buf, f) == NULL);
    MT_EXPECT_EQ(ucrt_feof(f), 1);
    MT_EXPECT_EQ(ucrt_ferror(f), 0);
    ucrt_clearerr(f);
    MT_EXPECT_EQ(ucrt_feof(f), 0);
    ucrt_rewind(f);
    MT_EXPECT_EQ(ucrt_fgetc(f), 'h');
    MT_EXPECT_EQ(ucrt_ungetc('Z', f), 'Z');
    MT_EXPECT_EQ(ucrt_fgetc(f), 'Z');
    MT_EXPECT_EQ(ucrt_fgetc(f), 'e');
    MT_EXPECT_EQ(ucrt_ftell(f), 2);
    ucrt_fclose(f);
}

/* The oracle: the same operations on glibc's FILE in binary mode. */
static char trace[16][96];
static int trace_n;
#define TRACE(...) snprintf(trace[trace_n++ & 15], 96, __VA_ARGS__)
static void dump_trace(void)
{
    for (int i = 0; i < 16; ++i)
        if (trace[(trace_n + i) & 15][0])
            printf("      %s\n", trace[(trace_n + i) & 15]);
}

static void differential_random_ops(void)
{
    for (int round = 0; round < 300; ++round) {
        ucrt_file *u;
        FILE *g;
        const char *mode = (round % 3 == 0) ? "w+b" : (round % 3 == 1) ? "r+b" : "a+b";
        char name_u[64], name_g[64];
        snprintf(name_u, sizeof name_u, "du%d", round);
        snprintf(name_g, sizeof name_g, "dg%d", round);
        setup();
        if (mode[0] == 'r') {
            unsigned char init[3000];
            for (size_t i = 0; i < sizeof init; ++i)
                init[i] = (unsigned char)(i * 7 + 1);
            put_raw(name_u, init, sizeof init);
            put_raw(name_g, init, sizeof init);
        }
        u = ucrt_fopen(path_of(name_u), mode);
        g = fopen(path_of(name_g), mode);
        MT_ASSERT(u && g);
        if (round % 4 == 0) {
            ucrt_setvbuf(u, NULL, UCRT_IOFBF, 16 + rnd64() % 100);
            setvbuf(g, NULL, _IOFBF, 16 + 0); /* sizes differ on purpose: results must not depend on them */
        }
        for (int op = 0; op < 400; ++op) {
            switch (rnd64() % 9) {
            case 0: case 1: { /* write a block */
                unsigned char data[300];
                size_t n = rnd64() % 300;
                TRACE("op %d: fwrite %zu", op, n);
                for (size_t i = 0; i < n; ++i)
                    data[i] = (unsigned char)rnd64();
                if (ucrt_fwrite(data, 1, n, u) != fwrite(data, 1, n, g)) {
                    MT_FAIL("round %d op %d: fwrite count", round, op);
                    return;
                }
                /* after writing, a read needs a seek in between: do one */
                ucrt_fseek(u, 0, UCRT_SEEK_CUR);
                fseek(g, 0, SEEK_CUR);
                break;
            }
            case 2: { /* read a block */
                unsigned char a[400], b[400];
                size_t n = rnd64() % 400, ra = ucrt_fread(a, 1, n, u), rb = fread(b, 1, n, g);
                TRACE("op %d: fread %zu -> %zu/%zu", op, n, ra, rb);
                if (ra != rb || memcmp(a, b, ra) != 0) {
                    MT_FAIL("round %d op %d: fread %zu vs %zu", round, op, ra, rb);
                    return;
                }
                break;
            }
            case 3: { /* seek */
                long off = (long)(rnd64() % 3500);
                int whence = (int)(rnd64() % 3);
                if (whence == 1)
                    off -= 1500;
                else if (whence == 2)
                    off = -(long)(rnd64() % 500);
                {
                    int ra = ucrt_fseek(u, off, whence), rb = fseek(g, off, whence);
                    TRACE("op %d: fseek %ld %d -> %d/%d", op, off, whence, ra, rb);
                    if ((ra == 0) != (rb == 0)) {
                        MT_FAIL("round %d op %d: fseek(%ld,%d) %d vs %d", round, op, off, whence, ra, rb);
                        return;
                    }
                }
                break;
            }
            case 4: /* tell */
                TRACE("op %d: ftell", op);
                if (ucrt_ftell(u) != ftell(g)) {
                    MT_FAIL("round %d op %d: ftell %ld vs %ld", round, op, (long)ucrt_ftell(u), ftell(g));
                    return;
                }
                break;
            case 5: { /* single characters */
                int ca, cb;
                if (rnd64() & 1) {
                    ca = ucrt_fgetc(u);
                    cb = fgetc(g);
                    TRACE("op %d: fgetc %d/%d", op, ca, cb);
                    if (ca != cb) {
                        MT_FAIL("round %d op %d: fgetc %d vs %d", round, op, ca, cb);
                        return;
                    }
                } else {
                    int c = (int)(rnd64() & 0xFF);
                    ca = ucrt_fputc(c, u);
                    cb = fputc(c, g);
                    ucrt_fseek(u, 0, UCRT_SEEK_CUR);
                    fseek(g, 0, SEEK_CUR);
                    if (ca != cb) {
                        MT_FAIL("round %d op %d: fputc %d vs %d", round, op, ca, cb);
                        return;
                    }
                }
                break;
            }
            case 6: { /* ungetc after a successful read */
                int ca = ucrt_fgetc(u), cb = fgetc(g);
                TRACE("op %d: getc+ungetc %d/%d", op, ca, cb);
                if (ca != cb) {
                    dump_trace();
                    MT_FAIL("round %d op %d: fgetc before ungetc: %d vs %d (pos %ld vs %ld)", round, op, ca, cb, (long)ucrt_ftell(u), ftell(g));
                    return;
                }
                if (ca != EOF) {
                    ucrt_ungetc(ca, u);
                    ungetc(cb, g);
                }
                break;
            }
            case 7: /* flush */
                TRACE("op %d: fflush", op);
                ucrt_fflush(u);
                fflush(g);
                break;
            default: /* rewind */
                TRACE("op %d: rewind", op);
                ucrt_rewind(u);
                rewind(g);
                break;
            }
            if (ucrt_ferror(u) != 0) {
                MT_FAIL("round %d op %d: unexpected error flag", round, op);
                return;
            }
        }
        ucrt_fclose(u);
        fclose(g);
        {
            static unsigned char ra[20000], rb[20000];
            size_t na = get_raw(name_u, ra, sizeof ra), nb = get_raw(name_g, rb, sizeof rb);
            if (na != nb || memcmp(ra, rb, na) != 0) {
                MT_FAIL("round %d (%s): file contents differ (%zu vs %zu bytes)", round, mode, na, nb);
                return;
            }
        }
    }
}

static void text_mode_streams(void)
{
    ucrt_file *f;
    char buf[64], raw[64];
    int64_t pos;
    setup();
    f = ucrt_fopen(path_of("t"), "w");
    MT_ASSERT(f != NULL);
    ucrt_fputs("line1\nline2\n", f);
    MT_EXPECT_EQ(ucrt_ftell(f), 14); /* the position counts the CR the file will hold */
    ucrt_fclose(f);
    MT_EXPECT_EQ((long)get_raw("t", raw, sizeof raw), 14);
    MT_EXPECT(memcmp(raw, "line1\r\nline2\r\n", 14) == 0);
    f = ucrt_fopen(path_of("t"), "r");
    MT_EXPECT(ucrt_fgets(buf, sizeof buf, f) == buf && strcmp(buf, "line1\n") == 0);
    MT_EXPECT_EQ(ucrt_ftell(f), 7); /* the raw offset, past the CR LF */
    MT_EXPECT_EQ(ucrt_fgetpos(f, &pos), 0);
    MT_EXPECT(ucrt_fgets(buf, sizeof buf, f) == buf && strcmp(buf, "line2\n") == 0);
    MT_EXPECT_EQ(ucrt_fsetpos(f, &pos), 0);
    MT_EXPECT(ucrt_fgets(buf, sizeof buf, f) == buf && strcmp(buf, "line2\n") == 0);
    MT_EXPECT_EQ(ucrt_ftell(f), 14);
    MT_EXPECT_EQ(ucrt_fseek(f, 7, UCRT_SEEK_SET), 0);
    MT_EXPECT_EQ(ucrt_fgetc(f), 'l');
    MT_EXPECT_EQ(ucrt_ftell(f), 8);
    ucrt_fclose(f);
    /* text mode appends and updates */
    f = ucrt_fopen(path_of("t"), "a+");
    ucrt_fputs("x\n", f);
    ucrt_rewind(f);
    {
        int n = 0, c;
        while ((c = ucrt_fgetc(f)) != UCRT_EOF)
            buf[n++] = (char)c;
        buf[n] = 0;
        MT_EXPECT(strcmp(buf, "line1\nline2\nx\n") == 0);
    }
    ucrt_fclose(f);
    /* an fread over a text stream returns translated bytes */
    put_raw("t2", "a\r\nb\r\nc", 7);
    f = ucrt_fopen(path_of("t2"), "r");
    MT_EXPECT_EQ((long)ucrt_fread(buf, 1, 10, f), 5);
    MT_EXPECT(memcmp(buf, "a\nb\nc", 5) == 0);
    MT_EXPECT_EQ(ucrt_feof(f), 1);
    ucrt_fclose(f);
    /* tell in a large text file crosses buffer boundaries correctly */
    {
        char *big = malloc(70000);
        size_t n = 0;
        for (int i = 0; i < 5000; ++i)
            n += (size_t)snprintf(big + n, 70000 - n, "row %d\r\n", i);
        put_raw("big", big, n);
        f = ucrt_fopen(path_of("big"), "r");
        for (int i = 0; i < 5000; ++i) {
            char want[32];
            int64_t at = ucrt_ftelli64(f);
            snprintf(want, sizeof want, "row %d\n", i);
            if (!ucrt_fgets(buf, sizeof buf, f) || strcmp(buf, want) != 0) {
                MT_FAIL("big text file, row %d: got '%s'", i, buf);
                break;
            }
            {
                char *found = strstr(big, "row ");
                (void)found;
            }
            if (i % 97 == 0) {
                /* seek back to an earlier position given by ftell and read the same row again */
                int64_t after = ucrt_ftelli64(f);
                ucrt_fseeki64(f, at, UCRT_SEEK_SET);
                if (!ucrt_fgets(buf, sizeof buf, f) || strcmp(buf, want) != 0 || ucrt_ftelli64(f) != after) {
                    MT_FAIL("big text file, row %d: seek back to %lld failed", i, (long long)at);
                    break;
                }
            }
        }
        ucrt_fclose(f);
        free(big);
    }
}

static void buffering_modes(void)
{
    ucrt_file *f;
    char raw[64], user[32];
    setup();
    f = ucrt_fopen(path_of("b"), "wb");
    MT_EXPECT_EQ(ucrt_setvbuf(f, NULL, UCRT_IONBF_MODE, 0), 0);
    ucrt_fputs("abc", f);
    MT_EXPECT_EQ((long)get_raw("b", raw, sizeof raw), 3); /* unbuffered: written at once */
    MT_EXPECT_EQ(ucrt_setvbuf(f, user, UCRT_IOFBF, sizeof user), 0);
    ucrt_fputs("defg", f);
    MT_EXPECT_EQ((long)get_raw("b", raw, sizeof raw), 3);
    MT_EXPECT(memcmp(user, "defg", 4) == 0); /* the data sits in the caller's buffer */
    ucrt_fclose(f);
    MT_EXPECT_EQ((long)get_raw("b", raw, sizeof raw), 7);
    f = ucrt_fopen(path_of("b"), "rb");
    MT_EXPECT_EQ(ucrt_setvbuf(f, NULL, 77, 10), -1); /* a bad mode */
    MT_EXPECT_EQ(ucrt_setvbuf(f, NULL, UCRT_IOFBF, 1), -1); /* too small */
    ucrt_fclose(f);
    /* a block bigger than the buffer goes straight through */
    {
        char *big = malloc(10000);
        memset(big, 'q', 10000);
        f = ucrt_fopen(path_of("b"), "wb");
        MT_EXPECT_EQ((long)ucrt_fwrite(big, 1, 10000, f), 10000);
        MT_EXPECT((long)get_raw("b", big, 10000) >= 8192);
        ucrt_fclose(f);
        free(big);
    }
}

static void mode_strings_and_errors(void)
{
    ucrt_file *f;
    setup();
    MT_EXPECT(ucrt_fopen(path_of("missing"), "r") == NULL);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_ENOENT);
    MT_EXPECT(ucrt_fopen(path_of("x"), "q") == NULL);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EINVAL);
    MT_EXPECT(ucrt_fopen(path_of("x"), "rbt") == NULL);
    MT_EXPECT(ucrt_fopen(NULL, "r") == NULL);
    f = ucrt_fopen(path_of("m"), "w+bN");
    MT_EXPECT(f != NULL);
    MT_EXPECT_EQ(ucrt_fwrite("abc", 1, 3, f), 3);
    ucrt_rewind(f);
    MT_EXPECT_EQ(ucrt_fgetc(f), 'a');
    ucrt_fclose(f);
    /* reading a write-only stream, writing a read-only one */
    f = ucrt_fopen(path_of("m"), "rb");
    MT_EXPECT_EQ(ucrt_fputc('x', f), UCRT_EOF);
    MT_EXPECT_EQ(ucrt_ferror(f), 1);
    ucrt_fclose(f);
    f = ucrt_fopen(path_of("m"), "wb");
    MT_EXPECT_EQ(ucrt_fgetc(f), UCRT_EOF);
    ucrt_fclose(f);
    /* exclusive modes via the descriptor layer, and fdopen */
    {
        int fd = ucrt_open(path_of("m"), UCRT_O_RDWR | UCRT_O_BINARY, 0);
        ucrt_file *g = ucrt_fdopen(fd, "r+b");
        MT_ASSERT(g != NULL);
        MT_EXPECT_EQ(ucrt_fileno(g), fd);
        MT_EXPECT_EQ(ucrt_fclose(g), 0);
        MT_EXPECT(ucrt_fd_get(fd) == NULL); /* closing the stream closes the descriptor */
    }
    {
        ucrt_file *out = NULL;
        MT_EXPECT_EQ(ucrt_fopen_s(&out, path_of("none"), "r"), UCRT_ENOENT);
        MT_EXPECT(out == NULL);
    }
}

static void stream_table_limit(void)
{
    ucrt_file *fs[UCRT_NSTREAM];
    int n = 0;
    ucrt_file *f;
    setup();
    put_raw("lim", "x", 1);
    while (n < UCRT_NSTREAM && (f = ucrt_fopen(path_of("lim"), "r")) != NULL)
        fs[n++] = f;
    MT_EXPECT_EQ(n, UCRT_NSTREAM - 3);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EMFILE);
    MT_EXPECT_EQ(ucrt_fclose(fs[10]), 0);
    f = ucrt_fopen(path_of("lim"), "r");
    MT_EXPECT(f == fs[10]);
    MT_EXPECT_EQ(ucrt_fcloseall(), n);
}

static void read_write_switching(void)
{
    ucrt_file *f;
    char buf[16], raw[32];
    setup();
    put_raw("rw", "0123456789", 10);
    f = ucrt_fopen(path_of("rw"), "r+b");
    MT_EXPECT_EQ((long)ucrt_fread(buf, 1, 4, f), 4);
    MT_EXPECT_EQ(ucrt_fseek(f, 0, UCRT_SEEK_CUR), 0);
    MT_EXPECT_EQ(ucrt_fputc('X', f), 'X'); /* overwrites '4' */
    MT_EXPECT_EQ(ucrt_fseek(f, 0, UCRT_SEEK_CUR), 0);
    MT_EXPECT_EQ(ucrt_fgetc(f), '5');
    /* without the seek, the switch still lands in the right place */
    MT_EXPECT_EQ(ucrt_fputc('Y', f), 'Y');     /* overwrites '6' */
    ucrt_fclose(f);
    MT_EXPECT_EQ((long)get_raw("rw", raw, sizeof raw), 10);
    MT_EXPECT(memcmp(raw, "0123X5Y789", 10) == 0);
}

static void wide_streams(void)
{
    ucrt_file *f;
    unsigned char raw[64];
    ucrt_wchar buf[16];
    setup();
    /* UTF-8 with a byte order mark */
    f = ucrt_fopen(path_of("u8"), "w,ccs=UTF-8");
    MT_ASSERT(f != NULL);
    ucrt_fputwc('A', f);
    ucrt_fputwc(0x00E9, f);
    ucrt_fputwc(0x20AC, f);
    ucrt_fputwc(0xD83D, f); /* a surrogate pair is one 4-byte sequence */
    ucrt_fputwc(0xDE00, f);
    ucrt_fputwc('\n', f);
    ucrt_fclose(f);
    {
        size_t n = get_raw("u8", raw, sizeof raw);
        static const unsigned char want[] = {0xEF, 0xBB, 0xBF, 'A', 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, '\r', '\n'};
        MT_EXPECT_EQ((long)n, (long)sizeof want);
        MT_EXPECT(memcmp(raw, want, sizeof want) == 0);
    }
    f = ucrt_fopen(path_of("u8"), "r,ccs=UTF-8");
    MT_EXPECT_EQ(ucrt_fgetwc(f), 'A');
    MT_EXPECT_EQ(ucrt_fgetwc(f), 0x00E9);
    MT_EXPECT_EQ(ucrt_fgetwc(f), 0x20AC);
    MT_EXPECT_EQ(ucrt_fgetwc(f), 0xD83D);
    MT_EXPECT_EQ(ucrt_fgetwc(f), 0xDE00);
    MT_EXPECT_EQ(ucrt_fgetwc(f), '\n');
    MT_EXPECT_EQ(ucrt_fgetwc(f), UCRT_WEOF_INT);
    ucrt_fclose(f);
    /* UTF-16LE */
    f = ucrt_fopen(path_of("u16"), "w,ccs=UTF-16LE");
    ucrt_fputws((const ucrt_wchar[]){'h', 'i', '\n', 0}, f);
    ucrt_fclose(f);
    {
        size_t n = get_raw("u16", raw, sizeof raw);
        static const unsigned char want[] = {0xFF, 0xFE, 'h', 0, 'i', 0, '\r', 0, '\n', 0};
        MT_EXPECT_EQ((long)n, (long)sizeof want);
        MT_EXPECT(memcmp(raw, want, sizeof want) == 0);
    }
    f = ucrt_fopen(path_of("u16"), "r,ccs=UNICODE");
    MT_EXPECT(ucrt_fgetws(buf, 16, f) == buf);
    MT_EXPECT(buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\n' && buf[3] == 0);
    ucrt_fclose(f);
    /* an ANSI text stream narrows to a byte, and refuses what does not fit */
    f = ucrt_fopen(path_of("an"), "w");
    MT_EXPECT_EQ(ucrt_fputwc('z', f), 'z');
    MT_EXPECT_EQ(ucrt_fputwc(0x20AC, f), UCRT_WEOF_INT);
    MT_EXPECT_EQ(ucrt_get_errno(), UCRT_EILSEQ);
    ucrt_fclose(f);
    /* a binary stream reads and writes two bytes per wide character */
    f = ucrt_fopen(path_of("bw"), "wb");
    ucrt_fputwc(0x1234, f);
    ucrt_fclose(f);
    MT_EXPECT_EQ((long)get_raw("bw", raw, sizeof raw), 2);
    MT_EXPECT(raw[0] == 0x34 && raw[1] == 0x12);
    f = ucrt_fopen(path_of("bw"), "rb");
    MT_EXPECT_EQ(ucrt_fgetwc(f), 0x1234);
    MT_EXPECT_EQ(ucrt_ungetwc('Q', f), 'Q');
    MT_EXPECT_EQ(ucrt_fgetwc(f), 'Q');
    ucrt_fclose(f);
    /* wide printf into a UTF-8 stream */
    f = ucrt_fopen(path_of("wp"), "w,ccs=UTF-8");
    MT_EXPECT_EQ(ucrt_fwprintf(f, (const ucrt_wchar[]){'%', 'd', ':', '%', 'S', 0}, 7, "ok"), 4);
    ucrt_fclose(f);
    MT_EXPECT_EQ((long)get_raw("wp", raw, sizeof raw), 7);
    MT_EXPECT(memcmp(raw + 3, "7:ok", 4) == 0);
}

static void printf_family(void)
{
    ucrt_file *f;
    char raw[128];
    setup();
    f = ucrt_fopen(path_of("pf"), "wb");
    MT_EXPECT_EQ(ucrt_fprintf(f, "%s|%5d|%-5d|%x|%c|%.3f|%e", "str", 42, 42, 255, 'c', 2.5, 12345.678), 39);
    ucrt_fclose(f);
    MT_EXPECT_EQ((long)get_raw("pf", raw, sizeof raw), 39);
    MT_EXPECT(memcmp(raw, "str|   42|42   |ff|c|2.500|1.234568e+04", 39) == 0);
    /* a long result spills through the buffer */
    f = ucrt_fopen(path_of("pf"), "wb");
    {
        char *big = malloc(20000);
        memset(big, 'z', 19999);
        big[19999] = 0;
        MT_EXPECT_EQ(ucrt_fprintf(f, "%s", big), 19999);
        free(big);
    }
    ucrt_fclose(f);
    {
        char *big = malloc(30000);
        MT_EXPECT_EQ((long)get_raw("pf", big, 30000), 19999);
        free(big);
    }
    /* printf to stdout goes to descriptor 1; point it at a file with dup2 */
    {
        int saved = ucrt_dup(1), fd = ucrt_open(path_of("so"), UCRT_O_CREAT | UCRT_O_TRUNC | UCRT_O_WRONLY | UCRT_O_BINARY, UCRT_S_IWRITE);
        MT_EXPECT_EQ(ucrt_dup2(fd, 1), 0);
        ucrt_close(fd);
        MT_EXPECT_EQ(ucrt_printf("to stdout %d\n", 5), 12);
        MT_EXPECT_EQ(ucrt_puts("line"), 1);
        ucrt_putchar('!');
        ucrt_fflush(NULL);
        ucrt_dup2(saved, 1);
        ucrt_close(saved);
        MT_EXPECT_EQ((long)get_raw("so", raw, sizeof raw), 12 + 5 + 1);
        MT_EXPECT(memcmp(raw, "to stdout 5\nline\n!", 18) == 0);
    }
}

static const mt_case_t cases[] = {
    MT_CASE(basic_write_read), MT_CASE(differential_random_ops), MT_CASE(text_mode_streams), MT_CASE(buffering_modes),
    MT_CASE(mode_strings_and_errors), MT_CASE(stream_table_limit), MT_CASE(read_write_switching), MT_CASE(wide_streams),
    MT_CASE(printf_family),
};

int main(void)
{
    int rc = MT_RUN("crt_stdio", cases);
    if (tmpdir[0]) {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -rf %s", tmpdir);
        (void)!system(cmd);
    }
    return rc;
}
