/*
 * shl_path_pure.h - the string and path functions of shlwapi.dll on UTF-16 text, without any operating system call: the Path*
 * family that cuts, joins and tests path strings, and the Str* family of searches and comparisons.
 *
 * The behaviour is that of Windows, including its quirks: a path is cut only at backslashes (a forward slash is part of a
 * name) when it is canonicalized, but PathFindFileName treats both; PathFindExtension restarts at a space; PathCommonPrefix of a
 * drive root reports three characters. Buffers are taken to hold SHL_MAX_PATH characters, as the callers assume.
 * The functions that look at the file system (PathFileExists, PathIsDirectory, ...) are in shlwapi.c.
 * Host tests: tests/ntdll-host/test_shl_path_pure.c.
 */
#ifndef SHL_PATH_PURE_H
#define SHL_PATH_PURE_H

#include <stddef.h>
#include <stdint.h>

typedef uint16_t shl_wchar;

#define SHL_MAX_PATH 260

/* ---- Characters ---------------------------------------------------------------------------------------------------------------- */

static inline shl_wchar shl_upper(shl_wchar c)
{
    if (c >= 'a' && c <= 'z')
        return (shl_wchar)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return (shl_wchar)(c - 32);
    if (c == 0xFF)
        return 0x178;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2)
        return (shl_wchar)(c - 32);
    if (c >= 0x430 && c <= 0x44F)
        return (shl_wchar)(c - 32);
    if (c >= 0x450 && c <= 0x45F)
        return (shl_wchar)(c - 80);
    return c;
}

static inline shl_wchar shl_lower(shl_wchar c)
{
    if (c >= 'A' && c <= 'Z')
        return (shl_wchar)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return (shl_wchar)(c + 32);
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2)
        return (shl_wchar)(c + 32);
    if (c >= 0x410 && c <= 0x42F)
        return (shl_wchar)(c + 32);
    if (c >= 0x400 && c <= 0x40F)
        return (shl_wchar)(c + 80);
    return c;
}

static inline int shl_isspace(shl_wchar c) { return c == ' ' || (c >= 9 && c <= 13); }
static inline int shl_isdigit(shl_wchar c) { return c >= '0' && c <= '9'; }
static inline int shl_isalnum(shl_wchar c) { return shl_isdigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0; }

static inline size_t shl_len(const shl_wchar *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

static inline void shl_copy(shl_wchar *d, const shl_wchar *s)
{
    while ((*d++ = *s++) != 0) {
    }
}

static inline shl_wchar *shl_chr(const shl_wchar *s, shl_wchar c)
{
    for (; *s; ++s)
        if (*s == c)
            return (shl_wchar *)s;
    return c == 0 ? (shl_wchar *)s : NULL;
}

/* ---- Roots and flags --------------------------------------------------------------------------------------------------------- */

static inline int shl_is_unc(const shl_wchar *p) { return p && p[0] == '\\' && p[1] == '\\'; }

/* No drive, no root slash: the path is relative. */
static inline int shl_is_relative(const shl_wchar *p)
{
    if (!p || !*p)
        return 1;
    return !(*p == '\\' || (*p && p[1] == ':'));
}

static inline int shl_is_unc_server(const shl_wchar *p)
{
    if (!(p && p[0] == '\\' && p[1] == '\\'))
        return 0;
    return shl_chr(p + 2, '\\') == NULL;
}

/* \\server\share with nothing after the share */
static inline int shl_is_unc_server_share(const shl_wchar *p)
{
    int seen = 0;
    if (!(p && p[0] == '\\' && p[1] == '\\'))
        return 0;
    for (p += 2; *p; ++p)
        if (*p == '\\') {
            if (seen)
                return 0;
            seen = 1;
        }
    return seen;
}

/* "X:\", "\", or a UNC server or share. */
static inline int shl_is_root(const shl_wchar *p)
{
    if (!p || !*p)
        return 0;
    if (p[1] == ':' && p[2] == '\\' && p[3] == 0)
        return 1;
    if (*p == '\\') {
        if (!p[1])
            return 1;
        if (p[1] == '\\') {
            int seen = 0;
            for (p += 2; *p; ++p)
                if (*p == '\\') {
                    if (seen)
                        return 0;
                    seen = 1;
                }
            return 1;
        }
    }
    return 0;
}

static inline shl_wchar *shl_skip_root(const shl_wchar *p)
{
    if (!p || !*p)
        return NULL;
    if (p[0] == '\\' && p[1] == '\\') {
        p += 2;
        if ((p = shl_chr(p, '\\')) && (p = shl_chr(p + 1, '\\')))
            ++p;
        return (shl_wchar *)p;
    }
    if (p[0] && p[1] == ':' && p[2] == '\\')
        return (shl_wchar *)p + 3;
    return NULL;
}

static inline int shl_drive_number(const shl_wchar *p)
{
    if (!p)
        return -1;
    if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\')
        p += 4;
    if (!p[0] || p[1] != ':')
        return -1;
    if (p[0] >= 'A' && p[0] <= 'Z')
        return p[0] - 'A';
    if (p[0] >= 'a' && p[0] <= 'z')
        return p[0] - 'a';
    return -1;
}

/* ---- Finding parts ------------------------------------------------------------------------------------------------------------ */

static inline shl_wchar *shl_find_file_name(const shl_wchar *p)
{
    const shl_wchar *last = p;
    while (p && *p) {
        if ((*p == '\\' || *p == '/' || *p == ':') && p[1] && p[1] != '\\' && p[1] != '/')
            last = p + 1;
        ++p;
    }
    return (shl_wchar *)last;
}

/* The last '.' that follows the last backslash or space; the end of the string if there is none. */
static inline shl_wchar *shl_find_extension(const shl_wchar *p)
{
    const shl_wchar *dot = NULL;
    if (!p)
        return NULL;
    for (; *p; ++p) {
        if (*p == '\\' || *p == ' ')
            dot = NULL;
        else if (*p == '.')
            dot = p;
    }
    return (shl_wchar *)(dot ? dot : p);
}

static inline shl_wchar *shl_find_next_component(const shl_wchar *p)
{
    shl_wchar *slash;
    if (!p || !*p)
        return NULL;
    if ((slash = shl_chr(p, '\\')) != NULL) {
        if (slash[1] == '\\')
            ++slash;
        return slash + 1;
    }
    return (shl_wchar *)p + shl_len(p);
}

/* The text after the first unquoted space. */
static inline shl_wchar *shl_get_args(const shl_wchar *p)
{
    int quoted = 0;
    if (!p)
        return NULL;
    for (; *p; ++p) {
        if (*p == ' ' && !quoted)
            return (shl_wchar *)p + 1;
        if (*p == '"')
            quoted = !quoted;
    }
    return (shl_wchar *)p;
}

/* Cuts the arguments (and the space before them) off a command line; with no arguments, drops one trailing space. */
static inline void shl_remove_args(shl_wchar *p)
{
    shl_wchar *args;
    if (!p)
        return;
    args = shl_get_args(p);
    if (*args) {
        args[-1] = 0;
    } else if (args > p && args[-1] == ' ') {
        args[-1] = 0;
    }
}

static inline int shl_is_file_spec(const shl_wchar *p)
{
    if (!p)
        return 0;
    for (; *p; ++p)
        if (*p == '\\' || *p == ':')
            return 0;
    return 1;
}

/* ---- Editing ---------------------------------------------------------------------------------------------------------------------- */

/* Returns the end of the string, or NULL if the path is too long to take another character. */
static inline shl_wchar *shl_add_backslash(shl_wchar *p)
{
    size_t len;
    if (!p || (len = shl_len(p)) >= SHL_MAX_PATH)
        return NULL;
    if (len) {
        p += len;
        if (p[-1] != '\\') {
            *p++ = '\\';
            *p = 0;
        }
    }
    return p;
}

static inline int shl_add_extension(shl_wchar *p, const shl_wchar *ext)
{
    size_t len;
    if (!p || !ext || *shl_find_extension(p))
        return 0;
    len = shl_len(p);
    if (len + shl_len(ext) >= SHL_MAX_PATH)
        return 0;
    shl_copy(p + len, ext);
    return 1;
}

static inline void shl_remove_extension(shl_wchar *p)
{
    if (!p)
        return;
    p = shl_find_extension(p);
    if (p && *p)
        *p = 0;
}

static inline int shl_rename_extension(shl_wchar *p, const shl_wchar *ext)
{
    shl_wchar *e = shl_find_extension(p);
    if (!e || (size_t)(e - p) + shl_len(ext) >= SHL_MAX_PATH)
        return 0;
    shl_copy(e, ext);
    return 1;
}

static inline void shl_strip_path(shl_wchar *p)
{
    shl_wchar *name = shl_find_file_name(p);
    if (name != p)
        shl_copy(p, name);
}

static inline void shl_unquote_spaces(shl_wchar *p)
{
    size_t len;
    if (!p || *p != '"')
        return;
    len = shl_len(p) - 1;
    if (p[len] == '"') {
        p[len] = 0;
        for (; *p; ++p)
            *p = p[1];
    }
}

static inline void shl_quote_spaces(shl_wchar *p)
{
    if (p && shl_chr(p, ' ')) {
        size_t len = shl_len(p) + 1, i;
        if (len + 2 < SHL_MAX_PATH) {
            for (i = len; i > 0; --i)
                p[i] = p[i - 1];
            p[0] = '"';
            p[len] = '"';
            p[len + 1] = 0;
        }
    }
}

static inline void shl_remove_blanks(shl_wchar *p)
{
    shl_wchar *start, *first;
    if (!p || !*p)
        return;
    start = first = p;
    while (*p == ' ')
        ++p;
    while (*p)
        *start++ = *p++;
    if (start != first)
        while (start[-1] == ' ')
            --start;
    *start = 0;
}

/* Returns the last character position, as Windows does. */
static inline shl_wchar *shl_remove_backslash(shl_wchar *p)
{
    shl_wchar *last;
    size_t len;
    if (!p)
        return NULL;
    len = shl_len(p);
    last = p + (len ? len - 1 : 0);
    if (len && !shl_is_root(p) && *last == '\\')
        *last = 0;
    return last;
}

static inline int shl_remove_file_spec(shl_wchar *p)
{
    shl_wchar *root_end, *last, *q;
    if (!p || !*p)
        return 0;
    if (p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))) {
        root_end = p + 2;
        if (*root_end == '\\')
            ++root_end;
    } else {
        root_end = p;
        if (*root_end == '\\')
            ++root_end;
        if (root_end[1] != '?') {
            if (*root_end == '\\')
                ++root_end;
            if (root_end - p > 1 && root_end[1] == ':')
                root_end += 2;
            if (*root_end == '\\' && root_end[1] && root_end[1] != '\\')
                ++root_end;
        }
    }
    last = NULL;
    for (q = root_end; *q; ++q)
        if (*q == '\\')
            last = q;
    if (last && last != root_end) {
        if (last[-1] == '\\')
            --last;
        *last = 0;
        return 1;
    }
    if (!*root_end)
        return 0;
    *root_end = 0;
    return 1;
}

static inline int shl_strip_to_root(shl_wchar *p)
{
    if (!p)
        return 0;
    while (!shl_is_root(p))
        if (!shl_remove_file_spec(p))
            return 0;
    return 1;
}

/* "X:\" in the buffer for drive 0..25; the buffer is left alone otherwise. */
static inline shl_wchar *shl_build_root(shl_wchar *p, int drive)
{
    if (!p)
        return NULL;
    if (drive < 0 || drive > 25) {
        p[0] = 0;
        return p;
    }
    p[0] = (shl_wchar)('A' + drive);
    p[1] = ':';
    p[2] = '\\';
    p[3] = 0;
    return p;
}

/* ---- Canonical forms and joining ----------------------------------------------------------------------------------------------- */

/* Resolves "." and ".." between backslashes. Only the backslash separates; "a/b" is one name. dest takes at most SHL_MAX_PATH. */
static inline int shl_canonicalize(shl_wchar *buffer, const shl_wchar *path)
{
    const shl_wchar *src = path;
    shl_wchar *dst = buffer;

    if (dst)
        *dst = 0;
    if (!dst || !path)
        return 0;
    if (!*path) {
        *dst++ = '\\';
        *dst = 0;
        return 1;
    }
    if (*src == '\\') {
        *dst++ = *src++;
    } else if (*src && src[1] == ':') {
        *dst++ = *src++;
        *dst++ = *src++;
        if (*src == '\\')
            *dst++ = *src++;
    }
    while (*src) {
        if (*src == '.') {
            if (src[1] == '\\' && (src == path || src[-1] == '\\' || src[-1] == ':')) {
                src += 2; /* ".\" */
            } else if (src[1] == '.' && dst != buffer && dst[-1] == '\\') {
                /* "\.." backs up a directory; it does not remove a UNC server name or the leading "\\" */
                *dst = 0;
                if (dst > buffer + 1 && dst[-1] == '\\' && (dst[-2] != '\\' || dst > buffer + 2)) {
                    if (dst[-2] == ':' && (dst > buffer + 3 || dst[-3] == ':')) {
                        dst -= 2;
                        while (dst > buffer && *dst != '\\')
                            --dst;
                        if (*dst == '\\')
                            ++dst;
                        else
                            dst = buffer;
                    } else if (dst[-2] != ':' && !shl_is_unc_server_share(buffer)) {
                        dst -= 2;
                    }
                }
                while (dst > buffer && *dst != '\\')
                    --dst;
                if (dst == buffer) {
                    *dst++ = '\\';
                    ++src;
                }
                src += 2; /* ".." */
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    if (dst - buffer == 2 && dst[-1] == ':')
        *dst++ = '\\'; /* a bare drive gets its root */
    *dst = 0;
    return 1;
}

/* Joins dir and file and canonicalizes the result. An absolute file replaces dir; a "\file" is rooted in dir's root. Returns
 * NULL (and an empty dest) when the result is too long. */
static inline shl_wchar *shl_combine(shl_wchar *dst, const shl_wchar *dir, const shl_wchar *file)
{
    shl_wchar tmp[SHL_MAX_PATH];
    int use_both = 0, strip = 0;
    size_t n;

    if (!dst)
        return NULL;
    if (!dir && !file) {
        dst[0] = 0;
        return NULL;
    }
    if ((!file || !*file) && dir) {
        n = shl_len(dir);
        if (n >= SHL_MAX_PATH)
            n = SHL_MAX_PATH - 1;
        for (size_t i = 0; i < n; ++i)
            tmp[i] = dir[i];
        tmp[n] = 0;
    } else if (!dir || !*dir || !shl_is_relative(file)) {
        if (!dir || !*dir || *file != '\\' || shl_is_unc(file)) {
            n = shl_len(file);
            if (n >= SHL_MAX_PATH)
                n = SHL_MAX_PATH - 1;
            for (size_t i = 0; i < n; ++i)
                tmp[i] = file[i];
            tmp[n] = 0;
        } else {
            use_both = 1;
            strip = 1;
        }
    } else {
        use_both = 1;
    }
    if (use_both) {
        n = shl_len(dir);
        if (n >= SHL_MAX_PATH)
            n = SHL_MAX_PATH - 1;
        for (size_t i = 0; i < n; ++i)
            tmp[i] = dir[i];
        tmp[n] = 0;
        if (strip) {
            shl_strip_to_root(tmp);
            ++file;
        }
        if (!shl_add_backslash(tmp) || shl_len(tmp) + shl_len(file) >= SHL_MAX_PATH) {
            dst[0] = 0;
            return NULL;
        }
        shl_copy(tmp + shl_len(tmp), file);
    }
    shl_canonicalize(dst, tmp);
    return dst;
}

/* path = path \ more, after dropping the backslashes at the start of more (unless it is a UNC path). */
static inline int shl_append(shl_wchar *path, const shl_wchar *more)
{
    if (path && more) {
        if (!shl_is_unc(more))
            while (*more == '\\')
                ++more;
        if (shl_combine(path, path, more))
            return 1;
    }
    return 0;
}

/* The length of the leading part shared by two paths (up to a backslash boundary), copied into out. A bare drive reports 3. */
static inline int shl_common_prefix(const shl_wchar *file1, const shl_wchar *file2, shl_wchar *out)
{
    const shl_wchar *i1 = file1, *i2 = file2;
    unsigned len = 0;
    if (out)
        *out = 0;
    if (!file1 || !file2)
        return 0;
    if (shl_is_unc(file1)) {
        if (!shl_is_unc(file2))
            return 0;
        i1 += 2;
        i2 += 2;
    } else if (shl_is_unc(file2)) {
        return 0;
    }
    for (;;) {
        if ((!*i1 || *i1 == '\\') && (!*i2 || *i2 == '\\'))
            len = (unsigned)(i1 - file1);
        if (!*i1 || shl_upper(*i1) != shl_upper(*i2))
            break;
        ++i1;
        ++i2;
    }
    if (len == 2)
        ++len;
    if (len && out) {
        for (unsigned k = 0; k < len; ++k)
            out[k] = file1[k];
        out[len] = 0;
    }
    return (int)len;
}

static inline int shl_is_prefix(const shl_wchar *prefix, const shl_wchar *path)
{
    return prefix && path && shl_common_prefix(path, prefix, NULL) == (int)shl_len(prefix);
}

static inline int shl_is_same_root(const shl_wchar *p1, const shl_wchar *p2)
{
    const shl_wchar *start;
    int len;
    if (!p1 || !p2 || !(start = shl_skip_root(p1)))
        return 0;
    len = shl_common_prefix(p1, p2, NULL) + 1;
    return start - p1 <= len;
}

/* ---- Wildcards -------------------------------------------------------------------------------------------------------------------- */

static inline int shl_match_mask(const shl_wchar *name, const shl_wchar *mask)
{
    while (*name && *mask && *mask != ';') {
        if (*mask == '*') {
            do {
                if (shl_match_mask(name, mask + 1))
                    return 1;
            } while (*name++);
            return 0;
        }
        if (shl_upper(*mask) != shl_upper(*name) && *mask != '?')
            return 0;
        ++name;
        ++mask;
    }
    if (!*name) {
        while (*mask == '*')
            ++mask;
        if (!*mask || *mask == ';')
            return 1;
    }
    return 0;
}

/* Whether the path matches any of the ';'-separated masks. "*.*" matches everything. */
static inline int shl_match_spec(const shl_wchar *path, const shl_wchar *mask)
{
    if (mask[0] == '*' && mask[1] == '.' && mask[2] == '*' && mask[3] == 0)
        return 1;
    while (*mask) {
        while (*mask == ' ')
            ++mask;
        if (shl_match_mask(path, mask))
            return 1;
        while (*mask && *mask != ';')
            ++mask;
        if (*mask == ';')
            ++mask;
    }
    return 0;
}

/* ---- Characters of file names ----------------------------------------------------------------------------------------------- */

#define SHL_GCT_INVALID 0x0u
#define SHL_GCT_LFNCHAR 0x1u
#define SHL_GCT_SHORTCHAR 0x2u
#define SHL_GCT_WILD 0x4u
#define SHL_GCT_SEPARATOR 0x8u

static inline unsigned shl_char_type(shl_wchar ch)
{
    unsigned flags = 0;
    if (!ch || ch < ' ' || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '/')
        return SHL_GCT_INVALID;
    if (ch == '*' || ch == '?')
        return SHL_GCT_WILD;
    if (ch == '\\' || ch == ':')
        return SHL_GCT_SEPARATOR;
    if (ch < 126) {
        if (((ch & 1) && ch != ';') || shl_isalnum(ch) || ch == '$' || ch == '&' || ch == '(' || ch == '.' || ch == '@' || ch == '^' ||
            ch == '\'' || ch == '`')
            flags |= SHL_GCT_SHORTCHAR;
    } else {
        flags |= SHL_GCT_SHORTCHAR;
    }
    return flags | SHL_GCT_LFNCHAR;
}

/* ---- Strings -------------------------------------------------------------------------------------------------------------------- */

static inline shl_wchar *shl_str_chr_i(const shl_wchar *s, shl_wchar c)
{
    c = shl_upper(c);
    for (; *s; ++s)
        if (shl_upper(*s) == c)
            return (shl_wchar *)s;
    return NULL;
}

static inline shl_wchar *shl_str_rchr(const shl_wchar *s, const shl_wchar *end, shl_wchar c)
{
    const shl_wchar *last = NULL;
    for (; *s && (!end || s < end); ++s)
        if (*s == c)
            last = s;
    return (shl_wchar *)last;
}

static inline shl_wchar *shl_str_rchr_i(const shl_wchar *s, const shl_wchar *end, shl_wchar c)
{
    const shl_wchar *last = NULL;
    c = shl_upper(c);
    for (; *s && (!end || s < end); ++s)
        if (shl_upper(*s) == c)
            last = s;
    return (shl_wchar *)last;
}

static inline int shl_str_cmp_n(const shl_wchar *a, const shl_wchar *b, size_t n, int fold)
{
    for (; n; --n, ++a, ++b) {
        shl_wchar x = fold ? shl_upper(*a) : *a, y = fold ? shl_upper(*b) : *b;
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            return 0;
    }
    return 0;
}

static inline int shl_str_cmp(const shl_wchar *a, const shl_wchar *b, int fold) { return shl_str_cmp_n(a, b, (size_t)-1, fold); }

static inline shl_wchar *shl_str_str(const shl_wchar *s, const shl_wchar *sub, int fold)
{
    size_t n = shl_len(sub);
    if (!n)
        return NULL;
    for (; *s; ++s)
        if (shl_str_cmp_n(s, sub, n, fold) == 0 && shl_len(s) >= n)
            return (shl_wchar *)s;
    return NULL;
}

static inline shl_wchar *shl_str_rstr_i(const shl_wchar *s, const shl_wchar *sub)
{
    size_t n = shl_len(sub);
    const shl_wchar *last = NULL;
    if (!n)
        return NULL;
    for (; *s; ++s)
        if (shl_len(s) >= n && shl_str_cmp_n(s, sub, n, 1) == 0)
            last = s;
    return (shl_wchar *)last;
}

static inline int shl_in_set(shl_wchar c, const shl_wchar *set, int fold)
{
    for (; *set; ++set)
        if (fold ? shl_upper(*set) == shl_upper(c) : *set == c)
            return 1;
    return 0;
}

static inline int shl_str_spn(const shl_wchar *s, const shl_wchar *set)
{
    int n = 0;
    while (s[n] && shl_in_set(s[n], set, 0))
        ++n;
    return n;
}

static inline int shl_str_cspn(const shl_wchar *s, const shl_wchar *set, int fold)
{
    int n = 0;
    while (s[n] && !shl_in_set(s[n], set, fold))
        ++n;
    return n;
}

static inline shl_wchar *shl_str_pbrk(const shl_wchar *s, const shl_wchar *set)
{
    for (; *s; ++s)
        if (shl_in_set(*s, set, 0))
            return (shl_wchar *)s;
    return NULL;
}

/* StrToInt64Ex: leading blanks, a sign, decimal digits (or "0x" and hex digits when allowed). Returns 0 when no digits follow. */
static inline int shl_str_to_int64(const shl_wchar *s, int allow_hex, int64_t *out)
{
    int neg = 0, hex = 0, any = 0;
    uint64_t v = 0;
    if (!s || !out)
        return 0;
    while (*s && shl_isspace(*s))
        ++s;
    if (*s == '-') {
        neg = 1;
        ++s;
    } else if (*s == '+') {
        ++s;
    }
    if (allow_hex && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        hex = 1;
        s += 2;
    }
    for (;; ++s) {
        unsigned d;
        if (*s >= '0' && *s <= '9')
            d = (unsigned)(*s - '0');
        else if (hex && *s >= 'a' && *s <= 'f')
            d = (unsigned)(*s - 'a' + 10);
        else if (hex && *s >= 'A' && *s <= 'F')
            d = (unsigned)(*s - 'A' + 10);
        else
            break;
        v = v * (hex ? 16u : 10u) + d;
        any = 1;
    }
    if (!any)
        return 0;
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 1;
}

/* Removes the characters of `trim` from both ends. Returns 1 if anything was removed. */
static inline int shl_str_trim(shl_wchar *s, const shl_wchar *trim)
{
    shl_wchar *start = s, *end;
    size_t n, i;
    while (*start && shl_in_set(*start, trim, 0))
        ++start;
    end = start + shl_len(start);
    while (end > start && shl_in_set(end[-1], trim, 0))
        --end;
    n = (size_t)(end - start);
    if (start == s && s[n] == 0)
        return 0;
    for (i = 0; i < n; ++i)
        s[i] = start[i];
    s[n] = 0;
    return 1;
}

#endif
