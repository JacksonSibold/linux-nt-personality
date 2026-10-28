/* test_w32k_rawinput.c - raw input (w32k_rawinput.c): registration rules, WM_INPUT delivery to the
 * registered window, the RAWINPUT byte layout, the foreground and sink rules and RIDEV_NOLEGACY. */
#include "minitest.h"
#include "w32k.h"

#include <string.h>

static uint16_t *to_w(const char *s, uint16_t *buf)
{
    size_t i = 0;
    for (; s[i]; ++i)
        buf[i] = (uint16_t)(unsigned char)s[i];
    buf[i] = 0;
    return buf;
}

static uint32_t rd32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static uint32_t rd16(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8);
}

static intptr_t proc(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    return w32k_def_window_proc(hwnd, msg, wp, lp);
}

static w32k_handle make_window(const char *cls_name, uint32_t style)
{
    uint16_t nm[32];
    w32k_wndclass wc;
    w32k_createwnd cs;
    uint16_t atom;
    memset(&wc, 0, sizeof(wc));
    wc.name = to_w(cls_name, nm);
    wc.proc = proc;
    atom = w32k_register_class(&wc);
    memset(&cs, 0, sizeof(cs));
    cs.cls = atom;
    cs.name = nm;
    cs.style = style;
    cs.x = 0;
    cs.y = 0;
    cs.cx = 300;
    cs.cy = 200;
    return w32k_create_window(&cs);
}

static w32k_rid_dev dev(uint16_t page, uint16_t usage, uint32_t flags, w32k_handle target)
{
    w32k_rid_dev d;
    d.usage_page = page;
    d.usage = usage;
    d.flags = flags;
    d.target = target;
    return d;
}

static void registration_rules(void)
{
    w32k_handle w;
    w32k_rid_dev d[2], out[4];
    w32k_init();
    w = make_window("RR", W32K_WS_VISIBLE);
    MT_ASSERT(w != 0);
    MT_EXPECT(!w32k_register_raw_input(NULL, 1));
    d[0] = dev(1, 2, 0, w);
    MT_EXPECT(!w32k_register_raw_input(d, 0));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
    MT_EXPECT_EQ(w32k_get_registered_raw_input(NULL, 0), 0);
    MT_EXPECT(w32k_register_raw_input(d, 1));
    MT_EXPECT_EQ(w32k_get_registered_raw_input(out, 4), 1);
    MT_EXPECT_EQ(out[0].usage, 2);
    MT_EXPECT_EQ(out[0].target, w);
    /* Registering the same usage again replaces the entry. */
    d[0] = dev(1, 2, W32K_RIDEV_INPUTSINK, w);
    MT_EXPECT(w32k_register_raw_input(d, 1));
    MT_EXPECT_EQ(w32k_get_registered_raw_input(out, 4), 1);
    MT_EXPECT_EQ(out[0].flags, W32K_RIDEV_INPUTSINK);
    /* A bad target fails and, being in the same call, keeps the good entry from being added. */
    d[0] = dev(1, 6, 0, w);
    d[1] = dev(1, 5, 0, 0x12345678);
    MT_EXPECT(!w32k_register_raw_input(d, 2));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_WINDOW_HANDLE);
    MT_EXPECT_EQ(w32k_get_registered_raw_input(NULL, 0), 1);
    /* RIDEV_REMOVE needs a null target. */
    d[0] = dev(1, 2, W32K_RIDEV_REMOVE, w);
    MT_EXPECT(!w32k_register_raw_input(d, 1));
    d[0] = dev(1, 2, W32K_RIDEV_REMOVE, 0);
    MT_EXPECT(w32k_register_raw_input(d, 1));
    MT_EXPECT_EQ(w32k_get_registered_raw_input(NULL, 0), 0);
    /* Removing what is not registered is accepted. */
    MT_EXPECT(w32k_register_raw_input(d, 1));
}

static void registration_table_full(void)
{
    w32k_rid_dev d;
    uint16_t u;
    w32k_init();
    for (u = 1; u <= 8; ++u) {
        d = dev(0xFF00, u, 0, 0);
        MT_EXPECT(w32k_register_raw_input(&d, 1));
    }
    d = dev(0xFF00, 9, 0, 0);
    MT_EXPECT(!w32k_register_raw_input(&d, 1));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_NOT_ENOUGH_MEMORY);
    MT_EXPECT_EQ(w32k_get_registered_raw_input(NULL, 0), 8);
}

static void mouse_event_layout(void)
{
    w32k_handle w;
    w32k_rid_dev d;
    w32k_msg m;
    const uint8_t *b;
    uint32_t size = 0;
    w32k_init();
    w = make_window("ML", W32K_WS_VISIBLE);
    w32k_set_foreground_window(w);
    d = dev(1, 2, 0, w);
    MT_EXPECT(w32k_register_raw_input(&d, 1));
    w32k_inject_raw_mouse(-7, 12, 0, 0);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    MT_EXPECT_EQ(m.hwnd, w);
    MT_EXPECT_EQ(m.message, W32K_WM_INPUT);
    MT_EXPECT_EQ(m.wparam, W32K_RIM_INPUT);
    b = w32k_raw_input_bytes((w32k_handle)m.lparam, &size);
    MT_ASSERT(b != NULL);
    MT_EXPECT_EQ(size, 48);
    MT_EXPECT_EQ(rd32(b, 0), W32K_RIM_TYPEMOUSE);
    MT_EXPECT_EQ(rd32(b, 4), 48);
    MT_EXPECT_EQ(rd32(b, 8), W32K_RAW_MOUSE_DEVICE);
    MT_EXPECT_EQ(rd32(b, 16), W32K_RIM_INPUT);
    MT_EXPECT_EQ(rd16(b, 24), 0);              /* MOUSE_MOVE_RELATIVE */
    MT_EXPECT_EQ(rd16(b, 28), 0);              /* usButtonFlags */
    MT_EXPECT_EQ((int32_t)rd32(b, 36), -7);    /* lLastX */
    MT_EXPECT_EQ((int32_t)rd32(b, 40), 12);    /* lLastY */
    /* An unknown handle is not a raw input. */
    MT_EXPECT(w32k_raw_input_bytes(0x1234, &size) == NULL);
    MT_EXPECT(w32k_raw_input_bytes(m.lparam + 1, &size) == NULL);
    /* Buttons and wheel carry their flags and data. */
    w32k_inject_raw_mouse(0, 0, W32K_RI_MOUSE_LEFT_BUTTON_DOWN, 0);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    b = w32k_raw_input_bytes((w32k_handle)m.lparam, &size);
    MT_ASSERT(b != NULL);
    MT_EXPECT_EQ(rd16(b, 28), W32K_RI_MOUSE_LEFT_BUTTON_DOWN);
    w32k_inject_raw_mouse(0, 0, W32K_RI_MOUSE_WHEEL, -120);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    b = w32k_raw_input_bytes((w32k_handle)m.lparam, &size);
    MT_ASSERT(b != NULL);
    MT_EXPECT_EQ(rd16(b, 28), W32K_RI_MOUSE_WHEEL);
    MT_EXPECT_EQ((int16_t)rd16(b, 30), -120);
}

static void keyboard_event_layout(void)
{
    w32k_handle w;
    w32k_rid_dev d;
    w32k_msg m;
    const uint8_t *b;
    uint32_t size = 0;
    w32k_init();
    w = make_window("KL", W32K_WS_VISIBLE);
    w32k_set_foreground_window(w);
    d = dev(1, 6, 0, w);
    MT_EXPECT(w32k_register_raw_input(&d, 1));
    w32k_inject_raw_key(0x11, 'W', 1, 0);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    b = w32k_raw_input_bytes((w32k_handle)m.lparam, &size);
    MT_ASSERT(b != NULL);
    MT_EXPECT_EQ(size, 40);
    MT_EXPECT_EQ(rd32(b, 0), W32K_RIM_TYPEKEYBOARD);
    MT_EXPECT_EQ(rd32(b, 4), 40);
    MT_EXPECT_EQ(rd32(b, 8), W32K_RAW_KEYBOARD_DEVICE);
    MT_EXPECT_EQ(rd16(b, 24), 0x11);        /* MakeCode */
    MT_EXPECT_EQ(rd16(b, 26), 0);           /* RI_KEY_MAKE */
    MT_EXPECT_EQ(rd16(b, 30), 'W');         /* VKey */
    MT_EXPECT_EQ(rd32(b, 32), W32K_WM_KEYDOWN);
    w32k_inject_raw_key(0x11, 'W', 0, 1);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    b = w32k_raw_input_bytes((w32k_handle)m.lparam, &size);
    MT_ASSERT(b != NULL);
    MT_EXPECT_EQ(rd16(b, 26), 3);           /* RI_KEY_BREAK | RI_KEY_E0 */
    MT_EXPECT_EQ(rd32(b, 32), W32K_WM_KEYUP);
}

static void foreground_and_sink(void)
{
    w32k_handle a, b;
    w32k_rid_dev d;
    w32k_msg m;
    w32k_init();
    a = make_window("FA", W32K_WS_VISIBLE);
    b = make_window("FB", W32K_WS_VISIBLE);
    w32k_set_foreground_window(b);
    /* Registered for a, which is not in the foreground, without INPUTSINK: nothing is delivered
     * for a different thread. Both windows are on the same model thread here, so they share the
     * foreground thread; register the sink case by making it explicit with a thread switch. */
    d = dev(1, 2, 0, a);
    MT_EXPECT(w32k_register_raw_input(&d, 1));
    w32k_inject_raw_mouse(1, 1, 0, 0);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    MT_EXPECT_EQ(m.hwnd, a);
    MT_EXPECT_EQ(m.wparam, W32K_RIM_INPUT); /* same thread as the foreground window */
    /* A window on another thread gets RIM_INPUTSINK with the flag, nothing without it. */
    {
        w32k_handle c;
        uint32_t prev = w32k_set_current_thread(2);
        MT_EXPECT(w32k_attach_thread(2));
        c = make_window("FC", W32K_WS_VISIBLE);
        w32k_set_current_thread(prev);
        MT_ASSERT(c != 0);
        w32k_set_foreground_window(b);
        d = dev(1, 2, 0, c);
        MT_EXPECT(w32k_register_raw_input(&d, 1));
        w32k_inject_raw_mouse(1, 1, 0, 0);
        w32k_set_current_thread(2);
        MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), W32K_WOULD_BLOCK);
        w32k_set_current_thread(prev);
        d = dev(1, 2, W32K_RIDEV_INPUTSINK, c);
        MT_EXPECT(w32k_register_raw_input(&d, 1));
        w32k_inject_raw_mouse(1, 1, 0, 0);
        w32k_set_current_thread(2);
        MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
        MT_EXPECT_EQ(m.wparam, W32K_RIM_INPUTSINK);
        w32k_set_current_thread(prev);
    }
}

static void exclude_and_dead_target(void)
{
    w32k_handle w;
    w32k_rid_dev d;
    w32k_msg m;
    w32k_init();
    w = make_window("EX", W32K_WS_VISIBLE);
    w32k_set_foreground_window(w);
    d = dev(1, 2, W32K_RIDEV_EXCLUDE, w);
    MT_EXPECT(!w32k_register_raw_input(&d, 1)); /* exclude takes no target */
    d = dev(1, 2, W32K_RIDEV_EXCLUDE, 0);
    MT_EXPECT(w32k_register_raw_input(&d, 1));
    w32k_inject_raw_mouse(1, 1, 0, 0);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), W32K_WOULD_BLOCK);
    /* A destroyed target stops delivery without an error. */
    d = dev(1, 2, 0, w);
    MT_EXPECT(w32k_register_raw_input(&d, 1));
    w32k_destroy_window(w);
    w32k_inject_raw_mouse(1, 1, 0, 0);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, 0, 0), W32K_WOULD_BLOCK);
}

static void injection_generates_raw(void)
{
    w32k_handle w;
    w32k_rid_dev d;
    w32k_msg m;
    const uint8_t *b;
    uint32_t size;
    w32k_init();
    w = make_window("IG", W32K_WS_VISIBLE);
    w32k_set_foreground_window(w);
    d = dev(1, 2, 0, w);
    MT_EXPECT(w32k_register_raw_input(&d, 1));
    w32k_set_cursor_pos(100, 100);
    /* The legacy absolute move derives a delta from the cursor movement. */
    w32k_inject_mouse_move(110, 95);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    b = w32k_raw_input_bytes((w32k_handle)m.lparam, &size);
    MT_ASSERT(b != NULL);
    MT_EXPECT_EQ((int32_t)rd32(b, 36), 10);
    MT_EXPECT_EQ((int32_t)rd32(b, 40), -5);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_MOUSEMOVE, W32K_WM_MOUSEMOVE), 1);
    /* A button press raises both WM_INPUT and the legacy message. */
    w32k_inject_mouse_button(W32K_VK_LBUTTON, 1, 110, 95);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    b = w32k_raw_input_bytes((w32k_handle)m.lparam, &size);
    MT_ASSERT(b != NULL);
    MT_EXPECT_EQ(rd16(b, 28), W32K_RI_MOUSE_LEFT_BUTTON_DOWN);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_LBUTTONDOWN, W32K_WM_LBUTTONDOWN), 1);
    w32k_inject_mouse_button(W32K_VK_RBUTTON, 0, 110, 95);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    b = w32k_raw_input_bytes((w32k_handle)m.lparam, &size);
    MT_ASSERT(b != NULL);
    MT_EXPECT_EQ(rd16(b, 28), W32K_RI_MOUSE_RIGHT_BUTTON_UP);
}

/* Evdev relative motion reaches raw input unclamped, even with the cursor against the screen edge. */
static void evdev_raw_motion_is_unclamped(void)
{
    uint8_t rec[48];
    w32k_handle w;
    w32k_rid_dev d;
    w32k_msg m;
    const uint8_t *b;
    uint32_t size;
    w32k_init();
    w = make_window("EV", W32K_WS_VISIBLE);
    w32k_set_foreground_window(w);
    d = dev(1, 2, 0, w);
    MT_EXPECT(w32k_register_raw_input(&d, 1));
    w32k_set_cursor_pos(0, 0);
    memset(rec, 0, sizeof(rec));
    /* EV_REL REL_X -40, then EV_SYN SYN_REPORT. */
    rec[16] = 0x02;
    rec[18] = 0x00;
    rec[20] = (uint8_t)(-40 & 0xFF);
    rec[21] = 0xFF;
    rec[22] = 0xFF;
    rec[23] = 0xFF;
    rec[24 + 16] = 0x00; /* EV_SYN */
    MT_EXPECT_EQ(w32k_evdev_feed(rec, 48), 2);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    b = w32k_raw_input_bytes((w32k_handle)m.lparam, &size);
    MT_ASSERT(b != NULL);
    MT_EXPECT_EQ((int32_t)rd32(b, 36), -40);
}

static void nolegacy_suppresses_messages(void)
{
    w32k_handle w;
    w32k_rid_dev d;
    w32k_msg m;
    w32k_init();
    w = make_window("NL", W32K_WS_VISIBLE);
    w32k_set_foreground_window(w);
    d = dev(1, 2, W32K_RIDEV_NOLEGACY, w);
    MT_EXPECT(w32k_register_raw_input(&d, 1));
    w32k_inject_mouse_move(50, 50);
    w32k_inject_mouse_button(W32K_VK_LBUTTON, 1, 50, 50);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_MOUSEMOVE, W32K_WM_LBUTTONDOWN), W32K_WOULD_BLOCK);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_INPUT, W32K_WM_INPUT), 1);
    /* The cursor and key state still follow the input. */
    {
        w32k_point pt;
        w32k_get_cursor_pos(&pt);
        MT_EXPECT_EQ(pt.x, 50);
    }
    MT_EXPECT(w32k_get_key_state(W32K_VK_LBUTTON) & 0x8000);
    /* Keyboard: NOLEGACY on usage 6 drops WM_KEYDOWN but keeps the key state. */
    d = dev(1, 6, W32K_RIDEV_NOLEGACY, w);
    MT_EXPECT(w32k_register_raw_input(&d, 1));
    w32k_inject_key('A', 0x1E, 1);
    MT_EXPECT_EQ(w32k_get_message(&m, 0, W32K_WM_KEYDOWN, W32K_WM_KEYDOWN), W32K_WOULD_BLOCK);
    MT_EXPECT(w32k_get_key_state('A') & 0x8000);
}

static const mt_case_t cases[] = {
    MT_CASE(registration_rules),
    MT_CASE(registration_table_full),
    MT_CASE(mouse_event_layout),
    MT_CASE(keyboard_event_layout),
    MT_CASE(foreground_and_sink),
    MT_CASE(exclude_and_dead_target),
    MT_CASE(injection_generates_raw),
    MT_CASE(evdev_raw_motion_is_unclamped),
    MT_CASE(nolegacy_suppresses_messages),
};

int main(void)
{
    return MT_RUN("w32k_rawinput", cases);
}
