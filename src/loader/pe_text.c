/* pe_text.c - see pe_text.h. */
#include "pe_text.h"

/* Bounded UTF-16 writer: sets ok=0 and drops the rest on overflow. */
struct u16w {
    uint16_t *buf;
    size_t cap;
    size_t n;
    int ok;
};

static void w_put(struct u16w *w, uint16_t c)
{
    if (!w->ok) return;
    if (w->n + 1 > w->cap) { w->ok = 0; return; }
    w->buf[w->n++] = c;
}

static void w_put_cp(struct u16w *w, uint32_t cp)
{
    if (cp >= 0x10000) {
        cp -= 0x10000;
        w_put(w, (uint16_t)(0xD800 | (cp >> 10)));
        w_put(w, (uint16_t)(0xDC00 | (cp & 0x3FF)));
    } else {
        w_put(w, (uint16_t)cp);
    }
}

/* Decodes one UTF-8 sequence starting at s[*i]. Returns the code point and
 * advances *i, or U+FFFD for an invalid or truncated sequence. */
static uint32_t utf8_next(const char *s, size_t *i)
{
    const unsigned char c = (unsigned char)s[*i];
    uint32_t cp;
    int extra;
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { (*i)++; return 0xFFFD; }
    (*i)++;
    for (int k = 0; k < extra; k++) {
        const unsigned char cc = (unsigned char)s[*i];
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
        (*i)++;
    }
    return cp;
}

static void w_utf8(struct u16w *w, const char *s)
{
    size_t i = 0;
    while (s[i]) w_put_cp(w, utf8_next(s, &i));
}

size_t pe_utf8_to_utf16(const char *s, uint16_t *out, size_t cap)
{
    struct u16w w = { out, cap, 0, 1 };
    w_utf8(&w, s);
    w_put(&w, 0);
    return w.ok ? w.n : 0;
}

size_t pe_build_command_line(int argc, const char *const *argv, uint16_t *buf, size_t cap)
{
    struct u16w w = { buf, cap, 0, 1 };
    for (int a = 0; a < argc; a++) {
        const char *s = argv[a];
        int quote = (s[0] == '\0');
        for (const char *p = s; *p; p++)
            if (*p == ' ' || *p == '\t') quote = 1;
        if (a > 0) w_put(&w, ' ');
        if (quote) w_put(&w, '"');
        for (size_t i = 0; s[i]; ) {
            if (s[i] == '"') {
                w_put(&w, '\\');
                w_put(&w, '"');
                i++;
            } else {
                w_put_cp(&w, utf8_next(s, &i));
            }
        }
        if (quote) w_put(&w, '"');
    }
    w_put(&w, 0);
    return w.ok ? w.n : 0;
}

size_t pe_build_environment(int envc, const char *const *envp, uint16_t *buf, size_t cap)
{
    struct u16w w = { buf, cap, 0, 1 };
    for (int e = 0; e < envc; e++) {
        w_utf8(&w, envp[e]);
        w_put(&w, 0);
    }
    w_put(&w, 0);
    if (envc == 0) w_put(&w, 0);  /* empty block: "\0\0", not a lone NUL */
    return w.ok ? w.n : 0;
}
