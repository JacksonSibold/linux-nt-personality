/* w32k_input.c - keyboard and mouse injection, key state, evdev input and the
 * clipboard (per window station).
 *
 * Input goes to the active desktop of the main station. Keyboard messages are
 * posted to the focus window of the foreground thread (falling back to the
 * foreground window). Mouse messages go to the capture window when there is
 * one, otherwise to the window under the cursor, in client coordinates. A
 * low-level hook (WH_KEYBOARD_LL / WH_MOUSE_LL) that returns nonzero blocks the
 * event before it is queued.
 */
#include "w32k_int.h"
#include "w32k_evdev.h"
#include "w32k_clip.h"

static int32_t evdev_rel_dx;
static int32_t evdev_rel_dy;
static uint8_t clip_pool_storage[W32K_CLIP_POOL];
/* Scratch for synthesised formats (w32k_clip.c). Valid until the next synthesis call. */
static uint8_t clip_synth_text[W32K_CLIP_POOL];
static uint16_t clip_synth_wide[W32K_CLIP_POOL];

static w32k_station *main_station(void)
{
    return w32k_station_of(w32k_g.main_station);
}

static void clamp_to_primary(int32_t *x, int32_t *y)
{
    const w32k_monitor *m = &w32k_g.monitors[0];
    for (uint32_t i = 0; i < w32k_g.monitor_count; ++i) {
        if (w32k_g.monitors[i].primary) {
            m = &w32k_g.monitors[i];
            break;
        }
    }
    if (*x < m->rc.left)
        *x = m->rc.left;
    if (*y < m->rc.top)
        *y = m->rc.top;
    if (*x >= m->rc.right)
        *x = m->rc.right - 1;
    if (*y >= m->rc.bottom)
        *y = m->rc.bottom - 1;
}

/* Target window for a mouse message at (x, y), screen coordinates. */
static w32k_handle mouse_target(w32k_desktop *d, int32_t x, int32_t y)
{
    if (d->capture && w32k_window_checked(d->capture))
        return d->capture;
    return w32k_window_at_point(d, x, y);
}

static uint32_t mouse_key_state(const w32k_station *s)
{
    uint32_t mk = 0;
    if (s->mouse_buttons & 0x01)
        mk |= W32K_MK_LBUTTON;
    if (s->mouse_buttons & 0x02)
        mk |= W32K_MK_RBUTTON;
    if (s->mouse_buttons & 0x04)
        mk |= W32K_MK_MBUTTON;
    if (s->key_down[W32K_VK_SHIFT])
        mk |= W32K_MK_SHIFT;
    if (s->key_down[W32K_VK_CONTROL])
        mk |= W32K_MK_CONTROL;
    return mk;
}

static void post_mouse(w32k_handle target, uint32_t msg, uintptr_t wp, int32_t sx, int32_t sy,
                       int wheel)
{
    w32k_window *w = w32k_window_of(target);
    intptr_t lp;
    int32_t cx = sx, cy = sy;
    if (!w)
        return;
    if (wheel) {
        lp = (intptr_t)(((uint32_t)(uint16_t)sy << 16) | ((uint32_t)(uint16_t)sx));
    } else {
        cx = sx - w->rect.left;
        cy = sy - w->rect.top;
        lp = (intptr_t)(((uint32_t)(uint16_t)cy << 16) | ((uint32_t)(uint16_t)cx));
    }
    w32k_post_message(target, msg, wp, lp);
}

static void run_mouse_ll(uint32_t msg, int32_t x, int32_t y, w32k_handle desk, int *blocked)
{
    struct {
        int32_t x, y;
        uint32_t msg;
    } info;
    info.x = x;
    info.y = y;
    info.msg = msg;
    *blocked = w32k_run_hooks(W32K_WH_MOUSE_LL, (int)msg, (uintptr_t)msg, (intptr_t)&info, desk);
}

void w32k_inject_mouse_move(int32_t x, int32_t y)
{
    w32k_station *s = main_station();
    w32k_desktop *d = s ? w32k_desktop_of(s->active_desktop) : NULL;
    int blocked = 0;
    w32k_handle target;
    if (!s || !d)
        return;
    clamp_to_primary(&x, &y);
    run_mouse_ll(W32K_WM_MOUSEMOVE, x, y, s->active_desktop, &blocked);
    if (blocked)
        return;
    s->cursor_x = x;
    s->cursor_y = y;
    target = mouse_target(d, x, y);
    if (target)
        post_mouse(target, W32K_WM_MOUSEMOVE, mouse_key_state(s), x, y, 0);
}

void w32k_inject_mouse_button(uint32_t vk, int down, int32_t x, int32_t y)
{
    w32k_station *s = main_station();
    w32k_desktop *d = s ? w32k_desktop_of(s->active_desktop) : NULL;
    uint32_t msg;
    uint8_t bit = 0;
    int blocked = 0;
    w32k_handle target;
    if (!s || !d)
        return;
    switch (vk) {
    case W32K_VK_LBUTTON:
        msg = down ? W32K_WM_LBUTTONDOWN : W32K_WM_LBUTTONUP;
        bit = 0x01;
        break;
    case W32K_VK_RBUTTON:
        msg = down ? W32K_WM_RBUTTONDOWN : W32K_WM_RBUTTONUP;
        bit = 0x02;
        break;
    case W32K_VK_MBUTTON:
        msg = down ? W32K_WM_MBUTTONDOWN : W32K_WM_MBUTTONUP;
        bit = 0x04;
        break;
    default:
        return;
    }
    clamp_to_primary(&x, &y);
    run_mouse_ll(msg, x, y, s->active_desktop, &blocked);
    if (blocked)
        return;
    s->cursor_x = x;
    s->cursor_y = y;
    if (down)
        s->mouse_buttons |= bit;
    else
        s->mouse_buttons &= (uint8_t)~bit;
    s->key_down[vk] = (uint8_t)(down != 0);
    if (down)
        s->key_pressed[vk] = 1;
    target = mouse_target(d, x, y);
    if (target)
        post_mouse(target, msg, mouse_key_state(s), x, y, 0);
}

void w32k_inject_wheel(int32_t delta, int32_t x, int32_t y)
{
    w32k_station *s = main_station();
    w32k_desktop *d = s ? w32k_desktop_of(s->active_desktop) : NULL;
    int blocked = 0;
    w32k_handle target;
    uintptr_t wp;
    if (!s || !d)
        return;
    clamp_to_primary(&x, &y);
    run_mouse_ll(W32K_WM_MOUSEWHEEL, x, y, s->active_desktop, &blocked);
    if (blocked)
        return;
    target = mouse_target(d, x, y);
    if (!target)
        return;
    wp = (uintptr_t)(((uint32_t)(uint16_t)(int16_t)delta << 16) | mouse_key_state(s));
    post_mouse(target, W32K_WM_MOUSEWHEEL, wp, x, y, 1);
}

void w32k_inject_key(uint32_t vk, uint32_t scan, int down)
{
    w32k_station *s = main_station();
    w32k_desktop *d;
    w32k_queue *fq;
    w32k_handle target;
    uint32_t msg;
    intptr_t lp;
    uint8_t was_down;
    int blocked = 0;
    struct {
        uint32_t vk, scan;
        int down;
    } info;
    if (!s || vk >= 256)
        return;
    d = w32k_desktop_of(s->active_desktop);
    if (!d)
        return;
    was_down = s->key_down[vk];
    info.vk = vk;
    info.scan = scan;
    info.down = down;
    blocked = w32k_run_hooks(W32K_WH_KEYBOARD_LL, down ? W32K_WM_KEYDOWN : W32K_WM_KEYUP,
                             (uintptr_t)vk, (intptr_t)&info, s->active_desktop);
    if (blocked)
        return;
    if (down) {
        s->key_down[vk] = 1;
        s->key_pressed[vk] = 1;
    } else {
        s->key_down[vk] = 0;
    }
    fq = NULL;
    if (d->foreground) {
        w32k_window *fw = w32k_window_checked(d->foreground);
        if (fw)
            fq = w32k_queue_for(fw->tid, 0);
    }
    target = 0;
    if (fq && fq->focus && w32k_window_checked(fq->focus))
        target = fq->focus;
    else if (d->foreground && w32k_window_checked(d->foreground))
        target = d->foreground;
    if (!target)
        return;
    {
        int sys = s->key_down[W32K_VK_MENU] != 0 || vk == W32K_VK_MENU;
        msg = down ? (sys ? W32K_WM_SYSKEYDOWN : W32K_WM_KEYDOWN)
                   : (sys ? W32K_WM_SYSKEYUP : W32K_WM_KEYUP);
        lp = (intptr_t)(((uint32_t)(scan & 0xFFu) << 16) | 1u);
        if (down && was_down)
            lp |= (intptr_t)(1u << 30);
        if (!down)
            lp |= (intptr_t)(1u << 31);
        if (sys)
            lp |= (intptr_t)(1u << 29);
    }
    w32k_post_message(target, msg, vk, lp);
}

uint16_t w32k_get_async_key_state(uint32_t vk)
{
    w32k_station *s = main_station();
    uint16_t r = 0;
    if (!s || vk >= 256)
        return 0;
    if (s->key_down[vk])
        r |= 0x8000u;
    if (s->key_pressed[vk]) {
        r |= 0x0001u;
        s->key_pressed[vk] = 0;
    }
    return r;
}

uint16_t w32k_get_key_state(uint32_t vk)
{
    w32k_station *s = main_station();
    if (!s || vk >= 256)
        return 0;
    return s->key_down[vk] ? 0x8000u : 0;
}

int w32k_get_cursor_pos(w32k_point *pt)
{
    w32k_station *s = main_station();
    if (!s || !pt)
        return 0;
    pt->x = s->cursor_x;
    pt->y = s->cursor_y;
    return 1;
}

int w32k_set_cursor_pos(int32_t x, int32_t y)
{
    w32k_station *s = main_station();
    if (!s)
        return 0;
    clamp_to_primary(&x, &y);
    s->cursor_x = x;
    s->cursor_y = y;
    return 1;
}

/* Decodes evdev records. Relative motion accumulates until SYN_REPORT. */
size_t w32k_evdev_feed(const uint8_t *buf, size_t len)
{
    size_t n = 0;
    w32k_station *s = main_station();
    for (size_t off = 0; off + W32K_EVDEV_RECORD_SIZE <= len; off += W32K_EVDEV_RECORD_SIZE) {
        w32k_evdev_ev ev;
        w32k_evdev_decode(buf + off, &ev);
        ++n;
        switch (ev.type) {
        case W32K_EV_KEY:
            if (ev.code == W32K_BTN_LEFT || ev.code == W32K_BTN_RIGHT || ev.code == W32K_BTN_MIDDLE) {
                uint32_t vk = ev.code == W32K_BTN_LEFT    ? W32K_VK_LBUTTON
                              : ev.code == W32K_BTN_RIGHT ? W32K_VK_RBUTTON
                                                          : W32K_VK_MBUTTON;
                if (s)
                    w32k_inject_mouse_button(vk, ev.value != 0, s->cursor_x, s->cursor_y);
            } else {
                uint32_t vk = w32k_evdev_key_to_vk(ev.code);
                /* value 0 = release, 1 = press, 2 = autorepeat (reported as a down). */
                if (vk)
                    w32k_inject_key(vk, ev.code, ev.value != 0);
            }
            break;
        case W32K_EV_REL:
            if (ev.code == W32K_REL_X)
                evdev_rel_dx += ev.value;
            else if (ev.code == W32K_REL_Y)
                evdev_rel_dy += ev.value;
            else if (ev.code == W32K_REL_WHEEL && s)
                w32k_inject_wheel(ev.value * 120, s->cursor_x, s->cursor_y);
            break;
        case W32K_EV_SYN:
            if (ev.code == W32K_SYN_REPORT && s && (evdev_rel_dx || evdev_rel_dy)) {
                w32k_inject_mouse_move(s->cursor_x + evdev_rel_dx, s->cursor_y + evdev_rel_dy);
                evdev_rel_dx = 0;
                evdev_rel_dy = 0;
            }
            break;
        default:
            break;
        }
    }
    return n;
}

/* ---- Clipboard -------------------------------------------------------------- */

void w32k_input_init(void)
{
    w32k_station *s = main_station();
    evdev_rel_dx = 0;
    evdev_rel_dy = 0;
    if (s) {
        s->clip_pool = clip_pool_storage;
        s->clip_pool_used = 0;
    }
}

static w32k_station *clip_station(void)
{
    w32k_station *s = main_station();
    return s && s->clip_pool ? s : NULL;
}

int w32k_open_clipboard(w32k_handle hwnd)
{
    w32k_station *s = clip_station();
    if (!s) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (s->clip_open && s->clip_open_hwnd != hwnd) {
        w32k_set_error(W32K_ERROR_ACCESS_DENIED);
        return 0;
    }
    s->clip_open = 1;
    s->clip_open_hwnd = hwnd;
    return 1;
}

int w32k_close_clipboard(void)
{
    w32k_station *s = clip_station();
    if (!s || !s->clip_open) {
        w32k_set_error(W32K_ERROR_CLIPBOARD_NOT_OPEN);
        return 0;
    }
    s->clip_open = 0;
    s->clip_open_hwnd = 0;
    return 1;
}

int w32k_empty_clipboard(void)
{
    w32k_station *s = clip_station();
    if (!s || !s->clip_open) {
        w32k_set_error(W32K_ERROR_CLIPBOARD_NOT_OPEN);
        return 0;
    }
    for (int i = 0; i < W32K_CLIP_FORMATS; ++i)
        s->clip_entries[i].used = 0;
    s->clip_pool_used = 0;
    s->clip_formats_used = 0;
    s->clip_owner = s->clip_open_hwnd;
    return 1;
}

int w32k_set_clipboard_data(uint32_t format, const void *data, size_t size)
{
    w32k_station *s = clip_station();
    int slot = -1;
    if (!s || !s->clip_open) {
        w32k_set_error(W32K_ERROR_CLIPBOARD_NOT_OPEN);
        return 0;
    }
    if (format == 0 || (size > 0 && !data) || size > W32K_CLIP_POOL) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (int i = 0; i < W32K_CLIP_FORMATS; ++i) {
        if (s->clip_entries[i].used && s->clip_entries[i].format == format) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < W32K_CLIP_FORMATS; ++i) {
            if (!s->clip_entries[i].used) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_QUOTA);
        return 0;
    }
    /* Replacing a format needs fresh pool space; the old bytes are not reclaimed. */
    if (s->clip_pool_used + size > W32K_CLIP_POOL) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (size)
        w32k_copy(s->clip_pool + s->clip_pool_used, data, size);
    s->clip_entries[slot].format = format;
    s->clip_entries[slot].offset = s->clip_pool_used;
    s->clip_entries[slot].size = (uint32_t)size;
    s->clip_entries[slot].used = 1;
    s->clip_pool_used += (uint32_t)size;
    return 1;
}

static uint32_t clip_synth_source(w32k_station *s, uint32_t format);

/* Converts the stored source format into the scratch buffers. Returns the synthesised
 * data, or NULL when the source is missing or too large. */
static const void *clip_synthesize(w32k_station *s, uint32_t format, size_t *size)
{
    uint32_t src_fmt = clip_synth_source(s, format);
    int src_idx = -1;
    size_t wrote;
    const uint8_t *src;
    size_t sz;
    if (!src_fmt)
        return NULL;
    for (int i = 0; i < W32K_CLIP_FORMATS; ++i) {
        if (s->clip_entries[i].used && s->clip_entries[i].format == src_fmt) {
            src_idx = i;
            break;
        }
    }
    if (src_idx < 0)
        return NULL;
    src = s->clip_pool + s->clip_entries[src_idx].offset;
    sz = s->clip_entries[src_idx].size;
    if (src_fmt == W32K_CF_UNICODETEXT) {
        /* UTF-16 source to CF_TEXT or CF_OEMTEXT bytes. The pool is not aligned for
         * 16-bit reads, so the units are copied out first. */
        size_t units = sz / 2u;
        size_t i;
        if (units > sizeof(clip_synth_wide) / sizeof(clip_synth_wide[0]))
            units = sizeof(clip_synth_wide) / sizeof(clip_synth_wide[0]);
        for (i = 0; i < units; ++i)
            clip_synth_wide[i] = (uint16_t)(src[2 * i] | ((uint16_t)src[2 * i + 1] << 8));
        wrote = w32k_clip_wide_to_text(clip_synth_wide, units, clip_synth_text, sizeof(clip_synth_text),
                                       format == W32K_CF_OEMTEXT);
        if (!wrote) {
            w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        if (size)
            *size = wrote;
        return clip_synth_text;
    }
    /* CF_TEXT or CF_OEMTEXT source to UTF-16. */
    wrote = w32k_clip_text_to_wide(src, sz, clip_synth_wide, sizeof(clip_synth_wide) / sizeof(clip_synth_wide[0]));
    if (!wrote) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (size)
        *size = wrote * 2u;
    return clip_synth_wide;
}

const void *w32k_get_clipboard_data(uint32_t format, size_t *size)
{
    w32k_station *s = clip_station();
    if (!s || !s->clip_open) {
        w32k_set_error(W32K_ERROR_CLIPBOARD_NOT_OPEN);
        return NULL;
    }
    for (int i = 0; i < W32K_CLIP_FORMATS; ++i) {
        if (s->clip_entries[i].used && s->clip_entries[i].format == format) {
            if (size)
                *size = s->clip_entries[i].size;
            return s->clip_pool + s->clip_entries[i].offset;
        }
    }
    return clip_synthesize(s, format, size);
}

static int clip_has(w32k_station *s, uint32_t format)
{
    for (int i = 0; i < W32K_CLIP_FORMATS; ++i) {
        if (s->clip_entries[i].used && s->clip_entries[i].format == format)
            return 1;
    }
    return 0;
}

/* The format a synthesised one is converted from, or 0. CF_TEXT and CF_OEMTEXT come
 * from CF_UNICODETEXT; CF_UNICODETEXT comes from CF_TEXT, or from CF_OEMTEXT when there
 * is no CF_TEXT. */
static uint32_t clip_synth_source(w32k_station *s, uint32_t format)
{
    if (format == W32K_CF_TEXT || format == W32K_CF_OEMTEXT)
        return clip_has(s, W32K_CF_UNICODETEXT) ? W32K_CF_UNICODETEXT : 0;
    if (format == W32K_CF_UNICODETEXT) {
        if (clip_has(s, W32K_CF_TEXT))
            return W32K_CF_TEXT;
        if (clip_has(s, W32K_CF_OEMTEXT))
            return W32K_CF_OEMTEXT;
    }
    return 0;
}

int w32k_is_clipboard_format_available(uint32_t format)
{
    w32k_station *s = clip_station();
    if (!s)
        return 0;
    return clip_has(s, format) || clip_synth_source(s, format) != 0;
}

int w32k_count_clipboard_formats(void)
{
    w32k_station *s = clip_station();
    int n = 0;
    if (!s)
        return 0;
    for (int i = 0; i < W32K_CLIP_FORMATS; ++i) {
        if (s->clip_entries[i].used)
            ++n;
    }
    return n;
}

uint32_t w32k_enum_clipboard_formats(uint32_t prev)
{
    w32k_station *s = clip_station();
    int start = 0;
    if (!s)
        return 0;
    if (prev) {
        int found = -1;
        for (int i = 0; i < W32K_CLIP_FORMATS; ++i) {
            if (s->clip_entries[i].used && s->clip_entries[i].format == prev) {
                found = i;
                break;
            }
        }
        if (found < 0)
            return 0;
        start = found + 1;
    }
    for (int i = start; i < W32K_CLIP_FORMATS; ++i) {
        if (s->clip_entries[i].used)
            return s->clip_entries[i].format;
    }
    return 0;
}

uint32_t w32k_register_clipboard_format(const uint16_t *name)
{
    size_t n = w32k_wlen(name, 64);
    if (!name || n == 0 || n >= 64) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (uint32_t i = 0; i < w32k_g.registered_format_count; ++i) {
        if (w32k_wequal_ci(w32k_g.registered_formats[i], name))
            return 0xC000u + i;
    }
    if (w32k_g.registered_format_count >= W32K_CLIP_REGISTERED_MAX) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_QUOTA);
        return 0;
    }
    w32k_wcopy(w32k_g.registered_formats[w32k_g.registered_format_count], name, 64);
    return 0xC000u + w32k_g.registered_format_count++;
}

w32k_handle w32k_get_clipboard_owner(void)
{
    w32k_station *s = clip_station();
    return s ? s->clip_owner : 0;
}
