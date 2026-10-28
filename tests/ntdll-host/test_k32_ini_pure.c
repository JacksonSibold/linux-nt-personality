/*
 * test_k32_ini_pure.c - tests for src/ntdll/k32_ini_pure.h: reading values, names and sections, integer values, the
 * edits of WritePrivateProfileString, the file encodings and binary structures.
 */
#include "minitest.h"
#include "k32_ini_pure.h"

#include <stdio.h>
#include <stdlib.h>

#define MAXW 4096

static size_t W(const char *s, kini_wchar *w)
{
    size_t i = 0;
    for (; s[i]; ++i)
        w[i] = (unsigned char)s[i];
    w[i] = 0;
    return i;
}

static void N(const kini_wchar *w, size_t n, char *s)
{
    for (size_t i = 0; i < n; ++i)
        s[i] = w[i] < 0x80 ? (char)w[i] : '?';
    s[n] = 0;
}

typedef struct {
    kini_wchar text[MAXW];
    size_t n;
} doc;

static void load(doc *d, const char *s) { d->n = W(s, d->text); }

static const char *sample =
    "; a comment\r\n"
    "[Display]\r\n"
    "Width=1920\r\n"
    "  Height = 1080  \r\n"
    "Title=\"My Game\"\r\n"
    "Quoted='single'\r\n"
    "Mismatch=\"half\r\n"
    "Empty=\r\n"
    "\r\n"
    "[audio]\r\n"
    "Volume=-5\r\n"
    "; Hidden=1\r\n"
    "[display]\r\n"
    "Extra=yes\r\n";

static void get(doc *d, const char *sec, const char *key, const char *def, char *res, size_t cap)
{
    kini_wchar s[64], k[64], df[64], out[MAXW];
    size_t n;
    W(sec, s);
    W(key, k);
    W(def ? def : "", df);
    n = kini_get_string(d->text, d->n, s, k, def ? df : NULL, out, cap);
    N(out, n, res);
}

static void reading_values(void)
{
    doc d;
    char r[256];
    load(&d, sample);
    get(&d, "Display", "Width", "x", r, 256);
    MT_EXPECT(strcmp(r, "1920") == 0);
    get(&d, "DISPLAY", "width", "x", r, 256); /* names ignore case */
    MT_EXPECT(strcmp(r, "1920") == 0);
    get(&d, "Display", "Height", "x", r, 256); /* trimmed on both sides */
    MT_EXPECT(strcmp(r, "1080") == 0);
    get(&d, "Display", "Title", "x", r, 256); /* matching quotes come off */
    MT_EXPECT(strcmp(r, "My Game") == 0);
    get(&d, "Display", "Quoted", "x", r, 256);
    MT_EXPECT(strcmp(r, "single") == 0);
    get(&d, "Display", "Mismatch", "x", r, 256); /* a lone quote stays */
    MT_EXPECT(strcmp(r, "\"half") == 0);
    get(&d, "Display", "Empty", "x", r, 256); /* present but empty is not the default */
    MT_EXPECT(strcmp(r, "") == 0);
    get(&d, "Display", "Missing", "fallback", r, 256);
    MT_EXPECT(strcmp(r, "fallback") == 0);
    get(&d, "Nope", "Width", NULL, r, 256);
    MT_EXPECT(strcmp(r, "") == 0);
    get(&d, "audio", "Hidden", "none", r, 256); /* a commented line is not a key */
    MT_EXPECT(strcmp(r, "none") == 0);
    get(&d, "Display", "Extra", "no", r, 256); /* a second section of the same name is searched too */
    MT_EXPECT(strcmp(r, "yes") == 0);
}

static void value_is_cut_to_the_buffer(void)
{
    doc d;
    kini_wchar s[16], k[16], out[8];
    size_t n;
    load(&d, sample);
    W("Display", s);
    W("Title", k);
    n = kini_get_string(d.text, d.n, s, k, NULL, out, 5);
    MT_EXPECT_EQ((long)n, 4L);
    MT_EXPECT(out[4] == 0 && out[0] == 'M');
}

static void names_and_sections(void)
{
    doc d;
    kini_wchar out[MAXW], s[16];
    size_t n, i, count;
    char flat[512];
    load(&d, sample);
    /* "Display\0audio\0display\0" and a final NUL; the count leaves out the last string's NUL */
    n = kini_get_names(d.text, d.n, NULL, out, MAXW);
    MT_EXPECT_EQ((long)n, (long)(8 + 6 + 8 - 1));
    MT_EXPECT(out[7] == 0 && out[n] == 0 && out[n + 1] == 0);
    W("Display", s);
    n = kini_get_names(d.text, d.n, s, out, MAXW);
    MT_EXPECT_EQ((long)n, (long)(6 + 7 + 6 + 7 + 9 + 6 + 6 - 1)); /* Width Height Title Quoted Mismatch Empty Extra */
    MT_EXPECT(out[0] == 'W' && out[n] == 0 && out[n + 1] == 0);
    n = kini_get_section(d.text, d.n, s, out, MAXW);
    N(out, 10, flat);
    MT_EXPECT(strncmp(flat, "Width=1920", 10) == 0);
    for (i = 0, count = 0; out[i]; i += kini_len(out + i) + 1)
        ++count;
    MT_EXPECT_EQ((long)count, 7L);
    /* a section with nothing in it */
    W("nothing", s);
    n = kini_get_names(d.text, d.n, s, out, MAXW);
    MT_EXPECT(n == 0 && out[0] == 0 && out[1] == 0);
    /* a buffer that is too small: the count is size - 2 and the list still ends in two NULs */
    n = kini_get_names(d.text, d.n, NULL, out, 10);
    MT_EXPECT_EQ((long)n, 8L);
    MT_EXPECT(out[8] == 0 && out[9] == 0);
}

static void integers(void)
{
    doc d;
    kini_wchar s[16], k[16];
    load(&d, sample);
    W("Display", s);
    W("Width", k);
    MT_EXPECT_EQ((long)kini_get_int(d.text, d.n, s, k, 7), 1920L);
    W("Nope", k);
    MT_EXPECT_EQ((long)kini_get_int(d.text, d.n, s, k, 7), 7L);
    W("audio", s);
    W("Volume", k);
    MT_EXPECT_EQ((int32_t)kini_get_int(d.text, d.n, s, k, 0), -5);
    W("Display", s);
    W("Title", k);
    MT_EXPECT_EQ((long)kini_get_int(d.text, d.n, s, k, 9), 0L); /* present, not a number */
}

static void set(doc *d, const char *sec, const char *key, const char *value)
{
    kini_wchar s[64], k[64], v[256], out[MAXW];
    size_t n, need;
    W(sec, s);
    if (key)
        W(key, k);
    if (value)
        W(value, v);
    need = kini_set(d->text, d->n, s, key ? k : NULL, value ? v : NULL, NULL, 0);
    MT_ASSERT(need < MAXW);
    n = kini_set(d->text, d->n, s, key ? k : NULL, value ? v : NULL, out, MAXW);
    MT_EXPECT(n == need);
    memcpy(d->text, out, n * sizeof(kini_wchar));
    d->n = n;
}

static int doc_is(doc *d, const char *expect)
{
    char buf[MAXW];
    N(d->text, d->n, buf);
    return strcmp(buf, expect) == 0;
}

static void writing_replaces_adds_and_removes(void)
{
    doc d;
    load(&d, "[a]\r\nx=1\r\ny=2\r\n[b]\r\nz=3\r\n");
    set(&d, "a", "x", "10");
    MT_EXPECT(doc_is(&d, "[a]\r\nx=10\r\ny=2\r\n[b]\r\nz=3\r\n"));
    set(&d, "A", "new", "n"); /* a new key goes at the end of its section */
    MT_EXPECT(doc_is(&d, "[a]\r\nx=10\r\ny=2\r\nnew=n\r\n[b]\r\nz=3\r\n"));
    set(&d, "c", "k", "v"); /* a new section goes at the end */
    MT_EXPECT(doc_is(&d, "[a]\r\nx=10\r\ny=2\r\nnew=n\r\n[b]\r\nz=3\r\n[c]\r\nk=v\r\n"));
    set(&d, "a", "y", NULL); /* removing a key */
    MT_EXPECT(doc_is(&d, "[a]\r\nx=10\r\nnew=n\r\n[b]\r\nz=3\r\n[c]\r\nk=v\r\n"));
    set(&d, "a", "absent", NULL); /* removing what is not there changes nothing */
    MT_EXPECT(doc_is(&d, "[a]\r\nx=10\r\nnew=n\r\n[b]\r\nz=3\r\n[c]\r\nk=v\r\n"));
    set(&d, "b", NULL, NULL); /* removing a section */
    MT_EXPECT(doc_is(&d, "[a]\r\nx=10\r\nnew=n\r\n[c]\r\nk=v\r\n"));
}

static void writing_keeps_other_lines_and_the_line_ending(void)
{
    doc d;
    load(&d, "; top\n[s]\n# not a comment here\nk=1\n");
    set(&d, "s", "k", "2");
    MT_EXPECT(doc_is(&d, "; top\n[s]\n# not a comment here\nk=2\n"));
    load(&d, "[s]\nk=1"); /* no final newline */
    set(&d, "t", "a", "b");
    MT_EXPECT(doc_is(&d, "[s]\nk=1\n[t]\na=b\n"));
    load(&d, "");
    set(&d, "new", "k", "v");
    MT_EXPECT(doc_is(&d, "[new]\r\nk=v\r\n"));
    /* a key added to a section that ends the file without a newline */
    load(&d, "[s]\r\nk=1");
    set(&d, "s", "j", "2");
    MT_EXPECT(doc_is(&d, "[s]\r\nk=1\r\nj=2\r\n"));
}

static void writing_a_whole_section(void)
{
    doc d;
    kini_wchar s[16], out[MAXW];
    kini_wchar pairs[64];
    size_t n;
    load(&d, "[a]\r\nold=1\r\nmore=2\r\n[b]\r\nkeep=3\r\n");
    W("a", s);
    {
        size_t o = 0;
        o += W("p=1", pairs + o) + 1;
        o += W("q=2", pairs + o) + 1;
        pairs[o] = 0;
    }
    n = kini_set_section(d.text, d.n, s, pairs, out, MAXW);
    memcpy(d.text, out, n * sizeof(kini_wchar));
    d.n = n;
    MT_EXPECT(doc_is(&d, "[a]\r\np=1\r\nq=2\r\n[b]\r\nkeep=3\r\n"));
    W("z", s);
    n = kini_set_section(d.text, d.n, s, pairs, out, MAXW);
    memcpy(d.text, out, n * sizeof(kini_wchar));
    d.n = n;
    MT_EXPECT(doc_is(&d, "[a]\r\np=1\r\nq=2\r\n[b]\r\nkeep=3\r\n[z]\r\np=1\r\nq=2\r\n"));
    W("a", s);
    n = kini_set_section(d.text, d.n, s, NULL, out, MAXW); /* NULL removes it */
    memcpy(d.text, out, n * sizeof(kini_wchar));
    d.n = n;
    MT_EXPECT(doc_is(&d, "[b]\r\nkeep=3\r\n[z]\r\np=1\r\nq=2\r\n"));
}

static void sizing_call_with_no_buffer(void)
{
    doc d;
    kini_wchar s[8], k[8], v[8], small[4];
    size_t need;
    load(&d, "[a]\r\nx=1\r\n");
    W("a", s);
    W("y", k);
    W("22", v);
    need = kini_set(d.text, d.n, s, k, v, NULL, 0);
    MT_EXPECT_EQ((long)need, (long)strlen("[a]\r\nx=1\r\ny=22\r\n"));
    MT_EXPECT(kini_set(d.text, d.n, s, k, v, small, 4) == need); /* a too-small buffer is not overrun */
}

static void file_encodings(void)
{
    kini_wchar t[64], back[64];
    uint8_t bytes[256];
    int enc;
    size_t n, b;
    /* ANSI: Latin-1 bytes */
    {
        const uint8_t ansi[] = {'[', 'a', ']', '\r', '\n', 'k', '=', 0xE9, '\r', '\n'};
        n = kini_decode(ansi, sizeof ansi, t, &enc);
        MT_EXPECT_EQ(enc, KINI_ANSI);
        MT_EXPECT(n == 10 && t[7] == 0xE9);
        b = kini_encode(t, n, enc, bytes);
        MT_EXPECT(b == sizeof ansi && memcmp(bytes, ansi, b) == 0);
    }
    /* UTF-16LE with a byte order mark */
    {
        const uint8_t u16[] = {0xFF, 0xFE, '[', 0, 'a', 0, ']', 0, 0x34, 0x26};
        n = kini_decode(u16, sizeof u16, t, &enc);
        MT_EXPECT_EQ(enc, KINI_UTF16LE);
        MT_EXPECT(n == 4 && t[3] == 0x2634);
        b = kini_encode(t, n, enc, bytes);
        MT_EXPECT(b == sizeof u16 && memcmp(bytes, u16, b) == 0);
    }
    /* UTF-8 with a byte order mark */
    {
        const uint8_t u8[] = {0xEF, 0xBB, 0xBF, 'k', 0xC3, 0xA9, 0xE2, 0x98, 0xB4};
        n = kini_decode(u8, sizeof u8, t, &enc);
        MT_EXPECT_EQ(enc, KINI_UTF8);
        MT_EXPECT(n == 3 && t[1] == 0xE9 && t[2] == 0x2634);
        b = kini_encode(t, n, enc, bytes);
        MT_EXPECT(b == sizeof u8 && memcmp(bytes, u8, b) == 0);
        (void)back;
    }
    /* characters an ANSI file cannot hold */
    t[0] = 'a';
    t[1] = 0x2634;
    b = kini_encode(t, 2, KINI_ANSI, bytes);
    MT_EXPECT(b == 2 && bytes[1] == '?');
}

static void binary_structures(void)
{
    const uint8_t data[] = {0x01, 0xAB, 0xFF, 0x00};
    uint8_t back[4] = {0};
    kini_wchar hex[32];
    size_t n = kini_hex_encode(data, sizeof data, hex);
    char s[40];
    MT_EXPECT_EQ((long)n, 10L);
    N(hex, n, s);
    MT_EXPECT(strcmp(s, "01ABFF0000") != 0); /* the checksum is 0xAB + 0x01 + 0xFF = 0xAB, not zero */
    MT_EXPECT(strcmp(s, "01ABFF00AB") == 0);
    MT_EXPECT(kini_hex_decode(hex, n, back, 4) && memcmp(back, data, 4) == 0);
    hex[9] = 'C';
    MT_EXPECT(!kini_hex_decode(hex, n, back, 4)); /* wrong checksum */
    MT_EXPECT(!kini_hex_decode(hex, n - 2, back, 4)); /* wrong length */
    hex[0] = 'g';
    MT_EXPECT(!kini_hex_decode(hex, n, back, 4)); /* not hex */
}

static const mt_case_t cases[] = {
    MT_CASE(reading_values), MT_CASE(value_is_cut_to_the_buffer), MT_CASE(names_and_sections), MT_CASE(integers),
    MT_CASE(writing_replaces_adds_and_removes), MT_CASE(writing_keeps_other_lines_and_the_line_ending),
    MT_CASE(writing_a_whole_section), MT_CASE(sizing_call_with_no_buffer), MT_CASE(file_encodings), MT_CASE(binary_structures),
};

int main(void)
{
    return MT_RUN("k32_ini_pure", cases);
}
