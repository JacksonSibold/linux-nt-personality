/* drvhost_fmt.c - DbgPrint and the debug sink.
 *
 * DbgPrint and DbgPrintEx are variadic with the Microsoft x64 convention.  A C
 * library vfprintf cannot consume that va_list, so the formatter is written
 * here against va_arg.  Both entry points call one ms_abi core, fmt_core, so
 * the variadic arguments are consumed exactly once.  Supported: %s %S %ls %c
 * %d %i %u %x %X %p %Z %wZ %% with the l, ll, I64, z size modifiers, a width,
 * and the '0' and '-' flags.
 */
#ifdef __KERNEL__
#include <linux/stdarg.h>
#else
#include <stdarg.h>
#endif

#include "drvhost_int.h"

static drvhost_dbg_sink_fn g_sink = NULL;
static void *g_sink_ctx = NULL;

void drvhost_set_dbg_sink(drvhost_dbg_sink_fn fn, void *ctx)
{
    g_sink = fn;
    g_sink_ctx = ctx;
}

void dh_emit(const char *text, SIZE_T len)
{
    if (g_sink) {
        g_sink(g_sink_ctx, text, len);
        return;
    }
#ifdef __KERNEL__
    pr_info("%.*s", (int)len, text);
#else
    fwrite(text, 1, len, stderr);
#endif
}

typedef struct fmtbuf {
    char *buf;
    size_t cap;
    size_t len;
} fmtbuf_t;

static void put(fmtbuf_t *b, char c)
{
    if (b->len + 1 < b->cap) b->buf[b->len] = c;
    b->len++;
}

static void put_n(fmtbuf_t *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) put(b, s[i]);
}

static void pad(fmtbuf_t *b, size_t n, char c)
{
    while (n-- > 0) put(b, c);
}

static void put_uint(fmtbuf_t *b, unsigned long long v, unsigned base, int upper,
                     int width, int zero, int left, int neg)
{
    char tmp[32];
    size_t n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v && n < sizeof tmp);
    size_t total = n + (neg ? 1 : 0);
    size_t fill = (width > 0 && (size_t)width > total) ? (size_t)width - total : 0;
    if (!left && !zero) pad(b, fill, ' ');
    if (neg) put(b, '-');
    if (!left && zero) pad(b, fill, '0');
    while (n > 0) put(b, tmp[--n]);
    if (left) pad(b, fill, ' ');
}

static void put_str(fmtbuf_t *b, const char *s, size_t n, int width, int left)
{
    size_t fill = (width > 0 && (size_t)width > n) ? (size_t)width - n : 0;
    if (!left) pad(b, fill, ' ');
    put_n(b, s, n);
    if (left) pad(b, fill, ' ');
}

static void put_wstr(fmtbuf_t *b, const WCHAR *s, size_t n, int width, int left)
{
    size_t fill = (width > 0 && (size_t)width > n) ? (size_t)width - n : 0;
    if (!left) pad(b, fill, ' ');
    for (size_t i = 0; i < n; i++) put(b, (s[i] < 0x80) ? (char)s[i] : '?');
    if (left) pad(b, fill, ' ');
}

/* The formatter core.  ms_abi, so the va_list it receives is the Microsoft
 * convention that the caller's variadic function was entered with. */
static DRVCALL void fmt_core(fmtbuf_t *b, const char *Format, __builtin_ms_va_list ap)
{
    for (const char *p = Format ? Format : ""; *p; p++) {
        if (*p != '%') { put(b, *p); continue; }
        p++;
        int left = 0, zero = 0, width = 0, lng = 0, wide_z = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else break;
        }
        if (*p == '*') { width = __builtin_va_arg(ap, int); p++; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = __builtin_va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
            if (prec < 0) prec = -1;
        }
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { lng = 2; p += 3; }
        else if (p[0] == 'l' && p[1] == 'l') { lng = 2; p += 2; }
        else if (p[0] == 'w' && p[1] == 'Z') { wide_z = 1; p += 1; }
        else if (*p == 'l' && p[1] == 's') { wide_z = 2; p += 1; }
        else if (*p == 'l' || *p == 'z' || *p == 'j' || *p == 't') { lng = 1; p++; }
        else if (*p == 'h') { p++; if (*p == 'h') p++; }
        char conv = *p;
        if (!conv) break;
        if (wide_z == 2) conv = 'S';
        switch (conv) {
        case '%':
            put(b, '%');
            break;
        case 'c':
            put(b, (char)__builtin_va_arg(ap, int));
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            { size_t sl = strlen(s); if (prec >= 0 && (size_t)prec < sl) sl = (size_t)prec; put_str(b, s, sl, width, left); }
            break;
        }
        case 'S': {
            const WCHAR *s = __builtin_va_arg(ap, const WCHAR *);
            if (!s) { put_str(b, "(null)", 6, width, left); break; }
            size_t n = 0;
            while (s[n]) n++;
            if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
            put_wstr(b, s, n, width, left);
            break;
        }
        case 'Z': {
            const void *arg = __builtin_va_arg(ap, const void *);
            if (!arg) { put_str(b, "(null)", 6, width, left); break; }
            if (wide_z == 1) {
                const UNICODE_STRING *u = arg;
                { size_t ul = u->Length / 2u; if (prec >= 0 && (size_t)prec < ul) ul = (size_t)prec; put_wstr(b, u->Buffer, ul, width, left); }
            } else {
                const ANSI_STRING *a = arg;
                { size_t al = a->Length; if (prec >= 0 && (size_t)prec < al) al = (size_t)prec; put_str(b, a->Buffer, al, width, left); }
            }
            break;
        }
        case 'p': {
            unsigned long long v = (unsigned long long)(uintptr_t)__builtin_va_arg(ap, void *);
            put(b, '0'); put(b, 'x');
            put_uint(b, v, 16, 0, 16, 1, 0, 0);
            break;
        }
        case 'd': case 'i': {
            long long v = (lng == 2) ? __builtin_va_arg(ap, long long)
                        : (lng == 1) ? (long long)__builtin_va_arg(ap, long)
                        : (long long)__builtin_va_arg(ap, int);
            int neg = v < 0;
            unsigned long long mag = neg ? (unsigned long long)(-(v + 1)) + 1ull : (unsigned long long)v;
            put_uint(b, mag, 10, 0, width, zero, left, neg);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long long v = (lng == 2) ? __builtin_va_arg(ap, unsigned long long)
                                 : (lng == 1) ? (unsigned long long)__builtin_va_arg(ap, unsigned long)
                                 : (unsigned long long)__builtin_va_arg(ap, unsigned int);
            unsigned base = conv == 'u' ? 10u : 16u;
            put_uint(b, v, base, conv == 'X', width, zero, left, 0);
            break;
        }
        default:
            put(b, '%');
            put(b, conv);
            break;
        }
    }
}

static void emit_buffer(fmtbuf_t *b, char *out)
{
    size_t n = b->len < b->cap ? b->len : b->cap - 1;
    if (b->len >= b->cap && b->cap >= 5) {
        memcpy(out + b->cap - 5, "...\n", 4);
        n = b->cap - 1;
    }
    dh_emit(out, n);
}

DRVCALL NTSTATUS DbgPrint(const CHAR *Format, ...)
{
    char out[1024];
    fmtbuf_t b = { out, sizeof out, 0 };
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, Format);
    fmt_core(&b, Format, ap);
    __builtin_ms_va_end(ap);
    emit_buffer(&b, out);
    return STATUS_SUCCESS;
}

DRVCALL NTSTATUS DbgPrintEx(ULONG ComponentId, ULONG Level, const CHAR *Format, ...)
{
    (void)ComponentId; (void)Level;
    char out[1024];
    fmtbuf_t b = { out, sizeof out, 0 };
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, Format);
    fmt_core(&b, Format, ap);
    __builtin_ms_va_end(ap);
    emit_buffer(&b, out);
    return STATUS_SUCCESS;
}

DRVCALL void DbgBreakPoint(void)
{
    static const char msg[] = "drvhost: DbgBreakPoint (no debugger attached)\n";
    dh_emit(msg, sizeof msg - 1);
}

const char *drvhost_status_name(NTSTATUS s)
{
    switch ((uint32_t)s) {
    case 0x00000000u: return "STATUS_SUCCESS";
    case 0x00000102u: return "STATUS_TIMEOUT";
    case 0x00000103u: return "STATUS_PENDING";
    case 0xC0000001u: return "STATUS_UNSUCCESSFUL";
    case 0xC000000Du: return "STATUS_INVALID_PARAMETER";
    case 0xC000000Eu: return "STATUS_NO_SUCH_DEVICE";
    case 0xC0000010u: return "STATUS_INVALID_DEVICE_REQUEST";
    case 0xC0000023u: return "STATUS_BUFFER_TOO_SMALL";
    case 0xC0000034u: return "STATUS_OBJECT_NAME_NOT_FOUND";
    case 0xC0000035u: return "STATUS_OBJECT_NAME_COLLISION";
    case 0xC000007Au: return "STATUS_PROCEDURE_NOT_FOUND";
    case 0xC000007Bu: return "STATUS_INVALID_IMAGE_FORMAT";
    case 0xC000010Eu: return "STATUS_IMAGE_ALREADY_LOADED";
    case 0xC000026Eu: return "STATUS_DRIVER_UNABLE_TO_LOAD";
    case 0xC00000BBu: return "STATUS_NOT_SUPPORTED";
    default:          return "STATUS_(unnamed)";
    }
}
