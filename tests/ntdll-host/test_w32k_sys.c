/* test_w32k_sys.c - clipboard, system metrics and parameters, monitors, display
 * settings, window stations and desktops, cursor and icon objects. */
#include "minitest.h"
#include "w32k.h"

#include <string.h>


static uint16_t reg_class(void)
{
    static const uint16_t nm[] = {'S', 'y', 's', 0};
    w32k_wndclass wc;
    memset(&wc, 0, sizeof(wc));
    wc.name = nm;
    wc.proc = w32k_def_window_proc;
    return w32k_register_class(&wc);
}

static w32k_handle make_win(int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t style)
{
    static const uint16_t nm[] = {'S', 'y', 's', 0};
    w32k_createwnd cs;
    reg_class(); /* a duplicate registration fails harmlessly */
    memset(&cs, 0, sizeof(cs));
    cs.cls = (uintptr_t)nm;
    cs.name = nm;
    cs.style = style;
    cs.x = x;
    cs.y = y;
    cs.cx = cx;
    cs.cy = cy;
    return w32k_create_window(&cs);
}

/* ---- Clipboard --------------------------------------------------------------- */

static void test_clipboard_requires_open(void)
{
    static const char text[] = "hi";
    w32k_init();
    MT_EXPECT(!w32k_close_clipboard());
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_CLIPBOARD_NOT_OPEN);
    MT_EXPECT(!w32k_set_clipboard_data(1, text, 2));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_CLIPBOARD_NOT_OPEN);
    MT_EXPECT(!w32k_empty_clipboard());
    MT_EXPECT(w32k_get_clipboard_data(1, NULL) == NULL);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_CLIPBOARD_NOT_OPEN);
}

static void test_clipboard_set_get_and_formats(void)
{
    static const uint16_t wide[] = {'h', 'i', 0};
    size_t n = 0;
    const void *p;
    w32k_init();
    MT_ASSERT(w32k_open_clipboard(0));
    MT_EXPECT(w32k_empty_clipboard());
    MT_EXPECT(w32k_set_clipboard_data(1, "hello", 6));       /* CF_TEXT */
    MT_EXPECT(w32k_set_clipboard_data(13, wide, sizeof(wide))); /* CF_UNICODETEXT */
    MT_EXPECT_EQ(w32k_count_clipboard_formats(), 2);
    MT_EXPECT(w32k_is_clipboard_format_available(1));
    MT_EXPECT(!w32k_is_clipboard_format_available(2));
    p = w32k_get_clipboard_data(1, &n);
    MT_ASSERT(p != NULL);
    MT_EXPECT_EQ(n, 6);
    MT_EXPECT(memcmp(p, "hello", 6) == 0);
    /* Enumeration follows insertion order, then ends with 0. */
    MT_EXPECT_EQ(w32k_enum_clipboard_formats(0), 1);
    MT_EXPECT_EQ(w32k_enum_clipboard_formats(1), 13);
    MT_EXPECT_EQ(w32k_enum_clipboard_formats(13), 0);
    /* Replacing a format keeps one entry for it. */
    MT_EXPECT(w32k_set_clipboard_data(1, "bye", 4));
    MT_EXPECT_EQ(w32k_count_clipboard_formats(), 2);
    p = w32k_get_clipboard_data(1, &n);
    MT_EXPECT(p != NULL && memcmp(p, "bye", 4) == 0);
    MT_EXPECT(w32k_close_clipboard());
    MT_EXPECT(w32k_get_clipboard_data(1, NULL) == NULL); /* closed: no data */
}

static void test_clipboard_invalid_and_owner(void)
{
    w32k_handle owner;
    w32k_init();
    MT_ASSERT(w32k_open_clipboard(0));
    MT_EXPECT(!w32k_set_clipboard_data(0, "x", 1));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
    MT_EXPECT(!w32k_set_clipboard_data(1, NULL, 4));
    MT_EXPECT(w32k_close_clipboard());
    owner = make_win(0, 0, 10, 10, 0);
    MT_ASSERT(owner != 0);
    MT_ASSERT(w32k_open_clipboard(owner));
    MT_EXPECT(w32k_empty_clipboard());
    MT_EXPECT(w32k_get_clipboard_owner() == owner);
    /* Another window cannot open the clipboard while it is open. */
    MT_EXPECT(!w32k_open_clipboard(0));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_ACCESS_DENIED);
    MT_EXPECT(w32k_close_clipboard());
}

static void test_clipboard_capacity_and_registered_formats(void)
{
    uint16_t name_a[] = {'H', 't', 'm', 'l', 0};
    uint16_t name_b[] = {'R', 't', 'f', 0};
    uint32_t f1, f2, f3;
    int ok = 0;
    w32k_init();
    f1 = w32k_register_clipboard_format(name_a);
    f2 = w32k_register_clipboard_format(name_b);
    MT_EXPECT(f1 >= 0xC000 && f2 == f1 + 1);
    MT_EXPECT(w32k_register_clipboard_format(name_a) == f1); /* same name, same id */
    f3 = w32k_register_clipboard_format(NULL);
    MT_EXPECT(f3 == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
    MT_ASSERT(w32k_open_clipboard(0));
    for (uint32_t i = 1; i <= 20; ++i)
        if (w32k_set_clipboard_data(i, "x", 1))
            ++ok;
    MT_EXPECT_EQ(ok, 16); /* 16 format slots */
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_NOT_ENOUGH_QUOTA);
    MT_EXPECT(w32k_close_clipboard());
}

/* ---- Metrics and parameters -------------------------------------------------- */

static void test_system_metrics_primary_and_virtual(void)
{
    w32k_rect rc;
    w32k_init();
    MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CXSCREEN), 1920);
    MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CYSCREEN), 1080);
    MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CMONITORS), 1);
    MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CXVIRTUALSCREEN), 1920);
    MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CYCAPTION), 23);
    MT_EXPECT_EQ(w32k_get_system_metrics(12345), 0);
    /* A second monitor to the right widens the virtual screen only. */
    rc.left = 1920;
    rc.top = 0;
    rc.right = 3840;
    rc.bottom = 1080;
    MT_EXPECT(w32k_set_monitor(1, &rc, 96, 0));
    MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CMONITORS), 2);
    MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CXVIRTUALSCREEN), 3840);
    MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CXSCREEN), 1920);
    MT_EXPECT(!w32k_set_monitor(9, &rc, 96, 0));
}

static void test_system_parameters_roundtrip(void)
{
    w32k_rect wa;
    int beep = 0;
    uint32_t speed = 0;
    w32k_init();
    MT_ASSERT(w32k_system_parameters_info(W32K_SPI_GETWORKAREA, 0, &wa));
    MT_EXPECT_EQ(wa.right, 1920);
    MT_EXPECT_EQ(wa.bottom, 1080);
    /* SETBEEP takes its BOOL in uiParam. */
    MT_EXPECT(w32k_system_parameters_info(W32K_SPI_SETBEEP, 0, NULL));
    MT_EXPECT(w32k_system_parameters_info(W32K_SPI_GETBEEP, 0, &beep));
    MT_EXPECT_EQ(beep, 0);
    MT_EXPECT(w32k_system_parameters_info(W32K_SPI_SETBEEP, 1, NULL));
    MT_EXPECT(w32k_system_parameters_info(W32K_SPI_GETBEEP, 0, &beep));
    MT_EXPECT_EQ(beep, 1);
    MT_EXPECT(w32k_system_parameters_info(W32K_SPI_GETKEYBOARDSPEED, 0, &speed));
    MT_EXPECT_EQ(speed, 31);
    MT_EXPECT(!w32k_system_parameters_info(0x9999, 0, &beep));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
    MT_EXPECT(!w32k_system_parameters_info(W32K_SPI_GETWORKAREA, 0, NULL));
}

/* ---- Monitors and display settings ------------------------------------------- */

static int enum_count;
static int enum_stop_after;
static int enum_cb(w32k_handle hmon, const w32k_rect *rc, intptr_t lp)
{
    (void)hmon;
    (void)rc;
    (void)lp;
    ++enum_count;
    return enum_count < enum_stop_after ? 1 : 0;
}

static void test_enum_display_monitors(void)
{
    w32k_rect rc;
    w32k_handle w;
    w32k_init();
    enum_count = 0;
    enum_stop_after = 100;
    MT_EXPECT(w32k_enum_display_monitors(enum_cb, 0));
    MT_EXPECT_EQ(enum_count, 1);
    rc.left = 1920;
    rc.top = 0;
    rc.right = 3840;
    rc.bottom = 1080;
    MT_ASSERT(w32k_set_monitor(1, &rc, 144, 0));
    enum_count = 0;
    MT_EXPECT(w32k_enum_display_monitors(enum_cb, 0));
    MT_EXPECT_EQ(enum_count, 2);
    /* The callback can stop the enumeration early. */
    enum_count = 0;
    enum_stop_after = 1;
    MT_EXPECT(!w32k_enum_display_monitors(enum_cb, 0));
    MT_EXPECT_EQ(enum_count, 1);
    /* A window on the second monitor reports that monitor's dpi. */
    w = make_win(2000, 100, 200, 200, 0);
    MT_ASSERT(w != 0);
    MT_EXPECT(w32k_monitor_from_window(w) == 2);
    MT_EXPECT_EQ(w32k_get_dpi_for_window(w), 144);
    MT_EXPECT(w32k_destroy_window(w));
}

static void test_display_modes_and_change(void)
{
    w32k_devmode dm;
    w32k_handle top;
    int seen = 0;
    w32k_init();
    MT_EXPECT(w32k_enum_display_settings(0, &dm));
    MT_EXPECT_EQ(dm.width, 1024);
    MT_EXPECT(!w32k_enum_display_settings(4, &dm));
    MT_ASSERT(w32k_enum_display_settings(W32K_ENUM_CURRENT_SETTINGS, &dm));
    MT_EXPECT_EQ(dm.width, 1920);
    MT_EXPECT_EQ(dm.height, 1080);
    /* Top-level window that should receive WM_DISPLAYCHANGE on a real change. */
    top = make_win(0, 0, 10, 10, 0);
    MT_ASSERT(top != 0);
    (void)seen;
    /* Test mode validates without applying. */
    {
        w32k_devmode want = {1280, 720, 32, 60};
        MT_EXPECT_EQ(w32k_change_display_settings(&want, W32K_CDS_TEST), W32K_DISP_CHANGE_SUCCESSFUL);
        MT_ASSERT(w32k_enum_display_settings(W32K_ENUM_CURRENT_SETTINGS, &dm));
        MT_EXPECT_EQ(dm.width, 1920);
        /* Applying it changes the mode and the primary monitor. */
        MT_EXPECT_EQ(w32k_change_display_settings(&want, 0), W32K_DISP_CHANGE_SUCCESSFUL);
        MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CXSCREEN), 1280);
        MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CYSCREEN), 720);
        MT_ASSERT(w32k_enum_display_settings(W32K_ENUM_CURRENT_SETTINGS, &dm));
        MT_EXPECT_EQ(dm.width, 1280);
    }
    /* Unknown mode is rejected. */
    {
        w32k_devmode bad = {999, 999, 32, 0};
        MT_EXPECT_EQ(w32k_change_display_settings(&bad, 0), W32K_DISP_CHANGE_BADMODE);
        w32k_devmode bpp16 = {1024, 768, 16, 0};
        MT_EXPECT_EQ(w32k_change_display_settings(&bpp16, 0), W32K_DISP_CHANGE_BADMODE);
    }
    /* NULL resets to the registry mode (1920x1080). */
    MT_EXPECT_EQ(w32k_change_display_settings(NULL, 0), W32K_DISP_CHANGE_SUCCESSFUL);
    MT_EXPECT_EQ(w32k_get_system_metrics(W32K_SM_CXSCREEN), 1920);
    MT_EXPECT(w32k_destroy_window(top));
}

/* ---- Stations, desktops, input desktop -------------------------------------- */

static void test_station_and_desktop_lifecycle(void)
{
    uint16_t other[] = {'O', 't', 'h', 'e', 'r', 0};
    uint16_t second[] = {'S', 'e', 'c', 'o', 'n', 'd', 0};
    uint16_t winsta0[] = {'w', 'i', 'n', 's', 't', 'a', '0', 0};
    w32k_handle st, d2, d_open;
    w32k_init();
    st = w32k_create_winstation(other);
    MT_ASSERT(st != 0);
    MT_EXPECT(w32k_create_winstation(other) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_ALREADY_EXISTS);
    MT_EXPECT(w32k_open_winstation(winsta0) != 0);  /* case-insensitive */
    MT_EXPECT(w32k_open_winstation(second) == 0);
    d2 = w32k_create_desktop(st, second);
    MT_ASSERT(d2 != 0);
    d_open = w32k_open_desktop(st, second);
    MT_EXPECT(d_open == d2);
    MT_EXPECT(w32k_create_desktop(st, second) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_ALREADY_EXISTS);
    /* A station with a desktop cannot be closed. */
    MT_EXPECT(!w32k_close_winstation(st));
    MT_EXPECT(w32k_close_desktop(d2));
    MT_EXPECT(w32k_close_winstation(st));
}

static void test_input_desktop_switch_isolates_input(void)
{
    static const uint16_t nm2[] = {'D', 'e', 's', 'k', '2', 0};
    w32k_handle main_default;
    w32k_handle d2, w_default;
    w32k_msg m;
    w32k_init();
    main_default = w32k_get_input_desktop();
    MT_ASSERT(main_default != 0);
    d2 = w32k_create_desktop(0, nm2);
    MT_ASSERT(d2 != 0);
    w_default = make_win(0, 0, 100, 100, W32K_WS_VISIBLE);
    MT_ASSERT(w_default != 0);
    MT_ASSERT(w32k_set_foreground_window(w_default));
    /* Move this thread to d2, which requires no windows on it: switch first. */
    MT_ASSERT(w32k_switch_desktop(d2));
    MT_EXPECT(w32k_get_input_desktop() == d2);
    /* Keys now go to the foreground of the active desktop, which is empty. Drain
     * the paint owed by the visible window first, then check no key arrived. */
    w32k_inject_key('Q', 0x10, 1);
    while (w32k_peek_message(&m, 0, 0, 0, W32K_PM_REMOVE))
        MT_EXPECT(m.message != W32K_WM_KEYDOWN);
    MT_EXPECT(w32k_switch_desktop(main_default));
    MT_EXPECT(w32k_get_input_desktop() == main_default);
    MT_EXPECT(w32k_destroy_window(w_default));
    MT_EXPECT(w32k_close_desktop(d2));
}

static void test_thread_desktop_busy_with_windows(void)
{
    static const uint16_t nm[] = {'T', 'h', 'D', 0};
    w32k_handle d, w, prev;
    w32k_init();
    prev = w32k_get_thread_desktop();
    MT_ASSERT(prev != 0);
    d = w32k_create_desktop(0, nm);
    MT_ASSERT(d != 0);
    w = make_win(0, 0, 10, 10, 0);
    MT_ASSERT(w != 0);
    MT_EXPECT(!w32k_set_thread_desktop(d));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_BUSY);
    MT_EXPECT(w32k_destroy_window(w));
    MT_EXPECT(w32k_set_thread_desktop(d));
    MT_EXPECT(w32k_get_thread_desktop() == d);
    MT_EXPECT(w32k_set_thread_desktop(prev));
}

/* ---- Cursors and icons -------------------------------------------------------- */

static void test_cursor_and_icon_objects(void)
{
    w32k_handle c, ic;
    w32k_init();
    MT_EXPECT(w32k_load_cursor(W32K_IDC_IBEAM) != 0);
    MT_EXPECT(w32k_load_cursor(W32K_IDC_IBEAM) == w32k_load_cursor(W32K_IDC_IBEAM));
    MT_EXPECT(w32k_load_cursor(0x1234) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), 1813); /* ERROR_RESOURCE_NAME_NOT_FOUND */
    MT_EXPECT(w32k_create_cursor(40, 0, 32, 32) == 0);     /* hot spot outside */
    c = w32k_create_cursor(4, 4, 32, 32);
    MT_ASSERT(c != 0);
    MT_EXPECT(w32k_destroy_cursor(c));
    MT_EXPECT(!w32k_destroy_cursor(c));
    ic = w32k_create_icon(16, 16, 1, 32);
    MT_ASSERT(ic != 0);
    MT_EXPECT(w32k_destroy_icon(ic));
    MT_EXPECT(w32k_create_icon(0, 16, 1, 32) == 0);
    MT_EXPECT(w32k_load_icon(W32K_IDI_APPLICATION) != 0);
    MT_EXPECT(!w32k_destroy_icon(w32k_load_icon(W32K_IDI_APPLICATION)));
}

int main(void)
{
    static const mt_case_t cases[] = {
        {"clipboard_requires_open", test_clipboard_requires_open},
        {"clipboard_set_get_formats", test_clipboard_set_get_and_formats},
        {"clipboard_invalid_owner", test_clipboard_invalid_and_owner},
        {"clipboard_capacity_registered", test_clipboard_capacity_and_registered_formats},
        {"metrics_primary_virtual", test_system_metrics_primary_and_virtual},
        {"parameters_roundtrip", test_system_parameters_roundtrip},
        {"enum_display_monitors", test_enum_display_monitors},
        {"display_modes_change", test_display_modes_and_change},
        {"station_desktop_lifecycle", test_station_and_desktop_lifecycle},
        {"input_desktop_switch", test_input_desktop_switch_isolates_input},
        {"thread_desktop_busy", test_thread_desktop_busy_with_windows},
        {"cursor_icon_objects", test_cursor_and_icon_objects},
    };
    setvbuf(stdout, NULL, _IONBF, 0);
    return mt_run("test_w32k_sys", cases, sizeof(cases) / sizeof(cases[0]));
}
