/* user32_ext.c - further user32.dll exports on the win32k model: RECT helpers, window
 * relations and state, SetWindowPos and placement, update region calls, monitors and
 * display devices, DPI awareness, keyboard layout calls, system colours, character case
 * and a few honest stubs (MessageBox, LoadString).
 *
 * The logic that needs no model state is in user32_pure.h (host tests:
 * tests/ntdll-host/test_user32_pure.c). The rest forwards to the model (w32k_win.c,
 * w32k_sys.c, w32k_core.c), tested by test_w32k_win.c and test_w32k_sys.c.
 */
#ifdef __MINGW32__
#include "win32k/w32k_abi.h"
#include "user32_pure.h"
#include <stdint.h>
#include <stddef.h>

typedef int32_t BOOL_T;

uint32_t w32k_shim_now(void);
uint32_t w32k_shim_tid(void);
int32_t ChangeDisplaySettingsW(void *devmode, uint32_t flags);

static void put32(void *p, size_t off, uint32_t v)
{
    uint8_t *b = (uint8_t *)p + off;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const void *p, size_t off)
{
    const uint8_t *b = (const uint8_t *)p + off;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void put64(void *p, size_t off, uint64_t v)
{
    put32(p, off, (uint32_t)v);
    put32(p, off + 4, (uint32_t)(v >> 32));
}

/* Copies an ASCII string into a WCHAR array of cap units, truncating. */
static void put_wstr(void *p, size_t off, const char *s, size_t cap)
{
    uint8_t *b = (uint8_t *)p + off;
    size_t i = 0;
    while (s[i] && i + 1 < cap) {
        b[2 * i] = (uint8_t)s[i];
        b[2 * i + 1] = 0;
        ++i;
    }
    for (; i < cap; ++i) {
        b[2 * i] = 0;
        b[2 * i + 1] = 0;
    }
}

static void zero_bytes(void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--)
        *b++ = 0;
}

/* ---- RECT ----------------------------------------------------------------------- */

BOOL_T SetRect(w32k_rect *rc, int32_t l, int32_t t, int32_t r, int32_t b)
{
    if (!rc)
        return 0;
    rc->left = l;
    rc->top = t;
    rc->right = r;
    rc->bottom = b;
    return 1;
}

BOOL_T SetRectEmpty(w32k_rect *rc)
{
    return SetRect(rc, 0, 0, 0, 0);
}

BOOL_T CopyRect(w32k_rect *dst, const w32k_rect *src)
{
    if (!dst || !src)
        return 0;
    *dst = *src;
    return 1;
}

BOOL_T OffsetRect(w32k_rect *rc, int32_t dx, int32_t dy)
{
    if (!rc)
        return 0;
    u32p_offset_rect(rc, dx, dy);
    return 1;
}

BOOL_T InflateRect(w32k_rect *rc, int32_t dx, int32_t dy)
{
    if (!rc)
        return 0;
    u32p_inflate_rect(rc, dx, dy);
    return 1;
}

BOOL_T IntersectRect(w32k_rect *dst, const w32k_rect *a, const w32k_rect *b)
{
    if (!dst || !a || !b)
        return 0;
    return u32p_intersect_rect(dst, a, b);
}

BOOL_T UnionRect(w32k_rect *dst, const w32k_rect *a, const w32k_rect *b)
{
    if (!dst || !a || !b)
        return 0;
    return u32p_union_rect(dst, a, b);
}

BOOL_T SubtractRect(w32k_rect *dst, const w32k_rect *a, const w32k_rect *b)
{
    if (!dst || !a || !b)
        return 0;
    return u32p_subtract_rect(dst, a, b);
}

/* POINT is 8 bytes, so the Microsoft x64 ABI passes it by value in one register. */
BOOL_T PtInRect(const w32k_rect *rc, w32k_point pt)
{
    return rc && u32p_pt_in_rect(rc, pt.x, pt.y);
}

BOOL_T IsRectEmpty(const w32k_rect *rc)
{
    return !rc || u32p_is_rect_empty(rc);
}

BOOL_T EqualRect(const w32k_rect *a, const w32k_rect *b)
{
    return a && b && u32p_equal_rect(a, b);
}

/* ---- Metrics and AdjustWindowRect ---------------------------------------------- */

/* Pixel sizes scale with the DPI; counts and flags do not. */
static int metric_scales(int32_t index)
{
    switch (index) {
    case W32K_SM_CXVSCROLL: case W32K_SM_CYHSCROLL: case W32K_SM_CYCAPTION: case W32K_SM_CXBORDER:
    case W32K_SM_CYBORDER: case W32K_SM_CXFIXEDFRAME: case W32K_SM_CYFIXEDFRAME: case W32K_SM_CYVTHUMB:
    case W32K_SM_CXHTHUMB: case W32K_SM_CXICON: case W32K_SM_CYICON: case W32K_SM_CXCURSOR:
    case W32K_SM_CYCURSOR: case W32K_SM_CYMENU: case 20: case 21: case W32K_SM_CXMIN: case W32K_SM_CYMIN:
    case W32K_SM_CXSIZE: case W32K_SM_CYSIZE: case W32K_SM_CXSIZEFRAME: case W32K_SM_CYSIZEFRAME:
    case W32K_SM_CXMINTRACK: case W32K_SM_CYMINTRACK: case W32K_SM_CXEDGE: case W32K_SM_CYEDGE:
    case W32K_SM_CXSMICON: case W32K_SM_CYSMICON: case W32K_SM_CYSMCAPTION: case W32K_SM_CXMENUCHECK:
    case W32K_SM_CYMENUCHECK: case W32K_SM_CXFOCUSBORDER: case W32K_SM_CYFOCUSBORDER:
    case W32K_SM_CXPADDEDBORDER:
        return 1;
    default:
        return 0;
    }
}

static int32_t scale_dpi(int32_t v, uint32_t dpi)
{
    if (dpi == 0 || dpi == 96)
        return v;
    return (int32_t)(((int64_t)v * dpi + 48) / 96);
}

int32_t GetSystemMetricsForDpi(int32_t index, uint32_t dpi)
{
    int32_t v = w32k_get_system_metrics(index);
    return metric_scales(index) ? scale_dpi(v, dpi) : v;
}

static void fill_metrics(u32p_metrics *m, uint32_t dpi)
{
    m->cxborder = GetSystemMetricsForDpi(W32K_SM_CXBORDER, dpi);
    m->cyborder = GetSystemMetricsForDpi(W32K_SM_CYBORDER, dpi);
    m->cxsizeframe = GetSystemMetricsForDpi(W32K_SM_CXSIZEFRAME, dpi);
    m->cysizeframe = GetSystemMetricsForDpi(W32K_SM_CYSIZEFRAME, dpi);
    m->cxpadded = GetSystemMetricsForDpi(W32K_SM_CXPADDEDBORDER, dpi);
    m->cxdlgframe = GetSystemMetricsForDpi(W32K_SM_CXFIXEDFRAME, dpi);
    m->cydlgframe = GetSystemMetricsForDpi(W32K_SM_CYFIXEDFRAME, dpi);
    m->cxedge = GetSystemMetricsForDpi(W32K_SM_CXEDGE, dpi);
    m->cyedge = GetSystemMetricsForDpi(W32K_SM_CYEDGE, dpi);
    m->cycaption = GetSystemMetricsForDpi(W32K_SM_CYCAPTION, dpi);
    m->cysmcaption = GetSystemMetricsForDpi(W32K_SM_CYSMCAPTION, dpi);
    m->cymenu = GetSystemMetricsForDpi(W32K_SM_CYMENU, dpi);
}

BOOL_T AdjustWindowRectExForDpi(w32k_rect *rc, uint32_t style, BOOL_T menu, uint32_t exstyle, uint32_t dpi)
{
    u32p_metrics m;
    if (!rc)
        return 0;
    fill_metrics(&m, dpi);
    u32p_adjust_window_rect(rc, style, menu != 0, exstyle, &m);
    return 1;
}

BOOL_T AdjustWindowRectEx(w32k_rect *rc, uint32_t style, BOOL_T menu, uint32_t exstyle)
{
    return AdjustWindowRectExForDpi(rc, style, menu, exstyle, 96);
}

BOOL_T AdjustWindowRect(w32k_rect *rc, uint32_t style, BOOL_T menu)
{
    return AdjustWindowRectExForDpi(rc, style, menu, 0, 96);
}

/* ---- Window relations and state -------------------------------------------------- */

uintptr_t GetDesktopWindow(void)
{
    return w32k_get_desktop_window();
}

int32_t GetClassNameW(uintptr_t hwnd, uint16_t *buf, int32_t max)
{
    return w32k_get_class_name(hwnd, buf, max);
}

uintptr_t GetWindow(uintptr_t hwnd, uint32_t cmd)
{
    return w32k_get_window_rel(hwnd, cmd);
}

static int first_cb(w32k_handle h, intptr_t lp)
{
    *(w32k_handle *)(uintptr_t)lp = h;
    return 0;
}

uintptr_t GetTopWindow(uintptr_t hwnd)
{
    w32k_handle first = 0;
    if (hwnd)
        return w32k_get_window_rel(hwnd, W32K_GW_CHILD);
    w32k_enum_windows(0, first_cb, (intptr_t)(uintptr_t)&first);
    return first;
}

#define GA_PARENT 1u
#define GA_ROOT 2u
#define GA_ROOTOWNER 3u

uintptr_t GetAncestor(uintptr_t hwnd, uint32_t flags)
{
    uintptr_t p, top = hwnd;
    if (!w32k_is_window(hwnd) || hwnd == w32k_get_desktop_window())
        return 0;
    if (flags == GA_PARENT) {
        p = w32k_get_parent(hwnd);
        return p ? p : w32k_get_desktop_window();
    }
    if (flags != GA_ROOT && flags != GA_ROOTOWNER)
        return 0;
    while ((p = w32k_get_parent(top)) != 0)
        top = p;
    return top;
}

BOOL_T IsChild(uintptr_t parent, uintptr_t child)
{
    uintptr_t p = w32k_get_parent(child);
    while (p) {
        if (p == parent)
            return 1;
        p = w32k_get_parent(p);
    }
    return 0;
}

BOOL_T IsIconic(uintptr_t hwnd)
{
    return w32k_get_window_state(hwnd) == W32K_SW_SHOWMINIMIZED;
}

BOOL_T IsZoomed(uintptr_t hwnd)
{
    return w32k_get_window_state(hwnd) == W32K_SW_SHOWMAXIMIZED;
}

BOOL_T EnableWindow(uintptr_t hwnd, BOOL_T enable)
{
    return w32k_enable_window(hwnd, enable != 0) == 1;
}

BOOL_T IsWindowEnabled(uintptr_t hwnd)
{
    return w32k_is_window_enabled(hwnd);
}

BOOL_T BringWindowToTop(uintptr_t hwnd)
{
    return w32k_set_zorder(hwnd, 0);
}

uint32_t GetWindowThreadProcessId(uintptr_t hwnd, uint32_t *pid)
{
    uint64_t p;
    if (!w32k_is_window(hwnd))
        return 0;
    if (pid) {
        __asm__ volatile("movq %%gs:0x40, %0" : "=r"(p) :: "memory");
        *pid = (uint32_t)p;
    }
    return w32k_get_window_thread(hwnd);
}

int32_t GetWindowLongW(uintptr_t hwnd, int32_t index)
{
    return (int32_t)w32k_get_window_long(hwnd, index);
}

int32_t SetWindowLongW(uintptr_t hwnd, int32_t index, int32_t value)
{
    /* A 32-bit style or id value is zero-extended, as the model stores unsigned fields. */
    return (int32_t)w32k_set_window_long(hwnd, index, (intptr_t)(uint32_t)value);
}

intptr_t GetWindowLongPtrA(uintptr_t hwnd, int32_t index)
{
    return w32k_get_window_long(hwnd, index);
}

uintptr_t GetClassLongPtrW(uintptr_t hwnd, int32_t index)
{
    intptr_t v = 0;
    return w32k_get_class_long(hwnd, index, &v) ? (uintptr_t)v : 0;
}

uintptr_t SetClassLongPtrW(uintptr_t hwnd, int32_t index, intptr_t value)
{
    intptr_t old = 0;
    return w32k_set_class_long(hwnd, index, value, &old) ? (uintptr_t)old : 0;
}

/* ---- Position, size and z-order ------------------------------------------------- */

#define SWP_NOSIZE 0x0001u
#define SWP_NOMOVE 0x0002u
#define SWP_NOZORDER 0x0004u
#define SWP_NOREDRAW 0x0008u
#define SWP_SHOWWINDOW 0x0040u
#define SWP_HIDEWINDOW 0x0080u

#define W32K_WS_EX_TOPMOST_BIT 0x00000008u

BOOL_T SetWindowPos(uintptr_t hwnd, intptr_t after, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags)
{
    w32k_rect rc;
    uintptr_t parent;
    if (!w32k_get_window_rect(hwnd, &rc))
        return 0;
    parent = w32k_get_parent(hwnd);
    if (parent) {
        w32k_rect pr;
        if (w32k_get_window_rect(parent, &pr)) {
            rc.left -= pr.left;
            rc.right -= pr.left;
            rc.top -= pr.top;
            rc.bottom -= pr.top;
        }
    }
    if (flags & SWP_NOMOVE) {
        x = rc.left;
        y = rc.top;
    }
    if (flags & SWP_NOSIZE) {
        cx = rc.right - rc.left;
        cy = rc.bottom - rc.top;
    }
    if (!((flags & SWP_NOMOVE) && (flags & SWP_NOSIZE)))
        w32k_move_window(hwnd, x, y, cx, cy, !(flags & SWP_NOREDRAW));
    if (!(flags & SWP_NOZORDER)) {
        intptr_t ex = w32k_get_window_long(hwnd, W32K_GWL_EXSTYLE);
        if (after == -1) { /* HWND_TOPMOST */
            w32k_set_window_long(hwnd, W32K_GWL_EXSTYLE, ex | W32K_WS_EX_TOPMOST_BIT);
            w32k_set_zorder(hwnd, 0);
        } else if (after == -2) { /* HWND_NOTOPMOST */
            w32k_set_window_long(hwnd, W32K_GWL_EXSTYLE, ex & ~(intptr_t)W32K_WS_EX_TOPMOST_BIT);
            w32k_set_zorder(hwnd, 0);
        } else if (after == 1) { /* HWND_BOTTOM */
            w32k_set_zorder(hwnd, 1);
        } else { /* HWND_TOP or a sibling: the model places the window on top */
            w32k_set_zorder(hwnd, 0);
        }
    }
    if (flags & SWP_SHOWWINDOW)
        w32k_show_window(hwnd, W32K_SW_SHOW);
    else if (flags & SWP_HIDEWINDOW)
        w32k_show_window(hwnd, W32K_SW_HIDE);
    return 1;
}

/* WINDOWPLACEMENT (44 bytes): length, flags, showCmd, ptMinPosition, ptMaxPosition, rcNormalPosition. */
BOOL_T GetWindowPlacement(uintptr_t hwnd, void *wp)
{
    w32k_rect rc;
    int st = w32k_get_window_state(hwnd);
    if (!wp || st < 0 || get32(wp, 0) != 44 || !w32k_get_window_rect(hwnd, &rc))
        return 0;
    put32(wp, 4, 0);
    put32(wp, 8, (uint32_t)st);
    put32(wp, 12, 0xFFFFFFFFu); /* ptMinPosition = (-1,-1): not set */
    put32(wp, 16, 0xFFFFFFFFu);
    put32(wp, 20, 0xFFFFFFFFu);
    put32(wp, 24, 0xFFFFFFFFu);
    put32(wp, 28, (uint32_t)rc.left);
    put32(wp, 32, (uint32_t)rc.top);
    put32(wp, 36, (uint32_t)rc.right);
    put32(wp, 40, (uint32_t)rc.bottom);
    return 1;
}

BOOL_T SetWindowPlacement(uintptr_t hwnd, const void *wp)
{
    int32_t l, t, r, b;
    if (!wp || get32(wp, 0) != 44 || !w32k_is_window(hwnd))
        return 0;
    l = (int32_t)get32(wp, 28);
    t = (int32_t)get32(wp, 32);
    r = (int32_t)get32(wp, 36);
    b = (int32_t)get32(wp, 40);
    w32k_move_window(hwnd, l, t, r - l, b - t, 1);
    w32k_show_window(hwnd, (int)get32(wp, 8));
    return 1;
}

/* ---- Update region ---------------------------------------------------------------- */

BOOL_T UpdateWindow(uintptr_t hwnd)
{
    return w32k_update_window(hwnd);
}

BOOL_T ValidateRect(uintptr_t hwnd, const w32k_rect *rc)
{
    (void)rc; /* one update region per window: any validation clears it */
    return hwnd ? w32k_validate_rect(hwnd) : 1;
}

BOOL_T GetUpdateRect(uintptr_t hwnd, w32k_rect *rc, BOOL_T erase)
{
    (void)erase;
    return w32k_get_update_rect(hwnd, rc);
}

#define RDW_INVALIDATE 0x0001u
#define RDW_ERASE 0x0004u
#define RDW_VALIDATE 0x0008u
#define RDW_ALLCHILDREN 0x0080u
#define RDW_UPDATENOW 0x0100u
#define RDW_ERASENOW 0x0200u

typedef struct redraw_ctx {
    uint32_t flags;
} redraw_ctx;

static int redraw_one(w32k_handle h, intptr_t lp)
{
    uint32_t flags = ((const redraw_ctx *)(uintptr_t)lp)->flags;
    if (flags & RDW_INVALIDATE)
        w32k_invalidate_rect(h, NULL, (flags & RDW_ERASE) != 0);
    else if (flags & RDW_VALIDATE)
        w32k_validate_rect(h);
    return 1;
}

BOOL_T RedrawWindow(uintptr_t hwnd, const w32k_rect *rc, uintptr_t rgn, uint32_t flags)
{
    redraw_ctx ctx;
    (void)rc;
    (void)rgn;
    if (!hwnd)
        hwnd = w32k_get_desktop_window();
    if (!w32k_is_window(hwnd))
        return 0;
    ctx.flags = flags;
    redraw_one(hwnd, (intptr_t)(uintptr_t)&ctx);
    if (flags & RDW_ALLCHILDREN)
        w32k_enum_windows(hwnd, redraw_one, (intptr_t)(uintptr_t)&ctx);
    if (flags & (RDW_UPDATENOW | RDW_ERASENOW))
        w32k_update_window(hwnd);
    return 1;
}

uintptr_t GetWindowDC(uintptr_t hwnd)
{
    return w32k_get_dc(hwnd);
}

/* MapWindowPoints: the points move from the client space of `from` (NULL: screen) to that of `to`.
 * Returns the offset applied, (dy << 16) | (dx & 0xFFFF), or 0 on failure. */
int32_t MapWindowPoints(uintptr_t from, uintptr_t to, w32k_point *pts, uint32_t count)
{
    w32k_point origin = {0, 0};
    uint32_t i;
    if (from && !w32k_is_window(from))
        return 0;
    if (to && !w32k_is_window(to))
        return 0;
    if (from)
        w32k_client_to_screen(from, &origin);
    if (to) {
        w32k_point o2 = {0, 0};
        w32k_client_to_screen(to, &o2);
        origin.x -= o2.x;
        origin.y -= o2.y;
    }
    for (i = 0; pts && i < count; ++i) {
        pts[i].x += origin.x;
        pts[i].y += origin.y;
    }
    return (int32_t)(((uint32_t)origin.y << 16) | ((uint32_t)origin.x & 0xFFFFu));
}

uintptr_t WindowFromPoint(w32k_point pt)
{
    uintptr_t h = w32k_window_from_point(pt.x, pt.y);
    return h ? h : w32k_get_desktop_window();
}

typedef struct find_ctx {
    const uint16_t *cls;
    const uint16_t *name;
    uintptr_t after;
    int past_after;
    uintptr_t found;
} find_ctx;

static uint16_t lower16(uint16_t c)
{
    return u32p_wlower(c);
}

static int wequal_ci(const uint16_t *a, const uint16_t *b)
{
    for (;; ++a, ++b) {
        if (lower16(*a) != lower16(*b))
            return 0;
        if (!*a)
            return 1;
    }
}

static int find_cb(w32k_handle h, intptr_t lp)
{
    find_ctx *c = (find_ctx *)(uintptr_t)lp;
    uint16_t buf[256];
    if (c->after && !c->past_after) {
        if (h == c->after)
            c->past_after = 1;
        return 1;
    }
    if (c->cls) {
        if ((uintptr_t)c->cls < 0x10000u) { /* a class atom */
            intptr_t atom = 0;
            if (!w32k_get_class_long(h, W32K_GCW_ATOM, &atom) || (uintptr_t)atom != (uintptr_t)c->cls)
                return 1;
        } else {
            if (!w32k_get_class_name(h, buf, 256) || !wequal_ci(buf, c->cls))
                return 1;
        }
    }
    if (c->name) {
        w32k_get_window_text(h, buf, 256);
        if (!wequal_ci(buf, c->name))
            return 1;
    }
    c->found = h;
    return 0;
}

/* FindWindowExW: direct children of parent (NULL: the top-level windows), after `after` when given. */
uintptr_t FindWindowExW(uintptr_t parent, uintptr_t after, const uint16_t *cls, const uint16_t *name)
{
    find_ctx c;
    c.cls = cls;
    c.name = name;
    c.after = after;
    c.past_after = 0;
    c.found = 0;
    if (!parent || parent == w32k_get_desktop_window()) {
        w32k_enum_windows(0, find_cb, (intptr_t)(uintptr_t)&c);
    } else {
        /* EnumChildWindows is recursive; walk the direct children by hand. */
        uintptr_t k = w32k_get_window_rel(parent, W32K_GW_CHILD);
        while (k) {
            if (!find_cb(k, (intptr_t)(uintptr_t)&c))
                break;
            k = w32k_get_window_rel(k, W32K_GW_HWNDNEXT);
        }
    }
    return c.found;
}

BOOL_T EnumWindows(void *proc, intptr_t lp)
{
    return w32k_enum_windows(0, (w32k_enumwndproc)proc, lp) != 0;
}

BOOL_T EnumChildWindows(uintptr_t parent, void *proc, intptr_t lp)
{
    if (!parent)
        return EnumWindows(proc, lp);
    return w32k_enum_windows(parent, (w32k_enumwndproc)proc, lp) != 0;
}

/* ---- Cursor ------------------------------------------------------------------------- */

static w32k_rect clip_rc;
static int clip_set;

int32_t ShowCursor(BOOL_T show)
{
    return w32k_show_cursor(show != 0);
}

BOOL_T ClipCursor(const w32k_rect *rc)
{
    if (rc) {
        clip_rc = *rc;
        clip_set = 1;
    } else {
        clip_set = 0;
    }
    return 1;
}

BOOL_T GetClipCursor(w32k_rect *rc)
{
    if (!rc)
        return 0;
    if (clip_set) {
        *rc = clip_rc;
    } else {
        rc->left = 0;
        rc->top = 0;
        rc->right = w32k_get_system_metrics(W32K_SM_CXVIRTUALSCREEN);
        rc->bottom = w32k_get_system_metrics(W32K_SM_CYVIRTUALSCREEN);
    }
    return 1;
}

/* CURSORINFO (24 bytes): cbSize, flags (CURSOR_SHOWING = 1), hCursor, ptScreenPos. */
BOOL_T GetCursorInfo(void *ci)
{
    w32k_point pt;
    int32_t count;
    if (!ci || get32(ci, 0) != 24)
        return 0;
    w32k_get_cursor_pos(&pt);
    count = w32k_get_cursor_count();
    put32(ci, 4, count >= 0 ? 1u : 0u);
    put64(ci, 8, w32k_get_cursor());
    put32(ci, 16, (uint32_t)pt.x);
    put32(ci, 20, (uint32_t)pt.y);
    return 1;
}

/* LoadImageW for cursors (IMAGE_CURSOR 2) and icons (IMAGE_ICON 1) by stock id. */
uintptr_t LoadImageW(const void *hinst, uintptr_t name, uint32_t type, int32_t cx, int32_t cy, uint32_t flags)
{
    (void)hinst;
    (void)cx;
    (void)cy;
    (void)flags;
    if (type == 2)
        return w32k_load_cursor(name);
    if (type == 1)
        return w32k_load_icon(name);
    w32k_set_last_error(W32K_ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

/* ---- Monitors and display devices -------------------------------------------------- */

#define MONITORINFOF_PRIMARY 1u

static void monitor_name(uint32_t hmon, char *out, size_t cap)
{
    static const char prefix[] = "\\\\.\\DISPLAY";
    size_t n = 0;
    while (prefix[n] && n + 3 < cap) {
        out[n] = prefix[n];
        ++n;
    }
    out[n++] = (char)('0' + (hmon % 10));
    out[n] = 0;
}

/* MONITORINFO (40 bytes) and MONITORINFOEXW (104 bytes, adds szDevice[32]). */
BOOL_T GetMonitorInfoW(uintptr_t hmon, void *mi)
{
    w32k_rect mon, work;
    int primary = 0;
    uint32_t size;
    if (!mi)
        return 0;
    size = get32(mi, 0);
    if ((size != 40 && size != 104) || !w32k_get_monitor_info(hmon, &mon, &work, &primary))
        return 0;
    put32(mi, 4, (uint32_t)mon.left);
    put32(mi, 8, (uint32_t)mon.top);
    put32(mi, 12, (uint32_t)mon.right);
    put32(mi, 16, (uint32_t)mon.bottom);
    put32(mi, 20, (uint32_t)work.left);
    put32(mi, 24, (uint32_t)work.top);
    put32(mi, 28, (uint32_t)work.right);
    put32(mi, 32, (uint32_t)work.bottom);
    put32(mi, 36, primary ? MONITORINFOF_PRIMARY : 0);
    if (size == 104) {
        char name[16];
        monitor_name((uint32_t)hmon, name, sizeof(name));
        put_wstr(mi, 40, name, 32);
    }
    return 1;
}

uintptr_t MonitorFromPoint(w32k_point pt, uint32_t flags)
{
    return w32k_monitor_from_point(pt.x, pt.y, flags);
}

uintptr_t MonitorFromRect(const w32k_rect *rc, uint32_t flags)
{
    return w32k_monitor_from_rect(rc, flags);
}

typedef struct nth_ctx {
    uint32_t want, seen;
    uintptr_t found;
} nth_ctx;

static int nth_cb(w32k_handle hmon, const w32k_rect *rc, intptr_t lp)
{
    nth_ctx *c = (nth_ctx *)(uintptr_t)lp;
    (void)rc;
    if (c->seen++ == c->want) {
        c->found = hmon;
        return 0;
    }
    return 1;
}

#define DISPLAY_DEVICE_ATTACHED_TO_DESKTOP 0x1u
#define DISPLAY_DEVICE_PRIMARY_DEVICE 0x4u

/* DISPLAY_DEVICEW (840 bytes): cb, DeviceName[32], DeviceString[128], StateFlags, DeviceID[128], DeviceKey[128]. */
BOOL_T EnumDisplayDevicesW(const uint16_t *device, uint32_t index, void *dd, uint32_t flags)
{
    nth_ctx c;
    char name[32];
    int primary = 0;
    (void)flags;
    if (!dd || get32(dd, 0) != 840)
        return 0;
    c.want = device ? 0 : index;
    c.seen = 0;
    c.found = 0;
    w32k_enum_display_monitors(nth_cb, (intptr_t)(uintptr_t)&c);
    if (!c.found || (device && index != 0))
        return 0;
    w32k_get_monitor_info(c.found, NULL, NULL, &primary);
    monitor_name((uint32_t)c.found, name, sizeof(name));
    zero_bytes((uint8_t *)dd + 4, 836);
    if (!device) {
        put_wstr(dd, 4, name, 32);
        put_wstr(dd, 68, "Personality display adapter", 128);
        put32(dd, 324, DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | (primary ? DISPLAY_DEVICE_PRIMARY_DEVICE : 0));
        put_wstr(dd, 328, "ntpers\\display", 128);
        put_wstr(dd, 584, "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Video\\ntpers\\0000", 128);
    } else {
        size_t n = 0;
        while (name[n])
            ++n;
        name[n] = '\\';
        name[n + 1] = 'M';
        name[n + 2] = 'o';
        name[n + 3] = 'n';
        name[n + 4] = 'i';
        name[n + 5] = 't';
        name[n + 6] = 'o';
        name[n + 7] = 'r';
        name[n + 8] = '0';
        name[n + 9] = 0;
        put_wstr(dd, 4, name, 32);
        put_wstr(dd, 68, "Generic PnP Monitor", 128);
        put32(dd, 324, DISPLAY_DEVICE_ATTACHED_TO_DESKTOP);
        put_wstr(dd, 328, "MONITOR\\ntpers", 128);
    }
    return 1;
}

int32_t ChangeDisplaySettingsExW(const uint16_t *name, void *devmode, void *hwnd, uint32_t flags, void *param)
{
    (void)name;
    (void)hwnd;
    (void)param;
    return ChangeDisplaySettingsW(devmode, flags);
}

/* ---- DPI awareness ----------------------------------------------------------------- */

#define DPI_CONTEXT_UNAWARE ((intptr_t)-1)

static int process_dpi_aware;
static intptr_t thread_dpi_context = DPI_CONTEXT_UNAWARE;

BOOL_T SetProcessDPIAware(void)
{
    process_dpi_aware = 1;
    thread_dpi_context = -2; /* DPI_AWARENESS_CONTEXT_SYSTEM_AWARE */
    return 1;
}

BOOL_T IsProcessDPIAware(void)
{
    return process_dpi_aware;
}

BOOL_T SetProcessDpiAwarenessContext(intptr_t ctx)
{
    if (ctx > -1 || ctx < -5)
        return 0; /* only the DPI_AWARENESS_CONTEXT_* pseudo handles are valid */
    process_dpi_aware = ctx != DPI_CONTEXT_UNAWARE;
    thread_dpi_context = ctx;
    return 1;
}

intptr_t SetThreadDpiAwarenessContext(intptr_t ctx)
{
    intptr_t old = thread_dpi_context;
    if (ctx > -1 || ctx < -5)
        return 0;
    thread_dpi_context = ctx;
    return old;
}

intptr_t GetThreadDpiAwarenessContext(void)
{
    return thread_dpi_context;
}

intptr_t GetWindowDpiAwarenessContext(uintptr_t hwnd)
{
    return w32k_is_window(hwnd) ? thread_dpi_context : 0;
}

BOOL_T AreDpiAwarenessContextsEqual(intptr_t a, intptr_t b)
{
    return a == b;
}

uint32_t GetDpiForSystem(void)
{
    uint32_t d = w32k_get_dpi_for_window(w32k_get_desktop_window());
    return d ? d : 96;
}

/* ---- Messages ----------------------------------------------------------------------- */

#define QS_KEY 0x0001u
#define QS_MOUSE 0x0006u
#define QS_POSTMESSAGE 0x0008u
#define QS_TIMER 0x0010u
#define QS_PAINT 0x0020u

uint32_t RegisterWindowMessageW(const uint16_t *name)
{
    return w32k_register_clipboard_format(name); /* windows messages and clipboard formats share the atom range */
}

uint32_t GetMessageTime(void)
{
    uint32_t prev = w32k_set_current_thread(w32k_shim_tid());
    uint32_t t = w32k_get_message_time();
    w32k_set_current_thread(prev);
    return t;
}

uint32_t GetMessagePos(void)
{
    uint32_t prev = w32k_set_current_thread(w32k_shim_tid());
    w32k_point p = w32k_get_message_pos();
    w32k_set_current_thread(prev);
    return ((uint32_t)p.y << 16) | ((uint32_t)p.x & 0xFFFFu);
}

BOOL_T InSendMessage(void)
{
    return 0; /* sends run on the caller's stack, so a window procedure is never replying to another thread */
}

/* GetQueueStatus: the flags of the kinds of message waiting, in both halves of the result. */
uint32_t GetQueueStatus(uint32_t flags)
{
    uint32_t tid = w32k_shim_tid(), prev = w32k_set_current_thread(tid), have = 0;
    w32k_msg m;
    w32k_shim_now();
    if (w32k_peek_message(&m, 0, W32K_WM_KEYDOWN, W32K_WM_SYSCHAR, 0))
        have |= QS_KEY;
    if (w32k_peek_message(&m, 0, W32K_WM_MOUSEMOVE, 0x020E, 0))
        have |= QS_MOUSE;
    if (w32k_peek_message(&m, 0, W32K_WM_TIMER, W32K_WM_TIMER, 0))
        have |= QS_TIMER;
    if (w32k_peek_message(&m, 0, W32K_WM_PAINT, W32K_WM_PAINT, 0))
        have |= QS_PAINT;
    if (w32k_peek_message(&m, 0, 0, 0, 0) && !(have & (QS_KEY | QS_MOUSE | QS_TIMER | QS_PAINT)))
        have |= QS_POSTMESSAGE;
    w32k_set_current_thread(prev);
    have &= flags;
    return (have << 16) | have;
}

intptr_t SendMessageTimeoutW(uintptr_t hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, uint32_t flags,
                             uint32_t timeout, uintptr_t *result)
{
    intptr_t r;
    (void)flags;
    (void)timeout;
    if (!w32k_is_window(hwnd))
        return 0;
    r = w32k_send_message(hwnd, msg, wp, lp);
    if (result)
        *result = (uintptr_t)r;
    return 1;
}

BOOL_T AttachThreadInput(uint32_t from, uint32_t to, BOOL_T attach)
{
    (void)from;
    (void)to;
    (void)attach;
    return 1; /* one input queue per model thread: nothing to share */
}

/* LASTINPUTINFO (8 bytes): cbSize, dwTime. */
BOOL_T GetLastInputInfo(void *li)
{
    if (!li || get32(li, 0) != 8)
        return 0;
    put32(li, 4, w32k_shim_now());
    return 1;
}

/* ---- Keyboard ------------------------------------------------------------------------ */

BOOL_T GetKeyboardState(uint8_t *state)
{
    uint32_t vk;
    uint32_t prev = w32k_set_current_thread(w32k_shim_tid());
    if (!state) {
        w32k_set_current_thread(prev);
        return 0;
    }
    for (vk = 0; vk < 256; ++vk) {
        uint16_t s = w32k_get_key_state(vk);
        state[vk] = (uint8_t)(((s & 0x8000u) ? 0x80u : 0u) | (s & 1u));
    }
    w32k_set_current_thread(prev);
    return 1;
}

uintptr_t GetKeyboardLayout(uint32_t tid)
{
    (void)tid;
    return 0x04090409u; /* en-US layout on the en-US language */
}

int32_t GetKeyboardType(int32_t kind)
{
    switch (kind) {
    case 0: return 4;  /* enhanced 101 or 102 key keyboard */
    case 1: return 0;
    case 2: return 12; /* function keys */
    default: return 0;
    }
}

uint32_t MapVirtualKeyW(uint32_t code, uint32_t type)
{
    switch (type) {
    case 0: return u32p_vk_to_scan(code);
    case 1: return u32p_scan_to_vk_fn(code, 0);
    case 2: { /* MAPVK_VK_TO_CHAR: the unshifted character, upper case for letters */
        uint16_t c = u32p_vk_char(code, 0);
        return c >= 'a' && c <= 'z' ? (uint32_t)(c - 0x20) : c;
    }
    case 3: return u32p_scan_to_vk_fn(code, 1);
    default: return 0;
    }
}

uint32_t MapVirtualKeyExW(uint32_t code, uint32_t type, uintptr_t hkl)
{
    (void)hkl;
    return MapVirtualKeyW(code, type);
}

int16_t VkKeyScanW(uint16_t ch)
{
    return u32p_vk_key_scan(ch);
}

int16_t VkKeyScanExW(uint16_t ch, uintptr_t hkl)
{
    (void)hkl;
    return u32p_vk_key_scan(ch);
}

/* ToUnicode on the US layout. Control with a letter gives the control character; Caps Lock
 * inverts the case of letters. Returns the number of characters written (0 or 1). */
int32_t ToUnicodeEx(uint32_t vk, uint32_t scan, const uint8_t *state, uint16_t *buf, int32_t cch,
                    uint32_t flags, uintptr_t hkl)
{
    int shift, ctrl, alt, caps;
    uint16_t c;
    (void)scan;
    (void)flags;
    (void)hkl;
    if (!state || !buf || cch < 1)
        return 0;
    shift = (state[U32P_VK_SHIFT] | state[U32P_VK_LSHIFT] | state[U32P_VK_RSHIFT]) & 0x80;
    ctrl = (state[U32P_VK_CONTROL] | state[U32P_VK_LCONTROL] | state[0xA3]) & 0x80;
    alt = (state[U32P_VK_MENU] | state[U32P_VK_LMENU] | state[0xA5]) & 0x80;
    caps = state[U32P_VK_CAPITAL] & 1;
    if (alt && !ctrl)
        return 0;
    if (vk >= 'A' && vk <= 'Z') {
        if (ctrl) {
            buf[0] = (uint16_t)(vk & 0x1F);
            return 1;
        }
        c = u32p_vk_char(vk, shift != 0 ? !caps : caps);
    } else {
        c = u32p_vk_char(vk, shift != 0);
        if (ctrl && c)
            return 0;
    }
    if (!c)
        return 0;
    buf[0] = c;
    return 1;
}

int32_t ToUnicode(uint32_t vk, uint32_t scan, const uint8_t *state, uint16_t *buf, int32_t cch, uint32_t flags)
{
    return ToUnicodeEx(vk, scan, state, buf, cch, flags, 0);
}

int32_t GetKeyNameTextW(int32_t lparam, uint16_t *buf, int32_t cch)
{
    if (!buf)
        return 0;
    return u32p_key_name((uint32_t)lparam, buf, cch);
}

/* ---- System colours, case mapping and double-click ------------------------------------ */

uint32_t GetSysColor(int32_t index)
{
    int ok;
    uint32_t c = u32p_sys_colorref(index, &ok);
    return ok ? c : 0;
}

static uintptr_t sys_brushes[U32P_COLOR_COUNT];

uintptr_t GetSysColorBrush(int32_t index)
{
    int ok;
    uint32_t c = u32p_sys_colorref(index, &ok);
    if (!ok)
        return 0;
    if (!sys_brushes[index])
        sys_brushes[index] = w32k_create_solid_brush(c);
    return sys_brushes[index];
}

uint32_t GetDoubleClickTime(void)
{
    return 500;
}

uint32_t GetCaretBlinkTime(void)
{
    return 530;
}

/* CharUpperW and CharLowerW take either a string or, when the pointer is below 0x10000, a single
 * character in the low word. */
uint16_t *CharUpperW(uint16_t *s)
{
    uint16_t *p = s;
    if ((uintptr_t)s <= 0xFFFFu)
        return (uint16_t *)(uintptr_t)u32p_wupper((uint16_t)(uintptr_t)s);
    for (; *p; ++p)
        *p = u32p_wupper(*p);
    return s;
}

uint16_t *CharLowerW(uint16_t *s)
{
    uint16_t *p = s;
    if ((uintptr_t)s <= 0xFFFFu)
        return (uint16_t *)(uintptr_t)u32p_wlower((uint16_t)(uintptr_t)s);
    for (; *p; ++p)
        *p = u32p_wlower(*p);
    return s;
}

uint32_t CharUpperBuffW(uint16_t *s, uint32_t n)
{
    uint32_t i;
    if (!s)
        return 0;
    for (i = 0; i < n; ++i)
        s[i] = u32p_wupper(s[i]);
    return n;
}

uint32_t CharLowerBuffW(uint16_t *s, uint32_t n)
{
    uint32_t i;
    if (!s)
        return 0;
    for (i = 0; i < n; ++i)
        s[i] = u32p_wlower(s[i]);
    return n;
}

uint16_t *CharNextW(const uint16_t *s)
{
    return (uint16_t *)(uintptr_t)(*s ? s + 1 : s);
}

uint16_t *CharPrevW(const uint16_t *start, const uint16_t *cur)
{
    return (uint16_t *)(uintptr_t)(cur > start ? cur - 1 : start);
}

BOOL_T IsCharAlphaW(uint16_t c)
{
    return u32p_wupper(c) != u32p_wlower(c);
}

BOOL_T IsCharUpperW(uint16_t c)
{
    return u32p_wlower(c) != c;
}

BOOL_T IsCharLowerW(uint16_t c)
{
    return u32p_wupper(c) != c;
}

/* ---- Raw input ---------------------------------------------------------------------------- */

#define RID_HEADER 0x10000005u
#define RID_INPUT 0x10000003u
#define RIDI_PREPARSEDDATA 0x20000005u
#define RIDI_DEVICENAME 0x20000007u
#define RIDI_DEVICEINFO 0x2000000Bu
#define ERROR_INSUFFICIENT_BUFFER_CODE 122

/* RAWINPUTDEVICE has the layout of w32k_rid_dev: USHORT, USHORT, DWORD, HWND. */
typedef char rawinputdevice_layout[(sizeof(w32k_rid_dev) == 16 && offsetof(w32k_rid_dev, flags) == 4 &&
                                    offsetof(w32k_rid_dev, target) == 8) ? 1 : -1];

BOOL_T RegisterRawInputDevices(const w32k_rid_dev *devs, uint32_t n, uint32_t cb)
{
    if (cb != sizeof(w32k_rid_dev)) {
        w32k_set_last_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return w32k_register_raw_input(devs, n);
}

uint32_t GetRegisteredRawInputDevices(w32k_rid_dev *out, uint32_t *num, uint32_t cb)
{
    uint32_t have;
    if (!num || cb != sizeof(w32k_rid_dev)) {
        w32k_set_last_error(W32K_ERROR_INVALID_PARAMETER);
        return (uint32_t)-1;
    }
    have = w32k_get_registered_raw_input(NULL, 0);
    if (!out) {
        *num = have;
        return 0;
    }
    if (*num < have) {
        *num = have;
        w32k_set_last_error(ERROR_INSUFFICIENT_BUFFER_CODE);
        return (uint32_t)-1;
    }
    w32k_get_registered_raw_input(out, have);
    return have;
}

static void copy_bytes(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s2 = (const uint8_t *)src;
    while (n--)
        *d++ = *s2++;
}

uint32_t GetRawInputData(uintptr_t hri, uint32_t cmd, void *data, uint32_t *size, uint32_t cb_header)
{
    uint32_t total = 0, want;
    const uint8_t *b;
    if (!size || cb_header != W32K_RAWINPUT_HEADER_SIZE || (cmd != RID_INPUT && cmd != RID_HEADER)) {
        w32k_set_last_error(W32K_ERROR_INVALID_PARAMETER);
        return (uint32_t)-1;
    }
    b = w32k_raw_input_bytes(hri, &total);
    if (!b) {
        w32k_set_last_error(W32K_ERROR_INVALID_HANDLE);
        return (uint32_t)-1;
    }
    want = cmd == RID_INPUT ? total : W32K_RAWINPUT_HEADER_SIZE;
    if (!data) {
        *size = want;
        return 0;
    }
    if (*size < want) {
        *size = want;
        w32k_set_last_error(ERROR_INSUFFICIENT_BUFFER_CODE);
        return (uint32_t)-1;
    }
    copy_bytes(data, b, want);
    return want;
}

uint32_t GetRawInputBuffer(void *data, uint32_t *size, uint32_t cb_header)
{
    (void)data;
    (void)cb_header;
    if (size)
        *size = 0;
    return 0; /* WM_INPUT is delivered through the queue; there is no buffered stream */
}

intptr_t DefRawInputProc(void *raw, int32_t n, uint32_t cb_header)
{
    (void)raw;
    (void)n;
    (void)cb_header;
    return 0;
}

/* RAWINPUTDEVICELIST (16 bytes): HANDLE hDevice, DWORD dwType. */
uint32_t GetRawInputDeviceList(void *list, uint32_t *num, uint32_t cb)
{
    if (!num || cb != 16) {
        w32k_set_last_error(W32K_ERROR_INVALID_PARAMETER);
        return (uint32_t)-1;
    }
    if (!list) {
        *num = 2;
        return 0;
    }
    if (*num < 2) {
        *num = 2;
        w32k_set_last_error(ERROR_INSUFFICIENT_BUFFER_CODE);
        return (uint32_t)-1;
    }
    zero_bytes(list, 32);
    put64(list, 0, W32K_RAW_MOUSE_DEVICE);
    put32(list, 8, W32K_RIM_TYPEMOUSE);
    put64(list, 16, W32K_RAW_KEYBOARD_DEVICE);
    put32(list, 24, W32K_RIM_TYPEKEYBOARD);
    return 2;
}

/* RID_DEVICE_INFO (32 bytes): cbSize, dwType, then a mouse or keyboard record. */
int32_t GetRawInputDeviceInfoW(uintptr_t dev, uint32_t cmd, void *data, uint32_t *size)
{
    static const char mouse_name[] = "\\\\?\\HID#VID_0000&PID_0001#0&0&0000#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";
    static const char kbd_name[] = "\\\\?\\HID#VID_0000&PID_0002#0&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}";
    const char *name;
    uint32_t n = 0;
    int keyboard;
    if (!size || (dev != W32K_RAW_MOUSE_DEVICE && dev != W32K_RAW_KEYBOARD_DEVICE)) {
        w32k_set_last_error(W32K_ERROR_INVALID_HANDLE);
        return -1;
    }
    keyboard = dev == W32K_RAW_KEYBOARD_DEVICE;
    switch (cmd) {
    case RIDI_DEVICENAME:
        name = keyboard ? kbd_name : mouse_name;
        while (name[n])
            ++n;
        ++n; /* the size counts characters, including the terminator */
        if (!data) {
            *size = n;
            return 0;
        }
        if (*size < n) {
            *size = n;
            w32k_set_last_error(ERROR_INSUFFICIENT_BUFFER_CODE);
            return -1;
        }
        put_wstr(data, 0, name, n);
        return (int32_t)n;
    case RIDI_DEVICEINFO:
        if (!data) {
            *size = 32;
            return 0;
        }
        if (*size < 32) {
            *size = 32;
            w32k_set_last_error(ERROR_INSUFFICIENT_BUFFER_CODE);
            return -1;
        }
        zero_bytes(data, 32);
        put32(data, 0, 32);
        put32(data, 4, keyboard ? W32K_RIM_TYPEKEYBOARD : W32K_RIM_TYPEMOUSE);
        if (keyboard) { /* type 4 (enhanced), subtype 0, scan code set 1, 12 function keys, 3 LEDs, 101 keys */
            put32(data, 8, 4);
            put32(data, 12, 0);
            put32(data, 16, 1);
            put32(data, 20, 12);
            put32(data, 24, 3);
            put32(data, 28, 101);
        } else { /* id, 5 buttons, 1000 Hz, no horizontal wheel */
            put32(data, 8, 1);
            put32(data, 12, 5);
            put32(data, 16, 1000);
            put32(data, 20, 0);
        }
        return 32;
    case RIDI_PREPARSEDDATA:
        if (!data) {
            *size = 0;
            return 0;
        }
        return 0;
    default:
        w32k_set_last_error(W32K_ERROR_INVALID_PARAMETER);
        return -1;
    }
}

/* ---- Honest stubs ----------------------------------------------------------------------- */

BOOL_T SetLayeredWindowAttributes(uintptr_t hwnd, uint32_t key, uint8_t alpha, uint32_t flags)
{
    (void)key;
    (void)alpha;
    (void)flags;
    return w32k_is_window(hwnd); /* the model composites nothing, so the attributes have no effect */
}

int32_t SetWindowRgn(uintptr_t hwnd, uintptr_t rgn, BOOL_T redraw)
{
    (void)rgn;
    (void)redraw;
    return w32k_is_window(hwnd) != 0; /* accepted; the model does not clip windows to a region */
}

#define MB_TYPEMASK 0x0000000Fu

/* MessageBoxW shows no dialog. It returns the button an unattended caller would accept: IDOK,
 * IDYES, IDRETRY or IDCONTINUE according to the buttons the type asks for. */
int32_t MessageBoxW(uintptr_t hwnd, const uint16_t *text, const uint16_t *caption, uint32_t type)
{
    (void)hwnd;
    (void)text;
    (void)caption;
    switch (type & MB_TYPEMASK) {
    case 0: return 1;  /* MB_OK: IDOK */
    case 1: return 1;  /* MB_OKCANCEL: IDOK */
    case 2: return 4;  /* MB_ABORTRETRYIGNORE: IDRETRY */
    case 3: return 6;  /* MB_YESNOCANCEL: IDYES */
    case 4: return 6;  /* MB_YESNO: IDYES */
    case 5: return 4;  /* MB_RETRYCANCEL: IDRETRY */
    case 6: return 11; /* MB_CANCELTRYCONTINUE: IDCONTINUE */
    default: return 1;
    }
}

/* LoadStringW needs the resource section of a module, which the personality does not read yet. */
int32_t LoadStringW(const void *hinst, uint32_t id, uint16_t *buf, int32_t cch)
{
    (void)hinst;
    (void)id;
    if (buf && cch > 0)
        buf[0] = 0;
    w32k_set_last_error(W32K_ERROR_RESOURCE_NAME_NOT_FOUND);
    return 0;
}
#endif
