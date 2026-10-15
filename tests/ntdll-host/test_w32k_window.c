/* test_w32k_window.c - window classes, window lifecycle, properties and the
 * create/show/destroy message order of the win32k model (src/ntdll/win32k). */
#include "minitest.h"
#include "w32k.h"

#include <string.h>

/* ---- Helpers ---------------------------------------------------------------- */

static int weq(const uint16_t *a, const uint16_t *b)
{
    for (size_t i = 0;; ++i) {
        if (a[i] != b[i])
            return 0;
        if (a[i] == 0)
            return 1;
    }
}

static uint16_t *to_w(const char *s, uint16_t *buf)
{
    size_t i = 0;
    for (; s[i]; ++i)
        buf[i] = (uint16_t)(unsigned char)s[i];
    buf[i] = 0;
    return buf;
}

#define WSTR(name, text) uint16_t name[64]; to_w(text, name)

#define LOG_MAX 256
static uint32_t log_msg[LOG_MAX];
static w32k_handle log_hwnd[LOG_MAX];
static int log_n;

static void log_reset(void)
{
    log_n = 0;
}

static intptr_t rec_proc(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    (void)wp;
    (void)lp;
    if (log_n < LOG_MAX) {
        log_msg[log_n] = msg;
        log_hwnd[log_n] = hwnd;
        ++log_n;
    }
    return w32k_def_window_proc(hwnd, msg, wp, lp);
}

/* Expect exactly the messages in want[] (in order) to have been logged. */
static int log_equals(const uint32_t *want, int n)
{
    if (log_n != n)
        return 0;
    for (int i = 0; i < n; ++i)
        if (log_msg[i] != want[i])
            return 0;
    return 1;
}

static w32k_wndclass make_class(const uint16_t *name, w32k_wndproc proc, int32_t wnd_extra)
{
    w32k_wndclass wc;
    memset(&wc, 0, sizeof(wc));
    wc.name = name;
    wc.style = 0;
    wc.proc = proc;
    wc.wnd_extra = wnd_extra;
    return wc;
}

static w32k_createwnd make_cs(uintptr_t cls, const uint16_t *name, uint32_t style, int32_t x,
                              int32_t y, int32_t cx, int32_t cy, uintptr_t parent)
{
    w32k_createwnd cs;
    memset(&cs, 0, sizeof(cs));
    cs.cls = cls;
    cs.name = name;
    cs.style = style;
    cs.x = x;
    cs.y = y;
    cs.cx = cx;
    cs.cy = cy;
    cs.parent = parent;
    return cs;
}

static uint16_t register_rec(const char *name_unused, const uint16_t *name, int32_t wnd_extra)
{
    w32k_wndclass wc = make_class(name, rec_proc, wnd_extra);
    (void)name_unused;
    return w32k_register_class(&wc);
}

/* ---- Classes ---------------------------------------------------------------- */

static void test_class_register_and_lookup(void)
{
    w32k_init();
    WSTR(nm, "SpikeA");
    uint16_t atom = register_rec("", nm, 0);
    MT_ASSERT(atom >= 0xC000);
    MT_EXPECT_EQ(w32k_get_last_error(), 0);
    /* Lookup by name (case-insensitive) and by atom. */
    WSTR(lower, "spikea");
    w32k_wndclass out;
    MT_ASSERT(w32k_get_class_info((uintptr_t)lower, &out));
    MT_EXPECT(out.proc == rec_proc);
    MT_ASSERT(w32k_get_class_info((uintptr_t)atom, &out));
    MT_EXPECT(weq(out.name, nm));
}

static void test_class_duplicate_rejected(void)
{
    w32k_init();
    WSTR(nm, "Dup");
    MT_ASSERT(register_rec("", nm, 0) != 0);
    MT_EXPECT(register_rec("", nm, 0) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_CLASS_ALREADY_EXISTS);
}

static void test_class_invalid_params(void)
{
    w32k_init();
    WSTR(nm, "Bad");
    w32k_wndclass wc = make_class(nm, NULL, 0);
    MT_EXPECT(w32k_register_class(&wc) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
    wc = make_class(nm, rec_proc, 65);
    MT_EXPECT(w32k_register_class(&wc) == 0);
    wc = make_class(nm, rec_proc, -1);
    MT_EXPECT(w32k_register_class(&wc) == 0);
    MT_EXPECT(w32k_register_class(NULL) == 0);
}

static void test_class_unregister_rules(void)
{
    w32k_init();
    WSTR(nm, "Unreg");
    w32k_handle w;
    w32k_createwnd cs;
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, nm, 0, 0, 0, 10, 10, 0);
    w = w32k_create_window(&cs);
    MT_ASSERT(w != 0);
    MT_EXPECT(w32k_unregister_class((uintptr_t)nm, NULL) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_CLASS_HAS_WINDOWS);
    MT_EXPECT(w32k_destroy_window(w));
    MT_EXPECT(w32k_unregister_class((uintptr_t)nm, NULL));
    MT_EXPECT(w32k_unregister_class((uintptr_t)nm, NULL) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_CLASS_DOES_NOT_EXIST);
}

static void test_class_registry_capacity_and_reuse(void)
{
    w32k_init();
    /* Fill the class pool with unique names, then free one and register again. */
    uint16_t names[300][8];
    int ok = 0;
    for (int i = 0; i < 300; ++i) {
        names[i][0] = 'C';
        names[i][1] = (uint16_t)('a' + (i % 26));
        names[i][2] = (uint16_t)('a' + ((i / 26) % 26));
        names[i][3] = (uint16_t)('0' + (i / 676));
        names[i][4] = 0;
        if (register_rec("", names[i], 0))
            ++ok;
    }
    MT_EXPECT_EQ(ok, 256); /* the class pool holds 256 entries */
    MT_EXPECT(register_rec("", names[299], 0) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_NOT_ENOUGH_MEMORY);
    MT_EXPECT(w32k_unregister_class((uintptr_t)names[10], NULL));
    MT_EXPECT(register_rec("", names[299], 0) != 0); /* the freed slot is reused */
    MT_EXPECT(register_rec("", names[298], 0) == 0);
}

/* ---- Create, show, move, destroy --------------------------------------------- */

static void test_create_window_basic_geometry(void)
{
    w32k_init();
    WSTR(nm, "Geo");
    w32k_handle w;
    w32k_rect rc, cr;
    w32k_point pt;
    w32k_createwnd cs;
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, nm, 0, 10, 20, 300, 200, 0);
    w = w32k_create_window(&cs);
    MT_ASSERT(w != 0);
    MT_EXPECT(w32k_is_window(w));
    MT_EXPECT(!w32k_is_window_visible(w));
    MT_EXPECT(w32k_get_window_rect(w, &rc));
    MT_EXPECT_EQ(rc.left, 10);
    MT_EXPECT_EQ(rc.top, 20);
    MT_EXPECT_EQ(rc.right, 310);
    MT_EXPECT_EQ(rc.bottom, 220);
    MT_EXPECT(w32k_get_client_rect(w, &cr));
    MT_EXPECT_EQ(cr.right, 300);
    MT_EXPECT_EQ(cr.bottom, 200);
    pt.x = 5;
    pt.y = 7;
    MT_EXPECT(w32k_client_to_screen(w, &pt));
    MT_EXPECT_EQ(pt.x, 15);
    MT_EXPECT_EQ(pt.y, 27);
    MT_EXPECT(w32k_screen_to_client(w, &pt));
    MT_EXPECT_EQ(pt.x, 5);
    MT_EXPECT_EQ(pt.y, 7);
    MT_EXPECT(w32k_destroy_window(w));
    MT_EXPECT(!w32k_is_window(w));
}

static void test_create_default_position_and_size(void)
{
    w32k_init();
    WSTR(nm, "Def");
    w32k_handle w;
    w32k_rect rc;
    w32k_createwnd cs;
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, nm, 0, (int32_t)W32K_CW_USEDEFAULT, (int32_t)W32K_CW_USEDEFAULT,
                 (int32_t)W32K_CW_USEDEFAULT, (int32_t)W32K_CW_USEDEFAULT, 0);
    w = w32k_create_window(&cs);
    MT_ASSERT(w != 0);
    MT_EXPECT(w32k_get_window_rect(w, &rc));
    MT_EXPECT_EQ(rc.right - rc.left, 640);
    MT_EXPECT_EQ(rc.bottom - rc.top, 480);
}

static void test_create_child_is_parent_relative(void)
{
    w32k_init();
    WSTR(nm, "Par");
    w32k_handle p, c;
    w32k_rect rc;
    w32k_createwnd cs;
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, nm, 0, 100, 100, 400, 300, 0);
    p = w32k_create_window(&cs);
    MT_ASSERT(p != 0);
    cs = make_cs((uintptr_t)nm, nm, W32K_WS_CHILD, 5, 6, 50, 40, (uintptr_t)p);
    c = w32k_create_window(&cs);
    MT_ASSERT(c != 0);
    MT_EXPECT(w32k_get_window_rect(c, &rc));
    MT_EXPECT_EQ(rc.left, 105);
    MT_EXPECT_EQ(rc.top, 106);
    MT_EXPECT(w32k_get_parent(c) == p);
    /* A child created with no parent is rejected. */
    cs = make_cs((uintptr_t)nm, nm, W32K_WS_CHILD, 0, 0, 1, 1, 0);
    MT_EXPECT(w32k_create_window(&cs) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
    MT_EXPECT(w32k_destroy_window(p));
    MT_EXPECT(!w32k_is_window(c)); /* destroyed with its parent */
}

static void test_move_window_repositions_children(void)
{
    w32k_init();
    WSTR(nm, "Mv");
    w32k_handle p, c;
    w32k_rect rc;
    w32k_createwnd cs;
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, nm, 0, 0, 0, 200, 200, 0);
    p = w32k_create_window(&cs);
    cs = make_cs((uintptr_t)nm, nm, W32K_WS_CHILD, 10, 10, 20, 20, (uintptr_t)p);
    c = w32k_create_window(&cs);
    MT_ASSERT(p && c);
    MT_EXPECT(w32k_move_window(p, 50, 60, 200, 200, 0));
    MT_EXPECT(w32k_get_window_rect(c, &rc));
    MT_EXPECT_EQ(rc.left, 60);
    MT_EXPECT_EQ(rc.top, 70);
    MT_EXPECT(w32k_move_window(c, 1, 2, 3, 4, 0));
    MT_EXPECT(w32k_get_window_rect(c, &rc));
    MT_EXPECT_EQ(rc.left, 51);
    MT_EXPECT_EQ(rc.top, 62);
    MT_EXPECT_EQ(rc.right - rc.left, 3);
}

static void test_stale_handle_rejected_after_reuse(void)
{
    w32k_init();
    WSTR(nm, "Stale");
    w32k_handle a, b;
    w32k_createwnd cs;
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, nm, 0, 0, 0, 10, 10, 0);
    a = w32k_create_window(&cs);
    MT_ASSERT(a != 0);
    MT_EXPECT(w32k_destroy_window(a));
    b = w32k_create_window(&cs);
    MT_ASSERT(b != 0);
    /* Same slot, different generation: the old handle must not resolve. */
    MT_EXPECT(a != b);
    MT_EXPECT(!w32k_is_window(a));
    MT_EXPECT(w32k_is_window(b));
    MT_EXPECT(w32k_destroy_window(a) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_WINDOW_HANDLE);
    MT_EXPECT(w32k_destroy_window(b));
    /* A forged handle with the wrong type tag is rejected. */
    MT_EXPECT(!w32k_is_window(b ^ 0x01000000u));
}

static void test_window_properties(void)
{
    w32k_init();
    WSTR(nm, "Props");
    w32k_handle w;
    w32k_createwnd cs;
    uint16_t buf[32];
    MT_ASSERT(register_rec("", nm, 16) != 0);
    cs = make_cs((uintptr_t)nm, nm, W32K_WS_VISIBLE, 0, 0, 10, 10, 0);
    cs.menu = 42;
    cs.param = (void *)0x1234;
    w = w32k_create_window(&cs);
    MT_ASSERT(w != 0);
    MT_EXPECT_EQ(w32k_get_window_long(w, W32K_GWLP_USERDATA), 0);
    MT_EXPECT_EQ(w32k_set_window_long(w, W32K_GWLP_USERDATA, 0x55), 0);
    MT_EXPECT_EQ(w32k_get_window_long(w, W32K_GWLP_USERDATA), 0x55);
    MT_EXPECT_EQ(w32k_get_window_long(w, W32K_GWL_STYLE), (intptr_t)(W32K_WS_VISIBLE));
    MT_EXPECT_EQ(w32k_set_window_long(w, W32K_GWL_STYLE, 0x123), (intptr_t)W32K_WS_VISIBLE);
    MT_EXPECT_EQ(w32k_get_window_long(w, W32K_GWL_STYLE), 0x123);
    /* Extra bytes: 16 bytes on this class, room for two pointer-sized slots. */
    MT_EXPECT_EQ(w32k_set_window_long(w, 8, 0x7777), 0);
    MT_EXPECT_EQ(w32k_get_window_long(w, 8), 0x7777);
    MT_EXPECT(w32k_get_window_long(w, 16) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_INDEX);
    MT_EXPECT(w32k_get_window_long(w, W32K_GWL_WNDPROC) == (intptr_t)rec_proc);
    MT_EXPECT(w32k_set_window_text(w, nm));
    MT_EXPECT_EQ(w32k_get_window_text(w, buf, 32), 5);
    MT_EXPECT(weq(buf, nm));
    MT_EXPECT_EQ(w32k_get_window_text(w, buf, 3), 2);
    MT_EXPECT_EQ(buf[2], 0);
    MT_EXPECT_EQ(w32k_get_window_long(w, W32K_GWL_ID), 0);
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_find_window_top_level(void)
{
    w32k_init();
    WSTR(nm, "Findme");
    w32k_handle a, b;
    w32k_createwnd cs;
    WSTR(title, "Hello");
    WSTR(hello_lc, "hello");
    uint16_t buf_nope[16];
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, title, 0, 0, 0, 10, 10, 0);
    a = w32k_create_window(&cs);
    cs = make_cs((uintptr_t)nm, nm, 0, 0, 0, 10, 10, 0);
    b = w32k_create_window(&cs);
    MT_ASSERT(a && b);
    MT_EXPECT(w32k_find_window((uintptr_t)nm, hello_lc) == a);
    MT_EXPECT(w32k_find_window((uintptr_t)nm, NULL) == b); /* topmost first */
    MT_EXPECT(w32k_find_window((uintptr_t)nm, title) == a);
    MT_EXPECT(w32k_find_window((uintptr_t)nm, to_w("nope", buf_nope)) == 0);
    MT_EXPECT(w32k_destroy_window(a));
    MT_EXPECT(w32k_destroy_window(b));
}

/* ---- Message order ----------------------------------------------------------- */

static void test_create_visible_message_order(void)
{
    w32k_init();
    WSTR(nm, "Order");
    w32k_handle w;
    w32k_createwnd cs;
    static const uint32_t want[] = {
        W32K_WM_NCCREATE,         W32K_WM_NCCALCSIZE,      W32K_WM_CREATE,
        W32K_WM_SHOWWINDOW,       W32K_WM_WINDOWPOSCHANGING, W32K_WM_WINDOWPOSCHANGED,
        W32K_WM_MOVE,             W32K_WM_SIZE,            W32K_WM_ACTIVATE,
        W32K_WM_SETFOCUS,
    };
    MT_ASSERT(register_rec("", nm, 0) != 0);
    log_reset();
    cs = make_cs((uintptr_t)nm, nm, W32K_WS_VISIBLE, 0, 0, 100, 100, 0);
    w = w32k_create_window(&cs);
    MT_ASSERT(w != 0);
    MT_EXPECT(log_equals(want, (int)(sizeof(want) / sizeof(want[0]))));
    MT_EXPECT(w32k_is_window_visible(w));
    MT_EXPECT(w32k_get_focus() == w);
    MT_EXPECT(w32k_get_active_window() == w);
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_destroy_message_order_with_child(void)
{
    w32k_init();
    WSTR(nm, "Tree");
    w32k_handle p, c;
    w32k_createwnd cs;
    static const uint32_t want[] = {
        W32K_WM_KILLFOCUS, W32K_WM_ACTIVATE, W32K_WM_DESTROY, /* parent */
        W32K_WM_DESTROY, W32K_WM_NCDESTROY,                   /* child  */
        W32K_WM_NCDESTROY,                                    /* parent */
    };
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, nm, W32K_WS_VISIBLE, 0, 0, 100, 100, 0);
    p = w32k_create_window(&cs);
    cs = make_cs((uintptr_t)nm, nm, W32K_WS_CHILD, 0, 0, 10, 10, (uintptr_t)p);
    c = w32k_create_window(&cs);
    MT_ASSERT(p && c);
    /* Give the parent focus and activation, then destroy: child gets no focus. */
    MT_EXPECT(w32k_get_focus() == p); /* shown top-level: it holds focus and activation */
    log_reset();
    MT_EXPECT(w32k_destroy_window(p));
    MT_EXPECT(log_equals(want, (int)(sizeof(want) / sizeof(want[0]))));
    MT_EXPECT(!w32k_is_window(c));
}

static void test_hidden_window_gets_no_paint_until_shown(void)
{
    w32k_init();
    WSTR(nm, "Paint");
    w32k_handle w;
    w32k_createwnd cs;
    w32k_msg m;
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, nm, 0, 0, 0, 100, 100, 0);
    w = w32k_create_window(&cs);
    MT_ASSERT(w != 0);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == W32K_WOULD_BLOCK);
    MT_EXPECT(w32k_show_window(w, W32K_SW_SHOW) == 0);
    /* Shown: one WM_PAINT is produced by GetMessage, then the queue is empty. */
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT_EQ(m.message, W32K_WM_PAINT);
    MT_EXPECT(m.hwnd == w);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == W32K_WOULD_BLOCK);
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_def_window_proc_close_destroys(void)
{
    w32k_init();
    WSTR(nm, "Close");
    w32k_handle w;
    w32k_createwnd cs;
    MT_ASSERT(register_rec("", nm, 0) != 0);
    cs = make_cs((uintptr_t)nm, nm, 0, 0, 0, 10, 10, 0);
    w = w32k_create_window(&cs);
    MT_ASSERT(w != 0);
    MT_EXPECT(w32k_send_message(w, W32K_WM_CLOSE, 0, 0) == 0);
    MT_EXPECT(!w32k_is_window(w));
}

static void test_unregistered_class_fails_create(void)
{
    w32k_init();
    WSTR(nm, "NoSuchClass");
    w32k_createwnd cs = make_cs((uintptr_t)nm, nm, 0, 0, 0, 10, 10, 0);
    MT_EXPECT(w32k_create_window(&cs) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_CLASS_DOES_NOT_EXIST);
}

static void test_destroy_thread_releases_windows(void)
{
    w32k_init();
    WSTR(nm, "Thr");
    w32k_handle w;
    w32k_createwnd cs;
    MT_ASSERT(register_rec("", nm, 0) != 0);
    MT_ASSERT(w32k_attach_thread(7));
    {
        uint32_t prev = w32k_set_current_thread(7);
        cs = make_cs((uintptr_t)nm, nm, 0, 0, 0, 10, 10, 0);
        w = w32k_create_window(&cs);
        MT_ASSERT(w != 0);
        MT_EXPECT_EQ(w32k_get_window_thread(w), 7);
        w32k_set_current_thread(prev);
    }
    w32k_detach_thread(7);
    MT_EXPECT(!w32k_is_window(w));
}

int main(void)
{
    static const mt_case_t cases[] = {
        {"class_register_and_lookup", test_class_register_and_lookup},
        {"class_duplicate_rejected", test_class_duplicate_rejected},
        {"class_invalid_params", test_class_invalid_params},
        {"class_unregister_rules", test_class_unregister_rules},
        {"class_registry_capacity", test_class_registry_capacity_and_reuse},
        {"create_window_geometry", test_create_window_basic_geometry},
        {"create_default_position_size", test_create_default_position_and_size},
        {"create_child_parent_relative", test_create_child_is_parent_relative},
        {"move_window_children", test_move_window_repositions_children},
        {"stale_handle_rejected", test_stale_handle_rejected_after_reuse},
        {"window_properties", test_window_properties},
        {"find_window_top_level", test_find_window_top_level},
        {"create_visible_message_order", test_create_visible_message_order},
        {"destroy_message_order_child", test_destroy_message_order_with_child},
        {"hidden_window_paint", test_hidden_window_gets_no_paint_until_shown},
        {"def_proc_close_destroys", test_def_window_proc_close_destroys},
        {"unregistered_class_fails", test_unregistered_class_fails_create},
        {"destroy_thread_releases", test_destroy_thread_releases_windows},
    };
    return mt_run("test_w32k_window", cases, sizeof(cases) / sizeof(cases[0]));
}
