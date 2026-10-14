/* test_w32k_input.c - keyboard and mouse injection, routing, async key state,
 * low-level hooks, and Linux evdev decoding (src/ntdll/win32k). */
#include "minitest.h"
#include "w32k.h"
#include "w32k_evdev.h"

#include <string.h>

#define REC_MAX 128
static uint32_t rec_msg[REC_MAX];
static uintptr_t rec_wp[REC_MAX];
static intptr_t rec_lp[REC_MAX];
static w32k_handle rec_h[REC_MAX];
static int rec_n;

static intptr_t rec_proc(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    if (rec_n < REC_MAX) {
        rec_msg[rec_n] = msg;
        rec_wp[rec_n] = wp;
        rec_lp[rec_n] = lp;
        rec_h[rec_n] = hwnd;
        ++rec_n;
    }
    return w32k_def_window_proc(hwnd, msg, wp, lp);
}

static void rec_reset(void)
{
    rec_n = 0;
}


static uint16_t reg_class(void)
{
    static const uint16_t nm[] = {'I', 'n', 'p', 0};
    w32k_wndclass wc;
    memset(&wc, 0, sizeof(wc));
    wc.name = nm;
    wc.proc = rec_proc;
    return w32k_register_class(&wc);
}

static w32k_handle win_at(int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t style,
                          w32k_handle parent)
{
    static const uint16_t nm[] = {'I', 'n', 'p', 0};
    w32k_createwnd cs;
    memset(&cs, 0, sizeof(cs));
    cs.cls = (uintptr_t)nm;
    cs.name = nm;
    cs.style = style;
    cs.x = x;
    cs.y = y;
    cs.cx = cx;
    cs.cy = cy;
    cs.parent = parent;
    return w32k_create_window(&cs);
}

/* ---- Keyboard routing -------------------------------------------------------- */

static void test_key_goes_to_focus_window(void)
{
    w32k_handle a, b;
    w32k_msg m;
    w32k_init();
    MT_ASSERT(reg_class() != 0);
    a = win_at(0, 0, 100, 100, W32K_WS_VISIBLE, 0);
    b = win_at(200, 0, 100, 100, W32K_WS_VISIBLE, 0);
    MT_ASSERT(a && b);
    /* b is the newest visible top-level, so it is active and focused. */
    MT_ASSERT(w32k_set_foreground_window(b));
    MT_EXPECT(w32k_get_focus() == b);
    w32k_inject_key('A', 0x1E, 1);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_KEYDOWN);
    MT_EXPECT(m.hwnd == b);
    MT_EXPECT_EQ(m.wparam, 'A');
    /* Autorepeat: the repeat bit (30) is set on a second keydown. */
    w32k_inject_key('A', 0x1E, 1);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT(((uintptr_t)m.lparam & (1u << 30)) != 0);
    w32k_inject_key('A', 0x1E, 0);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_KEYUP);
    MT_EXPECT(((uintptr_t)m.lparam & (1u << 31)) != 0);
    /* Moving focus redirects the keys. */
    MT_EXPECT(w32k_set_focus(a) == b);
    w32k_inject_key('B', 0x30, 1);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT(m.hwnd == a);
    MT_EXPECT(w32k_destroy_window(a));
    MT_EXPECT(w32k_destroy_window(b));
}

static void test_translate_produces_wm_char_with_shift(void)
{
    w32k_handle w;
    w32k_msg m;
    w32k_init();
    MT_ASSERT(reg_class() != 0);
    w = win_at(0, 0, 50, 50, W32K_WS_VISIBLE, 0);
    MT_ASSERT(w != 0 && w32k_set_foreground_window(w));
    w32k_inject_key('K', 0x25, 1);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT(w32k_translate_message(&m));
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_CHAR);
    MT_EXPECT_EQ(m.wparam, 'k');
    w32k_inject_key('K', 0x25, 0);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    w32k_inject_key(W32K_VK_SHIFT, 0x2A, 1);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    w32k_inject_key('K', 0x25, 1);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT(w32k_translate_message(&m));
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.wparam, 'K');
    /* Keys with no character produce no WM_CHAR. */
    w32k_inject_key(0x70, 0x3B, 1);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT(!w32k_translate_message(&m));
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_async_key_state_bits(void)
{
    w32k_init();
    MT_EXPECT_EQ(w32k_get_async_key_state('Q'), 0);
    w32k_inject_key('Q', 0x10, 1);
    /* Pressed since the last query (bit 0) and down (bit 15). */
    MT_EXPECT_EQ(w32k_get_async_key_state('Q'), 0x8001);
    MT_EXPECT_EQ(w32k_get_async_key_state('Q'), 0x8000);
    w32k_inject_key('Q', 0x10, 0);
    MT_EXPECT_EQ(w32k_get_async_key_state('Q'), 0);
    MT_EXPECT_EQ(w32k_get_key_state('Q'), 0);
    w32k_inject_key(0x41, 0, 1);
    MT_EXPECT_EQ(w32k_get_key_state(0x41), 0x8000);
    MT_EXPECT_EQ(w32k_get_async_key_state(256), 0);
}

static void test_no_foreground_drops_keys(void)
{
    w32k_msg m;
    w32k_init();
    w32k_inject_key('Z', 0x2C, 1);
    MT_EXPECT(!w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
}

/* ---- Mouse routing ----------------------------------------------------------- */

static void test_mouse_hit_test_and_client_coords(void)
{
    w32k_handle a, b;
    w32k_msg m;
    w32k_init();
    MT_ASSERT(reg_class() != 0);
    a = win_at(0, 0, 400, 400, W32K_WS_VISIBLE, 0);
    b = win_at(100, 100, 200, 200, W32K_WS_VISIBLE, 0); /* newer, on top of a */
    MT_ASSERT(a && b);
    rec_reset();
    w32k_inject_mouse_button(W32K_VK_LBUTTON, 1, 150, 160);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_LBUTTONDOWN);
    MT_EXPECT(m.hwnd == b);
    /* Client coordinates: 150-100, 160-100. */
    MT_EXPECT_EQ(m.lparam & 0xFFFF, 50);
    MT_EXPECT_EQ((m.lparam >> 16) & 0xFFFF, 60);
    MT_EXPECT_EQ(m.wparam & W32K_MK_LBUTTON, W32K_MK_LBUTTON);
    /* Outside b but inside a goes to a. */
    w32k_inject_mouse_move(20, 30);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_MOUSEMOVE);
    MT_EXPECT(m.hwnd == a);
    MT_EXPECT_EQ(m.lparam & 0xFFFF, 20);
    MT_EXPECT_EQ((m.lparam >> 16) & 0xFFFF, 30);
    w32k_inject_mouse_button(W32K_VK_LBUTTON, 0, 20, 30);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_LBUTTONUP);
    MT_EXPECT(m.hwnd == a);
    MT_EXPECT(w32k_destroy_window(a));
    MT_EXPECT(w32k_destroy_window(b));
}

static void test_mouse_capture_overrides_hit_test(void)
{
    w32k_handle a, b;
    w32k_msg m;
    w32k_init();
    MT_ASSERT(reg_class() != 0);
    a = win_at(0, 0, 100, 100, W32K_WS_VISIBLE, 0);
    b = win_at(200, 200, 100, 100, W32K_WS_VISIBLE, 0);
    MT_ASSERT(a && b);
    MT_EXPECT(w32k_set_capture(a) == 0);
    w32k_inject_mouse_move(250, 250); /* over b, but a holds capture */
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT(m.hwnd == a);
    MT_EXPECT_EQ(m.lparam & 0xFFFF, 250);
    w32k_release_capture();
    w32k_inject_mouse_move(250, 250);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT(m.hwnd == b);
    MT_EXPECT(w32k_destroy_window(a));
    MT_EXPECT(w32k_destroy_window(b));
}

static void test_wheel_uses_screen_coords_and_delta(void)
{
    w32k_handle a;
    w32k_msg m;
    w32k_init();
    MT_ASSERT(reg_class() != 0);
    a = win_at(100, 100, 300, 300, W32K_WS_VISIBLE, 0);
    MT_ASSERT(a != 0);
    w32k_inject_wheel(120, 110, 120);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_MOUSEWHEEL);
    MT_EXPECT_EQ((int16_t)(m.wparam >> 16), 120);
    /* lParam holds screen coordinates for the wheel message. */
    MT_EXPECT_EQ(m.lparam & 0xFFFF, 110);
    MT_EXPECT_EQ((m.lparam >> 16) & 0xFFFF, 120);
    MT_EXPECT(w32k_destroy_window(a));
}

static void test_cursor_clamped_to_primary(void)
{
    w32k_point pt;
    w32k_init();
    w32k_inject_mouse_move(5000, -40);
    MT_ASSERT(w32k_get_cursor_pos(&pt));
    MT_EXPECT_EQ(pt.x, 1919);
    MT_EXPECT_EQ(pt.y, 0);
    MT_EXPECT(w32k_set_cursor_pos(10, 20));
    MT_ASSERT(w32k_get_cursor_pos(&pt));
    MT_EXPECT_EQ(pt.x, 10);
    MT_EXPECT_EQ(pt.y, 20);
}

/* ---- Low-level hooks --------------------------------------------------------- */

static int ll_kbd_seen;
static intptr_t ll_kbd_block(int code, uintptr_t wp, intptr_t lp)
{
    (void)code;
    (void)lp;
    ++ll_kbd_seen;
    return wp == 'X' ? 1 : 0; /* block only X */
}

static int ll_mouse_seen;
static intptr_t ll_mouse_block(int code, uintptr_t wp, intptr_t lp)
{
    (void)code;
    (void)wp;
    (void)lp;
    ++ll_mouse_seen;
    return 1; /* block every mouse event */
}

static void test_low_level_keyboard_hook_blocks(void)
{
    w32k_handle a, hk;
    w32k_msg m;
    w32k_init();
    MT_ASSERT(reg_class() != 0);
    a = win_at(0, 0, 100, 100, W32K_WS_VISIBLE, 0);
    MT_ASSERT(a && w32k_set_foreground_window(a));
    /* A visible window owes WM_PAINT; drain it so only input is in the queue. */
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_PAINT);
    hk = w32k_set_windows_hook(W32K_WH_KEYBOARD_LL, ll_kbd_block, 0);
    MT_ASSERT(hk != 0);
    ll_kbd_seen = 0;
    w32k_inject_key('X', 0x2D, 1);
    MT_EXPECT_EQ(ll_kbd_seen, 1);
    MT_EXPECT(!w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE)); /* swallowed */
    /* Other keys pass through and do reach the queue. */
    w32k_inject_key('Y', 0x15, 1);
    MT_EXPECT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.wparam, 'Y');
    MT_EXPECT(w32k_unhook_windows_hook(hk));
    MT_EXPECT(w32k_destroy_window(a));
}

static void test_low_level_mouse_hook_blocks(void)
{
    w32k_handle a, hk;
    w32k_msg m;
    w32k_init();
    MT_ASSERT(reg_class() != 0);
    a = win_at(0, 0, 100, 100, W32K_WS_VISIBLE, 0);
    MT_ASSERT(a != 0);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_PAINT);
    hk = w32k_set_windows_hook(W32K_WH_MOUSE_LL, ll_mouse_block, 0);
    MT_ASSERT(hk != 0);
    ll_mouse_seen = 0;
    w32k_inject_mouse_move(10, 10);
    MT_EXPECT_EQ(ll_mouse_seen, 1);
    MT_EXPECT(!w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT(w32k_unhook_windows_hook(hk));
    MT_EXPECT(w32k_destroy_window(a));
}

/* ---- Evdev ------------------------------------------------------------------- */

static void evrec(uint8_t *p, uint16_t type, uint16_t code, int32_t value)
{
    memset(p, 0, W32K_EVDEV_RECORD_SIZE);
    p[16] = (uint8_t)(type & 0xFF);
    p[17] = (uint8_t)(type >> 8);
    p[18] = (uint8_t)(code & 0xFF);
    p[19] = (uint8_t)(code >> 8);
    p[20] = (uint8_t)(value & 0xFF);
    p[21] = (uint8_t)((value >> 8) & 0xFF);
    p[22] = (uint8_t)((value >> 16) & 0xFF);
    p[23] = (uint8_t)((value >> 24) & 0xFF);
}

static void test_evdev_decode_record(void)
{
    uint8_t p[W32K_EVDEV_RECORD_SIZE];
    w32k_evdev_ev ev;
    evrec(p, W32K_EV_REL, W32K_REL_X, -7);
    w32k_evdev_decode(p, &ev);
    MT_EXPECT_EQ(ev.type, W32K_EV_REL);
    MT_EXPECT_EQ(ev.code, W32K_REL_X);
    MT_EXPECT_EQ(ev.value, -7);
}

static void test_evdev_key_map(void)
{
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(30), 'A');     /* KEY_A */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(16), 'Q');     /* KEY_Q */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(44), 'Z');     /* KEY_Z */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(2), '1');      /* KEY_1 */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(11), '0');     /* KEY_0 */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(1), 0x1B);     /* KEY_ESC */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(28), 0x0D);    /* KEY_ENTER */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(57), 0x20);    /* KEY_SPACE */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(59), 0x70);    /* KEY_F1 */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(88), 0x7B);    /* KEY_F12 */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(103), 0x26);   /* KEY_UP */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(42), 0xA0);    /* KEY_LEFTSHIFT */
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(0), 0);
    MT_EXPECT_EQ(w32k_evdev_key_to_vk(70), 0);       /* KEY_SCROLLLOCK: unmapped */
}

static void test_evdev_feed_keys_and_motion(void)
{
    w32k_handle a;
    w32k_msg m;
    uint8_t buf[W32K_EVDEV_RECORD_SIZE * 8];
    size_t n = 0;
    w32k_init();
    MT_ASSERT(reg_class() != 0);
    a = win_at(0, 0, 300, 300, W32K_WS_VISIBLE, 0);
    MT_ASSERT(a && w32k_set_foreground_window(a));
    w32k_set_cursor_pos(100, 100);
    /* Press A, release A, then a relative move of (+5, -3) at SYN_REPORT. */
    evrec(buf + 0 * 24, W32K_EV_KEY, 30, 1);
    evrec(buf + 1 * 24, W32K_EV_SYN, W32K_SYN_REPORT, 0);
    evrec(buf + 2 * 24, W32K_EV_KEY, 30, 0);
    evrec(buf + 3 * 24, W32K_EV_SYN, W32K_SYN_REPORT, 0);
    evrec(buf + 4 * 24, W32K_EV_REL, W32K_REL_X, 5);
    evrec(buf + 5 * 24, W32K_EV_REL, W32K_REL_Y, -3);
    evrec(buf + 6 * 24, W32K_EV_SYN, W32K_SYN_REPORT, 0);
    n = w32k_evdev_feed(buf, 7 * 24);
    MT_EXPECT_EQ(n, 7);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_KEYDOWN);
    MT_EXPECT_EQ(m.wparam, 'A');
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_KEYUP);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_MOUSEMOVE);
    MT_EXPECT_EQ(m.lparam & 0xFFFF, 105);
    MT_EXPECT_EQ((m.lparam >> 16) & 0xFFFF, 97);
    MT_EXPECT(w32k_destroy_window(a));
}

static void test_evdev_buttons_and_wheel(void)
{
    w32k_handle a;
    w32k_msg m;
    uint8_t buf[W32K_EVDEV_RECORD_SIZE * 4];
    w32k_init();
    MT_ASSERT(reg_class() != 0);
    a = win_at(0, 0, 300, 300, W32K_WS_VISIBLE, 0);
    MT_ASSERT(a != 0);
    w32k_set_cursor_pos(50, 60);
    evrec(buf + 0 * 24, W32K_EV_KEY, W32K_BTN_RIGHT, 1);
    evrec(buf + 1 * 24, W32K_EV_SYN, W32K_SYN_REPORT, 0);
    evrec(buf + 2 * 24, W32K_EV_REL, W32K_REL_WHEEL, -1);
    evrec(buf + 3 * 24, W32K_EV_SYN, W32K_SYN_REPORT, 0);
    MT_EXPECT_EQ(w32k_evdev_feed(buf, 4 * 24), 4);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_RBUTTONDOWN);
    MT_ASSERT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.message, W32K_WM_MOUSEWHEEL);
    MT_EXPECT_EQ((int16_t)(m.wparam >> 16), -120);
    MT_EXPECT(w32k_destroy_window(a));
}

static void test_evdev_partial_record_ignored(void)
{
    uint8_t buf[W32K_EVDEV_RECORD_SIZE + 10];
    w32k_init();
    evrec(buf, W32K_EV_SYN, W32K_SYN_REPORT, 0);
    MT_EXPECT_EQ(w32k_evdev_feed(buf, sizeof(buf)), 1);
    MT_EXPECT_EQ(w32k_evdev_feed(buf, 10), 0);
}

int main(void)
{
    static const mt_case_t cases[] = {
        {"key_goes_to_focus", test_key_goes_to_focus_window},
        {"translate_wm_char_shift", test_translate_produces_wm_char_with_shift},
        {"async_key_state_bits", test_async_key_state_bits},
        {"no_foreground_drops_keys", test_no_foreground_drops_keys},
        {"mouse_hit_test_client", test_mouse_hit_test_and_client_coords},
        {"mouse_capture_overrides", test_mouse_capture_overrides_hit_test},
        {"wheel_screen_coords", test_wheel_uses_screen_coords_and_delta},
        {"cursor_clamped_primary", test_cursor_clamped_to_primary},
        {"ll_keyboard_hook_blocks", test_low_level_keyboard_hook_blocks},
        {"ll_mouse_hook_blocks", test_low_level_mouse_hook_blocks},
        {"evdev_decode_record", test_evdev_decode_record},
        {"evdev_key_map", test_evdev_key_map},
        {"evdev_feed_keys_motion", test_evdev_feed_keys_and_motion},
        {"evdev_buttons_wheel", test_evdev_buttons_and_wheel},
        {"evdev_partial_record", test_evdev_partial_record_ignored},
    };
    setvbuf(stdout, NULL, _IONBF, 0);
    return mt_run("test_w32k_input", cases, sizeof(cases) / sizeof(cases[0]));
}
