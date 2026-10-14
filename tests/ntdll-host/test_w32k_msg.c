/* test_w32k_msg.c - message queues: post, send, get, peek, dispatch, translate,
 * quit, timers, focus, activation, capture and hooks (src/ntdll/win32k). */
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

static uint32_t rec_msg[256];
static uintptr_t rec_wp[256];
static intptr_t rec_lp[256];
static uint32_t rec_tid[256];
static int rec_n;

static void rec_reset(void)
{
    rec_n = 0;
}

static intptr_t rec_proc(w32k_handle hwnd, uint32_t msg, uintptr_t wp, intptr_t lp)
{
    (void)hwnd;
    if (rec_n < 256) {
        rec_msg[rec_n] = msg;
        rec_wp[rec_n] = wp;
        rec_lp[rec_n] = lp;
        rec_tid[rec_n] = w32k_current_thread();
        ++rec_n;
    }
    return w32k_def_window_proc(hwnd, msg, wp, lp);
}

static uint16_t test_class_register(void)
{
    static const uint16_t nm[] = {'M', 's', 'g', 0};
    w32k_wndclass wc;
    memset(&wc, 0, sizeof(wc));
    wc.name = nm;
    wc.proc = rec_proc;
    return w32k_register_class(&wc);
}

static w32k_handle make_window(uint32_t style)
{
    static const uint16_t nm[] = {'M', 's', 'g', 0};
    w32k_createwnd cs;
    memset(&cs, 0, sizeof(cs));
    cs.cls = (uintptr_t)nm;
    cs.name = nm;
    cs.style = style;
    cs.cx = 100;
    cs.cy = 100;
    return w32k_create_window(&cs);
}

static w32k_handle make_hidden(void)
{
    return make_window(0);
}

/* ---- Post, get, peek ---------------------------------------------------------- */

static void test_post_get_fifo_order(void)
{
    w32k_init();
    w32k_msg m;
    w32k_handle w;
    MT_ASSERT(test_class_register() != 0);
    w = make_hidden();
    MT_ASSERT(w != 0);
    MT_ASSERT(w32k_post_message(w, W32K_WM_USER + 1, 11, 0));
    MT_ASSERT(w32k_post_message(w, W32K_WM_USER + 2, 22, 0));
    MT_ASSERT(w32k_post_message(w, W32K_WM_USER + 3, 33, 0));
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT_EQ(m.message, W32K_WM_USER + 1);
    MT_EXPECT_EQ(m.wparam, 11);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT_EQ(m.message, W32K_WM_USER + 2);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT_EQ(m.message, W32K_WM_USER + 3);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == W32K_WOULD_BLOCK);
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_get_message_filters(void)
{
    w32k_init();
    w32k_msg m;
    w32k_handle a, b;
    MT_ASSERT(test_class_register() != 0);
    a = make_hidden();
    b = make_hidden();
    MT_ASSERT(a && b);
    MT_ASSERT(w32k_post_message(a, W32K_WM_USER + 1, 1, 0));
    MT_ASSERT(w32k_post_message(b, W32K_WM_USER + 2, 2, 0));
    MT_ASSERT(w32k_post_message(a, W32K_WM_USER + 3, 3, 0));
    /* Filter by window: only b's message is taken, the rest stay queued. */
    MT_EXPECT(w32k_peek_message(&m, b, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.wparam, 2);
    /* Filter by range: only WM_USER+3 matches. */
    MT_EXPECT(w32k_peek_message(&m, 0, W32K_WM_USER + 3, W32K_WM_USER + 3, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.wparam, 3);
    MT_EXPECT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.wparam, 1);
    MT_EXPECT(!w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT(w32k_destroy_window(a));
    MT_EXPECT(w32k_destroy_window(b));
}

static void test_peek_noremove_keeps_message(void)
{
    w32k_init();
    w32k_msg m;
    w32k_handle w;
    MT_ASSERT(test_class_register() != 0);
    w = make_hidden();
    MT_ASSERT(w32k_post_message(w, W32K_WM_USER, 7, 0));
    MT_EXPECT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_NOREMOVE));
    MT_EXPECT_EQ(m.wparam, 7);
    MT_EXPECT(w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE));
    MT_EXPECT_EQ(m.wparam, 7);
    MT_EXPECT(!w32k_peek_message(&m, 0, 0, 0, W32K_PM_NOREMOVE));
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_quit_message_ends_loop(void)
{
    w32k_init();
    w32k_msg m;
    MT_ASSERT(test_class_register() != 0);
    w32k_post_quit_message(42);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 0);
    MT_EXPECT_EQ(m.message, W32K_WM_QUIT);
    MT_EXPECT_EQ(m.wparam, 42);
}

static void test_post_thread_message_and_dispatch_rules(void)
{
    w32k_init();
    w32k_msg m;
    w32k_handle w;
    MT_ASSERT(test_class_register() != 0);
    rec_reset();
    /* Thread messages have no window and are never dispatched to a proc. */
    MT_ASSERT(w32k_post_thread_message(w32k_current_thread(), W32K_WM_USER + 9, 5, 6));
    MT_ASSERT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT(m.hwnd == 0);
    MT_EXPECT(w32k_dispatch_message(&m) == 0);
    MT_EXPECT_EQ(rec_n, 0);
    MT_EXPECT(!w32k_post_thread_message(999, W32K_WM_USER, 0, 0));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
    /* A window message is dispatched to the window procedure. Creation logs its own
     * messages first, so the log is cleared after the window exists. */
    w = make_hidden();
    MT_ASSERT(w != 0);
    rec_reset();
    MT_ASSERT(w32k_post_message(w, W32K_WM_USER + 4, 77, 88));
    MT_ASSERT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT(w32k_dispatch_message(&m) == 0);
    MT_EXPECT_EQ(rec_n, 1);
    MT_EXPECT_EQ(rec_msg[0], W32K_WM_USER + 4);
    MT_EXPECT_EQ(rec_wp[0], 77);
    MT_EXPECT_EQ(rec_lp[0], 88);
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_queue_capacity_is_enforced(void)
{
    w32k_init();
    w32k_handle w;
    int accepted = 0;
    MT_ASSERT(test_class_register() != 0);
    w = make_hidden();
    for (int i = 0; i < 300; ++i)
        if (w32k_post_message(w, W32K_WM_USER, (uintptr_t)i, 0))
            ++accepted;
    MT_EXPECT_EQ(accepted, 256);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_NOT_ENOUGH_QUOTA);
    MT_EXPECT(w32k_destroy_window(w));
}

/* ---- Send, cross-thread ------------------------------------------------------- */

static void test_send_message_returns_proc_result(void)
{
    w32k_init();
    w32k_handle w;
    MT_ASSERT(test_class_register() != 0);
    w = make_hidden();
    MT_ASSERT(w != 0);
    rec_reset();
    static uint16_t text[8];
    /* make_window names the window "Msg". */
    MT_EXPECT_EQ(w32k_send_message(w, W32K_WM_GETTEXTLENGTH, 0, 0), 3);
    MT_EXPECT_EQ(w32k_send_message(w, W32K_WM_SETTEXT, 0, (intptr_t)to_w("hello", text)), 1);
    MT_EXPECT_EQ(w32k_send_message(w, W32K_WM_GETTEXTLENGTH, 0, 0), 5);
    MT_EXPECT(w32k_send_message(0, W32K_WM_USER, 0, 0) == 0);
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_cross_thread_send_runs_on_owner(void)
{
    w32k_init();
    w32k_handle w;
    uint32_t prev;
    MT_ASSERT(test_class_register() != 0);
    MT_ASSERT(w32k_attach_thread(5));
    prev = w32k_set_current_thread(5);
    w = make_hidden();
    w32k_set_current_thread(prev);
    MT_ASSERT(w != 0);
    rec_reset();
    /* Sent from thread 1: the owner's procedure runs with thread 5 current. */
    MT_EXPECT_EQ(w32k_send_message(w, W32K_WM_USER, 1, 2), 0);
    MT_EXPECT_EQ(rec_n, 1);
    MT_EXPECT_EQ(rec_tid[0], 5);
    MT_EXPECT_EQ(w32k_current_thread(), 1);
    /* Destroying a window owned by another thread is allowed; detaching thread 5 is then a no-op. */
    MT_EXPECT(w32k_destroy_window(w));
    w32k_detach_thread(5);
    MT_EXPECT(!w32k_is_window(w));
}

/* ---- Timers ------------------------------------------------------------------- */

static int timer_fired;
static uintptr_t timer_fired_id;

static void timer_cb(w32k_handle hwnd, uint32_t msg, uintptr_t id, uint32_t t)
{
    (void)hwnd;
    (void)msg;
    (void)t;
    ++timer_fired;
    timer_fired_id = id;
}

static void test_timer_fires_when_due(void)
{
    w32k_init();
    w32k_msg m;
    w32k_handle w;
    uintptr_t id;
    MT_ASSERT(test_class_register() != 0);
    w = make_hidden();
    w32k_set_now(1000);
    id = w32k_set_timer(w, 7, 50, NULL);
    MT_EXPECT_EQ(id, 7);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == W32K_WOULD_BLOCK);
    w32k_set_now(1049);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == W32K_WOULD_BLOCK);
    w32k_set_now(1050);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT_EQ(m.message, W32K_WM_TIMER);
    MT_EXPECT_EQ(m.wparam, 7);
    MT_EXPECT(m.hwnd == w);
    /* Rescheduled one period later. */
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == W32K_WOULD_BLOCK);
    MT_EXPECT(w32k_kill_timer(w, 7));
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_timer_priority_after_posted_and_paint(void)
{
    w32k_init();
    w32k_msg m;
    w32k_handle w;
    MT_ASSERT(test_class_register() != 0);
    w = make_window(W32K_WS_VISIBLE);
    w32k_set_now(0);
    MT_ASSERT(w32k_set_timer(w, 1, 10, NULL) == 1);
    w32k_set_now(100);
    MT_ASSERT(w32k_post_message(w, W32K_WM_USER, 0, 0));
    /* Posted first, then paint, then the due timer. */
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT_EQ(m.message, W32K_WM_USER);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT_EQ(m.message, W32K_WM_PAINT);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT_EQ(m.message, W32K_WM_TIMER);
    MT_EXPECT(w32k_kill_timer(w, 1));
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_timer_proc_dispatch_and_kill_purges(void)
{
    w32k_init();
    w32k_msg m;
    w32k_handle w;
    MT_ASSERT(test_class_register() != 0);
    w = make_hidden();
    timer_fired = 0;
    timer_fired_id = 0;
    w32k_set_now(0);
    MT_ASSERT(w32k_set_timer(w, 3, 10, timer_cb) == 3);
    w32k_set_now(10);
    MT_ASSERT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT(w32k_dispatch_message(&m) == 0);
    MT_EXPECT_EQ(timer_fired, 1);
    MT_EXPECT_EQ(timer_fired_id, 3);
    /* A timer message already queued is removed by KillTimer. */
    w32k_set_now(20);
    MT_EXPECT(w32k_kill_timer(w, 3) == 1);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == W32K_WOULD_BLOCK);
    MT_EXPECT(w32k_kill_timer(w, 3) == 0);
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_thread_timer_has_no_window(void)
{
    w32k_init();
    w32k_msg m;
    uintptr_t id;
    MT_ASSERT(test_class_register() != 0);
    w32k_set_now(0);
    id = w32k_set_timer(0, 0, 10, timer_cb);
    MT_EXPECT(id != 0);
    w32k_set_now(10);
    MT_ASSERT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT(m.hwnd == 0);
    MT_EXPECT_EQ(m.message, W32K_WM_TIMER);
    timer_fired = 0;
    MT_EXPECT(w32k_dispatch_message(&m) == 0);
    MT_EXPECT_EQ(timer_fired, 1);
    MT_EXPECT(w32k_kill_timer(0, id));
}

static void test_timer_table_capacity(void)
{
    w32k_init();
    w32k_handle w;
    int ok = 0;
    MT_ASSERT(test_class_register() != 0);
    w = make_hidden();
    for (uintptr_t i = 1; i <= 40; ++i)
        if (w32k_set_timer(w, i, 10, NULL) == i)
            ++ok;
    MT_EXPECT_EQ(ok, 32);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_NOT_ENOUGH_MEMORY);
    MT_EXPECT(w32k_destroy_window(w));
}

/* ---- Focus, activation, capture ----------------------------------------------- */

static void test_focus_messages_order(void)
{
    w32k_init();
    w32k_handle a, b;
    MT_ASSERT(test_class_register() != 0);
    a = make_window(0);
    b = make_window(0);
    MT_ASSERT(a && b);
    rec_reset();
    MT_EXPECT(w32k_set_focus(a) == 0);
    MT_EXPECT_EQ(rec_n, 1);
    MT_EXPECT_EQ(rec_msg[0], W32K_WM_SETFOCUS);
    rec_reset();
    MT_EXPECT(w32k_set_focus(b) == a);
    MT_EXPECT_EQ(rec_n, 2);
    MT_EXPECT_EQ(rec_msg[0], W32K_WM_KILLFOCUS);
    MT_EXPECT_EQ(rec_msg[1], W32K_WM_SETFOCUS);
    MT_EXPECT(w32k_get_focus() == b);
    MT_EXPECT(w32k_destroy_window(a));
    MT_EXPECT(w32k_destroy_window(b));
}

static void test_activation_moves_focus_with_it(void)
{
    w32k_init();
    w32k_handle a, b;
    MT_ASSERT(test_class_register() != 0);
    a = make_window(W32K_WS_VISIBLE);
    b = make_window(0);
    MT_ASSERT(a && b);
    MT_EXPECT(w32k_get_active_window() == a);
    MT_EXPECT(w32k_get_focus() == a);
    rec_reset();
    MT_EXPECT(w32k_set_active_window(b) == a);
    /* Old window deactivates, its focus is lost, new window activates and gets focus. */
    MT_EXPECT_EQ(rec_n, 4);
    MT_EXPECT_EQ(rec_msg[0], W32K_WM_ACTIVATE);
    MT_EXPECT_EQ(rec_wp[0], W32K_WA_INACTIVE);
    MT_EXPECT_EQ(rec_msg[1], W32K_WM_KILLFOCUS);
    MT_EXPECT_EQ(rec_msg[2], W32K_WM_ACTIVATE);
    MT_EXPECT_EQ(rec_wp[2], W32K_WA_ACTIVE);
    MT_EXPECT_EQ(rec_msg[3], W32K_WM_SETFOCUS);
    MT_EXPECT(w32k_get_focus() == b);
    /* A child window cannot be made active: the call fails and activation stays. */
    {
        static const uint16_t nm[] = {'M', 's', 'g', 0};
        w32k_createwnd cs;
        w32k_handle c;
        memset(&cs, 0, sizeof(cs));
        cs.cls = (uintptr_t)nm;
        cs.name = nm;
        cs.style = W32K_WS_CHILD;
        cs.parent = b;
        c = w32k_create_window(&cs);
        MT_ASSERT(c != 0);
        MT_EXPECT(w32k_set_active_window(c) == 0);
        MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
        MT_EXPECT(w32k_get_active_window() == b);
    }
    MT_EXPECT(w32k_destroy_window(a));
    MT_EXPECT(w32k_destroy_window(b));
}

static void test_destroy_active_activates_next(void)
{
    w32k_init();
    w32k_handle a, b;
    MT_ASSERT(test_class_register() != 0);
    a = make_window(W32K_WS_VISIBLE);
    b = make_window(W32K_WS_VISIBLE); /* newer: topmost, and active */
    MT_ASSERT(a && b);
    MT_EXPECT(w32k_get_active_window() == b);
    MT_EXPECT(w32k_destroy_window(b));
    MT_EXPECT(w32k_get_active_window() == a);
    MT_EXPECT(w32k_get_focus() == a);
    MT_EXPECT(w32k_destroy_window(a));
}

static void test_capture_messages(void)
{
    w32k_init();
    w32k_handle a, b;
    MT_ASSERT(test_class_register() != 0);
    a = make_window(0);
    b = make_window(0);
    MT_ASSERT(a && b);
    MT_EXPECT(w32k_get_capture() == 0);
    MT_EXPECT(w32k_set_capture(a) == 0);
    MT_EXPECT(w32k_get_capture() == a);
    rec_reset();
    /* Taking capture from a sends WM_CAPTURECHANGED to the old owner. */
    MT_EXPECT(w32k_set_capture(b) == a);
    MT_EXPECT_EQ(rec_n, 1);
    MT_EXPECT_EQ(rec_msg[0], W32K_WM_CAPTURECHANGED);
    MT_EXPECT(w32k_get_capture() == b);
    MT_EXPECT(w32k_release_capture());
    MT_EXPECT(w32k_get_capture() == 0);
    MT_EXPECT(!w32k_release_capture());
    MT_EXPECT(w32k_destroy_window(a));
    MT_EXPECT(w32k_destroy_window(b));
}

static void test_cursor_objects_per_queue(void)
{
    w32k_init();
    w32k_handle arrow = w32k_load_cursor(W32K_IDC_ARROW);
    w32k_handle cross = w32k_load_cursor(W32K_IDC_CROSS);
    w32k_handle mine;
    MT_ASSERT(arrow != 0 && cross != 0 && arrow != cross);
    MT_EXPECT(w32k_load_cursor(12345) == 0);
    MT_EXPECT_EQ(w32k_set_cursor(arrow), 0);
    MT_EXPECT(w32k_get_cursor() == arrow);
    MT_EXPECT(w32k_set_cursor(cross) == arrow);
    mine = w32k_create_cursor(1, 1, 16, 16);
    MT_ASSERT(mine != 0);
    MT_EXPECT(w32k_set_cursor(mine) == cross);
    MT_EXPECT(w32k_destroy_cursor(arrow) == 0); /* stock objects are not destroyed */
    MT_EXPECT(w32k_destroy_cursor(mine));
    MT_EXPECT(w32k_get_cursor() == 0); /* destroying the current cursor clears it */
}

/* ---- Hooks -------------------------------------------------------------------- */

static int hook_getmsg_calls;
static int hook_callwnd_calls;

static intptr_t getmsg_hook(int code, uintptr_t wp, intptr_t lp)
{
    (void)code;
    (void)wp;
    (void)lp;
    ++hook_getmsg_calls;
    return 0;
}

static intptr_t callwnd_hook(int code, uintptr_t wp, intptr_t lp)
{
    (void)code;
    (void)wp;
    (void)lp;
    ++hook_callwnd_calls;
    return 0;
}

static void test_hook_registration_and_calls(void)
{
    w32k_init();
    w32k_handle w, h1, h2;
    w32k_msg m;
    MT_ASSERT(test_class_register() != 0);
    w = make_hidden();
    h1 = w32k_set_windows_hook(W32K_WH_GETMESSAGE, getmsg_hook, 0);
    h2 = w32k_set_windows_hook(W32K_WH_CALLWNDPROC, callwnd_hook, 0);
    MT_ASSERT(h1 != 0 && h2 != 0);
    MT_EXPECT(w32k_set_windows_hook(999, getmsg_hook, 0) == 0);
    MT_EXPECT(w32k_set_windows_hook(W32K_WH_GETMESSAGE, NULL, 0) == 0);
    hook_getmsg_calls = 0;
    hook_callwnd_calls = 0;
    MT_ASSERT(w32k_post_message(w, W32K_WM_USER, 0, 0));
    MT_ASSERT(w32k_get_message(&m, 0, 0, 0) == 1);
    MT_EXPECT_EQ(hook_getmsg_calls, 1);
    MT_EXPECT(w32k_send_message(w, W32K_WM_USER, 0, 0) == 0);
    MT_EXPECT_EQ(hook_callwnd_calls, 1);
    MT_EXPECT(w32k_unhook_windows_hook(h1));
    MT_EXPECT(!w32k_unhook_windows_hook(h1));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_HOOK_HANDLE);
    MT_EXPECT(w32k_unhook_windows_hook(h2));
    MT_EXPECT(w32k_call_next_hook(h2, 0, 0, 0) == 0);
    MT_EXPECT(w32k_destroy_window(w));
}

/* Mirrors tests/kernel/s12/console_s12.c call for call: create a visible window,
 * post two messages, pump with PeekMessage/DispatchMessage, destroy, quit. The
 * expected order is the one the S12 probe checks. */
static void test_s12_probe_sequence(void)
{
    w32k_init();
    static const uint32_t want[] = {
        W32K_WM_NCCREATE, W32K_WM_NCCALCSIZE, W32K_WM_CREATE,
        W32K_WM_SHOWWINDOW, W32K_WM_WINDOWPOSCHANGING, W32K_WM_WINDOWPOSCHANGED, W32K_WM_MOVE, W32K_WM_SIZE,
        W32K_WM_ACTIVATE, W32K_WM_SETFOCUS,
        W32K_WM_USER + 1, W32K_WM_USER + 2, W32K_WM_PAINT,
        W32K_WM_KILLFOCUS, W32K_WM_ACTIVATE, W32K_WM_DESTROY, W32K_WM_NCDESTROY,
    };
    w32k_msg m;
    w32k_handle w;
    int pumped = 0;
    MT_ASSERT(test_class_register() != 0);
    rec_reset();
    w = make_window(W32K_WS_VISIBLE);
    MT_ASSERT(w != 0);
    MT_ASSERT(w32k_post_message(w, W32K_WM_USER + 1, 0, 0));
    MT_ASSERT(w32k_post_message(w, W32K_WM_USER + 2, 0, 0));
    while (w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE)) {
        w32k_translate_message(&m);
        w32k_dispatch_message(&m);
        ++pumped;
    }
    MT_EXPECT_EQ(pumped, 3);
    MT_EXPECT(w32k_destroy_window(w));
    MT_EXPECT_EQ(rec_n, (int)(sizeof(want) / sizeof(want[0])));
    for (int i = 0; i < rec_n && i < (int)(sizeof(want) / sizeof(want[0])); ++i)
        MT_EXPECT_EQ(rec_msg[i], want[i]);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == W32K_WOULD_BLOCK);
    w32k_post_quit_message(0);
    MT_EXPECT(w32k_get_message(&m, 0, 0, 0) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const mt_case_t cases[] = {
        {"post_get_fifo_order", test_post_get_fifo_order},
        {"get_message_filters", test_get_message_filters},
        {"peek_noremove_keeps", test_peek_noremove_keeps_message},
        {"quit_message_ends_loop", test_quit_message_ends_loop},
        {"thread_message_dispatch_rules", test_post_thread_message_and_dispatch_rules},
        {"queue_capacity", test_queue_capacity_is_enforced},
        {"send_returns_proc_result", test_send_message_returns_proc_result},
        {"cross_thread_send_owner", test_cross_thread_send_runs_on_owner},
        {"timer_fires_when_due", test_timer_fires_when_due},
        {"timer_priority_order", test_timer_priority_after_posted_and_paint},
        {"timer_proc_dispatch_kill", test_timer_proc_dispatch_and_kill_purges},
        {"thread_timer_no_window", test_thread_timer_has_no_window},
        {"timer_table_capacity", test_timer_table_capacity},
        {"focus_messages_order", test_focus_messages_order},
        {"activation_moves_focus", test_activation_moves_focus_with_it},
        {"destroy_active_activates_next", test_destroy_active_activates_next},
        {"capture_messages", test_capture_messages},
        {"cursor_objects_per_queue", test_cursor_objects_per_queue},
        {"hook_registration_calls", test_hook_registration_and_calls},
        {"s12_probe_sequence", test_s12_probe_sequence},
    };
    return mt_run("test_w32k_msg", cases, sizeof(cases) / sizeof(cases[0]));
}
