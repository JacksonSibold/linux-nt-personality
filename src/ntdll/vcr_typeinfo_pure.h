/*
 * vcr_typeinfo_pure.h - the parts of type_info that need no operating system: comparison and hashing of decorated names, and
 * a decoder from the decorated name of a type (".?AVexception@std@@") to the readable name ("class std::exception").
 *
 * The decoder reads class, struct, union and enum names with their scopes, template instances with type arguments (each
 * template has its own table of back-references, as in the Microsoft scheme), the built-in types, and pointers and references.
 * Anything else (function types, non-type template arguments, lambdas) makes it fail, and the caller shows the decorated name.
 * Host tests: tests/ntdll-host/test_vcr_typeinfo_pure.c.
 */
#ifndef VCR_TYPEINFO_PURE_H
#define VCR_TYPEINFO_PURE_H

#include <stddef.h>
#include <stdint.h>

/* __std_type_info_compare: negative, zero or positive like strcmp, on the names without the leading '.'. */
static inline int vcr_ti_compare(const char *a, const char *b)
{
    if (a == b)
        return 0;
    ++a;
    ++b;
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* __std_type_info_hash: 64-bit FNV-1a over the name without the leading '.'. */
static inline uint64_t vcr_ti_hash(const char *name)
{
    uint64_t h = 14695981039346656037ull;
    for (++name; *name; ++name) {
        h ^= (unsigned char)*name;
        h *= 1099511628211ull;
    }
    return h;
}

/* ---- Decoder ------------------------------------------------------------------------------------------------------------------ */

#define VCR_UN_STR 400
#define VCR_UN_CTX 8
#define VCR_UN_PIECES 12

typedef struct {
    char s[VCR_UN_STR];
} vcr_un_str;

typedef struct {
    vcr_un_str names[10];
    int n;
} vcr_un_ctx;

typedef struct {
    const char *p;
    int err;
    int depth;
    vcr_un_ctx ctx[VCR_UN_CTX];
} vcr_un;

static inline size_t vcr_un_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

static inline void vcr_un_cat(vcr_un *u, char *dst, const char *src)
{
    size_t d = vcr_un_len(dst), i = 0;
    while (src[i]) {
        if (d + 1 >= VCR_UN_STR) {
            u->err = 1;
            break;
        }
        dst[d++] = src[i++];
    }
    dst[d] = 0;
}

static inline void vcr_un_remember(vcr_un *u, const char *s)
{
    vcr_un_ctx *c = &u->ctx[u->depth];
    if (c->n < 10) {
        size_t i = 0;
        while (s[i] && i + 1 < VCR_UN_STR) {
            c->names[c->n].s[i] = s[i];
            ++i;
        }
        c->names[c->n].s[i] = 0;
        ++c->n;
    }
}

static void vcr_un_type(vcr_un *u, char *out);

/* An identifier up to its '@'. */
static inline void vcr_un_simple(vcr_un *u, char *out, int memorize)
{
    size_t n = 0;
    out[0] = 0;
    if (*u->p == '?' || *u->p == '@' || !*u->p) {
        u->err = 1;
        return;
    }
    while (*u->p && *u->p != '@') {
        if (n + 1 >= VCR_UN_STR) {
            u->err = 1;
            return;
        }
        out[n++] = *u->p++;
    }
    out[n] = 0;
    if (*u->p != '@') {
        u->err = 1;
        return;
    }
    ++u->p;
    if (memorize)
        vcr_un_remember(u, out);
}

static inline void vcr_un_backref(vcr_un *u, char *out)
{
    int i = *u->p - '0';
    ++u->p;
    if (i >= u->ctx[u->depth].n) {
        u->err = 1;
        out[0] = 0;
        return;
    }
    out[0] = 0;
    vcr_un_cat(u, out, u->ctx[u->depth].names[i].s);
}

/* After "?$": name, arguments up to '@'. The table of back-references is a fresh one for the duration. */
static inline void vcr_un_template(vcr_un *u, char *out)
{
    int first = 1;
    out[0] = 0;
    if (u->depth + 1 >= VCR_UN_CTX) {
        u->err = 1;
        return;
    }
    ++u->depth;
    u->ctx[u->depth].n = 0;
    vcr_un_simple(u, out, 1);
    if (u->err) {
        --u->depth;
        return;
    }
    vcr_un_cat(u, out, "<");
    while (!u->err && *u->p && *u->p != '@') {
        char tmp[VCR_UN_STR];
        if (*u->p == '$') {
            u->err = 1; /* non-type arguments are not decoded */
            break;
        }
        vcr_un_type(u, tmp);
        if (u->err)
            break;
        if (!first)
            vcr_un_cat(u, out, ",");
        first = 0;
        vcr_un_cat(u, out, tmp);
    }
    if (!u->err && *u->p == '@')
        ++u->p;
    else
        u->err = 1;
    if (!u->err && out[vcr_un_len(out) - 1] == '>')
        vcr_un_cat(u, out, " ");
    vcr_un_cat(u, out, ">");
    --u->depth;
}

/* A qualified name: the innermost name first, then its scopes outward, ending in '@'. */
static inline void vcr_un_qname(vcr_un *u, char *out)
{
    char local[VCR_UN_PIECES][VCR_UN_STR];
    char piece[VCR_UN_STR];
    int n = 0, i;
    out[0] = 0;
    while (!u->err && *u->p && *u->p != '@') {
        piece[0] = 0;
        if (*u->p >= '0' && *u->p <= '9') {
            vcr_un_backref(u, piece);
        } else if (u->p[0] == '?' && u->p[1] == '$') {
            u->p += 2;
            vcr_un_template(u, piece);
            if (!u->err)
                vcr_un_remember(u, piece);
        } else if (u->p[0] == '?' && u->p[1] == 'A') {
            /* an anonymous namespace: ?A0x<hash>@ */
            ++u->p;
            while (*u->p && *u->p != '@')
                ++u->p;
            if (*u->p == '@')
                ++u->p;
            vcr_un_cat(u, piece, "`anonymous namespace'");
        } else {
            vcr_un_simple(u, piece, 1);
        }
        if (u->err)
            break;
        if (n >= VCR_UN_PIECES) {
            u->err = 1;
            break;
        }
        local[n][0] = 0;
        vcr_un_cat(u, local[n], piece);
        ++n;
    }
    if (!u->err && *u->p == '@')
        ++u->p;
    else
        u->err = 1;
    if (u->err || n == 0) {
        u->err = 1;
        return;
    }
    for (i = n - 1; i >= 0; --i) {
        vcr_un_cat(u, out, local[i]);
        if (i)
            vcr_un_cat(u, out, "::");
    }
}

static inline const char *vcr_un_builtin(char c)
{
    switch (c) {
    case 'X': return "void";
    case 'D': return "char";
    case 'C': return "signed char";
    case 'E': return "unsigned char";
    case 'F': return "short";
    case 'G': return "unsigned short";
    case 'H': return "int";
    case 'I': return "unsigned int";
    case 'J': return "long";
    case 'K': return "unsigned long";
    case 'M': return "float";
    case 'N': return "double";
    case 'O': return "long double";
    default: return NULL;
    }
}

static inline const char *vcr_un_cv(char c)
{
    switch (c) {
    case 'A': return "";
    case 'B': return " const";
    case 'C': return " volatile";
    case 'D': return " const volatile";
    default: return NULL;
    }
}

static void vcr_un_type(vcr_un *u, char *out)
{
    const char *b;
    char c = *u->p;
    out[0] = 0;
    if (u->err)
        return;
    if ((b = vcr_un_builtin(c)) != NULL) {
        ++u->p;
        vcr_un_cat(u, out, b);
        return;
    }
    switch (c) {
    case '_': {
        const char *w = NULL;
        switch (u->p[1]) {
        case 'N': w = "bool"; break;
        case 'J': w = "__int64"; break;
        case 'K': w = "unsigned __int64"; break;
        case 'W': w = "wchar_t"; break;
        case 'S': w = "char16_t"; break;
        case 'U': w = "char32_t"; break;
        case 'Q': w = "char8_t"; break;
        case 'L': w = "__int128"; break;
        case 'M': w = "unsigned __int128"; break;
        default: break;
        }
        if (!w) {
            u->err = 1;
            return;
        }
        u->p += 2;
        vcr_un_cat(u, out, w);
        return;
    }
    case 'T':
    case 'U':
    case 'V': {
        char name[VCR_UN_STR];
        ++u->p;
        vcr_un_qname(u, name);
        if (u->err)
            return;
        vcr_un_cat(u, out, c == 'T' ? "union " : (c == 'U' ? "struct " : "class "));
        vcr_un_cat(u, out, name);
        return;
    }
    case 'W': {
        char name[VCR_UN_STR];
        ++u->p; /* W, then the underlying type digit */
        if (*u->p < '0' || *u->p > '7') {
            u->err = 1;
            return;
        }
        ++u->p;
        vcr_un_qname(u, name);
        if (u->err)
            return;
        vcr_un_cat(u, out, "enum ");
        vcr_un_cat(u, out, name);
        return;
    }
    case 'P':
    case 'Q':
    case 'R':
    case 'S':
    case 'A': {
        char inner[VCR_UN_STR];
        const char *self = c == 'Q' ? " const" : (c == 'R' ? " volatile" : (c == 'S' ? " const volatile" : ""));
        const char *cv;
        int ptr64 = 0;
        ++u->p;
        if (*u->p == 'E') {
            ptr64 = 1;
            ++u->p;
        }
        cv = vcr_un_cv(*u->p);
        if (!cv) {
            u->err = 1;
            return;
        }
        ++u->p;
        vcr_un_type(u, inner);
        if (u->err)
            return;
        vcr_un_cat(u, out, inner);
        vcr_un_cat(u, out, cv);
        vcr_un_cat(u, out, c == 'A' ? " &" : " *");
        vcr_un_cat(u, out, self);
        if (ptr64)
            vcr_un_cat(u, out, " __ptr64");
        return;
    }
    case '$': {
        if (u->p[1] == '$' && u->p[2] == 'T') {
            u->p += 3;
            vcr_un_cat(u, out, "std::nullptr_t");
            return;
        }
        if (u->p[1] == '$' && u->p[2] == 'Q') {
            char inner[VCR_UN_STR];
            const char *cv;
            int ptr64 = 0;
            u->p += 3;
            if (*u->p == 'E') {
                ptr64 = 1;
                ++u->p;
            }
            cv = vcr_un_cv(*u->p);
            if (!cv) {
                u->err = 1;
                return;
            }
            ++u->p;
            vcr_un_type(u, inner);
            if (u->err)
                return;
            vcr_un_cat(u, out, inner);
            vcr_un_cat(u, out, cv);
            vcr_un_cat(u, out, " &&");
            if (ptr64)
                vcr_un_cat(u, out, " __ptr64");
            return;
        }
        u->err = 1;
        return;
    }
    default:
        u->err = 1;
        return;
    }
}

/* Decodes the decorated name of a type. Returns the length of the result in out, or 0 if the name is not understood. */
static inline size_t vcr_ti_undecorate(const char *decorated, char *out, size_t cap)
{
    vcr_un_str result;
    vcr_un u;
    size_t n;
    u.p = decorated;
    u.err = 0;
    u.depth = 0;
    u.ctx[0].n = 0;
    if (*u.p != '.')
        return 0;
    ++u.p;
    if (u.p[0] == '?' && u.p[1] == 'A')
        u.p += 2;
    vcr_un_type(&u, result.s);
    if (u.err || *u.p)
        return 0;
    n = vcr_un_len(result.s);
    if (n + 1 > cap)
        return 0;
    for (size_t i = 0; i <= n; ++i)
        out[i] = result.s[i];
    return n;
}

#endif
