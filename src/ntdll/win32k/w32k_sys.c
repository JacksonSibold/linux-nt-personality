/* w32k_sys.c - model initialisation, system metrics and parameters, monitors,
 * display modes, and cursor and icon objects.
 *
 * The metric and parameter defaults are the model's configured values. They are
 * not sampled from a Windows system. Monitor 0 is the primary display; the
 * virtual screen is the bounding box of all active monitors.
 */
#include "w32k_int.h"

static const uint16_t winsta0_name[] = {'W', 'i', 'n', 'S', 't', 'a', '0', 0};
static const uint16_t default_desktop_name[] = {'D', 'e', 'f', 'a', 'u', 'l', 't', 0};

/* Stock cursor ids in the order of w32k_g.stock_cursors. */
static const uintptr_t stock_cursor_ids[W32K_STOCK_CURSORS] = {
    W32K_IDC_ARROW, W32K_IDC_IBEAM, W32K_IDC_WAIT, W32K_IDC_CROSS, W32K_IDC_HAND};

static const w32k_devmode default_modes[W32K_MAX_MODES] = {
    {1024, 768, 32, 60},
    {1280, 720, 32, 60},
    {1920, 1080, 32, 60},
    {800, 600, 32, 60},
};

static const w32k_monitor *primary_monitor(void)
{
    for (uint32_t i = 0; i < W32K_MAX_MONITORS; ++i) {
        if (w32k_g.monitors[i].active && w32k_g.monitors[i].primary)
            return &w32k_g.monitors[i];
    }
    for (uint32_t i = 0; i < W32K_MAX_MONITORS; ++i) {
        if (w32k_g.monitors[i].active)
            return &w32k_g.monitors[i];
    }
    return &w32k_g.monitors[0];
}

static uint32_t active_monitor_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < W32K_MAX_MONITORS; ++i) {
        if (w32k_g.monitors[i].active)
            ++n;
    }
    return n;
}

void w32k_init(void)
{
    w32k_core_reset();
    w32k_zero(&w32k_g, sizeof(w32k_g));
    w32k_g.cur_tid = 1;

    /* Primary monitor: 1920x1080 at 96 dpi. */
    w32k_g.monitors[0].rc.left = 0;
    w32k_g.monitors[0].rc.top = 0;
    w32k_g.monitors[0].rc.right = 1920;
    w32k_g.monitors[0].rc.bottom = 1080;
    w32k_g.monitors[0].dpi = 96;
    w32k_g.monitors[0].primary = 1;
    w32k_g.monitors[0].active = 1;
    w32k_g.monitor_count = 1;

    for (uint32_t i = 0; i < W32K_MAX_MODES; ++i)
        w32k_g.display.modes[i] = default_modes[i];
    w32k_g.display.mode_count = W32K_MAX_MODES;
    w32k_g.display.current = default_modes[2];

    w32k_g.beep_enabled = 1;
    w32k_g.double_click_ms = 500;
    w32k_g.key_repeat_speed = 31;
    w32k_g.mouse_speed = 10;

    w32k_gdi_init();

    w32k_g.main_station = w32k_create_winstation(winsta0_name);
    w32k_g.default_desktop = w32k_create_desktop(w32k_g.main_station, default_desktop_name);

    for (int i = 0; i < W32K_STOCK_CURSORS; ++i) {
        w32k_g.stock_cursors[i] = w32k_load_cursor(stock_cursor_ids[i]);
    }
    w32k_g.stock_icons[0] = w32k_load_icon(W32K_IDI_APPLICATION);

    w32k_input_init();
    w32k_attach_thread(1);
    w32k_g.cur_tid = 1;
}

/* ---- System metrics and parameters ------------------------------------------ */

int32_t w32k_get_system_metrics(int index)
{
    const w32k_monitor *p = primary_monitor();
    int32_t pw = p->rc.right - p->rc.left;
    int32_t ph = p->rc.bottom - p->rc.top;
    int32_t vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
    int first = 1;
    for (uint32_t i = 0; i < W32K_MAX_MONITORS; ++i) {
        const w32k_monitor *m = &w32k_g.monitors[i];
        if (!m->active)
            continue;
        if (first || m->rc.left < vx0)
            vx0 = m->rc.left;
        if (first || m->rc.top < vy0)
            vy0 = m->rc.top;
        if (first || m->rc.right > vx1)
            vx1 = m->rc.right;
        if (first || m->rc.bottom > vy1)
            vy1 = m->rc.bottom;
        first = 0;
    }
    switch (index) {
    case W32K_SM_CXSCREEN: return pw;
    case W32K_SM_CYSCREEN: return ph;
    case W32K_SM_CXVSCROLL: return 17;
    case W32K_SM_CYHSCROLL: return 17;
    case W32K_SM_CYCAPTION: return 23;
    case W32K_SM_CXBORDER: return 1;
    case W32K_SM_CYBORDER: return 1;
    case W32K_SM_CXICON: return 32;
    case W32K_SM_CYICON: return 32;
    case W32K_SM_CXCURSOR: return 32;
    case W32K_SM_CYCURSOR: return 32;
    case W32K_SM_CYMENU: return 19;
    case W32K_SM_CXFULLSCREEN: return pw;
    case W32K_SM_CYFULLSCREEN: return ph - 23;
    case W32K_SM_MOUSEPRESENT: return 1;
    case 20: return 17;                 /* SM_CYVSCROLL */
    case 21: return 17;                 /* SM_CXHSCROLL */
    case W32K_SM_CXDOUBLECLK: return 4;
    case W32K_SM_CYDOUBLECLK: return 4;
    case W32K_SM_CMOUSEBUTTONS: return 3;
    case W32K_SM_XVIRTUALSCREEN: return vx0;
    case W32K_SM_YVIRTUALSCREEN: return vy0;
    case W32K_SM_CXVIRTUALSCREEN: return vx1 - vx0;
    case W32K_SM_CYVIRTUALSCREEN: return vy1 - vy0;
    case W32K_SM_CMONITORS: return (int32_t)active_monitor_count();
    case W32K_SM_SAMEDISPLAYFORMAT: return 1;
    default: return 0;
    }
}

int w32k_system_parameters_info(uint32_t action, uint32_t ui_param, void *pv)
{
    switch (action) {
    case W32K_SPI_GETWORKAREA:
        if (!pv)
            break;
        *(w32k_rect *)pv = primary_monitor()->rc;
        return 1;
    case W32K_SPI_GETBEEP:
        if (!pv)
            break;
        *(int *)pv = w32k_g.beep_enabled;
        return 1;
    case W32K_SPI_SETBEEP:
        w32k_g.beep_enabled = ui_param != 0;
        return 1;
    case W32K_SPI_GETSCREENSAVEACTIVE:
        if (!pv)
            break;
        *(int *)pv = w32k_g.screen_saver_active;
        return 1;
    case W32K_SPI_SETSCREENSAVEACTIVE:
        /* The BOOL is passed in pvParam, as in Win32. */
        w32k_g.screen_saver_active = (uintptr_t)pv != 0;
        return 1;
    case W32K_SPI_GETKEYBOARDSPEED:
        if (!pv)
            break;
        *(uint32_t *)pv = w32k_g.key_repeat_speed;
        return 1;
    case W32K_SPI_GETMOUSESPEED:
        if (!pv)
            break;
        *(uint32_t *)pv = w32k_g.mouse_speed;
        return 1;
    default:
        break;
    }
    w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
    return 0;
}

/* ---- Monitors --------------------------------------------------------------- */

int w32k_set_monitor(uint32_t index, const w32k_rect *rc, uint32_t dpi, int primary)
{
    if (index >= W32K_MAX_MONITORS || !rc || w32k_rect_empty(rc)) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (primary) {
        for (uint32_t i = 0; i < W32K_MAX_MONITORS; ++i)
            w32k_g.monitors[i].primary = 0;
    }
    w32k_g.monitors[index].rc = *rc;
    w32k_g.monitors[index].dpi = dpi ? dpi : 96;
    w32k_g.monitors[index].primary = primary != 0;
    w32k_g.monitors[index].active = 1;
    w32k_g.monitor_count = active_monitor_count();
    return 1;
}

int w32k_enum_display_monitors(w32k_monitorenumproc proc, intptr_t lparam)
{
    for (uint32_t i = 0; i < W32K_MAX_MONITORS; ++i) {
        const w32k_monitor *m = &w32k_g.monitors[i];
        if (!m->active)
            continue;
        if (!proc((w32k_handle)(i + 1u), &m->rc, lparam))
            return 0;
    }
    return 1;
}

static uint32_t monitor_index_for_rect(const w32k_rect *rc)
{
    int32_t cx = (rc->left + rc->right) / 2;
    int32_t cy = (rc->top + rc->bottom) / 2;
    for (uint32_t i = 0; i < W32K_MAX_MONITORS; ++i) {
        const w32k_monitor *m = &w32k_g.monitors[i];
        if (m->active && w32k_rect_contains(&m->rc, cx, cy))
            return i;
    }
    for (uint32_t i = 0; i < W32K_MAX_MONITORS; ++i) {
        if (w32k_g.monitors[i].active && w32k_g.monitors[i].primary)
            return i;
    }
    return 0;
}

w32k_handle w32k_monitor_from_window(w32k_handle hwnd)
{
    w32k_window *w = w32k_window_checked(hwnd);
    if (!w)
        return 0;
    return monitor_index_for_rect(&w->rect) + 1u;
}

uint32_t w32k_dpi_for_rect(const w32k_rect *rc)
{
    return w32k_g.monitors[monitor_index_for_rect(rc)].dpi;
}

/* ---- Display settings ------------------------------------------------------- */

int w32k_enum_display_settings(uint32_t index, w32k_devmode *out)
{
    if (!out)
        return 0;
    if (index == W32K_ENUM_CURRENT_SETTINGS || index == W32K_ENUM_REGISTRY_SETTINGS) {
        *out = w32k_g.display.current;
        return 1;
    }
    if (index >= w32k_g.display.mode_count)
        return 0;
    *out = w32k_g.display.modes[index];
    return 1;
}

void w32k_broadcast_displaychange(void)
{
    w32k_station *s = w32k_station_of(w32k_g.main_station);
    w32k_desktop *d = s ? w32k_desktop_of(s->active_desktop) : NULL;
    w32k_handle cur;
    if (!d)
        return;
    cur = d->top_head;
    while (cur) {
        w32k_window *w = w32k_window_of(cur);
        w32k_handle next;
        if (!w)
            break;
        next = w->next_sibling;
        if (!w->in_destroy)
            w32k_send_message(cur, W32K_WM_DISPLAYCHANGE, w32k_g.display.current.bpp,
                              (intptr_t)(((uint32_t)w32k_g.display.current.height << 16) |
                                         (w32k_g.display.current.width & 0xFFFFu)));
        cur = next;
    }
}

int w32k_change_display_settings(const w32k_devmode *mode, uint32_t flags)
{
    const w32k_devmode *want = mode;
    w32k_monitor *primary;
    uint32_t i;
    if (!want)
        want = &w32k_g.display.modes[2]; /* registry default is the 1920x1080 mode */
    for (i = 0; i < w32k_g.display.mode_count; ++i) {
        const w32k_devmode *m = &w32k_g.display.modes[i];
        if (m->width == want->width && m->height == want->height && m->bpp == want->bpp &&
            (want->freq == 0 || m->freq == want->freq))
            break;
    }
    if (i == w32k_g.display.mode_count)
        return W32K_DISP_CHANGE_BADMODE;
    if (flags & W32K_CDS_TEST)
        return W32K_DISP_CHANGE_SUCCESSFUL;
    w32k_g.display.current = w32k_g.display.modes[i];
    primary = (w32k_monitor *)primary_monitor();
    primary->rc.right = primary->rc.left + (int32_t)w32k_g.display.current.width;
    primary->rc.bottom = primary->rc.top + (int32_t)w32k_g.display.current.height;
    w32k_broadcast_displaychange();
    return W32K_DISP_CHANGE_SUCCESSFUL;
}

/* ---- Cursor and icon objects ------------------------------------------------ */

w32k_handle w32k_load_cursor(uintptr_t id)
{
    int idx = -1;
    w32k_handle h;
    w32k_cursor_obj *c;
    for (int i = 0; i < W32K_STOCK_CURSORS; ++i) {
        if (stock_cursor_ids[i] == id)
            idx = i;
    }
    if (idx < 0) {
        w32k_set_error(W32K_ERROR_RESOURCE_NAME_NOT_FOUND);
        return 0;
    }
    if (w32k_g.stock_cursors[idx])
        return w32k_g.stock_cursors[idx];
    c = (w32k_cursor_obj *)w32k_pool_alloc(&w32k_pool_cursors, &h);
    if (!c)
        return 0;
    c->w = 32;
    c->h = 32;
    c->stock = 1;
    w32k_g.stock_cursors[idx] = h;
    return h;
}

w32k_handle w32k_create_cursor(int32_t xhot, int32_t yhot, int32_t w, int32_t h)
{
    w32k_handle hh;
    w32k_cursor_obj *c;
    if (w <= 0 || h <= 0 || xhot < 0 || yhot < 0 || xhot >= w || yhot >= h) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    c = (w32k_cursor_obj *)w32k_pool_alloc(&w32k_pool_cursors, &hh);
    if (!c) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    c->xhot = xhot;
    c->yhot = yhot;
    c->w = w;
    c->h = h;
    return hh;
}

int w32k_destroy_cursor(w32k_handle cursor)
{
    w32k_cursor_obj *c = (w32k_cursor_obj *)w32k_pool_get(&w32k_pool_cursors, cursor);
    w32k_queue *q;
    if (!c) {
        w32k_set_error(W32K_ERROR_INVALID_CURSOR_HANDLE);
        return 0;
    }
    if (c->stock) {
        w32k_set_error(W32K_ERROR_ACCESS_DENIED);
        return 0;
    }
    q = w32k_cur_queue();
    if (q && q->cursor == cursor)
        q->cursor = 0;
    w32k_pool_free(&w32k_pool_cursors, c);
    return 1;
}

w32k_handle w32k_load_icon(uintptr_t id)
{
    w32k_handle h;
    w32k_icon_obj *ic;
    if (id != W32K_IDI_APPLICATION) {
        w32k_set_error(W32K_ERROR_RESOURCE_NAME_NOT_FOUND);
        return 0;
    }
    if (w32k_g.stock_icons[0])
        return w32k_g.stock_icons[0];
    ic = (w32k_icon_obj *)w32k_pool_alloc(&w32k_pool_icons, &h);
    if (!ic)
        return 0;
    ic->w = 32;
    ic->h = 32;
    ic->planes = 1;
    ic->bpp = 32;
    ic->stock = 1;
    w32k_g.stock_icons[0] = h;
    return h;
}

w32k_handle w32k_create_icon(int32_t w, int32_t h, uint32_t planes, uint32_t bpp)
{
    w32k_handle hh;
    w32k_icon_obj *ic;
    if (w <= 0 || h <= 0 || planes == 0 || bpp == 0) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    ic = (w32k_icon_obj *)w32k_pool_alloc(&w32k_pool_icons, &hh);
    if (!ic) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    ic->w = w;
    ic->h = h;
    ic->planes = planes;
    ic->bpp = bpp;
    return hh;
}

int w32k_destroy_icon(w32k_handle icon)
{
    w32k_icon_obj *ic = (w32k_icon_obj *)w32k_pool_get(&w32k_pool_icons, icon);
    if (!ic) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (ic->stock) {
        w32k_set_error(W32K_ERROR_ACCESS_DENIED);
        return 0;
    }
    w32k_pool_free(&w32k_pool_icons, ic);
    return 1;
}
