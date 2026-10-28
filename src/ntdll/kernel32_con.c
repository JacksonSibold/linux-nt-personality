/*
 * kernel32_con.c - the console functions beyond reading and writing text: screen buffer information, cursor and attributes,
 * cell access, scrolling, the title, Ctrl handlers and console input records.
 *
 * The state lives in this process (k32_con_pure.h); no console host is reached. WriteConsole sends its bytes to the standard
 * output file and does not update the cells, so a program that mixes the two sees the cells it wrote itself. Characters in
 * the A variants are taken as Latin-1.
 */
#ifdef __MINGW32__
#ifdef DUMMYUNIONNAME
#undef DUMMYUNIONNAME
#endif
#ifdef DUMMYSTRUCTNAME
#undef DUMMYSTRUCTNAME
#endif
#include "nt_types.h"
#include "ntstatus.h"
#include "k32_con_pure.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#undef DUMMYUNIONNAME
#undef DUMMYSTRUCTNAME
#ifndef WINAPI
#define WINAPI __stdcall
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

typedef void VOID;

#define KC_ERROR_ACCESS_DENIED 5u
#define KC_ERROR_INVALID_HANDLE 6u
#define KC_ERROR_INVALID_PARAMETER 87u
#define KC_ERROR_NOT_ENOUGH_MEMORY 8u
#define KC_ERROR_HANDLE_EOF 38u

void SetLastError(DWORD e);
DWORD k32_console_handle(HANDLE h, int *output);
BOOL WINAPI ReadFile(HANDLE, PVOID, DWORD, DWORD *, PVOID);
HANDLE WINAPI CreateThread(PVOID, SIZE_T, DWORD (WINAPI *)(PVOID), PVOID, DWORD, DWORD *);
BOOL WINAPI CloseHandle(HANDLE);
VOID WINAPI ExitProcess(unsigned int);
DWORD WINAPI GetCurrentProcessId(void);
PVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T n);
BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, PVOID p);
HANDLE WINAPI GetProcessHeap(void);

/* ---- The shared console ---------------------------------------------------------------------------------------------------- */

static kcon g_con;
static volatile LONG g_con_state; /* 0 not started, 1 starting, 2 ready */
static volatile LONG g_con_lock;

static kcon *con_get(void)
{
    if (g_con_state != 2) {
        if (__sync_bool_compare_and_swap(&g_con_state, 0, 1)) {
            HANDLE heap = GetProcessHeap();
            size_t cells = (size_t)KCON_MAX_W * KCON_MAX_H * sizeof(uint16_t);
            uint16_t *ch = (uint16_t *)HeapAlloc(heap, 0, cells);
            uint16_t *at = (uint16_t *)HeapAlloc(heap, 0, cells);
            if (!ch || !at) {
                if (ch) HeapFree(heap, 0, ch);
                if (at) HeapFree(heap, 0, at);
                g_con_state = 0;
                return NULL;
            }
            kcon_init(&g_con, ch, at);
            __sync_synchronize();
            g_con_state = 2;
        } else {
            while (g_con_state != 2)
                __asm__ volatile("pause");
        }
    }
    return &g_con;
}

static void con_lock(void)
{
    while (__sync_lock_test_and_set(&g_con_lock, 1))
        __asm__ volatile("pause");
}

static void con_unlock(void) { __sync_lock_release(&g_con_lock); }

/* The handle must be a console output handle; sets the last error and returns NULL otherwise. */
static kcon *con_out(HANDLE h)
{
    int output = 0;
    DWORD err = k32_console_handle(h, &output);
    if (!err && !output)
        err = KC_ERROR_INVALID_HANDLE;
    if (err) {
        SetLastError(err);
        return NULL;
    }
    {
        kcon *c = con_get();
        if (!c)
            SetLastError(KC_ERROR_NOT_ENOUGH_MEMORY);
        return c;
    }
}

static kcon *con_in(HANDLE h)
{
    int output = 1;
    DWORD err = k32_console_handle(h, &output);
    if (!err && output)
        err = KC_ERROR_INVALID_HANDLE;
    if (err) {
        SetLastError(err);
        return NULL;
    }
    {
        kcon *c = con_get();
        if (!c)
            SetLastError(KC_ERROR_NOT_ENOUGH_MEMORY);
        return c;
    }
}

static BOOL con_done(uint32_t err)
{
    SetLastError(err);
    return err == 0 ? TRUE : FALSE;
}

typedef struct {
    int16_t X, Y;
} COORD_;
typedef struct {
    int16_t Left, Top, Right, Bottom;
} SMALL_RECT_;

static kcon_coord to_coord(COORD_ c)
{
    kcon_coord k;
    k.x = c.X;
    k.y = c.Y;
    return k;
}

static COORD_ from_coord(kcon_coord k)
{
    COORD_ c;
    c.X = k.x;
    c.Y = k.y;
    return c;
}

static kcon_rect to_rect(const SMALL_RECT_ *r)
{
    kcon_rect k;
    k.left = r->Left;
    k.top = r->Top;
    k.right = r->Right;
    k.bottom = r->Bottom;
    return k;
}

static void from_rect(SMALL_RECT_ *r, kcon_rect k)
{
    r->Left = k.left;
    r->Top = k.top;
    r->Right = k.right;
    r->Bottom = k.bottom;
}

/* ---- Screen buffer information ------------------------------------------------------------------------------------------- */

typedef struct {
    COORD_ dwSize, dwCursorPosition;
    uint16_t wAttributes;
    SMALL_RECT_ srWindow;
    COORD_ dwMaximumWindowSize;
} CSBI;

BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE h, CSBI *out)
{
    kcon *c = con_out(h);
    kcon_info i;
    if (!c)
        return FALSE;
    if (!out)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    kcon_get_info(c, &i);
    con_unlock();
    out->dwSize = from_coord(i.size);
    out->dwCursorPosition = from_coord(i.cursor);
    out->wAttributes = i.attr;
    from_rect(&out->srWindow, i.window);
    out->dwMaximumWindowSize = from_coord(i.max_window);
    return con_done(0);
}

typedef struct {
    ULONG cbSize;
    COORD_ dwSize, dwCursorPosition;
    uint16_t wAttributes;
    SMALL_RECT_ srWindow;
    COORD_ dwMaximumWindowSize;
    uint16_t wPopupAttributes;
    int32_t bFullscreenSupported;
    uint32_t ColorTable[16];
} CSBIEX;

static const uint32_t g_con_palette[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
                                            0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};

BOOL WINAPI GetConsoleScreenBufferInfoEx(HANDLE h, CSBIEX *out)
{
    CSBI base;
    int k;
    if (!out || out->cbSize != sizeof *out) {
        SetLastError(KC_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!GetConsoleScreenBufferInfo(h, &base))
        return FALSE;
    out->dwSize = base.dwSize;
    out->dwCursorPosition = base.dwCursorPosition;
    out->wAttributes = base.wAttributes;
    out->srWindow = base.srWindow;
    out->dwMaximumWindowSize = base.dwMaximumWindowSize;
    out->wPopupAttributes = 0xF5;
    out->bFullscreenSupported = 0;
    for (k = 0; k < 16; ++k)
        out->ColorTable[k] = g_con_palette[k];
    return TRUE;
}

BOOL WINAPI SetConsoleScreenBufferInfoEx(HANDLE h, const CSBIEX *in)
{
    kcon *c = con_out(h);
    uint32_t err;
    if (!c)
        return FALSE;
    if (!in || in->cbSize != sizeof *in)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    err = kcon_set_size(c, to_coord(in->dwSize));
    if (!err)
        err = kcon_set_cursor(c, to_coord(in->dwCursorPosition));
    if (!err) {
        c->attr = in->wAttributes;
        err = kcon_set_window(c, 1, to_rect(&in->srWindow));
    }
    con_unlock();
    return con_done(err);
}

BOOL WINAPI SetConsoleScreenBufferSize(HANDLE h, COORD_ size)
{
    kcon *c = con_out(h);
    uint32_t err;
    if (!c)
        return FALSE;
    con_lock();
    err = kcon_set_size(c, to_coord(size));
    con_unlock();
    return con_done(err);
}

COORD_ WINAPI GetLargestConsoleWindowSize(HANDLE h)
{
    COORD_ z = {0, 0};
    kcon *c = con_out(h);
    if (!c)
        return z;
    return from_coord(kcon_largest_window(c));
}

BOOL WINAPI SetConsoleWindowInfo(HANDLE h, BOOL absolute, const SMALL_RECT_ *r)
{
    kcon *c = con_out(h);
    uint32_t err;
    if (!c)
        return FALSE;
    if (!r)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    err = kcon_set_window(c, absolute != 0, to_rect(r));
    con_unlock();
    return con_done(err);
}

BOOL WINAPI SetConsoleCursorPosition(HANDLE h, COORD_ p)
{
    kcon *c = con_out(h);
    uint32_t err;
    if (!c)
        return FALSE;
    con_lock();
    err = kcon_set_cursor(c, to_coord(p));
    con_unlock();
    return con_done(err);
}

typedef struct {
    ULONG dwSize;
    int32_t bVisible;
} CCI;

BOOL WINAPI GetConsoleCursorInfo(HANDLE h, CCI *out)
{
    kcon *c = con_out(h);
    if (!c)
        return FALSE;
    if (!out)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    out->dwSize = c->cursor_size;
    out->bVisible = c->cursor_visible;
    con_unlock();
    return con_done(0);
}

BOOL WINAPI SetConsoleCursorInfo(HANDLE h, const CCI *in)
{
    kcon *c = con_out(h);
    uint32_t err;
    if (!c)
        return FALSE;
    if (!in)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    err = kcon_set_cursor_info(c, in->dwSize, in->bVisible);
    con_unlock();
    return con_done(err);
}

BOOL WINAPI SetConsoleTextAttribute(HANDLE h, uint16_t attr)
{
    kcon *c = con_out(h);
    if (!c)
        return FALSE;
    con_lock();
    c->attr = attr;
    con_unlock();
    return con_done(0);
}

BOOL WINAPI SetConsoleActiveScreenBuffer(HANDLE h) { return con_out(h) ? con_done(0) : FALSE; }

BOOL WINAPI GetConsoleDisplayMode(ULONG *mode)
{
    if (!mode) {
        SetLastError(KC_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *mode = 0; /* windowed */
    SetLastError(0);
    return TRUE;
}

PVOID WINAPI GetConsoleWindow(void) { return NULL; }

BOOL WINAPI GetNumberOfConsoleMouseButtons(DWORD *n)
{
    if (!n) {
        SetLastError(KC_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *n = 3;
    SetLastError(0);
    return TRUE;
}

/* ---- Cell access ------------------------------------------------------------------------------------------------------------- */

static uint16_t widen(uint8_t b) { return b; }
static char narrow(uint16_t w) { return w < 0x100 ? (char)w : '?'; }

BOOL WINAPI FillConsoleOutputCharacterW(HANDLE h, WCHAR ch, DWORD n, COORD_ at, DWORD *written)
{
    kcon *c = con_out(h);
    size_t d = 0;
    uint32_t err;
    if (!c)
        return FALSE;
    con_lock();
    err = kcon_fill_chars(c, to_coord(at), ch, n, &d);
    con_unlock();
    if (written)
        *written = (DWORD)d;
    return con_done(err);
}

BOOL WINAPI FillConsoleOutputCharacterA(HANDLE h, char ch, DWORD n, COORD_ at, DWORD *written)
{
    return FillConsoleOutputCharacterW(h, widen((uint8_t)ch), n, at, written);
}

BOOL WINAPI FillConsoleOutputAttribute(HANDLE h, uint16_t attr, DWORD n, COORD_ at, DWORD *written)
{
    kcon *c = con_out(h);
    size_t d = 0;
    uint32_t err;
    if (!c)
        return FALSE;
    con_lock();
    err = kcon_fill_attrs(c, to_coord(at), attr, n, &d);
    con_unlock();
    if (written)
        *written = (DWORD)d;
    return con_done(err);
}

BOOL WINAPI WriteConsoleOutputCharacterW(HANDLE h, const WCHAR *s, DWORD n, COORD_ at, DWORD *written)
{
    kcon *c = con_out(h);
    size_t d = 0;
    uint32_t err;
    if (!c)
        return FALSE;
    if (!s && n)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    err = kcon_write_chars(c, to_coord(at), s, n, &d);
    con_unlock();
    if (written)
        *written = (DWORD)d;
    return con_done(err);
}

BOOL WINAPI WriteConsoleOutputCharacterA(HANDLE h, const char *s, DWORD n, COORD_ at, DWORD *written)
{
    kcon *c = con_out(h);
    size_t d = 0, k;
    uint32_t err;
    if (!c)
        return FALSE;
    if (!s && n)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    err = 0;
    {
        kcon_coord p = to_coord(at);
        if (!kcon_in_buffer(c, p)) {
            err = KCON_ERR_INVALID_PARAMETER;
        } else {
            size_t m = kcon_span(c, p, n);
            for (k = 0; k < m; ++k, kcon_step(c, &p))
                c->chars[kcon_cell(p.x, p.y)] = widen((uint8_t)s[k]);
            d = m;
        }
    }
    con_unlock();
    if (written)
        *written = (DWORD)d;
    return con_done(err);
}

BOOL WINAPI WriteConsoleOutputAttribute(HANDLE h, const uint16_t *a, DWORD n, COORD_ at, DWORD *written)
{
    kcon *c = con_out(h);
    size_t d = 0;
    uint32_t err;
    if (!c)
        return FALSE;
    if (!a && n)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    err = kcon_write_attrs(c, to_coord(at), a, n, &d);
    con_unlock();
    if (written)
        *written = (DWORD)d;
    return con_done(err);
}

BOOL WINAPI ReadConsoleOutputCharacterW(HANDLE h, WCHAR *out, DWORD n, COORD_ at, DWORD *read)
{
    kcon *c = con_out(h);
    size_t d = 0;
    uint32_t err;
    if (!c)
        return FALSE;
    if (!out && n)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    err = kcon_read_chars(c, to_coord(at), out, n, &d);
    con_unlock();
    if (read)
        *read = (DWORD)d;
    return con_done(err);
}

BOOL WINAPI ReadConsoleOutputCharacterA(HANDLE h, char *out, DWORD n, COORD_ at, DWORD *read)
{
    kcon *c = con_out(h);
    size_t d = 0, k;
    uint32_t err = 0;
    if (!c)
        return FALSE;
    if (!out && n)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    {
        kcon_coord p = to_coord(at);
        if (!kcon_in_buffer(c, p)) {
            err = KCON_ERR_INVALID_PARAMETER;
        } else {
            size_t m = kcon_span(c, p, n);
            for (k = 0; k < m; ++k, kcon_step(c, &p))
                out[k] = narrow(c->chars[kcon_cell(p.x, p.y)]);
            d = m;
        }
    }
    con_unlock();
    if (read)
        *read = (DWORD)d;
    return con_done(err);
}

BOOL WINAPI ReadConsoleOutputAttribute(HANDLE h, uint16_t *out, DWORD n, COORD_ at, DWORD *read)
{
    kcon *c = con_out(h);
    size_t d = 0;
    uint32_t err;
    if (!c)
        return FALSE;
    if (!out && n)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    err = kcon_read_attrs(c, to_coord(at), out, n, &d);
    con_unlock();
    if (read)
        *read = (DWORD)d;
    return con_done(err);
}

BOOL WINAPI WriteConsoleOutputW(HANDLE h, const kcon_char_info *src, COORD_ size, COORD_ at, SMALL_RECT_ *region)
{
    kcon *c = con_out(h);
    kcon_rect rg;
    uint32_t err;
    if (!c)
        return FALSE;
    if (!src || !region)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    rg = to_rect(region);
    con_lock();
    err = kcon_write_rect(c, src, to_coord(size), to_coord(at), &rg);
    con_unlock();
    from_rect(region, rg);
    return con_done(err);
}

BOOL WINAPI ReadConsoleOutputW(HANDLE h, kcon_char_info *dst, COORD_ size, COORD_ at, SMALL_RECT_ *region)
{
    kcon *c = con_out(h);
    kcon_rect rg;
    uint32_t err;
    if (!c)
        return FALSE;
    if (!dst || !region)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    rg = to_rect(region);
    con_lock();
    err = kcon_read_rect(c, dst, to_coord(size), to_coord(at), &rg);
    con_unlock();
    from_rect(region, rg);
    return con_done(err);
}

/* The A forms use CHAR_INFO with a one-byte character in the same 4-byte cell. */
BOOL WINAPI WriteConsoleOutputA(HANDLE h, const kcon_char_info *src, COORD_ size, COORD_ at, SMALL_RECT_ *region)
{
    size_t n = (size_t)(size.X > 0 ? size.X : 0) * (size_t)(size.Y > 0 ? size.Y : 0), k;
    kcon_char_info *tmp;
    BOOL r;
    if (!src || !region || !n) {
        SetLastError(KC_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    tmp = (kcon_char_info *)HeapAlloc(GetProcessHeap(), 0, n * sizeof *tmp);
    if (!tmp) {
        SetLastError(KC_ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    for (k = 0; k < n; ++k) {
        tmp[k].ch = widen((uint8_t)src[k].ch);
        tmp[k].attr = src[k].attr;
    }
    r = WriteConsoleOutputW(h, tmp, size, at, region);
    HeapFree(GetProcessHeap(), 0, tmp);
    return r;
}

BOOL WINAPI ReadConsoleOutputA(HANDLE h, kcon_char_info *dst, COORD_ size, COORD_ at, SMALL_RECT_ *region)
{
    BOOL r = ReadConsoleOutputW(h, dst, size, at, region);
    if (r) {
        size_t n = (size_t)size.X * (size_t)size.Y, k;
        for (k = 0; k < n; ++k)
            dst[k].ch = (uint8_t)narrow(dst[k].ch);
    }
    return r;
}

typedef struct {
    uint16_t ch, attr;
} CHAR_INFO_;

BOOL WINAPI ScrollConsoleScreenBufferW(HANDLE h, const SMALL_RECT_ *src, const SMALL_RECT_ *clip, COORD_ dest, const CHAR_INFO_ *fill)
{
    kcon *c = con_out(h);
    kcon_rect s, cl;
    uint32_t err;
    if (!c)
        return FALSE;
    if (!src || !fill)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    s = to_rect(src);
    if (clip)
        cl = to_rect(clip);
    con_lock();
    err = kcon_scroll(c, s, clip ? &cl : NULL, to_coord(dest), fill->ch, fill->attr);
    con_unlock();
    return con_done(err);
}

BOOL WINAPI ScrollConsoleScreenBufferA(HANDLE h, const SMALL_RECT_ *src, const SMALL_RECT_ *clip, COORD_ dest, const CHAR_INFO_ *fill)
{
    CHAR_INFO_ f;
    if (!fill) {
        SetLastError(KC_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    f.ch = widen((uint8_t)fill->ch);
    f.attr = fill->attr;
    return ScrollConsoleScreenBufferW(h, src, clip, dest, &f);
}

/* ---- Title --------------------------------------------------------------------------------------------------------------------- */

DWORD WINAPI GetConsoleTitleW(WCHAR *out, DWORD size)
{
    kcon *c = con_get();
    size_t n;
    if (!c || !out) {
        SetLastError(KC_ERROR_INVALID_PARAMETER);
        return 0;
    }
    con_lock();
    n = kcon_get_title(c, out, size);
    con_unlock();
    SetLastError(0);
    return (DWORD)n;
}

DWORD WINAPI GetConsoleTitleA(char *out, DWORD size)
{
    uint16_t tmp[KCON_TITLE_MAX];
    size_t n, k;
    kcon *c = con_get();
    if (!c || !out) {
        SetLastError(KC_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!size)
        return 0;
    con_lock();
    n = kcon_get_title(c, tmp, size < KCON_TITLE_MAX ? size : KCON_TITLE_MAX);
    con_unlock();
    for (k = 0; k < n; ++k)
        out[k] = narrow(tmp[k]);
    out[n] = 0;
    SetLastError(0);
    return (DWORD)n;
}

BOOL WINAPI SetConsoleTitleW(const WCHAR *s)
{
    kcon *c = con_get();
    size_t n = 0;
    if (!c || !s)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    while (s[n])
        ++n;
    con_lock();
    kcon_set_title(c, s, n);
    con_unlock();
    return con_done(0);
}

BOOL WINAPI SetConsoleTitleA(const char *s)
{
    uint16_t tmp[KCON_TITLE_MAX];
    size_t n = 0;
    if (!s)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    while (s[n] && n < KCON_TITLE_MAX - 1) {
        tmp[n] = widen((uint8_t)s[n]);
        ++n;
    }
    tmp[n] = 0;
    return SetConsoleTitleW((const WCHAR *)tmp);
}

/* ---- Ctrl handlers ----------------------------------------------------------------------------------------------------------- */

BOOL WINAPI SetConsoleCtrlHandler(int (WINAPI *handler)(DWORD), BOOL add)
{
    kcon *c = con_get();
    uint32_t err;
    if (!c)
        return con_done(KC_ERROR_NOT_ENOUGH_MEMORY);
    con_lock();
    /* The model keeps plain function pointers; the calling convention is the same for one DWORD argument. */
    err = kcon_set_handler(c, (kcon_handler)(void *)handler, add != 0);
    con_unlock();
    return con_done(err);
}

static DWORD WINAPI ctrl_thread(PVOID arg)
{
    kcon snap;
    kcon *c = con_get();
    uint32_t event = (uint32_t)(uintptr_t)arg;
    if (!c)
        return 0;
    /* Handlers may call SetConsoleCtrlHandler, so they run on a copy of the list with no lock held. */
    con_lock();
    snap = *c;
    con_unlock();
    if (!kcon_dispatch(&snap, event))
        ExitProcess(0xC000013Au); /* STATUS_CONTROL_C_EXIT */
    return 0;
}

BOOL WINAPI GenerateConsoleCtrlEvent(DWORD event, DWORD group)
{
    HANDLE t;
    (void)group;
    if (event != 0 && event != 1) {
        SetLastError(KC_ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    t = CreateThread(NULL, 0, ctrl_thread, (PVOID)(uintptr_t)event, 0, NULL);
    if (!t)
        return FALSE;
    CloseHandle(t);
    SetLastError(0);
    return TRUE;
}

/* ---- Input records ----------------------------------------------------------------------------------------------------------- */

static BOOL read_key_records(HANDLE h, kcon_key_record *out, DWORD n, DWORD *got, int peek)
{
    kcon *c = con_in(h);
    DWORD i = 0;
    if (got)
        *got = 0;
    if (!c)
        return FALSE;
    if (!out || !n)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    for (; i < n; ++i) {
        con_lock();
        if (c->key_up_pending) {
            if (peek) {
                kcon_make_key(&out[i], c->key_pending_ch, 0);
            } else {
                kcon_next_key(c, 0, 0, &out[i]);
            }
            con_unlock();
            continue;
        }
        con_unlock();
        if (peek || i > 0)
            break; /* nothing is read ahead for a peek, and a read returns after one key's records */
        {
            uint8_t b = 0;
            DWORD r = 0;
            if (!ReadFile(h, &b, 1, &r, NULL))
                return FALSE;
            if (r == 0)
                return con_done(KC_ERROR_HANDLE_EOF);
            con_lock();
            kcon_next_key(c, 1, b, &out[i]);
            con_unlock();
        }
    }
    if (got)
        *got = i;
    return con_done(0);
}

BOOL WINAPI ReadConsoleInputW(HANDLE h, kcon_key_record *out, DWORD n, DWORD *got) { return read_key_records(h, out, n, got, 0); }
BOOL WINAPI ReadConsoleInputA(HANDLE h, kcon_key_record *out, DWORD n, DWORD *got)
{
    BOOL r = read_key_records(h, out, n, got, 0);
    if (r && got) {
        DWORD k;
        for (k = 0; k < *got; ++k)
            out[k].ch = (uint8_t)narrow(out[k].ch);
    }
    return r;
}
BOOL WINAPI PeekConsoleInputW(HANDLE h, kcon_key_record *out, DWORD n, DWORD *got) { return read_key_records(h, out, n, got, 1); }
BOOL WINAPI PeekConsoleInputA(HANDLE h, kcon_key_record *out, DWORD n, DWORD *got) { return read_key_records(h, out, n, got, 1); }

BOOL WINAPI GetNumberOfConsoleInputEvents(HANDLE h, DWORD *n)
{
    kcon *c = con_in(h);
    if (!c)
        return FALSE;
    if (!n)
        return con_done(KC_ERROR_INVALID_PARAMETER);
    con_lock();
    *n = c->key_up_pending ? 1 : 0;
    con_unlock();
    return con_done(0);
}

BOOL WINAPI FlushConsoleInputBuffer(HANDLE h)
{
    kcon *c = con_in(h);
    if (!c)
        return FALSE;
    con_lock();
    c->key_up_pending = 0;
    con_unlock();
    return con_done(0);
}

/* ---- Attaching ---------------------------------------------------------------------------------------------------------------- */

/* A process always has the console of its standard handles here; the calls below report that. */
BOOL WINAPI AllocConsole(void)
{
    SetLastError(KC_ERROR_ACCESS_DENIED);
    return FALSE;
}

BOOL WINAPI FreeConsole(void)
{
    SetLastError(0);
    return TRUE;
}

BOOL WINAPI AttachConsole(DWORD pid)
{
    (void)pid;
    SetLastError(KC_ERROR_ACCESS_DENIED);
    return FALSE;
}

DWORD WINAPI GetConsoleProcessList(DWORD *list, DWORD count)
{
    if (!list || !count) {
        SetLastError(KC_ERROR_INVALID_PARAMETER);
        return 0;
    }
    list[0] = GetCurrentProcessId();
    SetLastError(0);
    return 1;
}

#endif /* __MINGW32__ */
