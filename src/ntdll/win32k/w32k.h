/* w32k.h - userspace model of the win32k-style windowing service (spike).
 *
 * This is the service layer behind user32/gdi32 for the NT personality. It is
 * plain C with no OS dependency: every object lives in fixed static pools, so
 * the same sources build as a MinGW DLL (src/ntdll/user32_win32k.c,
 * gdi32_win32k.c) and as host test code (tests/ntdll-host/test_w32k_*.c).
 *
 * Model and limits (see docs/modules/ntdll-win32.md for the shim layer):
 *  - Threads are identified by a 32-bit id. Each attached thread owns a message
 *    queue and is attached to a desktop. The "current thread" is set with
 *    w32k_set_current_thread(); every API acts for that thread, as in Win32.
 *  - SendMessage to a window owned by another thread runs the window procedure
 *    inline, with the current thread switched to the owner. The model has no
 *    real blocking, so a send cannot deadlock; it also cannot be interleaved.
 *  - GetMessage itself never blocks. When nothing is ready it returns
 *    W32K_WOULD_BLOCK. The user32 shim waits on the thread's NT event and retries
 *    (docs/decisions/0076-win32k-hosting.md). The model only reports readiness and
 *    calls the wake hook.
 *  - Input (keyboard and mouse) is delivered into the target thread's queue at
 *    injection time, in FIFO order with posted messages.
 *  - Timers, paint and the clock are driven by w32k_set_now(); there is no
 *    real time source inside the model.
 *  - Text is not rasterised: TextOut draws one solid cell per character
 *    (see w32k_gdi.c). No real font engine is implemented.
 *
 * Handles are 32-bit: bits 0..15 index+1, bits 16..23 generation, bits 24..31
 * object type. A stale or wrongly typed handle is rejected, not dereferenced.
 */
#ifndef W32K_H
#define W32K_H

#include <stddef.h>
#include <stdint.h>

#include "w32k_font.h"

/* Pointer-sized so a window procedure receives HWND in full, as on x64. Handle
 * values stay below 2^32 (see the encoding above). */
typedef uintptr_t w32k_handle;

#define W32K_HANDLE_TYPE(h)  ((uint32_t)((h) >> 24))
#define W32K_HANDLE_GEN(h)   ((uint32_t)(((h) >> 16) & 0xFFu))
#define W32K_HANDLE_INDEX(h) ((uint32_t)(((h) & 0xFFFFu)))

enum w32k_type {
    W32K_T_NONE = 0,
    W32K_T_STATION = 1,
    W32K_T_DESKTOP = 2,
    W32K_T_WINDOW = 3,
    W32K_T_CLASS = 4,
    W32K_T_CURSOR = 5,
    W32K_T_ICON = 6,
    W32K_T_HOOK = 7,
    W32K_T_DC = 8,
    W32K_T_BITMAP = 9,
    W32K_T_BRUSH = 10,
    W32K_T_PEN = 11,
    W32K_T_FONT = 12,
    W32K_T_REGION = 13,
    W32K_T_PALETTE = 14,
};

/* GetMessage return values. */
#define W32K_WOULD_BLOCK (-2)

/* Win32 error codes used by the model (same numeric values as winerror.h). */
#define W32K_ERROR_INVALID_FUNCTION 1
#define W32K_ERROR_FILE_NOT_FOUND 2
#define W32K_ERROR_ACCESS_DENIED 5
#define W32K_ERROR_INVALID_HANDLE 6
#define W32K_ERROR_NOT_ENOUGH_MEMORY 8
#define W32K_ERROR_INVALID_DATA 13
#define W32K_ERROR_BUSY 170
#define W32K_ERROR_ALREADY_EXISTS 183
#define W32K_ERROR_INVALID_PARAMETER 87
#define W32K_ERROR_CALL_NOT_IMPLEMENTED 120
#define W32K_ERROR_INVALID_FLAGS 1004
#define W32K_ERROR_INVALID_WINDOW_HANDLE 1400
#define W32K_ERROR_INVALID_CURSOR_HANDLE 1402
#define W32K_ERROR_INVALID_MENU_HANDLE 1401
#define W32K_ERROR_CLASS_ALREADY_EXISTS 1410
#define W32K_ERROR_CLASS_DOES_NOT_EXIST 1411
#define W32K_ERROR_CLASS_HAS_WINDOWS 1412
#define W32K_ERROR_INVALID_INDEX 1413
#define W32K_ERROR_CLIPBOARD_NOT_OPEN 1418
#define W32K_ERROR_NOT_ENOUGH_QUOTA 1816
#define W32K_ERROR_INVALID_HOOK_HANDLE 1404
#define W32K_ERROR_RESOURCE_NAME_NOT_FOUND 1813

/* Window messages (same numeric values as winuser.h). */
#define W32K_WM_NULL 0x0000
#define W32K_WM_CREATE 0x0001
#define W32K_WM_DESTROY 0x0002
#define W32K_WM_MOVE 0x0003
#define W32K_WM_SIZE 0x0005
#define W32K_WM_ACTIVATE 0x0006
#define W32K_WM_SETFOCUS 0x0007
#define W32K_WM_KILLFOCUS 0x0008
#define W32K_WM_SETTEXT 0x000C
#define W32K_WM_GETTEXT 0x000D
#define W32K_WM_GETTEXTLENGTH 0x000E
#define W32K_WM_PAINT 0x000F
#define W32K_WM_CLOSE 0x0010
#define W32K_WM_QUIT 0x0012
#define W32K_WM_ERASEBKGND 0x0014
#define W32K_WM_SHOWWINDOW 0x0018
#define W32K_WM_WINDOWPOSCHANGING 0x0046
#define W32K_WM_WINDOWPOSCHANGED 0x0047
#define W32K_WM_DISPLAYCHANGE 0x007E
#define W32K_WM_NCCREATE 0x0081
#define W32K_WM_NCDESTROY 0x0082
#define W32K_WM_NCCALCSIZE 0x0083
#define W32K_WM_NCHITTEST 0x0084
#define W32K_WM_KEYDOWN 0x0100
#define W32K_WM_KEYUP 0x0101
#define W32K_WM_CHAR 0x0102
#define W32K_WM_SYSKEYDOWN 0x0104
#define W32K_WM_SYSKEYUP 0x0105
#define W32K_WM_SYSCHAR 0x0106
#define W32K_WM_COMMAND 0x0111
#define W32K_WM_TIMER 0x0113
#define W32K_WM_MOUSEMOVE 0x0200
#define W32K_WM_LBUTTONDOWN 0x0201
#define W32K_WM_LBUTTONUP 0x0202
#define W32K_WM_RBUTTONDOWN 0x0204
#define W32K_WM_RBUTTONUP 0x0205
#define W32K_WM_MBUTTONDOWN 0x0207
#define W32K_WM_MBUTTONUP 0x0208
#define W32K_WM_MOUSEWHEEL 0x020A
#define W32K_WM_CAPTURECHANGED 0x0215
#define W32K_WM_USER 0x0400
#define W32K_WM_APP 0x8000

/* Window styles used by the model. */
#define W32K_WS_VISIBLE 0x10000000u
#define W32K_WS_CHILD 0x40000000u
#define W32K_WS_POPUP 0x80000000u
#define W32K_WS_DISABLED 0x08000000u
#define W32K_WS_EX_TOPMOST 0x00000008u
#define W32K_WS_EX_TOOLWINDOW 0x00000080u

/* ShowWindow commands. */
#define W32K_SW_HIDE 0
#define W32K_SW_SHOWNORMAL 1
#define W32K_SW_SHOWMINIMIZED 2
#define W32K_SW_SHOWMAXIMIZED 3
#define W32K_SW_SHOWNOACTIVATE 4
#define W32K_SW_SHOW 5
#define W32K_SW_MINIMIZE 6
#define W32K_SW_RESTORE 9

/* GetWindowLongPtr / SetWindowLongPtr indices. */
#define W32K_GWL_WNDPROC (-4)
#define W32K_GWL_HINSTANCE (-6)
#define W32K_GWL_HWNDPARENT (-8)
#define W32K_GWL_STYLE (-16)
#define W32K_GWL_EXSTYLE (-20)
#define W32K_GWLP_USERDATA (-21)
#define W32K_GWL_ID (-12)

/* PeekMessage flags. */
#define W32K_PM_NOREMOVE 0x0000
#define W32K_PM_REMOVE 0x0001
#define W32K_PM_NOYIELD 0x0002

/* Activation states (WM_ACTIVATE wParam). */
#define W32K_WA_INACTIVE 0
#define W32K_WA_ACTIVE 1

/* Virtual keys used by the model (VK_* values). */
#define W32K_VK_BACK 0x08
#define W32K_VK_TAB 0x09
#define W32K_VK_RETURN 0x0D
#define W32K_VK_SHIFT 0x10
#define W32K_VK_CONTROL 0x11
#define W32K_VK_MENU 0x12
#define W32K_VK_ESCAPE 0x1B
#define W32K_VK_SPACE 0x20
#define W32K_VK_LBUTTON 0x01
#define W32K_VK_RBUTTON 0x02
#define W32K_VK_MBUTTON 0x04

/* Mouse key state (MK_* wParam bits). */
#define W32K_MK_LBUTTON 0x0001
#define W32K_MK_RBUTTON 0x0002
#define W32K_MK_SHIFT 0x0004
#define W32K_MK_CONTROL 0x0008
#define W32K_MK_MBUTTON 0x0010

/* Hook types (WH_*). */
#define W32K_WH_MSGFILTER (-1)
#define W32K_WH_KEYBOARD 2
#define W32K_WH_GETMESSAGE 3
#define W32K_WH_CALLWNDPROC 4
#define W32K_WH_CBT 5
#define W32K_WH_MOUSE 7
#define W32K_WH_SHELL 10
#define W32K_WH_KEYBOARD_LL 13
#define W32K_WH_MOUSE_LL 14

/* System metric indices (SM_*). */
#define W32K_SM_CXSCREEN 0
#define W32K_SM_CYSCREEN 1
#define W32K_SM_CXVSCROLL 2
#define W32K_SM_CYHSCROLL 3
#define W32K_SM_CYCAPTION 4
#define W32K_SM_CXBORDER 5
#define W32K_SM_CYBORDER 6
#define W32K_SM_CXICON 11
#define W32K_SM_CYICON 12
#define W32K_SM_CXCURSOR 13
#define W32K_SM_CYCURSOR 14
#define W32K_SM_CYMENU 15
#define W32K_SM_CXFULLSCREEN 16
#define W32K_SM_CYFULLSCREEN 17
#define W32K_SM_MOUSEPRESENT 19
#define W32K_SM_CXDOUBLECLK 36
#define W32K_SM_CYDOUBLECLK 37
#define W32K_SM_CMOUSEBUTTONS 43
#define W32K_SM_CXVIRTUALSCREEN 78
#define W32K_SM_CYVIRTUALSCREEN 79
#define W32K_SM_XVIRTUALSCREEN 76
#define W32K_SM_YVIRTUALSCREEN 77
#define W32K_SM_CMONITORS 80
#define W32K_SM_SAMEDISPLAYFORMAT 81

/* SystemParametersInfo actions used by the model (SPI_*). */
#define W32K_SPI_GETBEEP 0x0001
#define W32K_SPI_SETBEEP 0x0002
#define W32K_SPI_GETMOUSESPEED 0x0070
#define W32K_SPI_GETSCREENSAVEACTIVE 0x0010
#define W32K_SPI_SETSCREENSAVEACTIVE 0x0011
#define W32K_SPI_GETKEYBOARDSPEED 0x000A
#define W32K_SPI_GETWORKAREA 0x0030

/* ChangeDisplaySettings flags and results. */
#define W32K_CDS_UPDATEREGISTRY 0x00000001u
#define W32K_CDS_TEST 0x00000002u
#define W32K_DISP_CHANGE_SUCCESSFUL 0
#define W32K_DISP_CHANGE_FAILED (-1)
#define W32K_DISP_CHANGE_BADMODE (-2)
#define W32K_ENUM_CURRENT_SETTINGS ((uint32_t)-1)
#define W32K_ENUM_REGISTRY_SETTINGS ((uint32_t)-2)

/* GDI constants used by the model. */
#define W32K_SRCCOPY 0x00CC0020u
#define W32K_SRCPAINT 0x00EE0086u
#define W32K_SRCAND 0x008800C6u
#define W32K_SRCINVERT 0x00660046u
#define W32K_NOTSRCCOPY 0x00330008u
#define W32K_PATCOPY 0x00F00021u
#define W32K_BLACKNESS 0x00000042u
#define W32K_WHITENESS 0x00FF0062u
#define W32K_DSTINVERT 0x00550009u
#define W32K_CLR_INVALID 0xFFFFFFFFu
#define W32K_TRANSPARENT 1
#define W32K_OPAQUE 2
#define W32K_OBJ_PEN 1
#define W32K_OBJ_BRUSH 2
#define W32K_OBJ_DC 3
#define W32K_OBJ_FONT 6
#define W32K_OBJ_BITMAP 7
#define W32K_HORZRES 8
#define W32K_VERTRES 10
#define W32K_BITSPIXEL 12
#define W32K_PLANES 14
#define W32K_LOGPIXELSX 88
#define W32K_LOGPIXELSY 90

/* Stock objects (GetStockObject indices). */
#define W32K_WHITE_BRUSH 0
#define W32K_BLACK_BRUSH 4
#define W32K_NULL_BRUSH 5
#define W32K_WHITE_PEN 6
#define W32K_BLACK_PEN 7
#define W32K_NULL_PEN 8
#define W32K_SYSTEM_FONT 13

/* Stock cursor and icon ids (MAKEINTRESOURCE values). */
#define W32K_IDC_ARROW 32512u
#define W32K_IDC_IBEAM 32513u
#define W32K_IDC_WAIT 32514u
#define W32K_IDC_CROSS 32515u
#define W32K_IDC_HAND 32649u
#define W32K_IDI_APPLICATION 32512u

/* A rectangle, inclusive-exclusive as in RECT. */
typedef struct w32k_rect {
    int32_t left, top, right, bottom;
} w32k_rect;

typedef struct w32k_point {
    int32_t x, y;
} w32k_point;

typedef struct w32k_msg {
    w32k_handle hwnd;
    uint32_t message;
    uintptr_t wparam;
    intptr_t lparam;
    uint32_t time;
    w32k_point pt;
} w32k_msg;

typedef intptr_t (*w32k_wndproc)(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);
typedef void (*w32k_timerproc)(w32k_handle hwnd, uint32_t msg, uintptr_t id, uint32_t time);
typedef intptr_t (*w32k_hookproc)(int code, uintptr_t wp, intptr_t lp);
typedef int (*w32k_monitorenumproc)(w32k_handle hmon, const w32k_rect *rect, intptr_t lparam);

typedef struct w32k_wndclass {
    const uint16_t *name;
    uint32_t style;
    w32k_wndproc proc;
    int32_t cls_extra;
    int32_t wnd_extra;
    w32k_handle bg_brush;
    w32k_handle cursor;
    w32k_handle icon;
    const uint16_t *menu_name;
    const void *hinst;
} w32k_wndclass;

/* Field order and pointer-sized handle fields match CREATESTRUCTW on x64, so the
 * pointer passed as lParam of WM_NCCREATE / WM_CREATE can be read as one. */
typedef struct w32k_createwnd {
    void *param;                 /* lpCreateParams */
    const void *hinst;           /* hInstance */
    uintptr_t menu;              /* hMenu, or child id */
    uintptr_t parent;            /* hwndParent (0 for top-level) */
    int32_t cy, cx, y, x;
    uint32_t style;
    const uint16_t *name;        /* lpszName */
    uintptr_t cls;               /* lpszClass: class name pointer, or atom in the low 16 bits */
    uint32_t exstyle;
} w32k_createwnd;

#define W32K_CW_USEDEFAULT 0x80000000u

typedef struct w32k_paintstruct {
    w32k_handle hdc;
    int erase;
    w32k_rect rc;
} w32k_paintstruct;

typedef struct w32k_devmode {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t freq;
} w32k_devmode;

typedef struct w32k_size {
    int32_t cx, cy;
} w32k_size;

/* ---- Lifecycle, thread context, last error --------------------------------- */

void w32k_init(void);
/* Runs w32k_init once per process; later calls return at once. win32u.dll, user32.dll and
 * gdi32.dll each call it on attach, so the load order does not matter. */
void w32k_init_once(void);
void w32k_set_now(uint32_t ms);
uint32_t w32k_now(void);
uint32_t w32k_get_last_error(void);
void w32k_set_last_error(uint32_t err);
uint32_t w32k_set_current_thread(uint32_t tid);
uint32_t w32k_current_thread(void);
int w32k_attach_thread(uint32_t tid);
void w32k_detach_thread(uint32_t tid);

/* ---- Window stations and desktops ------------------------------------------ */

w32k_handle w32k_create_winstation(const uint16_t *name);
w32k_handle w32k_open_winstation(const uint16_t *name);
int w32k_close_winstation(w32k_handle station);
w32k_handle w32k_create_desktop(w32k_handle station, const uint16_t *name);
w32k_handle w32k_open_desktop(w32k_handle station, const uint16_t *name);
int w32k_close_desktop(w32k_handle desktop);
int w32k_set_thread_desktop(w32k_handle desktop);
w32k_handle w32k_get_thread_desktop(void);
int w32k_switch_desktop(w32k_handle desktop);
w32k_handle w32k_get_input_desktop(void);

/* ---- Window classes and windows -------------------------------------------- */

uint16_t w32k_register_class(const w32k_wndclass *wc);
int w32k_unregister_class(uintptr_t cls, const void *hinst);
int w32k_get_class_info(uintptr_t cls, w32k_wndclass *out);
w32k_handle w32k_create_window(const w32k_createwnd *cs);
int w32k_destroy_window(w32k_handle hwnd);
int w32k_show_window(w32k_handle hwnd, int cmd);
int w32k_move_window(w32k_handle hwnd, int32_t x, int32_t y, int32_t cx, int32_t cy, int repaint);
int w32k_get_window_rect(w32k_handle hwnd, w32k_rect *rc);
int w32k_get_client_rect(w32k_handle hwnd, w32k_rect *rc);
int w32k_client_to_screen(w32k_handle hwnd, w32k_point *pt);
int w32k_screen_to_client(w32k_handle hwnd, w32k_point *pt);
int w32k_is_window(w32k_handle hwnd);
int w32k_is_window_visible(w32k_handle hwnd);
w32k_handle w32k_get_parent(w32k_handle hwnd);
uint32_t w32k_get_window_thread(w32k_handle hwnd);
w32k_handle w32k_find_window(uintptr_t cls, const uint16_t *name);
intptr_t w32k_get_window_long(w32k_handle hwnd, int index);
intptr_t w32k_set_window_long(w32k_handle hwnd, int index, intptr_t value);
int w32k_set_window_text(w32k_handle hwnd, const uint16_t *text);
int w32k_get_window_text(w32k_handle hwnd, uint16_t *buf, int max);
uint32_t w32k_get_dpi_for_window(w32k_handle hwnd);
int w32k_invalidate_rect(w32k_handle hwnd, const w32k_rect *rc, int erase);
w32k_handle w32k_begin_paint(w32k_handle hwnd, w32k_paintstruct *ps);
int w32k_end_paint(w32k_handle hwnd, const w32k_paintstruct *ps);
intptr_t w32k_def_window_proc(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);

/* ---- Messages, queues, timers, focus, capture, activation, hooks ----------- */

int w32k_post_message(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);
int w32k_post_thread_message(uint32_t tid, uint32_t msg, uintptr_t wp, intptr_t lp);
intptr_t w32k_send_message(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp);
int w32k_get_message(w32k_msg *msg, w32k_handle hwnd, uint32_t min, uint32_t max);

/* Blocking support. The model never sleeps itself. A blocking shim (user32_win32k.c)
 * installs a wake hook and waits on its side. The hook is called with the thread id
 * whenever a message is queued to that thread, or when a window's paint region is
 * invalidated. W32K_NO_TIMER is returned by w32k_timer_wait_ms when no timer is set. */
typedef void (*w32k_wake_fn)(uint32_t tid);
#define W32K_NO_TIMER 0xFFFFFFFFu
void w32k_set_wake_hook(w32k_wake_fn fn);
uint32_t w32k_timer_wait_ms(void);
int w32k_peek_message(w32k_msg *msg, w32k_handle hwnd, uint32_t min, uint32_t max, uint32_t flags);
intptr_t w32k_dispatch_message(const w32k_msg *msg);
int w32k_translate_message(const w32k_msg *msg);
void w32k_post_quit_message(int exit_code);
uintptr_t w32k_set_timer(w32k_handle hwnd, uintptr_t id, uint32_t elapse, w32k_timerproc proc);
int w32k_kill_timer(w32k_handle hwnd, uintptr_t id);
w32k_handle w32k_set_focus(w32k_handle hwnd);
w32k_handle w32k_get_focus(void);
w32k_handle w32k_set_active_window(w32k_handle hwnd);
w32k_handle w32k_get_active_window(void);
int w32k_set_foreground_window(w32k_handle hwnd);
w32k_handle w32k_get_foreground_window(void);
w32k_handle w32k_set_capture(w32k_handle hwnd);
int w32k_release_capture(void);
w32k_handle w32k_get_capture(void);
w32k_handle w32k_set_cursor(w32k_handle cursor);
w32k_handle w32k_get_cursor(void);
w32k_handle w32k_set_windows_hook(int type, w32k_hookproc proc, uint32_t tid);
int w32k_unhook_windows_hook(w32k_handle hook);
intptr_t w32k_call_next_hook(w32k_handle hook, int code, uintptr_t wp, intptr_t lp);

/* ---- Input ------------------------------------------------------------------ */

void w32k_inject_key(uint32_t vk, uint32_t scan, int down);
void w32k_inject_mouse_move(int32_t x, int32_t y);
void w32k_inject_mouse_button(uint32_t vk, int down, int32_t x, int32_t y);
void w32k_inject_wheel(int32_t delta, int32_t x, int32_t y);
uint16_t w32k_get_async_key_state(uint32_t vk);
uint16_t w32k_get_key_state(uint32_t vk);
int w32k_get_cursor_pos(w32k_point *pt);
int w32k_set_cursor_pos(int32_t x, int32_t y);
size_t w32k_evdev_feed(const uint8_t *buf, size_t len);

/* ---- Clipboard -------------------------------------------------------------- */

int w32k_open_clipboard(w32k_handle hwnd);
int w32k_close_clipboard(void);
int w32k_empty_clipboard(void);
int w32k_set_clipboard_data(uint32_t format, const void *data, size_t size);
const void *w32k_get_clipboard_data(uint32_t format, size_t *size);
int w32k_is_clipboard_format_available(uint32_t format);
int w32k_count_clipboard_formats(void);
uint32_t w32k_enum_clipboard_formats(uint32_t prev);
uint32_t w32k_register_clipboard_format(const uint16_t *name);
w32k_handle w32k_get_clipboard_owner(void);

/* ---- System metrics, parameters, monitors, display settings ----------------- */

int32_t w32k_get_system_metrics(int index);
int w32k_system_parameters_info(uint32_t action, uint32_t ui_param, void *pv);
int w32k_enum_display_monitors(w32k_monitorenumproc proc, intptr_t lparam);
w32k_handle w32k_monitor_from_window(w32k_handle hwnd);
int w32k_set_monitor(uint32_t index, const w32k_rect *rc, uint32_t dpi, int primary);
int w32k_enum_display_settings(uint32_t index, w32k_devmode *out);
int w32k_change_display_settings(const w32k_devmode *mode, uint32_t flags);

/* ---- Cursor and icon objects ------------------------------------------------ */

w32k_handle w32k_load_cursor(uintptr_t id);
w32k_handle w32k_create_cursor(int32_t xhot, int32_t yhot, int32_t w, int32_t h);
int w32k_destroy_cursor(w32k_handle cursor);
w32k_handle w32k_load_icon(uintptr_t id);
w32k_handle w32k_create_icon(int32_t w, int32_t h, uint32_t planes, uint32_t bpp);
int w32k_destroy_icon(w32k_handle icon);

/* ---- GDI objects and surfaces ----------------------------------------------- */

w32k_handle w32k_get_dc(w32k_handle hwnd);
int w32k_release_dc(w32k_handle hwnd, w32k_handle dc);
w32k_handle w32k_create_compatible_dc(w32k_handle dc);
int w32k_delete_dc(w32k_handle dc);
w32k_handle w32k_create_dib_section(int32_t w, int32_t h, uint32_t bpp, void **bits);
w32k_handle w32k_create_solid_brush(uint32_t color);
w32k_handle w32k_create_pen(uint32_t style, int32_t width, uint32_t color);
w32k_handle w32k_create_font(int32_t height);
w32k_handle w32k_create_font_indirect(const w32k_logfont *lf);
int w32k_get_text_metrics(w32k_handle dc, w32k_tm *tm);
w32k_handle w32k_select_object(w32k_handle dc, w32k_handle obj);
int w32k_delete_object(w32k_handle obj);
uint32_t w32k_get_object_type(w32k_handle obj);
w32k_handle w32k_get_stock_object(int index);
int w32k_fill_rect(w32k_handle dc, const w32k_rect *rc, w32k_handle brush);
uint32_t w32k_set_pixel(w32k_handle dc, int32_t x, int32_t y, uint32_t color);
uint32_t w32k_get_pixel(w32k_handle dc, int32_t x, int32_t y);
int w32k_bitblt(w32k_handle dst, int32_t x, int32_t y, int32_t w, int32_t h,
                w32k_handle src, int32_t sx, int32_t sy, uint32_t rop);
int w32k_stretchblt(w32k_handle dst, int32_t x, int32_t y, int32_t w, int32_t h,
                    w32k_handle src, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                    uint32_t rop);
int w32k_text_out(w32k_handle dc, int32_t x, int32_t y, const uint16_t *s, int len);
int w32k_get_text_extent(w32k_handle dc, const uint16_t *s, int len, w32k_size *sz);
uint32_t w32k_set_text_color(w32k_handle dc, uint32_t color);
uint32_t w32k_get_text_color(w32k_handle dc);
uint32_t w32k_set_bk_color(w32k_handle dc, uint32_t color);
uint32_t w32k_get_bk_color(w32k_handle dc);
int w32k_set_bk_mode(w32k_handle dc, int mode);
int w32k_get_device_caps(w32k_handle dc, int index);
/* Direct access to a bitmap's pixels (32bpp, top-down, BGRA little-endian). */
uint32_t *w32k_bitmap_bits(w32k_handle bitmap, int32_t *w, int32_t *h);
/* ---- Regions (w32k_region.c) ------------------------------------------------- */

#define W32K_OBJ_REGION 4
#define W32K_NULLREGION 1
#define W32K_SIMPLEREGION 2
#define W32K_COMPLEXREGION 3
#define W32K_RGN_AND 1
#define W32K_RGN_OR 2
#define W32K_RGN_XOR 3
#define W32K_RGN_DIFF 4
#define W32K_RGN_COPY 5

w32k_handle w32k_create_rect_rgn(int32_t l, int32_t t, int32_t r, int32_t b);
w32k_handle w32k_create_rect_rgn_indirect(const w32k_rect *rc);
w32k_handle w32k_create_elliptic_rgn(int32_t l, int32_t t, int32_t r, int32_t b);
w32k_handle w32k_create_round_rect_rgn(int32_t l, int32_t t, int32_t r, int32_t b, int32_t ew, int32_t eh);
w32k_handle w32k_create_polygon_rgn(const w32k_point *pts, int n, int fill_winding);
int w32k_set_rect_rgn(w32k_handle rgn, int32_t l, int32_t t, int32_t r, int32_t b);
int w32k_combine_rgn(w32k_handle dst, w32k_handle a, w32k_handle b, int mode);
int w32k_get_rgn_box(w32k_handle rgn, w32k_rect *out);
int w32k_pt_in_region(w32k_handle rgn, int32_t x, int32_t y);
int w32k_rect_in_region(w32k_handle rgn, const w32k_rect *rc);
int w32k_equal_rgn(w32k_handle a, w32k_handle b);
int w32k_offset_rgn(w32k_handle rgn, int32_t dx, int32_t dy);
int w32k_delete_region(w32k_handle rgn);
/* DC clip: SelectClipRgn copies rgn (rgn 0 clears the clip). GetClipRgn returns 1 when a
 * clip is set, 0 when none, and copies it into rgn. GetClipBox returns the region type of
 * the clip's bounds. All coordinates are device coordinates. */
int w32k_select_clip_rgn(w32k_handle dc, w32k_handle rgn);
int w32k_get_clip_rgn(w32k_handle dc, w32k_handle rgn);
int w32k_get_clip_box(w32k_handle dc, w32k_rect *out);

/* The screen surface (fixed 1920x1080 framebuffer). Window DCs draw into it. */
w32k_handle w32k_framebuffer(void);

#endif /* W32K_H */
