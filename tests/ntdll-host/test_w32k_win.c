/* test_w32k_win.c - window state and tree queries (w32k_win.c): show state, enable state,
 * GetWindow relations, the desktop window, class names, update region helpers,
 * enumeration and the cursor counter. */
#include "minitest.h"
#include "w32k.h"

#include <string.h>

static int weq(const uint16_t *a, const char *b)
{
    for (size_t i = 0;; ++i) {
        if (a[i] != (uint16_t)(unsigned char)b[i])
            return 0;
        if (!b[i])
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

static int paints, enables, last_enable;

static intptr_t proc(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    if (msg == W32K_WM_PAINT)
        ++paints;
    if (msg == W32K_WM_ENABLE) {
        ++enables;
        last_enable = (int)wp;
    }
    return w32k_def_window_proc(hwnd, msg, wp, lp);
}

static uintptr_t register_class(const char *name)
{
    uint16_t nm[32];
    w32k_wndclass wc;
    memset(&wc, 0, sizeof(wc));
    wc.name = to_w(name, nm);
    wc.proc = proc;
    {
        uint16_t atom = w32k_register_class(&wc);
        MT_EXPECT(atom != 0);
        return atom;
    }
}

static w32k_handle make_window(uintptr_t cls, const char *title, uint32_t style, uintptr_t parent)
{
    uint16_t t[32];
    w32k_createwnd cs;
    memset(&cs, 0, sizeof(cs));
    cs.cls = cls;
    cs.name = to_w(title, t);
    cs.style = style;
    cs.x = 10;
    cs.y = 20;
    cs.cx = 100;
    cs.cy = 50;
    cs.parent = parent;
    return w32k_create_window(&cs);
}

static void show_state(void)
{
    w32k_handle w;
    uintptr_t cls;
    w32k_init();
    cls = register_class("S");
    w = make_window(cls, "w", W32K_WS_VISIBLE, 0);
    MT_ASSERT(w != 0);
    MT_EXPECT_EQ(w32k_get_window_state(w), W32K_SW_SHOWNORMAL);
    w32k_show_window(w, W32K_SW_SHOWMAXIMIZED);
    MT_EXPECT_EQ(w32k_get_window_state(w), W32K_SW_SHOWMAXIMIZED);
    w32k_show_window(w, W32K_SW_SHOWMINIMIZED);
    MT_EXPECT_EQ(w32k_get_window_state(w), W32K_SW_SHOWMINIMIZED);
    w32k_show_window(w, W32K_SW_RESTORE);
    MT_EXPECT_EQ(w32k_get_window_state(w), W32K_SW_SHOWNORMAL);
    MT_EXPECT_EQ(w32k_get_window_state(0x12345678), -1);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_WINDOW_HANDLE);
}

static void enable_state(void)
{
    w32k_handle w;
    uintptr_t cls;
    w32k_init();
    cls = register_class("E");
    w = make_window(cls, "w", 0, 0);
    enables = 0;
    MT_EXPECT(w32k_is_window_enabled(w));
    MT_EXPECT_EQ(w32k_enable_window(w, 0), 0); /* was enabled */
    MT_EXPECT(!w32k_is_window_enabled(w));
    MT_EXPECT_EQ(enables, 1);
    MT_EXPECT_EQ(last_enable, 0);
    MT_EXPECT_EQ(w32k_enable_window(w, 0), 1); /* already disabled: no message */
    MT_EXPECT_EQ(enables, 1);
    MT_EXPECT_EQ(w32k_enable_window(w, 1), 1);
    MT_EXPECT(w32k_is_window_enabled(w));
    MT_EXPECT_EQ(enables, 2);
    MT_EXPECT_EQ(last_enable, 1);
    MT_EXPECT_EQ(w32k_enable_window(w, 1), 0);
    MT_EXPECT_EQ(enables, 2);
    MT_EXPECT(!w32k_is_window_enabled(0x12345678));
    MT_EXPECT_EQ(w32k_enable_window(0x12345678, 1), -1);
}

/* Top-level windows are listed topmost first, and the newest is the topmost. */
static void get_window_relations(void)
{
    w32k_handle a, b, c, k1, k2;
    uintptr_t cls;
    w32k_init();
    cls = register_class("R");
    a = make_window(cls, "a", 0, 0);
    b = make_window(cls, "b", 0, 0);
    c = make_window(cls, "c", 0, 0);
    MT_EXPECT_EQ(w32k_get_window_rel(c, W32K_GW_HWNDFIRST), c);
    MT_EXPECT_EQ(w32k_get_window_rel(c, W32K_GW_HWNDNEXT), b);
    MT_EXPECT_EQ(w32k_get_window_rel(b, W32K_GW_HWNDNEXT), a);
    MT_EXPECT_EQ(w32k_get_window_rel(a, W32K_GW_HWNDNEXT), 0);
    MT_EXPECT_EQ(w32k_get_window_rel(b, W32K_GW_HWNDPREV), c);
    MT_EXPECT_EQ(w32k_get_window_rel(c, W32K_GW_HWNDPREV), 0);
    MT_EXPECT_EQ(w32k_get_window_rel(b, W32K_GW_HWNDLAST), a);
    MT_EXPECT_EQ(w32k_get_window_rel(a, W32K_GW_OWNER), 0);
    MT_EXPECT_EQ(w32k_get_window_rel(a, W32K_GW_CHILD), 0);
    k1 = make_window(cls, "k1", W32K_WS_CHILD, a);
    k2 = make_window(cls, "k2", W32K_WS_CHILD, a);
    MT_ASSERT(k1 && k2);
    MT_EXPECT_EQ(w32k_get_window_rel(a, W32K_GW_CHILD), k2);
    MT_EXPECT_EQ(w32k_get_window_rel(k1, W32K_GW_HWNDFIRST), k2);
    MT_EXPECT_EQ(w32k_get_window_rel(k2, W32K_GW_HWNDNEXT), k1);
    MT_EXPECT_EQ(w32k_get_window_rel(a, 99), 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
}

static void desktop_window(void)
{
    w32k_handle d, w;
    w32k_rect rc;
    uint16_t name[16];
    uintptr_t cls;
    w32k_init();
    cls = register_class("D");
    d = w32k_get_desktop_window();
    MT_ASSERT(d != 0);
    MT_EXPECT_EQ(w32k_get_desktop_window(), d); /* the same handle every time */
    MT_EXPECT(w32k_is_window(d));
    MT_EXPECT(w32k_is_window_visible(d));
    MT_EXPECT(w32k_get_window_rect(d, &rc));
    MT_EXPECT_EQ(rc.left, 0);
    MT_EXPECT_EQ(rc.right, 1920);
    MT_EXPECT_EQ(rc.bottom, 1080);
    MT_EXPECT_EQ(w32k_get_class_name(d, name, 16), 6);
    MT_EXPECT(weq(name, "#32769"));
    MT_EXPECT_EQ(w32k_get_parent(d), 0);
    /* It is not a top-level window of the desktop's list. */
    w = make_window(cls, "w", 0, 0);
    MT_EXPECT_EQ(w32k_get_window_rel(w, W32K_GW_HWNDFIRST), w);
    MT_EXPECT_EQ(w32k_get_window_rel(w, W32K_GW_HWNDLAST), w);
    /* It cannot be destroyed. */
    MT_EXPECT(!w32k_destroy_window(d));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_ACCESS_DENIED);
    MT_EXPECT(w32k_is_window(d));
    /* A DC on it covers the screen. */
    {
        w32k_handle dc = w32k_get_dc(d);
        MT_EXPECT(dc != 0);
        if (dc)
            MT_EXPECT(w32k_release_dc(d, dc));
    }
}

static void class_names(void)
{
    uint16_t name[16];
    w32k_handle w;
    uintptr_t cls;
    w32k_init();
    cls = register_class("ClassNm");
    w = make_window(cls, "w", 0, 0);
    MT_EXPECT_EQ(w32k_get_class_name(w, name, 16), 7);
    MT_EXPECT(weq(name, "ClassNm"));
    /* A short buffer truncates and terminates. */
    MT_EXPECT_EQ(w32k_get_class_name(w, name, 4), 3);
    MT_EXPECT(weq(name, "Cla"));
    MT_EXPECT_EQ(w32k_get_class_name(w, name, 0), 0);
    MT_EXPECT_EQ(w32k_get_class_name(0x12345678, name, 16), 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_WINDOW_HANDLE);
}

static void update_region_helpers(void)
{
    w32k_handle w;
    w32k_rect rc;
    uintptr_t cls;
    w32k_init();
    cls = register_class("U");
    w = make_window(cls, "w", W32K_WS_VISIBLE, 0);
    paints = 0;
    /* Showing a window invalidates it. */
    MT_EXPECT(w32k_get_update_rect(w, &rc));
    MT_EXPECT_EQ(rc.right - rc.left, 100);
    MT_EXPECT_EQ(rc.bottom - rc.top, 50);
    MT_EXPECT(w32k_validate_rect(w));
    MT_EXPECT(!w32k_get_update_rect(w, &rc));
    MT_EXPECT_EQ(rc.right - rc.left, 0);
    /* UpdateWindow with nothing pending sends nothing. */
    MT_EXPECT(w32k_update_window(w));
    MT_EXPECT_EQ(paints, 0);
    /* With a pending region it sends WM_PAINT now, and clears the region. */
    w32k_invalidate_rect(w, NULL, 1);
    MT_EXPECT(w32k_update_window(w));
    MT_EXPECT_EQ(paints, 1);
    MT_EXPECT(!w32k_get_update_rect(w, &rc));
    {
        w32k_msg m;
        MT_EXPECT(!w32k_peek_message(&m, w, W32K_WM_PAINT, W32K_WM_PAINT, W32K_PM_REMOVE));
    }
    MT_EXPECT(!w32k_validate_rect(0x12345678));
    MT_EXPECT(!w32k_update_window(0x12345678));
    MT_EXPECT(!w32k_get_update_rect(0x12345678, &rc));
}

static int enum_count, enum_stop_after;
static w32k_handle enum_seen[16];

static int collect_cb(w32k_handle h, intptr_t lp)
{
    (void)lp;
    if (enum_count < 16)
        enum_seen[enum_count] = h;
    ++enum_count;
    return enum_stop_after < 0 || enum_count < enum_stop_after;
}

static int destroy_next_cb(w32k_handle h, intptr_t lp)
{
    w32k_handle victim = (w32k_handle)lp;
    ++enum_count;
    (void)h;
    if (victim)
        w32k_destroy_window(victim);
    return 1;
}

static void enumeration(void)
{
    w32k_handle a, b, c, k1, g1;
    uintptr_t cls;
    w32k_init();
    cls = register_class("En");
    a = make_window(cls, "a", 0, 0);
    b = make_window(cls, "b", 0, 0);
    c = make_window(cls, "c", 0, 0);
    k1 = make_window(cls, "k1", W32K_WS_CHILD, b);
    g1 = make_window(cls, "g1", W32K_WS_CHILD, k1);
    (void)a;
    /* Top level, topmost first, children not listed. */
    enum_count = 0;
    enum_stop_after = -1;
    MT_EXPECT(w32k_enum_windows(0, collect_cb, 0));
    MT_EXPECT_EQ(enum_count, 3);
    MT_EXPECT_EQ(enum_seen[0], c);
    MT_EXPECT_EQ(enum_seen[1], b);
    MT_EXPECT_EQ(enum_seen[2], a);
    /* The callback can stop the walk. */
    enum_count = 0;
    enum_stop_after = 2;
    MT_EXPECT(!w32k_enum_windows(0, collect_cb, 0));
    MT_EXPECT_EQ(enum_count, 2);
    /* Children are walked depth first, grandchildren included. */
    enum_count = 0;
    enum_stop_after = -1;
    MT_EXPECT(w32k_enum_windows(b, collect_cb, 0));
    MT_EXPECT_EQ(enum_count, 2);
    MT_EXPECT_EQ(enum_seen[0], k1);
    MT_EXPECT_EQ(enum_seen[1], g1);
    /* A window with no children enumerates nothing and succeeds. */
    enum_count = 0;
    MT_EXPECT(w32k_enum_windows(a, collect_cb, 0));
    MT_EXPECT_EQ(enum_count, 0);
    MT_EXPECT(!w32k_enum_windows(0x12345678, collect_cb, 0));
    MT_EXPECT(!w32k_enum_windows(0, NULL, 0));
    /* A callback that destroys a window not yet visited: it is skipped, not called. */
    enum_count = 0;
    MT_EXPECT(w32k_enum_windows(0, destroy_next_cb, (intptr_t)a));
    MT_EXPECT_EQ(enum_count, 2);
    /* The desktop window is never listed. */
    (void)w32k_get_desktop_window();
    enum_count = 0;
    enum_stop_after = -1;
    MT_EXPECT(w32k_enum_windows(0, collect_cb, 0));
    MT_EXPECT_EQ(enum_count, 2);
}

static void cursor_counter(void)
{
    w32k_init();
    MT_EXPECT_EQ(w32k_show_cursor(0), -1);
    MT_EXPECT_EQ(w32k_show_cursor(0), -2);
    MT_EXPECT_EQ(w32k_show_cursor(1), -1);
    MT_EXPECT_EQ(w32k_show_cursor(1), 0);
    MT_EXPECT_EQ(w32k_show_cursor(1), 1);
}

static void class_values(void)
{
    uint16_t nm[16];
    w32k_wndclass wc;
    w32k_handle w;
    intptr_t v = 0, old = 0;
    uint16_t atom;
    w32k_init();
    memset(&wc, 0, sizeof(wc));
    wc.name = to_w("CV", nm);
    wc.proc = proc;
    wc.style = 0x0003;
    wc.wnd_extra = 16;
    wc.bg_brush = 0x1234;
    wc.hinst = (const void *)0x400000;
    atom = w32k_register_class(&wc);
    w = make_window(atom, "w", 0, 0);
    MT_ASSERT(w != 0);
    MT_EXPECT(w32k_get_class_long(w, W32K_GCL_STYLE, &v));
    MT_EXPECT_EQ(v, 3);
    MT_EXPECT(w32k_get_class_long(w, W32K_GCL_CBWNDEXTRA, &v));
    MT_EXPECT_EQ(v, 16);
    MT_EXPECT(w32k_get_class_long(w, W32K_GCL_HBRBACKGROUND, &v));
    MT_EXPECT_EQ(v, 0x1234);
    MT_EXPECT(w32k_get_class_long(w, W32K_GCL_HMODULE, &v));
    MT_EXPECT_EQ(v, 0x400000);
    MT_EXPECT(w32k_get_class_long(w, W32K_GCL_WNDPROC, &v));
    MT_EXPECT_EQ(v, (intptr_t)proc);
    MT_EXPECT(w32k_get_class_long(w, W32K_GCW_ATOM, &v));
    MT_EXPECT_EQ(v, atom);
    MT_EXPECT(!w32k_get_class_long(w, 12, &v));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_INDEX);
    MT_EXPECT(w32k_set_class_long(w, W32K_GCL_HBRBACKGROUND, 0x77, &old));
    MT_EXPECT_EQ(old, 0x1234);
    MT_EXPECT(w32k_get_class_long(w, W32K_GCL_HBRBACKGROUND, &v));
    MT_EXPECT_EQ(v, 0x77);
    MT_EXPECT(!w32k_set_class_long(w, W32K_GCW_ATOM, 1, &old)); /* fixed at registration */
    MT_EXPECT(!w32k_get_class_long(0x12345678, W32K_GCL_STYLE, &v));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_WINDOW_HANDLE);
}

static void zorder(void)
{
    w32k_handle a, b, c;
    uintptr_t cls;
    w32k_init();
    cls = register_class("Z");
    a = make_window(cls, "a", 0, 0);
    b = make_window(cls, "b", 0, 0);
    c = make_window(cls, "c", 0, 0); /* order, topmost first: c b a */
    MT_EXPECT(w32k_set_zorder(a, 0));
    MT_EXPECT_EQ(w32k_get_window_rel(a, W32K_GW_HWNDFIRST), a); /* a c b */
    MT_EXPECT_EQ(w32k_get_window_rel(a, W32K_GW_HWNDNEXT), c);
    MT_EXPECT_EQ(w32k_get_window_rel(c, W32K_GW_HWNDNEXT), b);
    MT_EXPECT(w32k_set_zorder(a, 1)); /* c b a */
    MT_EXPECT_EQ(w32k_get_window_rel(a, W32K_GW_HWNDFIRST), c);
    MT_EXPECT_EQ(w32k_get_window_rel(a, W32K_GW_HWNDLAST), a);
    MT_EXPECT(w32k_set_zorder(c, 1)); /* b a c */
    MT_EXPECT_EQ(w32k_get_window_rel(c, W32K_GW_HWNDFIRST), b);
    MT_EXPECT_EQ(w32k_get_window_rel(b, W32K_GW_HWNDLAST), c);
    MT_EXPECT(!w32k_set_zorder(w32k_get_desktop_window(), 0));
    MT_EXPECT(!w32k_set_zorder(0x12345678, 0));
}

static void from_point(void)
{
    w32k_handle a, b, k;
    uintptr_t cls;
    w32k_init();
    cls = register_class("P");
    a = make_window(cls, "a", W32K_WS_VISIBLE, 0); /* 10,20 .. 110,70 */
    MT_EXPECT_EQ(w32k_window_from_point(50, 40), a);
    MT_EXPECT_EQ(w32k_window_from_point(500, 500), 0);
    b = make_window(cls, "b", W32K_WS_VISIBLE, 0); /* same rectangle, newer, on top */
    MT_EXPECT_EQ(w32k_window_from_point(50, 40), b);
    w32k_set_zorder(a, 0);
    MT_EXPECT_EQ(w32k_window_from_point(50, 40), a);
    k = make_window(cls, "k", W32K_WS_VISIBLE | W32K_WS_CHILD, a);
    MT_EXPECT_EQ(w32k_window_from_point(50, 40), k);
}

static void message_time_and_pos(void)
{
    uint16_t nm[8];
    w32k_wndclass wc;
    w32k_handle w;
    w32k_msg m;
    w32k_point pt;
    w32k_init();
    memset(&wc, 0, sizeof(wc));
    wc.name = to_w("MT", nm);
    wc.proc = proc;
    w = make_window(w32k_register_class(&wc), "w", 0, 0);
    MT_EXPECT_EQ(w32k_get_message_time(), 0);
    w32k_set_now(1234);
    w32k_set_cursor_pos(77, 88);
    MT_EXPECT(w32k_post_message(w, W32K_WM_USER, 0, 0));
    MT_EXPECT_EQ(w32k_get_message(&m, 0, 0, 0), 1);
    MT_EXPECT_EQ(w32k_get_message_time(), m.time);
    pt = w32k_get_message_pos();
    MT_EXPECT_EQ(pt.x, 77);
    MT_EXPECT_EQ(pt.y, 88);
}

static void monitors(void)
{
    w32k_rect mon, work, second = {1920, 0, 3840, 1080}, r;
    int primary = 0;
    w32k_handle h;
    w32k_init();
    MT_EXPECT(w32k_get_monitor_info(1, &mon, &work, &primary));
    MT_EXPECT_EQ(mon.right, 1920);
    MT_EXPECT(work.right == mon.right && work.bottom == mon.bottom);
    MT_EXPECT(primary);
    MT_EXPECT(!w32k_get_monitor_info(2, &mon, &work, &primary));
    MT_EXPECT(!w32k_get_monitor_info(0, NULL, NULL, NULL));
    MT_EXPECT_EQ(w32k_monitor_from_point(5, 5, W32K_MONITOR_DEFAULTTONULL), 1);
    MT_EXPECT_EQ(w32k_monitor_from_point(-50, 5, W32K_MONITOR_DEFAULTTONULL), 0);
    MT_EXPECT_EQ(w32k_monitor_from_point(-50, 5, W32K_MONITOR_DEFAULTTOPRIMARY), 1);
    MT_EXPECT_EQ(w32k_monitor_from_point(-50, 5, W32K_MONITOR_DEFAULTTONEAREST), 1);
    MT_EXPECT(w32k_set_monitor(1, &second, 96, 0));
    MT_EXPECT_EQ(w32k_monitor_from_point(2000, 10, W32K_MONITOR_DEFAULTTONULL), 2);
    MT_EXPECT_EQ(w32k_monitor_from_point(5000, 10, W32K_MONITOR_DEFAULTTONEAREST), 2);
    MT_EXPECT_EQ(w32k_monitor_from_point(-30, 10, W32K_MONITOR_DEFAULTTONEAREST), 1);
    /* A rectangle straddling both monitors belongs to the one with more of it. */
    r.left = 1800; r.top = 0; r.right = 2300; r.bottom = 100;
    h = w32k_monitor_from_rect(&r, W32K_MONITOR_DEFAULTTONULL);
    MT_EXPECT_EQ(h, 2);
    r.left = 1000; r.right = 2000;
    MT_EXPECT_EQ(w32k_monitor_from_rect(&r, W32K_MONITOR_DEFAULTTONULL), 1);
    r.left = 9000; r.right = 9100;
    MT_EXPECT_EQ(w32k_monitor_from_rect(&r, W32K_MONITOR_DEFAULTTONULL), 0);
    MT_EXPECT_EQ(w32k_monitor_from_rect(&r, W32K_MONITOR_DEFAULTTONEAREST), 2);
    MT_EXPECT_EQ(w32k_monitor_from_rect(NULL, W32K_MONITOR_DEFAULTTOPRIMARY), 0);
}

static const mt_case_t cases[] = {
    MT_CASE(show_state),
    MT_CASE(enable_state),
    MT_CASE(get_window_relations),
    MT_CASE(desktop_window),
    MT_CASE(class_names),
    MT_CASE(update_region_helpers),
    MT_CASE(enumeration),
    MT_CASE(cursor_counter),
    MT_CASE(class_values),
    MT_CASE(zorder),
    MT_CASE(from_point),
    MT_CASE(message_time_and_pos),
    MT_CASE(monitors),
};

int main(void)
{
    return MT_RUN("w32k_win", cases);
}
