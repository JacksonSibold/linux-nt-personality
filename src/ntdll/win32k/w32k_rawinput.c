/* w32k_rawinput.c - raw input (WM_INPUT): device registration, the event ring and the
 * RAWINPUT byte layout of the x64 headers.
 *
 * Events are produced by the input layer (w32k_input.c) as it injects mouse and keyboard
 * input. A registration names a usage page and usage (mouse 1/2, keyboard 1/6) and a target
 * window. WM_INPUT goes to that window when its thread owns the foreground window (wParam
 * RIM_INPUT), or with RIDEV_INPUTSINK also when it does not (RIM_INPUTSINK). A registration
 * with RIDEV_NOLEGACY stops the legacy messages for that device.
 */
#include "w32k_int.h"

#define RAW_MAX_REGS 8
#define RAW_RING 256
#define RAW_TYPE_TAG 0x7Au

static w32k_rid_dev regs[RAW_MAX_REGS];
static uint32_t reg_count;

typedef struct raw_event {
    uint32_t serial;
    uint32_t size;
    uint8_t bytes[W32K_RAWINPUT_MOUSE_SIZE];
} raw_event;

static raw_event ring[RAW_RING];
static uint32_t next_serial = 1;

void w32k_rawinput_reset(void)
{
    w32k_zero(regs, sizeof(regs));
    w32k_zero(ring, sizeof(ring));
    reg_count = 0;
    next_serial = 1;
}

static void st32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
    p[off + 2] = (uint8_t)(v >> 16);
    p[off + 3] = (uint8_t)(v >> 24);
}

static void st16(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
}

static void st64(uint8_t *p, uint32_t off, uint64_t v)
{
    st32(p, off, (uint32_t)v);
    st32(p, off + 4, (uint32_t)(v >> 32));
}

/* RIDEV_EXCLUDE (0x10) alone; with RIDEV_PAGEONLY (0x20) the two bits make RIDEV_NOLEGACY (0x30). */
static int is_exclude(uint32_t flags)
{
    return (flags & 0x30u) == W32K_RIDEV_EXCLUDE;
}

static int find_reg(uint16_t page, uint16_t usage)
{
    for (uint32_t i = 0; i < reg_count; ++i) {
        if (regs[i].usage_page == page && regs[i].usage == usage)
            return (int)i;
    }
    return -1;
}

int w32k_register_raw_input(const w32k_rid_dev *devs, uint32_t n)
{
    w32k_rid_dev work[RAW_MAX_REGS];
    uint32_t count = reg_count;
    if (!devs || n == 0) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* Validate every entry first so a bad one leaves the table unchanged. */
    for (uint32_t i = 0; i < n; ++i) {
        if (devs[i].flags & W32K_RIDEV_REMOVE) {
            if (devs[i].target) {
                w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
                return 0;
            }
        } else if (devs[i].target && !w32k_window_checked(devs[i].target)) {
            w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        if (is_exclude(devs[i].flags) && devs[i].target) {
            w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    for (uint32_t i = 0; i < count; ++i)
        work[i] = regs[i];
    for (uint32_t i = 0; i < n; ++i) {
        int at = -1;
        for (uint32_t k = 0; k < count; ++k) {
            if (work[k].usage_page == devs[i].usage_page && work[k].usage == devs[i].usage)
                at = (int)k;
        }
        if (devs[i].flags & W32K_RIDEV_REMOVE) {
            if (at >= 0) {
                for (uint32_t k = (uint32_t)at; k + 1 < count; ++k)
                    work[k] = work[k + 1];
                --count;
            }
            continue;
        }
        if (at < 0) {
            if (count == RAW_MAX_REGS) {
                w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            at = (int)count++;
        }
        work[at] = devs[i];
    }
    for (uint32_t i = 0; i < count; ++i)
        regs[i] = work[i];
    reg_count = count;
    return 1;
}

uint32_t w32k_get_registered_raw_input(w32k_rid_dev *out, uint32_t cap)
{
    if (out) {
        for (uint32_t i = 0; i < reg_count && i < cap; ++i)
            out[i] = regs[i];
    }
    return reg_count;
}

/* The registration for a device class, or NULL. */
static const w32k_rid_dev *reg_for(int keyboard)
{
    int i = find_reg(1, keyboard ? 6 : 2);
    if (i < 0)
        return NULL;
    return &regs[i];
}

int w32k_raw_nolegacy(int keyboard)
{
    const w32k_rid_dev *r = reg_for(keyboard);
    return r && (r->flags & W32K_RIDEV_NOLEGACY) == W32K_RIDEV_NOLEGACY;
}

static w32k_handle live_target(const w32k_rid_dev *r)
{
    w32k_desktop *d = w32k_cur_desktop();
    w32k_station *s = w32k_station_of(w32k_g.main_station);
    if (r->target && w32k_window_checked(r->target))
        return r->target;
    /* No explicit target: the focus window of the foreground thread. */
    d = s ? w32k_desktop_of(s->active_desktop) : d;
    if (d && d->foreground) {
        w32k_window *fw = w32k_window_checked(d->foreground);
        w32k_queue *fq = fw ? w32k_queue_for(fw->tid, 0) : NULL;
        if (fq && fq->focus && w32k_window_checked(fq->focus))
            return fq->focus;
        return d->foreground;
    }
    return 0;
}

/* Stores an event and posts WM_INPUT for it. */
static void deliver(int keyboard, const uint8_t *bytes, uint32_t size)
{
    const w32k_rid_dev *r = reg_for(keyboard);
    w32k_handle target, hri;
    w32k_window *tw;
    w32k_station *s = w32k_station_of(w32k_g.main_station);
    w32k_desktop *d = s ? w32k_desktop_of(s->active_desktop) : NULL;
    w32k_window *fg;
    int foreground;
    raw_event *e;
    if (!r || is_exclude(r->flags))
        return;
    target = live_target(r);
    tw = target ? w32k_window_checked(target) : NULL;
    if (!tw)
        return;
    fg = d && d->foreground ? w32k_window_checked(d->foreground) : NULL;
    foreground = fg && fg->tid == tw->tid;
    if (!foreground && !(r->flags & W32K_RIDEV_INPUTSINK))
        return;
    e = &ring[next_serial % RAW_RING];
    w32k_zero(e, sizeof(*e));
    w32k_copy(e->bytes, bytes, size);
    e->size = size;
    e->serial = next_serial;
    hri = ((w32k_handle)RAW_TYPE_TAG << 24) | (w32k_handle)next_serial;
    if (++next_serial > 0xFFFFFFu)
        next_serial = 1; /* the handle carries 24 bits of the serial */
    /* The header's wParam field repeats the message wParam. */
    st64(e->bytes, 16, foreground ? W32K_RIM_INPUT : W32K_RIM_INPUTSINK);
    w32k_post_message(target, W32K_WM_INPUT, foreground ? W32K_RIM_INPUT : W32K_RIM_INPUTSINK, (intptr_t)hri);
}

void w32k_inject_raw_mouse(int32_t dx, int32_t dy, uint16_t button_flags, int16_t wheel)
{
    uint8_t b[W32K_RAWINPUT_MOUSE_SIZE];
    if (!reg_for(0))
        return;
    w32k_zero(b, sizeof(b));
    st32(b, 0, W32K_RIM_TYPEMOUSE);
    st32(b, 4, W32K_RAWINPUT_MOUSE_SIZE);
    st64(b, 8, W32K_RAW_MOUSE_DEVICE);
    /* RAWMOUSE at 24: usFlags (MOUSE_MOVE_RELATIVE = 0), pad, usButtonFlags, usButtonData,
     * ulRawButtons, lLastX, lLastY, ulExtraInformation. */
    st16(b, 24, 0);
    st16(b, 28, button_flags);
    st16(b, 30, (uint16_t)wheel);
    st32(b, 36, (uint32_t)dx);
    st32(b, 40, (uint32_t)dy);
    deliver(0, b, W32K_RAWINPUT_MOUSE_SIZE);
}

void w32k_inject_raw_key(uint16_t make_code, uint16_t vkey, int down, int extended)
{
    uint8_t b[W32K_RAWINPUT_MOUSE_SIZE];
    if (!reg_for(1))
        return;
    w32k_zero(b, sizeof(b));
    st32(b, 0, W32K_RIM_TYPEKEYBOARD);
    st32(b, 4, W32K_RAWINPUT_KEYBOARD_SIZE);
    st64(b, 8, W32K_RAW_KEYBOARD_DEVICE);
    /* RAWKEYBOARD at 24: MakeCode, Flags (RI_KEY_BREAK 1, RI_KEY_E0 2), Reserved, VKey, Message,
     * ExtraInformation. */
    st16(b, 24, make_code);
    st16(b, 26, (uint32_t)((down ? 0 : 1) | (extended ? 2 : 0)));
    st16(b, 30, vkey);
    st32(b, 32, down ? W32K_WM_KEYDOWN : W32K_WM_KEYUP);
    deliver(1, b, W32K_RAWINPUT_KEYBOARD_SIZE);
}

const uint8_t *w32k_raw_input_bytes(w32k_handle hri, uint32_t *size)
{
    uint32_t serial = (uint32_t)(hri & 0xFFFFFFu);
    raw_event *e;
    if ((hri >> 24) != RAW_TYPE_TAG || serial == 0)
        return NULL;
    e = &ring[serial % RAW_RING];
    if (e->serial != serial)
        return NULL;
    if (size)
        *size = e->size;
    return e->bytes;
}
