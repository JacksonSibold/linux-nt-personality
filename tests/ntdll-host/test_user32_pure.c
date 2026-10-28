/*
 * test_user32_pure.c - host tests for src/ntdll/user32_pure.h: RECT arithmetic,
 * AdjustWindowRectEx, system colours, case mapping and the US keyboard tables.
 */
#include "minitest.h"
#include "user32_pure.h"

static w32k_rect R(int l, int t, int r, int b)
{
    w32k_rect x;
    x.left = l;
    x.top = t;
    x.right = r;
    x.bottom = b;
    return x;
}

static void rect_basics(void)
{
    w32k_rect a = R(0, 0, 10, 10), b = R(0, 0, 10, 10), e = R(5, 5, 5, 9);
    MT_EXPECT(u32p_equal_rect(&a, &b));
    MT_EXPECT(!u32p_is_rect_empty(&a));
    MT_EXPECT(u32p_is_rect_empty(&e));
    MT_EXPECT(u32p_pt_in_rect(&a, 0, 0));
    MT_EXPECT(u32p_pt_in_rect(&a, 9, 9));
    MT_EXPECT(!u32p_pt_in_rect(&a, 10, 5)); /* right and bottom are exclusive */
    MT_EXPECT(!u32p_pt_in_rect(&a, -1, 5));
    u32p_offset_rect(&a, 3, -2);
    MT_EXPECT_EQ(a.left, 3);
    MT_EXPECT_EQ(a.top, -2);
    MT_EXPECT_EQ(a.right, 13);
    MT_EXPECT_EQ(a.bottom, 8);
    u32p_inflate_rect(&a, 2, 1);
    MT_EXPECT_EQ(a.left, 1);
    MT_EXPECT_EQ(a.right, 15);
    MT_EXPECT_EQ(a.top, -3);
    MT_EXPECT_EQ(a.bottom, 9);
    u32p_inflate_rect(&a, -2, -1);
    MT_EXPECT_EQ(a.left, 3);
    MT_EXPECT_EQ(a.bottom, 8);
}

static void rect_intersect(void)
{
    w32k_rect a = R(0, 0, 10, 10), b = R(5, 5, 20, 20), c = R(10, 0, 20, 10), d;
    MT_EXPECT(u32p_intersect_rect(&d, &a, &b));
    MT_EXPECT(u32p_equal_rect(&d, &(w32k_rect){5, 5, 10, 10}));
    /* Touching rectangles do not intersect and the result is zeroed. */
    d = R(1, 2, 3, 4);
    MT_EXPECT(!u32p_intersect_rect(&d, &a, &c));
    MT_EXPECT(u32p_equal_rect(&d, &(w32k_rect){0, 0, 0, 0}));
    /* An empty input gives an empty result even when the other covers it. */
    {
        w32k_rect e = R(5, 5, 5, 5);
        MT_EXPECT(!u32p_intersect_rect(&d, &a, &e));
    }
}

static void rect_union(void)
{
    w32k_rect a = R(0, 0, 4, 4), b = R(10, 10, 12, 20), e = R(100, 100, 100, 100), d;
    MT_EXPECT(u32p_union_rect(&d, &a, &b));
    MT_EXPECT(u32p_equal_rect(&d, &(w32k_rect){0, 0, 12, 20}));
    MT_EXPECT(u32p_union_rect(&d, &a, &e)); /* the empty rectangle is ignored */
    MT_EXPECT(u32p_equal_rect(&d, &a));
    MT_EXPECT(u32p_union_rect(&d, &e, &b));
    MT_EXPECT(u32p_equal_rect(&d, &b));
    MT_EXPECT(!u32p_union_rect(&d, &e, &e));
    MT_EXPECT(u32p_equal_rect(&d, &(w32k_rect){0, 0, 0, 0}));
}

static void rect_subtract(void)
{
    w32k_rect a = R(0, 0, 10, 10), d;
    /* b spans a vertically and covers its left part: the left edge moves in. */
    MT_EXPECT(u32p_subtract_rect(&d, &a, &(w32k_rect){-5, -5, 4, 15}));
    MT_EXPECT(u32p_equal_rect(&d, &(w32k_rect){4, 0, 10, 10}));
    /* covers the right part */
    MT_EXPECT(u32p_subtract_rect(&d, &a, &(w32k_rect){6, 0, 20, 10}));
    MT_EXPECT(u32p_equal_rect(&d, &(w32k_rect){0, 0, 6, 10}));
    /* covers the top part */
    MT_EXPECT(u32p_subtract_rect(&d, &a, &(w32k_rect){0, -1, 10, 3}));
    MT_EXPECT(u32p_equal_rect(&d, &(w32k_rect){0, 3, 10, 10}));
    /* a hole in the middle is not a rectangle: a is unchanged */
    MT_EXPECT(u32p_subtract_rect(&d, &a, &(w32k_rect){3, 3, 6, 6}));
    MT_EXPECT(u32p_equal_rect(&d, &a));
    /* fully covered: empty */
    MT_EXPECT(!u32p_subtract_rect(&d, &a, &(w32k_rect){-1, -1, 11, 11}));
    MT_EXPECT(u32p_is_rect_empty(&d));
    /* no overlap: unchanged */
    MT_EXPECT(u32p_subtract_rect(&d, &a, &(w32k_rect){20, 20, 30, 30}));
    MT_EXPECT(u32p_equal_rect(&d, &a));
}

static u32p_metrics win10(void)
{
    u32p_metrics m;
    m.cxborder = m.cyborder = 1;
    m.cxsizeframe = m.cysizeframe = 4;
    m.cxpadded = 4;
    m.cxdlgframe = m.cydlgframe = 3;
    m.cxedge = m.cyedge = 2;
    m.cycaption = 23;
    m.cysmcaption = 22;
    m.cymenu = 20;
    return m;
}

#define WS_OVERLAPPEDWINDOW 0x00CF0000u

static void adjust_overlapped_window(void)
{
    u32p_metrics m = win10();
    w32k_rect r = R(0, 0, 800, 600);
    /* A Windows 10 caption window at 96 dpi grows by 8 on the sides and bottom and by 31 on top. */
    u32p_adjust_window_rect(&r, WS_OVERLAPPEDWINDOW, 0, 0, &m);
    MT_EXPECT_EQ(r.left, -8);
    MT_EXPECT_EQ(r.right, 808);
    MT_EXPECT_EQ(r.top, -31);
    MT_EXPECT_EQ(r.bottom, 608);
}

static void adjust_other_styles(void)
{
    u32p_metrics m = win10();
    w32k_rect r = R(0, 0, 100, 100);
    /* Plain WS_BORDER: one pixel, no caption. */
    u32p_adjust_window_rect(&r, U32P_WS_BORDER, 0, 0, &m);
    MT_EXPECT(u32p_equal_rect(&r, &(w32k_rect){-1, -1, 101, 101}));
    /* WS_CAPTION without a thick frame: dialog frame (3) plus the caption. */
    r = R(0, 0, 100, 100);
    u32p_adjust_window_rect(&r, U32P_WS_CAPTION, 0, 0, &m);
    MT_EXPECT(u32p_equal_rect(&r, &(w32k_rect){-3, -26, 103, 103}));
    /* A tool window uses the small caption. */
    r = R(0, 0, 100, 100);
    u32p_adjust_window_rect(&r, U32P_WS_CAPTION, 0, U32P_WS_EX_TOOLWINDOW, &m);
    MT_EXPECT_EQ(r.top, -25);
    /* A menu adds its height on top. */
    r = R(0, 0, 100, 100);
    u32p_adjust_window_rect(&r, U32P_WS_BORDER, 1, 0, &m);
    MT_EXPECT_EQ(r.top, -21);
    /* A client edge adds 2 on each side. */
    r = R(0, 0, 100, 100);
    u32p_adjust_window_rect(&r, 0, 0, U32P_WS_EX_CLIENTEDGE, &m);
    MT_EXPECT(u32p_equal_rect(&r, &(w32k_rect){-2, -2, 102, 102}));
    /* A popup with no border style does not change. */
    r = R(0, 0, 100, 100);
    u32p_adjust_window_rect(&r, 0x80000000u, 0, 0, &m);
    MT_EXPECT(u32p_equal_rect(&r, &(w32k_rect){0, 0, 100, 100}));
    /* A minimised window has no frame to add. */
    r = R(0, 0, 100, 100);
    u32p_adjust_window_rect(&r, WS_OVERLAPPEDWINDOW | U32P_WS_MINIMIZE, 1, 0, &m);
    MT_EXPECT(u32p_equal_rect(&r, &(w32k_rect){0, 0, 100, 100}));
}

static void system_colours(void)
{
    int ok = 0;
    /* COLOR_WINDOW is white, COLOR_BTNFACE 0xF0F0F0, COLOR_HIGHLIGHT RGB(0,120,215) = 0x00D77800. */
    MT_EXPECT_EQ(u32p_sys_colorref(5, &ok), 0x00FFFFFFu);
    MT_EXPECT(ok);
    MT_EXPECT_EQ(u32p_sys_colorref(15, &ok), 0x00F0F0F0u);
    MT_EXPECT_EQ(u32p_sys_colorref(13, &ok), 0x00D77800u);
    MT_EXPECT_EQ(u32p_sys_colorref(2, &ok), 0x00D1B499u); /* RGB(0x99,0xB4,0xD1) as BGR */
    MT_EXPECT_EQ(u32p_sys_colorref(31, &ok), 0);
    MT_EXPECT(!ok);
    MT_EXPECT_EQ(u32p_sys_colorref(-1, &ok), 0);
    MT_EXPECT(!ok);
    MT_EXPECT_EQ(u32p_sys_colorref(0, 0), 0x00C8C8C8u); /* ok may be NULL */
}

static void case_mapping(void)
{
    MT_EXPECT_EQ(u32p_wupper('a'), 'A');
    MT_EXPECT_EQ(u32p_wupper('z'), 'Z');
    MT_EXPECT_EQ(u32p_wupper('5'), '5');
    MT_EXPECT_EQ(u32p_wlower('Q'), 'q');
    MT_EXPECT_EQ(u32p_wupper(0xE9), 0xC9);  /* e acute */
    MT_EXPECT_EQ(u32p_wlower(0xC9), 0xE9);
    MT_EXPECT_EQ(u32p_wupper(0xF7), 0xF7);  /* division sign has no case */
    MT_EXPECT_EQ(u32p_wlower(0xD7), 0xD7);  /* multiplication sign has no case */
    MT_EXPECT_EQ(u32p_wupper(0xFF), 0x178);
    MT_EXPECT_EQ(u32p_wlower(0x178), 0xFF);
    MT_EXPECT_EQ(u32p_wupper(0x3B1), 0x391); /* alpha */
    MT_EXPECT_EQ(u32p_wupper(0x3C2), 0x3C2); /* final sigma has no simple upper in this table */
    MT_EXPECT_EQ(u32p_wlower(0x3A3), 0x3C3);
    MT_EXPECT_EQ(u32p_wupper(0x430), 0x410); /* Cyrillic a */
    MT_EXPECT_EQ(u32p_wupper(0x451), 0x401);
    MT_EXPECT_EQ(u32p_wlower(0x401), 0x451);
    MT_EXPECT_EQ(u32p_wupper(0x4E00), 0x4E00);
}

static void scan_vk_tables(void)
{
    /* Spot checks against set 1 scan codes. */
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x01, 0), 0x1B);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x11, 0), 'W');
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x1E, 0), 'A');
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x39, 0), 0x20);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x2A, 0), U32P_VK_SHIFT);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x2A, 1), U32P_VK_LSHIFT);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x36, 1), U32P_VK_RSHIFT);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x1D, 0), U32P_VK_CONTROL);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x38, 0), U32P_VK_MENU);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x3B, 0), 0x70);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x58, 0), 0x7B);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0x59, 0), 0);
    MT_EXPECT_EQ(u32p_scan_to_vk_fn(0xFF, 0), 0);
    MT_EXPECT_EQ(u32p_vk_to_scan('W'), 0x11);
    MT_EXPECT_EQ(u32p_vk_to_scan(U32P_VK_SHIFT), 0x2A);
    MT_EXPECT_EQ(u32p_vk_to_scan(U32P_VK_RSHIFT), 0x36);
    MT_EXPECT_EQ(u32p_vk_to_scan(0x70), 0x3B);
    MT_EXPECT_EQ(u32p_vk_to_scan(0x01), 0); /* mouse button: no scan code */
}

static void scan_vk_round_trip(void)
{
    uint32_t s;
    for (s = 1; s < 0x59; ++s) {
        uint32_t vk = u32p_scan_to_vk_fn(s, 1);
        if (!vk)
            continue;
        MT_EXPECT_EQ(u32p_vk_to_scan(vk), s);
    }
}

static void vk_chars(void)
{
    MT_EXPECT_EQ(u32p_vk_char('A', 0), 'a');
    MT_EXPECT_EQ(u32p_vk_char('A', 1), 'A');
    MT_EXPECT_EQ(u32p_vk_char('1', 0), '1');
    MT_EXPECT_EQ(u32p_vk_char('1', 1), '!');
    MT_EXPECT_EQ(u32p_vk_char('0', 1), ')');
    MT_EXPECT_EQ(u32p_vk_char(0xBA, 0), ';');
    MT_EXPECT_EQ(u32p_vk_char(0xBA, 1), ':');
    MT_EXPECT_EQ(u32p_vk_char(0xC0, 1), '~');
    MT_EXPECT_EQ(u32p_vk_char(0xDB, 0), '[');
    MT_EXPECT_EQ(u32p_vk_char(0xDE, 1), '"');
    MT_EXPECT_EQ(u32p_vk_char(0x20, 0), ' ');
    MT_EXPECT_EQ(u32p_vk_char(0x65, 0), '5');   /* numpad 5 */
    MT_EXPECT_EQ(u32p_vk_char(0x70, 0), 0);      /* F1 makes no character */
    MT_EXPECT_EQ(u32p_vk_char(0x0D, 0), '\r');
}

static void vk_key_scan(void)
{
    MT_EXPECT_EQ(u32p_vk_key_scan('a'), 0x41);
    MT_EXPECT_EQ(u32p_vk_key_scan('A'), 0x141);
    MT_EXPECT_EQ(u32p_vk_key_scan('1'), 0x31);
    MT_EXPECT_EQ(u32p_vk_key_scan('!'), 0x131);
    MT_EXPECT_EQ(u32p_vk_key_scan('*'), 0x138); /* shift+8, not the numpad key */
    MT_EXPECT_EQ(u32p_vk_key_scan('+'), 0x1BB);
    MT_EXPECT_EQ(u32p_vk_key_scan(' '), 0x20);
    MT_EXPECT_EQ(u32p_vk_key_scan(';'), 0xBA);
    MT_EXPECT_EQ(u32p_vk_key_scan(0x20AC), -1);
}

static int name_is(uint32_t lparam, const char *want)
{
    uint16_t buf[32];
    int n = u32p_key_name(lparam, buf, 32), i;
    for (i = 0; want[i]; ++i)
        if (i >= n || buf[i] != (uint16_t)want[i])
            return 0;
    return n == i && buf[n] == 0;
}

static void key_names(void)
{
    uint16_t buf[4];
    MT_EXPECT(name_is(0x01u << 16, "Esc"));
    MT_EXPECT(name_is(0x1Eu << 16, "A"));
    MT_EXPECT(name_is(0x11u << 16, "W"));
    MT_EXPECT(name_is(0x39u << 16, "Space"));
    MT_EXPECT(name_is(0x3Bu << 16, "F1"));
    MT_EXPECT(name_is(0x28u << 16, "'"));
    MT_EXPECT(name_is(0x02u << 16, "1"));
    MT_EXPECT_EQ(u32p_key_name(0x59u << 16, buf, 4), 0);
    /* A short buffer truncates and terminates. */
    MT_EXPECT_EQ(u32p_key_name(0x39u << 16, buf, 4), 3);
    MT_EXPECT_EQ(buf[0], 'S');
    MT_EXPECT_EQ(buf[3], 0);
    MT_EXPECT_EQ(u32p_key_name(0x39u << 16, buf, 0), 0);
}

static const mt_case_t cases[] = {
    MT_CASE(rect_basics),
    MT_CASE(rect_intersect),
    MT_CASE(rect_union),
    MT_CASE(rect_subtract),
    MT_CASE(adjust_overlapped_window),
    MT_CASE(adjust_other_styles),
    MT_CASE(system_colours),
    MT_CASE(case_mapping),
    MT_CASE(scan_vk_tables),
    MT_CASE(scan_vk_round_trip),
    MT_CASE(vk_chars),
    MT_CASE(vk_key_scan),
    MT_CASE(key_names),
};

int main(void)
{
    return MT_RUN("user32_pure", cases);
}
