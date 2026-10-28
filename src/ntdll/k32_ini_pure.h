/*
 * k32_ini_pure.h - the INI file rules behind GetPrivateProfileString, WritePrivateProfileString and relatives.
 *
 * Everything works on UTF-16 text in memory. Reading follows Windows: section and key names compare without regard
 * to case; a line is "[section]", "key=value" or a comment (first non-blank character ';'); the key is trimmed and
 * the value is trimmed and, if it is enclosed in matching single or double quotes, unquoted; the first matching key
 * wins and sections of the same name are searched in turn. Writing keeps every other line as it is, replaces or removes
 * the line it changes, and appends a missing key at the end of its section and a missing section at the end of the file.
 * Host tests: tests/ntdll-host/test_k32_ini_pure.c.
 */
#ifndef K32_INI_PURE_H
#define K32_INI_PURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint16_t kini_wchar;

enum { KINI_ANSI = 0, KINI_UTF16LE = 1, KINI_UTF8 = 2 };

/* ---- characters --------------------------------------------------------------------------------------------------------- */

static inline kini_wchar kini_fold(kini_wchar c)
{
    if (c >= 'A' && c <= 'Z')
        return (kini_wchar)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return (kini_wchar)(c + 32);
    return c;
}

static inline int kini_is_blank(kini_wchar c) { return c == ' ' || c == '\t'; }
static inline int kini_is_eol(kini_wchar c) { return c == '\r' || c == '\n'; }

static inline size_t kini_len(const kini_wchar *s)
{
    size_t n = 0;
    while (s && s[n])
        ++n;
    return n;
}

/* Name equality, ignoring case: s[0..n) against the NUL-terminated name. */
static inline int kini_name_eq(const kini_wchar *s, size_t n, const kini_wchar *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (!name[i] || kini_fold(s[i]) != kini_fold(name[i]))
            return 0;
    return name[i] == 0;
}

/* ---- text encoding ------------------------------------------------------------------------------------------------------ */

/* Bytes to UTF-16 text. A UTF-16LE byte order mark or a UTF-8 one selects that encoding; anything else is ANSI, taken as
 * Latin-1. out must hold n code units. Returns the count; *enc gets the encoding. */
static inline size_t kini_decode(const uint8_t *b, size_t n, kini_wchar *out, int *enc)
{
    size_t i = 0, o = 0;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        *enc = KINI_UTF16LE;
        for (i = 2; i + 1 < n; i += 2)
            out[o++] = (kini_wchar)(b[i] | (b[i + 1] << 8));
        return o;
    }
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        *enc = KINI_UTF8;
        i = 3;
        while (i < n) {
            uint32_t c = b[i];
            if (c < 0x80) {
                out[o++] = (kini_wchar)c;
                ++i;
            } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
                out[o++] = (kini_wchar)(((c & 0x1F) << 6) | (b[i + 1] & 0x3F));
                i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
                out[o++] = (kini_wchar)(((c & 0x0F) << 12) | ((b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F));
                i += 3;
            } else {
                out[o++] = 0xFFFD;
                ++i;
            }
        }
        return o;
    }
    *enc = KINI_ANSI;
    for (; i < n; ++i)
        out[o++] = b[i];
    return o;
}

/* UTF-16 text to bytes in the file's encoding; returns the byte count and writes when out is non-NULL (out needs
 * 3 * n + 3 bytes at most). Characters an ANSI file cannot hold are written as '?'. */
static inline size_t kini_encode(const kini_wchar *t, size_t n, int enc, uint8_t *out)
{
    size_t o = 0;
#define KINI_B(v) do { if (out) out[o] = (uint8_t)(v); ++o; } while (0)
    if (enc == KINI_UTF16LE) {
        KINI_B(0xFF);
        KINI_B(0xFE);
        for (size_t i = 0; i < n; ++i) {
            KINI_B(t[i] & 0xFF);
            KINI_B(t[i] >> 8);
        }
    } else if (enc == KINI_UTF8) {
        KINI_B(0xEF);
        KINI_B(0xBB);
        KINI_B(0xBF);
        for (size_t i = 0; i < n; ++i) {
            uint32_t c = t[i];
            if (c < 0x80) {
                KINI_B(c);
            } else if (c < 0x800) {
                KINI_B(0xC0 | (c >> 6));
                KINI_B(0x80 | (c & 0x3F));
            } else {
                KINI_B(0xE0 | (c >> 12));
                KINI_B(0x80 | ((c >> 6) & 0x3F));
                KINI_B(0x80 | (c & 0x3F));
            }
        }
    } else {
        for (size_t i = 0; i < n; ++i)
            KINI_B(t[i] < 0x100 ? t[i] : '?');
    }
#undef KINI_B
    return o;
}

/* ---- line scanning ------------------------------------------------------------------------------------------------------- */

typedef struct {
    size_t start, end;  /* the line without its terminator */
    size_t next;        /* where the following line starts */
} kini_line;

static inline int kini_next_line(const kini_wchar *t, size_t n, size_t pos, kini_line *l)
{
    size_t e;
    if (pos >= n)
        return 0;
    for (e = pos; e < n && !kini_is_eol(t[e]); ++e)
        ;
    l->start = pos;
    l->end = e;
    l->next = e;
    if (e < n) {
        if (t[e] == '\r' && e + 1 < n && t[e + 1] == '\n')
            l->next = e + 2;
        else
            l->next = e + 1;
    }
    return 1;
}

/* Trims blanks off both ends of [s, e). */
static inline void kini_trim(const kini_wchar *t, size_t *s, size_t *e)
{
    while (*s < *e && kini_is_blank(t[*s]))
        ++*s;
    while (*e > *s && kini_is_blank(t[*e - 1]))
        --*e;
}

typedef enum { KINI_BLANK, KINI_COMMENT, KINI_SECTION, KINI_PAIR, KINI_TEXT } kini_kind;

/* Classifies a line. For a section, [*a, *b) is its name; for a pair, [*a, *b) is the trimmed key and [*c, *d) the value
 * before unquoting. */
static inline kini_kind kini_classify(const kini_wchar *t, kini_line l, size_t *a, size_t *b, size_t *c, size_t *d)
{
    size_t s = l.start, e = l.end, eq;
    kini_trim(t, &s, &e);
    if (s == e)
        return KINI_BLANK;
    if (t[s] == ';')
        return KINI_COMMENT;
    if (t[s] == '[') {
        size_t close;
        for (close = s + 1; close < e && t[close] != ']'; ++close)
            ;
        if (close < e) {
            *a = s + 1;
            *b = close;
            return KINI_SECTION;
        }
    }
    for (eq = s; eq < e && t[eq] != '='; ++eq)
        ;
    if (eq == e)
        return KINI_TEXT;
    *a = s;
    *b = eq;
    kini_trim(t, a, b);
    *c = eq + 1;
    *d = e;
    kini_trim(t, c, d);
    return KINI_PAIR;
}

static inline void kini_unquote(const kini_wchar *t, size_t *c, size_t *d)
{
    if (*d - *c >= 2 && (t[*c] == '"' || t[*c] == '\'') && t[*d - 1] == t[*c]) {
        ++*c;
        --*d;
    }
}

/* ---- reading -------------------------------------------------------------------------------------------------------------- */

/* Finds the value of key in the sections called sec. Returns 1 and [*vs, *ve) (unquoted) when present. */
static inline int kini_find_value(const kini_wchar *t, size_t n, const kini_wchar *sec, const kini_wchar *key, size_t *vs, size_t *ve)
{
    int in = 0;
    kini_line l;
    for (size_t pos = 0; kini_next_line(t, n, pos, &l); pos = l.next) {
        size_t a, b, c, d;
        switch (kini_classify(t, l, &a, &b, &c, &d)) {
        case KINI_SECTION: in = kini_name_eq(t + a, b - a, sec); break;
        case KINI_PAIR:
            if (in && kini_name_eq(t + a, b - a, key)) {
                kini_unquote(t, &c, &d);
                *vs = c;
                *ve = d;
                return 1;
            }
            break;
        default: break;
        }
    }
    return 0;
}

/* Copies [s, e) to out as a NUL-terminated string cut to cap; returns the characters stored (not counting the NUL). */
static inline size_t kini_copy_str(const kini_wchar *s, size_t n, kini_wchar *out, size_t cap)
{
    size_t k = n < cap ? n : (cap ? cap - 1 : 0);
    if (!cap)
        return 0;
    if (k)
        memcpy(out, s, k * sizeof *out);
    out[k] = 0;
    return k;
}

/* GetPrivateProfileString for a section and key. A missing value yields the default (empty when NULL). */
static inline size_t kini_get_string(const kini_wchar *t, size_t n, const kini_wchar *sec, const kini_wchar *key,
                                     const kini_wchar *def, kini_wchar *out, size_t cap)
{
    size_t vs, ve;
    if (kini_find_value(t, n, sec, key, &vs, &ve))
        return kini_copy_str(t + vs, ve - vs, out, cap);
    return kini_copy_str(def, kini_len(def), out, cap);
}

/* Appends one string and its NUL to a double-null list; stops (and sets *full) when it does not fit. */
static inline void kini_list_add(kini_wchar *out, size_t cap, size_t *used, const kini_wchar *s, size_t n, int *full)
{
    if (*full)
        return;
    if (*used + n + 2 > cap) {
        size_t room = cap > *used + 2 ? cap - *used - 2 : 0;
        if (room)
            memcpy(out + *used, s, room * sizeof *out);
        *used += room;
        *full = 1;
        return;
    }
    if (n)
        memcpy(out + *used, s, n * sizeof *out);
    out[*used + n] = 0;
    *used += n + 1;
}

/* Terminates a list: every string already carries its NUL, so one more NUL ends it. The return value is what Windows
 * reports: the characters before the last string's terminator (0 for an empty list), or cap - 2 when the list was cut. */
static inline size_t kini_list_end(kini_wchar *out, size_t cap, size_t used, int full)
{
    if (cap == 0)
        return 0;
    if (cap == 1) {
        out[0] = 0;
        return 0;
    }
    if (full || used + 1 > cap) {
        out[cap - 2] = 0;
        out[cap - 1] = 0;
        return cap - 2;
    }
    out[used] = 0;
    if (!used)
        out[1] = 0; /* an empty list is two NULs */
    return used ? used - 1 : 0;
}

/* The keys of a section (key == NULL in GetPrivateProfileString) or, with sec == NULL, the section names. A list ends
 * with an extra NUL; the return value counts the characters before it. */
static inline size_t kini_get_names(const kini_wchar *t, size_t n, const kini_wchar *sec, kini_wchar *out, size_t cap)
{
    size_t used = 0;
    int in = 0, full = 0;
    kini_line l;
    for (size_t pos = 0; kini_next_line(t, n, pos, &l); pos = l.next) {
        size_t a, b, c, d;
        switch (kini_classify(t, l, &a, &b, &c, &d)) {
        case KINI_SECTION:
            if (!sec)
                kini_list_add(out, cap, &used, t + a, b - a, &full);
            else
                in = kini_name_eq(t + a, b - a, sec);
            break;
        case KINI_PAIR:
            if (sec && in)
                kini_list_add(out, cap, &used, t + a, b - a, &full);
            break;
        default: break;
        }
    }
    return kini_list_end(out, cap, used, full);
}

/* GetPrivateProfileSection: the "key=value" lines of a section, comments left out. */
static inline size_t kini_get_section(const kini_wchar *t, size_t n, const kini_wchar *sec, kini_wchar *out, size_t cap)
{
    size_t used = 0;
    int in = 0, full = 0;
    kini_line l;
    for (size_t pos = 0; kini_next_line(t, n, pos, &l); pos = l.next) {
        size_t a, b, c, d;
        kini_kind k = kini_classify(t, l, &a, &b, &c, &d);
        if (k == KINI_SECTION) {
            in = kini_name_eq(t + a, b - a, sec);
        } else if (in && (k == KINI_PAIR || k == KINI_TEXT)) {
            size_t s = l.start, e = l.end;
            kini_trim(t, &s, &e);
            kini_list_add(out, cap, &used, t + s, e - s, &full);
        }
    }
    return kini_list_end(out, cap, used, full);
}

/* GetPrivateProfileInt: the value as a base-10 integer (a leading minus allowed), or def when the key is absent. */
static inline uint32_t kini_get_int(const kini_wchar *t, size_t n, const kini_wchar *sec, const kini_wchar *key, int32_t def)
{
    size_t vs, ve, i;
    int neg = 0;
    uint32_t v = 0;
    if (!kini_find_value(t, n, sec, key, &vs, &ve))
        return (uint32_t)def;
    i = vs;
    if (i < ve && t[i] == '-') {
        neg = 1;
        ++i;
    }
    for (; i < ve && t[i] >= '0' && t[i] <= '9'; ++i)
        v = v * 10 + (uint32_t)(t[i] - '0');
    return neg ? (uint32_t)(0 - v) : v;
}

/* ---- writing ---------------------------------------------------------------------------------------------------------------- */

/* The text produced by an edit goes to out; the function always returns the length the whole result needs, and writes
 * only as much as cap allows, so a caller can size a buffer by calling with cap 0. */
typedef struct {
    kini_wchar *out;
    size_t cap, len;
} kini_sink;

static inline void kini_put(kini_sink *k, const kini_wchar *s, size_t n)
{
    if (n && k->out && k->len + n <= k->cap)
        memcpy(k->out + k->len, s, n * sizeof *s);
    k->len += n;
}

static inline void kini_put_ascii(kini_sink *k, const char *s)
{
    for (; *s; ++s) {
        kini_wchar c = (unsigned char)*s;
        kini_put(k, &c, 1);
    }
}

/* The terminator style of the text: that of its first line break, CRLF when there is none. */
static inline const char *kini_eol_of(const kini_wchar *t, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (t[i] == '\r')
            return i + 1 < n && t[i + 1] == '\n' ? "\r\n" : "\r";
        if (t[i] == '\n')
            return "\n";
    }
    return "\r\n";
}

static inline int kini_ends_with_eol(const kini_wchar *t, size_t n) { return n == 0 || kini_is_eol(t[n - 1]); }

/*
 * WritePrivateProfileString. value == NULL removes the key; key == NULL removes the whole section; otherwise key=value is
 * set. Returns the length of the new text.
 */
static inline size_t kini_set(const kini_wchar *t, size_t n, const kini_wchar *sec, const kini_wchar *key, const kini_wchar *value,
                              kini_wchar *out, size_t cap)
{
    kini_sink k = {out, cap, 0};
    const char *eol = kini_eol_of(t, n);
    int in = 0, done = 0, section_seen = 0;
    size_t insert_at = 0; /* where a new key goes: just after the last pair or text line of the section */
    size_t copied = 0;
    kini_line l;

    if (!key) { /* remove a whole section */
        for (size_t pos = 0; kini_next_line(t, n, pos, &l); pos = l.next) {
            size_t a, b, c, d;
            kini_kind kind = kini_classify(t, l, &a, &b, &c, &d);
            if (kind == KINI_SECTION)
                in = kini_name_eq(t + a, b - a, sec);
            if (!in)
                kini_put(&k, t + l.start, l.next - l.start);
        }
        return k.len;
    }
    /* find the section's insertion point and the key's line */
    {
        size_t key_s = 0, key_e = 0;
        int found_key = 0;
        in = 0;
        for (size_t pos = 0; kini_next_line(t, n, pos, &l); pos = l.next) {
            size_t a, b, c, d;
            kini_kind kind = kini_classify(t, l, &a, &b, &c, &d);
            if (kind == KINI_SECTION) {
                in = kini_name_eq(t + a, b - a, sec);
                if (in) {
                    section_seen = 1;
                    if (!found_key && insert_at < l.next)
                        insert_at = l.next;
                }
            } else if (in && (kind == KINI_PAIR || kind == KINI_TEXT)) {
                if (!found_key)
                    insert_at = l.next;
                if (kind == KINI_PAIR && !found_key && kini_name_eq(t + a, b - a, key)) {
                    found_key = 1;
                    key_s = l.start;
                    key_e = l.next;
                }
            }
        }
        if (found_key) {
            copied = key_s;
            kini_put(&k, t, key_s);
            if (value) {
                kini_put(&k, key, kini_len(key));
                kini_put_ascii(&k, "=");
                kini_put(&k, value, kini_len(value));
                kini_put_ascii(&k, eol);
            }
            kini_put(&k, t + key_e, n - key_e);
            return k.len;
        }
    }
    if (!value) { /* nothing to remove */
        kini_put(&k, t, n);
        return k.len;
    }
    if (section_seen) {
        kini_put(&k, t, insert_at);
        if (insert_at > 0 && !kini_is_eol(t[insert_at - 1]))
            kini_put_ascii(&k, eol);
        kini_put(&k, key, kini_len(key));
        kini_put_ascii(&k, "=");
        kini_put(&k, value, kini_len(value));
        kini_put_ascii(&k, eol);
        kini_put(&k, t + insert_at, n - insert_at);
        return k.len;
    }
    (void)done;
    (void)copied;
    kini_put(&k, t, n);
    if (!kini_ends_with_eol(t, n))
        kini_put_ascii(&k, eol);
    kini_put_ascii(&k, "[");
    kini_put(&k, sec, kini_len(sec));
    kini_put_ascii(&k, "]");
    kini_put_ascii(&k, eol);
    kini_put(&k, key, kini_len(key));
    kini_put_ascii(&k, "=");
    kini_put(&k, value, kini_len(value));
    kini_put_ascii(&k, eol);
    return k.len;
}

/* WritePrivateProfileSection: replace the section's lines with the "key=value" strings of the double-null list `pairs`
 * (or add the section). A NULL list removes the section. */
static inline size_t kini_set_section(const kini_wchar *t, size_t n, const kini_wchar *sec, const kini_wchar *pairs, kini_wchar *out,
                                      size_t cap)
{
    kini_sink k = {out, cap, 0};
    const char *eol = kini_eol_of(t, n);
    int in = 0, wrote = 0;
    kini_line l;
    for (size_t pos = 0; kini_next_line(t, n, pos, &l); pos = l.next) {
        size_t a, b, c, d;
        kini_kind kind = kini_classify(t, l, &a, &b, &c, &d);
        if (kind == KINI_SECTION) {
            in = kini_name_eq(t + a, b - a, sec);
            if (in && !wrote && pairs) {
                kini_put(&k, t + l.start, l.next - l.start);
                if (l.next == l.end)
                    kini_put_ascii(&k, eol);
                for (const kini_wchar *p = pairs; *p; p += kini_len(p) + 1) {
                    kini_put(&k, p, kini_len(p));
                    kini_put_ascii(&k, eol);
                }
                wrote = 1;
                continue;
            }
        }
        if (in)
            continue; /* the old lines of the section (and a later section of the same name) are dropped */
        kini_put(&k, t + l.start, l.next - l.start);
        if (l.next == l.end && l.next == n)
            kini_put_ascii(&k, eol);
    }
    if (!wrote && pairs) {
        kini_put_ascii(&k, "[");
        kini_put(&k, sec, kini_len(sec));
        kini_put_ascii(&k, "]");
        kini_put_ascii(&k, eol);
        for (const kini_wchar *p = pairs; *p; p += kini_len(p) + 1) {
            kini_put(&k, p, kini_len(p));
            kini_put_ascii(&k, eol);
        }
    }
    return k.len;
}

/* ---- Binary structures --------------------------------------------------------------------------------------------------------- */

/* A structure is stored as hexadecimal bytes followed by one checksum byte (the sum of the data bytes modulo 256). */
static inline size_t kini_hex_encode(const uint8_t *data, size_t n, kini_wchar *out)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t sum = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        sum = (uint8_t)(sum + data[i]);
        out[o++] = (kini_wchar)hex[data[i] >> 4];
        out[o++] = (kini_wchar)hex[data[i] & 15];
    }
    out[o++] = (kini_wchar)hex[sum >> 4];
    out[o++] = (kini_wchar)hex[sum & 15];
    return o;
}

static inline int kini_hex_digit(kini_wchar c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* Decodes n data bytes from [s, s+len): 0 when the text is not exactly that many bytes plus a matching checksum. */
static inline int kini_hex_decode(const kini_wchar *s, size_t len, uint8_t *out, size_t n)
{
    uint8_t sum = 0;
    if (len != (n + 1) * 2)
        return 0;
    for (size_t i = 0; i <= n; ++i) {
        int h = kini_hex_digit(s[2 * i]), l = kini_hex_digit(s[2 * i + 1]);
        uint8_t v;
        if (h < 0 || l < 0)
            return 0;
        v = (uint8_t)(h << 4 | l);
        if (i < n) {
            out[i] = v;
            sum = (uint8_t)(sum + v);
        } else if (v != sum) {
            return 0;
        }
    }
    return 1;
}

#endif /* K32_INI_PURE_H */
