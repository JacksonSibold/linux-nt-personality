/* user32_win32k.c - user32.dll exports backed by the win32k model (src/ntdll/win32k).
 *
 * Each export converts the Win32 ABI arguments (HWND and friends are 64-bit,
 * BOOL is 32-bit) to the model's calls, and converts results back. The
 * structures that differ from the model's own are copied field by field
 * (PAINTSTRUCT, DEVMODEW, WNDCLASSEXW). MSG and POINT/RECT have the same layout
 * as the model's, so they are passed through.
 *
 * Not exported here, because the model has no equivalent yet: SetClipboardData
 * and GetClipboardData (they need global memory handles), the CreateFontW family
 * (the model has font handles but not the LOGFONT path), and the message-box
 * and window-class menu APIs. GetMessageW does not block: see the note at
 * GetMessageW.
 */
#ifdef __MINGW32__
#include "win32k/w32k_abi.h"
#include <stdint.h>
#include <stddef.h>

typedef int32_t BOOL_T;

static void *dm_field_ptr(void *p, size_t off)
{
    return (uint8_t *)p + off;
}

static uint32_t rd32(const void *p, size_t off)
{
    const uint8_t *b = (const uint8_t *)p + off;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void wr32(void *p, size_t off, uint32_t v)
{
    uint8_t *b = (uint8_t *)dm_field_ptr(p, off);
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

/* ---- Window classes and windows ---------------------------------------------- */

uint16_t RegisterClassExW(const W32K_WNDCLASSEXW *wc)
{
    w32k_wndclass m;
    if (!wc)
        return 0;
    m.name = wc->lpszClassName;
    m.style = wc->style;
    m.proc = wc->lpfnWndProc;
    m.cls_extra = wc->cbClsExtra;
    m.wnd_extra = wc->cbWndExtra;
    m.bg_brush = wc->hbrBackground;
    m.cursor = wc->hCursor;
    m.icon = wc->hIcon;
    m.menu_name = wc->lpszMenuName;
    m.hinst = wc->hInstance;
    return w32k_register_class(&m);
}

BOOL_T UnregisterClassW(const uint16_t *name, const void *hinst)
{
    return w32k_unregister_class((uintptr_t)name, hinst) != 0;
}

uintptr_t CreateWindowExW(uint32_t exstyle, const uint16_t *cls, const uint16_t *name, uint32_t style,
                          int32_t x, int32_t y, int32_t cx, int32_t cy, uintptr_t parent,
                          uintptr_t menu, const void *hinst, void *param)
{
    w32k_createwnd cs;
    cs.param = param;
    cs.hinst = hinst;
    cs.menu = menu;
    cs.parent = parent;
    cs.cy = cy;
    cs.cx = cx;
    cs.y = y;
    cs.x = x;
    cs.style = style;
    cs.name = name;
    cs.cls = (uintptr_t)cls;
    cs.exstyle = exstyle;
    return w32k_create_window(&cs);
}

BOOL_T DestroyWindow(uintptr_t hwnd)
{
    return w32k_destroy_window(hwnd) != 0;
}

BOOL_T ShowWindow(uintptr_t hwnd, int32_t cmd)
{
    return w32k_show_window(hwnd, cmd) != 0;
}

BOOL_T MoveWindow(uintptr_t hwnd, int32_t x, int32_t y, int32_t cx, int32_t cy, BOOL_T repaint)
{
    return w32k_move_window(hwnd, x, y, cx, cy, repaint != 0) != 0;
}

BOOL_T GetWindowRect(uintptr_t hwnd, w32k_rect *rc)
{
    return w32k_get_window_rect(hwnd, rc) != 0;
}

BOOL_T GetClientRect(uintptr_t hwnd, w32k_rect *rc)
{
    return w32k_get_client_rect(hwnd, rc) != 0;
}

BOOL_T ClientToScreen(uintptr_t hwnd, w32k_point *pt)
{
    return w32k_client_to_screen(hwnd, pt) != 0;
}

BOOL_T ScreenToClient(uintptr_t hwnd, w32k_point *pt)
{
    return w32k_screen_to_client(hwnd, pt) != 0;
}

BOOL_T IsWindow(uintptr_t hwnd)
{
    return w32k_is_window(hwnd) != 0;
}

BOOL_T IsWindowVisible(uintptr_t hwnd)
{
    return w32k_is_window_visible(hwnd) != 0;
}

uintptr_t GetParent(uintptr_t hwnd)
{
    return w32k_get_parent(hwnd);
}

intptr_t SetWindowLongPtrW(uintptr_t hwnd, int32_t index, intptr_t value)
{
    return w32k_set_window_long(hwnd, index, value);
}

intptr_t GetWindowLongPtrW(uintptr_t hwnd, int32_t index)
{
    return w32k_get_window_long(hwnd, index);
}

BOOL_T SetWindowTextW(uintptr_t hwnd, const uint16_t *text)
{
    return w32k_set_window_text(hwnd, text) != 0;
}

int32_t GetWindowTextW(uintptr_t hwnd, uint16_t *buf, int32_t max)
{
    return w32k_get_window_text(hwnd, buf, max);
}

uintptr_t FindWindowW(const uint16_t *cls, const uint16_t *name)
{
    return w32k_find_window((uintptr_t)cls, name);
}

uint32_t GetDpiForWindow(uintptr_t hwnd)
{
    return w32k_get_dpi_for_window(hwnd);
}

BOOL_T InvalidateRect(uintptr_t hwnd, const w32k_rect *rc, BOOL_T erase)
{
    return w32k_invalidate_rect(hwnd, rc, erase != 0) != 0;
}

uintptr_t BeginPaint(uintptr_t hwnd, W32K_PAINTSTRUCT *ps)
{
    w32k_paintstruct m;
    uintptr_t dc;
    if (!ps)
        return 0;
    dc = w32k_begin_paint(hwnd, &m);
    ps->hdc = dc;
    ps->fErase = m.erase;
    ps->rcPaint = m.rc;
    ps->fRestore = 0;
    ps->fIncUpdate = 0;
    return dc;
}

BOOL_T EndPaint(uintptr_t hwnd, const W32K_PAINTSTRUCT *ps)
{
    w32k_paintstruct m;
    if (!ps)
        return 0;
    m.hdc = ps->hdc;
    m.erase = ps->fErase;
    m.rc = ps->rcPaint;
    return w32k_end_paint(hwnd, &m) != 0;
}

intptr_t DefWindowProcW(uintptr_t hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    return w32k_def_window_proc(hwnd, msg, wp, lp);
}

/* ---- Messages ----------------------------------------------------------------- */

BOOL_T PostMessageW(uintptr_t hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    return w32k_post_message(hwnd, msg, wp, lp) != 0;
}

BOOL_T PostThreadMessageW(uint32_t tid, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    return w32k_post_thread_message(tid, msg, wp, lp) != 0;
}

intptr_t SendMessageW(uintptr_t hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    return w32k_send_message(hwnd, msg, wp, lp);
}

/* GetMessageW: 1 for a message, 0 for WM_QUIT, -1 on error. The model does not
 * block. When nothing is ready it returns -1 with ERROR_TIMEOUT (1460), so the
 * caller's loop ends rather than hangs. The kernel wait path that would block
 * is not wired in this spike. */
int32_t GetMessageW(void *msg, uintptr_t hwnd, uint32_t min, uint32_t max)
{
    int r = w32k_get_message((w32k_msg *)msg, hwnd, min, max);
    if (r == W32K_WOULD_BLOCK) {
        w32k_set_last_error(1460);
        return -1;
    }
    return r;
}

BOOL_T PeekMessageW(void *msg, uintptr_t hwnd, uint32_t min, uint32_t max, uint32_t flags)
{
    return w32k_peek_message((w32k_msg *)msg, hwnd, min, max, flags) != 0;
}

intptr_t DispatchMessageW(const void *msg)
{
    return w32k_dispatch_message((const w32k_msg *)msg);
}

BOOL_T TranslateMessage(const void *msg)
{
    return w32k_translate_message((const w32k_msg *)msg) != 0;
}

void PostQuitMessage(int32_t code)
{
    w32k_post_quit_message(code);
}

uintptr_t SetTimer(uintptr_t hwnd, uintptr_t id, uint32_t elapse, void *proc)
{
    return w32k_set_timer(hwnd, id, elapse, (w32k_timerproc)proc);
}

BOOL_T KillTimer(uintptr_t hwnd, uintptr_t id)
{
    return w32k_kill_timer(hwnd, id) != 0;
}

/* ---- Focus, activation, capture, cursor --------------------------------------- */

uintptr_t SetFocus(uintptr_t hwnd)
{
    return w32k_set_focus(hwnd);
}

uintptr_t GetFocus(void)
{
    return w32k_get_focus();
}

uintptr_t SetActiveWindow(uintptr_t hwnd)
{
    return w32k_set_active_window(hwnd);
}

uintptr_t GetActiveWindow(void)
{
    return w32k_get_active_window();
}

BOOL_T SetForegroundWindow(uintptr_t hwnd)
{
    return w32k_set_foreground_window(hwnd) != 0;
}

uintptr_t GetForegroundWindow(void)
{
    return w32k_get_foreground_window();
}

uintptr_t SetCapture(uintptr_t hwnd)
{
    return w32k_set_capture(hwnd);
}

BOOL_T ReleaseCapture(void)
{
    return w32k_release_capture() != 0;
}

uintptr_t GetCapture(void)
{
    return w32k_get_capture();
}

uintptr_t SetCursor(uintptr_t cursor)
{
    return w32k_set_cursor(cursor);
}

uintptr_t GetCursor(void)
{
    return w32k_get_cursor();
}

BOOL_T SetCursorPos(int32_t x, int32_t y)
{
    return w32k_set_cursor_pos(x, y) != 0;
}

BOOL_T GetCursorPos(w32k_point *pt)
{
    return w32k_get_cursor_pos(pt) != 0;
}

uintptr_t LoadCursorW(const void *hinst, uintptr_t id)
{
    (void)hinst;
    return w32k_load_cursor(id);
}

uintptr_t CreateCursor(const void *hinst, int32_t xhot, int32_t yhot, int32_t w, int32_t h,
                       const void *and_bits, const void *xor_bits)
{
    (void)hinst;
    (void)and_bits;
    (void)xor_bits;
    return w32k_create_cursor(xhot, yhot, w, h);
}

BOOL_T DestroyCursor(uintptr_t cursor)
{
    return w32k_destroy_cursor(cursor) != 0;
}

uintptr_t LoadIconW(const void *hinst, uintptr_t id)
{
    (void)hinst;
    return w32k_load_icon(id);
}

uintptr_t CreateIcon(const void *hinst, int32_t w, int32_t h, uint8_t planes, uint8_t bpp,
                     const void *and_bits, const void *xor_bits)
{
    (void)hinst;
    (void)and_bits;
    (void)xor_bits;
    return w32k_create_icon(w, h, planes, bpp);
}

BOOL_T DestroyIcon(uintptr_t icon)
{
    return w32k_destroy_icon(icon) != 0;
}

/* ---- Keyboard state, metrics, parameters -------------------------------------- */

int16_t GetAsyncKeyState(int32_t vk)
{
    return (int16_t)w32k_get_async_key_state((uint32_t)vk);
}

int16_t GetKeyState(int32_t vk)
{
    return (int16_t)w32k_get_key_state((uint32_t)vk);
}

int32_t GetSystemMetrics(int32_t index)
{
    return w32k_get_system_metrics(index);
}

BOOL_T SystemParametersInfoW(uint32_t action, uint32_t ui_param, void *pv, uint32_t win_ini)
{
    (void)win_ini;
    return w32k_system_parameters_info(action, ui_param, pv) != 0;
}

/* ---- Clipboard (no global memory: SetClipboardData is not exported) ---------- */

BOOL_T OpenClipboard(uintptr_t hwnd)
{
    return w32k_open_clipboard(hwnd) != 0;
}

BOOL_T CloseClipboard(void)
{
    return w32k_close_clipboard() != 0;
}

BOOL_T EmptyClipboard(void)
{
    return w32k_empty_clipboard() != 0;
}

BOOL_T IsClipboardFormatAvailable(uint32_t format)
{
    return w32k_is_clipboard_format_available(format) != 0;
}

int32_t CountClipboardFormats(void)
{
    return w32k_count_clipboard_formats();
}

uint32_t EnumClipboardFormats(uint32_t prev)
{
    return w32k_enum_clipboard_formats(prev);
}

uint32_t RegisterClipboardFormatW(const uint16_t *name)
{
    return w32k_register_clipboard_format(name);
}

uintptr_t GetClipboardOwner(void)
{
    return w32k_get_clipboard_owner();
}

/* ---- Monitors and display settings -------------------------------------------- */

/* EnumDisplayMonitors callback adapter: the model calls proc(hmon, rc, lp); the
 * Win32 callback is proc(hmon, hdc, lprc, lp). */
typedef struct mon_enum_ctx {
    void *proc;
    intptr_t lp;
} mon_enum_ctx;

static int mon_adapter(w32k_handle hmon, const w32k_rect *rc, intptr_t lp)
{
    mon_enum_ctx *ctx = (mon_enum_ctx *)(uintptr_t)lp;
    BOOL_T (*user)(uintptr_t, void *, w32k_rect *, intptr_t) =
        (BOOL_T (*)(uintptr_t, void *, w32k_rect *, intptr_t))ctx->proc;
    /* The model's rect is const; the Win32 callback may read it only. */
    return user(hmon, NULL, (w32k_rect *)rc, ctx->lp) != 0;
}

BOOL_T EnumDisplayMonitors(void *hdc, const w32k_rect *clip, void *proc, intptr_t lp)
{
    mon_enum_ctx ctx;
    (void)hdc;
    (void)clip;
    ctx.proc = proc;
    ctx.lp = lp;
    return w32k_enum_display_monitors(mon_adapter, (intptr_t)(uintptr_t)&ctx) != 0;
}

uintptr_t MonitorFromWindow(uintptr_t hwnd, uint32_t flags)
{
    (void)flags;
    return w32k_monitor_from_window(hwnd);
}

BOOL_T EnumDisplaySettingsW(const uint16_t *name, uint32_t mode, void *devmode)
{
    w32k_devmode m;
    (void)name;
    if (!devmode)
        return 0;
    if (!w32k_enum_display_settings(mode, &m))
        return 0;
    wr32(devmode, W32K_DM_OFF_SIZE, W32K_DEVMODEW_SIZE); /* dmSize is a WORD; the high half is zero */
    wr32(devmode, W32K_DM_OFF_FIELDS, W32K_DM_BITSPERPEL | W32K_DM_PELSWIDTH | W32K_DM_PELSHEIGHT | W32K_DM_DISPLAYFREQUENCY);
    wr32(devmode, W32K_DM_OFF_BITSPERPEL, m.bpp);
    wr32(devmode, W32K_DM_OFF_PELSWIDTH, m.width);
    wr32(devmode, W32K_DM_OFF_PELSHEIGHT, m.height);
    wr32(devmode, W32K_DM_OFF_FREQUENCY, m.freq);
    return 1;
}

int32_t ChangeDisplaySettingsW(void *devmode, uint32_t flags)
{
    w32k_devmode m;
    if (!devmode) {
        return w32k_change_display_settings(NULL, flags);
    }
    m.width = rd32(devmode, W32K_DM_OFF_PELSWIDTH);
    m.height = rd32(devmode, W32K_DM_OFF_PELSHEIGHT);
    m.bpp = rd32(devmode, W32K_DM_OFF_BITSPERPEL);
    m.freq = rd32(devmode, W32K_DM_OFF_FREQUENCY);
    return w32k_change_display_settings(&m, flags);
}

/* ---- Hooks ---------------------------------------------------------------------- */

uintptr_t SetWindowsHookExW(int32_t type, void *proc, const void *hmod, uint32_t tid)
{
    (void)hmod;
    return w32k_set_windows_hook(type, (w32k_hookproc)proc, tid);
}

BOOL_T UnhookWindowsHookEx(uintptr_t hook)
{
    return w32k_unhook_windows_hook(hook) != 0;
}

intptr_t CallNextHookEx(uintptr_t hook, int32_t code, uintptr_t wp, intptr_t lp)
{
    return w32k_call_next_hook(hook, code, wp, lp);
}

/* ---- Device contexts for windows ----------------------------------------------- */

uintptr_t GetDC(uintptr_t hwnd)
{
    return w32k_get_dc(hwnd);
}

int32_t ReleaseDC(uintptr_t hwnd, uintptr_t dc)
{
    return w32k_release_dc(hwnd, dc) != 0;
}

BOOL_T FillRect(uintptr_t dc, const w32k_rect *rc, uintptr_t brush)
{
    return w32k_fill_rect(dc, rc, brush) != 0;
}

#endif /* __MINGW32__ */
