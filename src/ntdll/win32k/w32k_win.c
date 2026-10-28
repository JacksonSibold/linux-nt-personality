/* w32k_win.c - window state and tree queries: show state, enable state, GetWindow
 * relations, the desktop window, class names, update-region helpers, window
 * enumeration and the cursor display counter.
 *
 * The model keeps one update region per window (the whole client area), so
 * validation and GetUpdateRect work on that single flag.
 */
#include "w32k_int.h"

static const uint16_t desktop_class_name[] = {'#', '3', '2', '7', '6', '9', 0};

static w32k_handle first_sibling(const w32k_window *w)
{
    if (w->parent) {
        w32k_window *p = w32k_window_of(w->parent);
        return p ? p->first_child : 0;
    }
    {
        w32k_desktop *d = w32k_desktop_of(w->desktop);
        return d ? d->top_head : 0;
    }
}

int w32k_get_window_state(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return -1;
    }
    return w->state ? w->state : W32K_SW_SHOWNORMAL;
}

int w32k_enable_window(w32k_handle hwnd, int enable)
{
    w32k_window *w = w32k_window_checked(hwnd);
    int was_disabled;
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return -1;
    }
    was_disabled = (w->style & W32K_WS_DISABLED) != 0;
    if (enable && was_disabled) {
        w->style &= ~W32K_WS_DISABLED;
        w32k_call_proc(hwnd, W32K_WM_ENABLE, 1, 0);
    } else if (!enable && !was_disabled) {
        w->style |= W32K_WS_DISABLED;
        w32k_call_proc(hwnd, W32K_WM_ENABLE, 0, 0);
    }
    return was_disabled;
}

int w32k_is_window_enabled(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    return w && !(w->style & W32K_WS_DISABLED);
}

w32k_handle w32k_get_window_rel(w32k_handle hwnd, uint32_t cmd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    w32k_handle cur, prev = 0;
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    switch (cmd) {
    case W32K_GW_CHILD:
        return w->first_child;
    case W32K_GW_OWNER:
        return 0; /* the model has no owner relation */
    case W32K_GW_HWNDNEXT:
        return w->next_sibling;
    case W32K_GW_HWNDFIRST:
        return first_sibling(w);
    case W32K_GW_HWNDLAST:
    case W32K_GW_HWNDPREV:
        for (cur = first_sibling(w); cur; ) {
            w32k_window *s = w32k_window_of(cur);
            if (!s)
                break;
            if (cmd == W32K_GW_HWNDPREV && cur == hwnd)
                return prev;
            if (cmd == W32K_GW_HWNDLAST && !s->next_sibling)
                return cur;
            prev = cur;
            cur = s->next_sibling;
        }
        return 0;
    default:
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
}

w32k_handle w32k_get_desktop_window(void)
{
    w32k_queue *q = w32k_cur_queue();
    w32k_desktop *d = q ? w32k_desktop_of(q->desktop) : NULL;
    w32k_window *w;
    w32k_handle h = 0;
    const w32k_monitor *m = &w32k_g.monitors[0];
    if (!d)
        return 0;
    if (d->desktop_window && w32k_window_of(d->desktop_window))
        return d->desktop_window;
    w = (w32k_window *)w32k_pool_alloc(&w32k_pool_windows, &h);
    if (!w) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    w->desktop = q->desktop;
    w->tid = 0;
    w->cls = 0; /* no class: this is what marks the desktop window */
    w->style = W32K_WS_VISIBLE | W32K_WS_POPUP;
    w->rect = m->rc;
    w->visible = 1;
    w->state = W32K_SW_SHOWNORMAL;
    w->dpi = m->dpi ? m->dpi : 96;
    d->desktop_window = h;
    return h;
}

int w32k_get_class_name(w32k_handle hwnd, uint16_t *buf, int max)
{
    w32k_window *w = w32k_window_checked(hwnd);
    const uint16_t *name;
    int n = 0;
    if (!w || !buf || max <= 0) {
        w32k_set_error(w ? W32K_ERROR_INVALID_PARAMETER : W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    if (!w->cls) {
        name = desktop_class_name;
    } else {
        w32k_class *c = w32k_class_of(w->cls);
        if (!c) {
            w32k_set_error(W32K_ERROR_CLASS_DOES_NOT_EXIST);
            return 0;
        }
        name = c->name;
    }
    while (name[n] && n < max - 1) {
        buf[n] = name[n];
        ++n;
    }
    buf[n] = 0;
    return n;
}

int w32k_validate_rect(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    w->needs_paint = 0;
    return 1;
}

int w32k_update_window(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    if (w->needs_paint && w->visible) {
        w32k_call_proc(hwnd, W32K_WM_PAINT, 0, 0);
        /* DefWindowProc validates on WM_PAINT; the model's does not, so clear it here. */
        w = w32k_window_of(hwnd);
        if (w)
            w->needs_paint = 0;
    }
    return 1;
}

int w32k_get_update_rect(w32k_handle hwnd, w32k_rect *rc)
{
    w32k_window *w = w32k_window_checked(hwnd);
    w32k_rect empty = {0, 0, 0, 0};
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        if (rc)
            *rc = empty;
        return 0;
    }
    if (!w->needs_paint) {
        if (rc)
            *rc = empty;
        return 0;
    }
    if (rc)
        w32k_get_client_rect(hwnd, rc);
    return 1;
}

#define ENUM_MAX W32K_MAX_WINDOWS

static uint32_t collect(w32k_handle first, w32k_handle *out, uint32_t n, int recurse)
{
    w32k_handle cur;
    for (cur = first; cur && n < ENUM_MAX; ) {
        w32k_window *w = w32k_window_of(cur);
        if (!w)
            break;
        if (!w->in_destroy) {
            out[n++] = cur;
            if (recurse)
                n = collect(w->first_child, out, n, 1);
        }
        cur = w->next_sibling;
    }
    return n;
}

int w32k_enum_windows(w32k_handle parent, w32k_enumwndproc proc, intptr_t lparam)
{
    w32k_handle snap[ENUM_MAX]; /* on the stack: a callback may enumerate again */
    uint32_t n, i;
    if (!proc) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (parent) {
        w32k_window *p = w32k_window_checked(parent);
        if (!p) {
            w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        n = collect(p->first_child, snap, 0, 1);
    } else {
        w32k_desktop *d = w32k_cur_desktop();
        n = collect(d ? d->top_head : 0, snap, 0, 0);
    }
    for (i = 0; i < n; ++i) {
        if (!w32k_window_of(snap[i]))
            continue; /* destroyed by an earlier callback */
        if (!proc(snap[i], lparam))
            return 0;
    }
    return 1;
}

int32_t w32k_show_cursor(int show)
{
    int32_t c = (int32_t)w32k_g.cursor_show_count;
    c += show ? 1 : -1;
    w32k_g.cursor_show_count = (uint32_t)c;
    return c;
}

/* ---- Class values ----------------------------------------------------------- */

static w32k_class *class_of_window(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    w32k_class *c;
    if (!w) {
        w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
        return NULL;
    }
    c = w->cls ? w32k_class_of(w->cls) : NULL;
    if (!c)
        w32k_set_error(W32K_ERROR_CLASS_DOES_NOT_EXIST);
    return c;
}

int w32k_get_class_long(w32k_handle hwnd, int index, intptr_t *out)
{
    w32k_class *c = class_of_window(hwnd);
    if (!c || !out)
        return 0;
    switch (index) {
    case W32K_GCL_MENUNAME: *out = (intptr_t)c->menu_name; return 1;
    case W32K_GCL_HBRBACKGROUND: *out = (intptr_t)c->bg_brush; return 1;
    case W32K_GCL_HCURSOR: *out = (intptr_t)c->cursor; return 1;
    case W32K_GCL_HICON: *out = (intptr_t)c->icon; return 1;
    case W32K_GCL_HMODULE: *out = (intptr_t)c->hinst; return 1;
    case W32K_GCL_CBWNDEXTRA: *out = c->wnd_extra; return 1;
    case W32K_GCL_CBCLSEXTRA: *out = c->cls_extra; return 1;
    case W32K_GCL_WNDPROC: *out = (intptr_t)c->proc; return 1;
    case W32K_GCL_STYLE: *out = (intptr_t)c->style; return 1;
    case W32K_GCW_ATOM: *out = (intptr_t)c->atom; return 1;
    default:
        w32k_set_error(W32K_ERROR_INVALID_INDEX);
        return 0;
    }
}

int w32k_set_class_long(w32k_handle hwnd, int index, intptr_t value, intptr_t *old)
{
    w32k_class *c = class_of_window(hwnd);
    intptr_t prev;
    if (!c)
        return 0;
    if (!w32k_get_class_long(hwnd, index, &prev))
        return 0;
    switch (index) {
    case W32K_GCL_HBRBACKGROUND: c->bg_brush = (w32k_handle)value; break;
    case W32K_GCL_HCURSOR: c->cursor = (w32k_handle)value; break;
    case W32K_GCL_HICON: c->icon = (w32k_handle)value; break;
    case W32K_GCL_HMODULE: c->hinst = (const void *)value; break;
    case W32K_GCL_WNDPROC: c->proc = (w32k_wndproc)(uintptr_t)value; break;
    case W32K_GCL_STYLE: c->style = (uint32_t)value; break;
    default:
        /* The atom, the menu name and the extra sizes are fixed at registration. */
        w32k_set_error(W32K_ERROR_INVALID_INDEX);
        return 0;
    }
    if (old)
        *old = prev;
    return 1;
}

/* ---- Z-order and hit testing ------------------------------------------------- */

int w32k_set_zorder(w32k_handle hwnd, int bottom)
{
    w32k_window *w = w32k_window_checked(hwnd);
    w32k_handle *link;
    if (!w || !w->cls) {
        w32k_set_error(w ? W32K_ERROR_ACCESS_DENIED : W32K_ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    w32k_unlink_window(w, hwnd);
    if (w->parent) {
        w32k_window *p = w32k_window_of(w->parent);
        link = p ? &p->first_child : NULL;
    } else {
        w32k_desktop *d = w32k_desktop_of(w->desktop);
        link = d ? &d->top_head : NULL;
    }
    if (!link)
        return 0;
    if (bottom) {
        while (*link) {
            w32k_window *s = w32k_window_of(*link);
            if (!s)
                break;
            link = &s->next_sibling;
        }
    }
    w->next_sibling = *link;
    *link = hwnd;
    return 1;
}

w32k_handle w32k_window_from_point(int32_t x, int32_t y)
{
    w32k_desktop *d = w32k_cur_desktop();
    return d ? w32k_window_at_point(d, x, y) : 0;
}

int32_t w32k_get_cursor_count(void)
{
    return (int32_t)w32k_g.cursor_show_count;
}
