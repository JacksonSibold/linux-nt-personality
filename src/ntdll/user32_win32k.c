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
 * and window-class menu APIs. GetMessageW, WaitMessage and MsgWaitForMultipleObjects
 * block on the calling thread's NT event; see the blocking-retrieval note above
 * GetMessageW and docs/decisions/0076-win32k-hosting.md.
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

/* ---- Blocking retrieval (kernel wait path) -----------------------------------
 * Each thread that retrieves messages owns one auto-reset NT event, created on its
 * first GetMessageW, WaitMessage or MsgWaitForMultipleObjects. The model's wake hook
 * (wake_thread) sets it when a message is queued to the thread or a paint becomes
 * pending. An auto-reset event stays signalled until a wait takes it, so a wake that
 * lands between a failed retrieval and the wait is not lost. A pending timer bounds
 * the wait at its due time. The model clock is read from the system time before each
 * retrieval, so timers fire while the thread sleeps. */

#define W32K_WAITERS_MAX 64
#define W32K_WAIT_INFINITE 0xFFFFFFFFu
#define W32K_WAIT_OBJECT_0 0u
#define W32K_WAIT_TIMEOUT 258u
#define W32K_WAIT_FAILED 0xFFFFFFFFu
#define W32K_STATUS_TIMEOUT ((int32_t)0x00000102)
#define W32K_EVENT_ALL_ACCESS 0x001F0003u
#define W32K_SYNCHRONIZATION_EVENT 1u
#define W32K_WAIT_ANY 1u

int32_t NtCreateEvent(uintptr_t *h, uint32_t access, void *oa, uint32_t type, uint8_t initial);
int32_t NtSetEvent(uintptr_t h, int32_t *prev);
int32_t NtWaitForSingleObject(uintptr_t h, uint8_t alertable, int64_t *timeout);
int32_t NtWaitForMultipleObjects(uint32_t count, uintptr_t *h, uint32_t wait_type, uint8_t alertable,
                                 int64_t *timeout);
int32_t NtQuerySystemTime(int64_t *t);

static struct {
    uint32_t tid;
    uintptr_t ev;
} w32k_waiters[W32K_WAITERS_MAX];

/* The OS thread id of the calling thread, from the TEB client id. */
static uint32_t os_tid(void)
{
    uint64_t tid;
    __asm__ volatile("movq %%gs:0x48, %0" : "=r"(tid) :: "memory");
    return (uint32_t)tid;
}

static uintptr_t waiter_find(uint32_t tid)
{
    for (uint32_t i = 0; i < W32K_WAITERS_MAX; ++i) {
        if (w32k_waiters[i].ev && w32k_waiters[i].tid == tid)
            return w32k_waiters[i].ev;
    }
    return 0;
}

/* The event of tid, created on first use. 0 when the table is full or NtCreateEvent fails. */
static uintptr_t waiter_event(uint32_t tid)
{
    uintptr_t h = waiter_find(tid), created = 0;
    if (h)
        return h;
    for (uint32_t i = 0; i < W32K_WAITERS_MAX; ++i) {
        if (!w32k_waiters[i].ev) {
            if (NtCreateEvent(&created, W32K_EVENT_ALL_ACCESS, NULL, W32K_SYNCHRONIZATION_EVENT, 0) < 0)
                return 0;
            w32k_waiters[i].tid = tid;
            w32k_waiters[i].ev = created;
            return created;
        }
    }
    return 0;
}

static void wake_thread(uint32_t tid)
{
    uintptr_t h = waiter_find(tid);
    int32_t prev = 0;
    if (h)
        NtSetEvent(h, &prev);
}

static void sync_clock(void)
{
    int64_t t = 0;
    if (NtQuerySystemTime(&t) >= 0)
        w32k_set_now((uint32_t)((uint64_t)t / 10000u));
}

/* Waits on ev until it is signalled, the earliest timer is due, or limit_ms passes
 * (W32K_WAIT_INFINITE for none). Returns the NT status. */
static int32_t wait_on_queue(uintptr_t ev, uint32_t limit_ms, uintptr_t *extra, uint32_t extra_n,
                             uint32_t *idx_out)
{
    uint32_t t = w32k_timer_wait_ms();
    uint32_t ms = limit_ms;
    int64_t rel;
    uintptr_t arr[65];
    int32_t st;
    if (t != W32K_NO_TIMER && t < ms)
        ms = t;
    if (extra_n == 0) {
        if (ms == W32K_WAIT_INFINITE)
            return NtWaitForSingleObject(ev, 0, NULL);
        rel = -(int64_t)ms * 10000;
        return NtWaitForSingleObject(ev, 0, &rel);
    }
    for (uint32_t i = 0; i < extra_n; ++i)
        arr[i] = extra[i];
    arr[extra_n] = ev;
    if (ms == W32K_WAIT_INFINITE) {
        st = NtWaitForMultipleObjects(extra_n + 1, arr, W32K_WAIT_ANY, 0, NULL);
    } else {
        rel = -(int64_t)ms * 10000;
        st = NtWaitForMultipleObjects(extra_n + 1, arr, W32K_WAIT_ANY, 0, &rel);
    }
    if (idx_out && st >= 0 && st <= (int32_t)extra_n)
        *idx_out = (uint32_t)st;
    return st;
}

/* GetMessageW: 1 for a message, 0 for WM_QUIT, -1 on error. Blocks until a message
 * arrives. The caller's own thread id is bound to the model for the call. */
int32_t GetMessageW(void *msg, uintptr_t hwnd, uint32_t min, uint32_t max)
{
    uint32_t tid = os_tid(), prev = w32k_set_current_thread(tid);
    uintptr_t ev = waiter_event(tid);
    int32_t r;
    for (;;) {
        int got;
        sync_clock();
        got = w32k_get_message((w32k_msg *)msg, hwnd, min, max);
        if (got != W32K_WOULD_BLOCK) {
            r = got;
            break;
        }
        if (!ev) {
            w32k_set_last_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
            r = -1;
            break;
        }
        wait_on_queue(ev, W32K_WAIT_INFINITE, NULL, 0, NULL);
    }
    w32k_set_current_thread(prev);
    return r;
}

/* WaitMessage: returns when the calling thread has a message (not removed). */
BOOL_T WaitMessage(void)
{
    uint32_t tid = os_tid(), prev = w32k_set_current_thread(tid);
    uintptr_t ev = waiter_event(tid);
    w32k_msg probe;
    BOOL_T ok = 1;
    for (;;) {
        sync_clock();
        if (w32k_peek_message(&probe, 0, 0, 0, 0))
            break;
        if (!ev) {
            ok = 0;
            break;
        }
        wait_on_queue(ev, W32K_WAIT_INFINITE, NULL, 0, NULL);
    }
    w32k_set_current_thread(prev);
    return ok;
}

/* MsgWaitForMultipleObjects with fWaitAll FALSE: returns WAIT_OBJECT_0 + i when handle i
 * is signalled, WAIT_OBJECT_0 + nCount when a message is queued, WAIT_TIMEOUT on
 * expiry, WAIT_FAILED otherwise. dwWakeMask is not narrowed: any queued message wakes. */
uint32_t MsgWaitForMultipleObjects(uint32_t count, const uintptr_t *handles, BOOL_T wait_all,
                                   uint32_t ms, uint32_t wake_mask)
{
    uint32_t tid = os_tid(), prev = w32k_set_current_thread(tid);
    uintptr_t ev = waiter_event(tid);
    uintptr_t local[64];
    uint32_t result = W32K_WAIT_FAILED;
    uint32_t deadline = 0;
    w32k_msg probe;
    (void)wake_mask;
    if (count > 63 || wait_all || !ev) {
        w32k_set_last_error(wait_all ? W32K_ERROR_CALL_NOT_IMPLEMENTED : W32K_ERROR_INVALID_PARAMETER);
        w32k_set_current_thread(prev);
        return W32K_WAIT_FAILED;
    }
    for (uint32_t i = 0; i < count; ++i)
        local[i] = handles ? handles[i] : 0;
    sync_clock();
    if (ms != W32K_WAIT_INFINITE)
        deadline = w32k_now() + ms;
    for (;;) {
        uint32_t idx = count;
        int32_t st;
        sync_clock();
        if (w32k_peek_message(&probe, 0, 0, 0, 0)) {
            result = W32K_WAIT_OBJECT_0 + count;
            break;
        }
        if (ms != W32K_WAIT_INFINITE) {
            uint32_t now = w32k_now();
            if (now >= deadline) {
                result = W32K_WAIT_TIMEOUT;
                break;
            }
            st = wait_on_queue(ev, deadline - now, local, count, &idx);
        } else {
            st = wait_on_queue(ev, W32K_WAIT_INFINITE, local, count, &idx);
        }
        if (st >= 0 && st <= (int32_t)count) {
            if (st < (int32_t)count) {
                result = W32K_WAIT_OBJECT_0 + (uint32_t)st;
                break;
            }
            continue;
        }
        if (st == W32K_STATUS_TIMEOUT)
            continue;
        result = W32K_WAIT_FAILED;
        break;
    }
    w32k_set_current_thread(prev);
    return result;
}

BOOL_T PeekMessageW(void *msg, uintptr_t hwnd, uint32_t min, uint32_t max, uint32_t flags)
{
    uint32_t tid = os_tid(), prev = w32k_set_current_thread(tid);
    BOOL_T r;
    sync_clock();
    r = w32k_peek_message((w32k_msg *)msg, hwnd, min, max, flags) != 0;
    w32k_set_current_thread(prev);
    return r;
}

/* Called from user32.c DllMain on DLL_PROCESS_ATTACH, on the loading thread. Sets up
 * the model (it was never initialised in the DLL before), makes the loading thread
 * the default thread for the shim, and installs the wake hook. */
void w32k_shim_attach(void)
{
    w32k_init();
    w32k_set_current_thread(os_tid());
    w32k_set_wake_hook(wake_thread);
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
