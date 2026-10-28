/* test_w32k_gdi.c - DCs, the software 32bpp surface, raster operations, stretch,
 * placeholder text, selectable and stock objects, and the pixel arena. */
#include "minitest.h"
#include "w32k.h"

#include <string.h>

static uint16_t reg_class(void)
{
    static const uint16_t nm[] = {'G', 'd', 'i', 0};
    w32k_wndclass wc;
    memset(&wc, 0, sizeof(wc));
    wc.name = nm;
    wc.proc = w32k_def_window_proc;
    return w32k_register_class(&wc);
}

static w32k_handle win(int32_t x, int32_t y, int32_t cx, int32_t cy)
{
    static const uint16_t nm[] = {'G', 'd', 'i', 0};
    w32k_createwnd cs;
    memset(&cs, 0, sizeof(cs));
    reg_class();
    cs.cls = (uintptr_t)nm;
    cs.name = nm;
    cs.x = x;
    cs.y = y;
    cs.cx = cx;
    cs.cy = cy;
    return w32k_create_window(&cs);
}

/* A memory DC with a w x h DIB selected into it. */
static w32k_handle mem_dc(int32_t w, int32_t h, void **bits, w32k_handle *bmp_out)
{
    w32k_handle dc = w32k_create_compatible_dc(0);
    w32k_handle b = w32k_create_dib_section(w, h, 32, bits);
    if (!dc || !b)
        mt_fail(__FILE__, __LINE__, "mem_dc: create");
    if (w32k_select_object(dc, b) == 0)
        mt_fail(__FILE__, __LINE__, "mem_dc: select");
    if (bmp_out)
        *bmp_out = b;
    return dc;
}

/* ---- DCs and window surfaces ------------------------------------------------- */

static void test_screen_dc_caps(void)
{
    w32k_handle dc;
    w32k_init();
    dc = w32k_get_dc(0);
    MT_ASSERT(dc != 0);
    MT_EXPECT_EQ(w32k_get_device_caps(dc, W32K_HORZRES), 1920);
    MT_EXPECT_EQ(w32k_get_device_caps(dc, W32K_VERTRES), 1080);
    MT_EXPECT_EQ(w32k_get_device_caps(dc, W32K_BITSPIXEL), 32);
    MT_EXPECT_EQ(w32k_get_device_caps(dc, W32K_LOGPIXELSX), 96);
    MT_EXPECT(w32k_release_dc(0, dc));
    MT_EXPECT(!w32k_release_dc(0, dc)); /* already released */
}

static void test_window_dc_origin_and_clip(void)
{
    w32k_handle w, dc;
    uint32_t bg;
    w32k_init();
    w = win(100, 50, 40, 30);
    MT_ASSERT(w != 0);
    dc = w32k_get_dc(w);
    MT_ASSERT(dc != 0);
    /* Logical (0,0) is the client origin. Inside the client area SetPixel works. */
    MT_EXPECT(w32k_set_pixel(dc, 0, 0, 0x00112233) == 0x00112233);
    MT_EXPECT(w32k_get_pixel(dc, 0, 0) == 0x00112233);
    /* Outside the client rectangle: clipped. */
    MT_EXPECT(w32k_set_pixel(dc, 40, 0, 0x00FFFFFF) == W32K_CLR_INVALID);
    MT_EXPECT(w32k_get_pixel(dc, -1, 0) == W32K_CLR_INVALID);
    /* Pixel really landed on the framebuffer at (100, 50). */
    {
        int32_t fw, fh;
        uint32_t *fb = w32k_bitmap_bits(w32k_framebuffer(), &fw, &fh);
        MT_ASSERT(fb != NULL);
        MT_EXPECT_EQ(fb[50 * fw + 100], 0x00332211u); /* COLORREF 0x00112233 -> XRGB */
    }
    bg = w32k_get_bk_color(dc);
    MT_EXPECT_EQ(bg, 0x00FFFFFFu);
    MT_EXPECT(w32k_release_dc(w, dc));
    MT_EXPECT(w32k_destroy_window(w));
    MT_EXPECT(w32k_get_dc(w) == 0); /* window gone */
}

static void test_fill_rect_and_colors_on_memory_dc(void)
{
    void *bits;
    w32k_handle dc, red, rc_brush;
    w32k_rect r;
    uint32_t *px;
    w32k_init();
    dc = mem_dc(4, 4, &bits, NULL);
    red = w32k_create_solid_brush(0x000000FFu); /* COLORREF red */
    MT_ASSERT(red != 0);
    r.left = 1;
    r.top = 1;
    r.right = 3;
    r.bottom = 3;
    MT_EXPECT(w32k_fill_rect(dc, &r, red));
    px = (uint32_t *)bits;
    MT_EXPECT_EQ(px[0], 0u);              /* outside */
    MT_EXPECT_EQ(px[1 * 4 + 1], 0x00FF0000u); /* XRGB red */
    MT_EXPECT_EQ(px[2 * 4 + 2], 0x00FF0000u);
    MT_EXPECT_EQ(px[3 * 4 + 3], 0u);
    /* Null brush fills nothing and still succeeds. */
    rc_brush = w32k_get_stock_object(W32K_NULL_BRUSH);
    MT_EXPECT(w32k_fill_rect(dc, &r, rc_brush));
    MT_EXPECT_EQ(px[1 * 4 + 1], 0x00FF0000u);
    MT_EXPECT(w32k_delete_dc(dc));
    MT_EXPECT(w32k_delete_object(red));
}

static void test_dib_section_rules(void)
{
    void *bits = NULL;
    w32k_handle b;
    w32k_init();
    MT_EXPECT(w32k_create_dib_section(4, 4, 24, &bits) == 0); /* only 32 bpp */
    MT_EXPECT(w32k_create_dib_section(0, 4, 32, &bits) == 0);
    b = w32k_create_dib_section(4, 4, 32, &bits);
    MT_ASSERT(b != 0 && bits != NULL);
    MT_EXPECT_EQ(w32k_get_object_type(b), W32K_OBJ_BITMAP);
    MT_EXPECT(w32k_delete_object(b));
    MT_EXPECT(!w32k_delete_object(b)); /* stale */
}

/* ---- Raster operations -------------------------------------------------------- */

static void test_bitblt_rops(void)
{
    void *sbits, *dbits;
    w32k_handle src, dst, sb, db, brush;
    uint32_t *s, *d;
    w32k_init();
    src = mem_dc(2, 1, &sbits, &sb);
    dst = mem_dc(2, 1, &dbits, &db);
    s = (uint32_t *)sbits;
    d = (uint32_t *)dbits;
    s[0] = 0x00F0F0F0u;
    s[1] = 0x000F0F0Fu;
    d[0] = 0x00FF00FFu;
    d[1] = 0x00000000u;
    /* SRCCOPY */
    MT_EXPECT(w32k_bitblt(dst, 0, 0, 2, 1, src, 0, 0, W32K_SRCCOPY));
    MT_EXPECT_EQ(d[0], 0x00F0F0F0u);
    MT_EXPECT_EQ(d[1], 0x000F0F0Fu);
    d[0] = 0x00FF00FFu;
    d[1] = 0x00FFFFFFu;
    /* SRCAND, SRCINVERT, SRCPAINT, NOTSRCCOPY on known values. */
    MT_EXPECT(w32k_bitblt(dst, 0, 0, 2, 1, src, 0, 0, W32K_SRCAND));
    MT_EXPECT_EQ(d[0], 0x00F0F0F0u & 0x00FF00FFu);
    d[0] = 0x00FF00FFu;
    MT_EXPECT(w32k_bitblt(dst, 0, 0, 2, 1, src, 0, 0, W32K_SRCINVERT));
    MT_EXPECT_EQ(d[0], 0x00F0F0F0u ^ 0x00FF00FFu);
    d[0] = 0x00FF00FFu;
    MT_EXPECT(w32k_bitblt(dst, 0, 0, 2, 1, src, 0, 0, W32K_SRCPAINT));
    MT_EXPECT_EQ(d[0], 0x00F0F0F0u | 0x00FF00FFu);
    MT_EXPECT(w32k_bitblt(dst, 0, 0, 2, 1, src, 0, 0, W32K_NOTSRCCOPY));
    MT_EXPECT_EQ(d[0], (~0x00F0F0F0u) & 0x00FFFFFFu);
    /* Source-free ROPs. */
    brush = w32k_create_solid_brush(0x00336699u);
    MT_ASSERT(w32k_select_object(dst, brush) != 0);
    MT_EXPECT(w32k_bitblt(dst, 0, 0, 2, 1, 0, 0, 0, W32K_PATCOPY));
    MT_EXPECT_EQ(d[0], 0x00996633u); /* COLORREF 0x00336699 -> XRGB */
    MT_EXPECT(w32k_bitblt(dst, 0, 0, 2, 1, 0, 0, 0, W32K_WHITENESS));
    MT_EXPECT_EQ(d[1], 0x00FFFFFFu);
    MT_EXPECT(w32k_bitblt(dst, 0, 0, 2, 1, 0, 0, 0, W32K_BLACKNESS));
    MT_EXPECT_EQ(d[1], 0u);
    d[1] = 0x00123456u;
    MT_EXPECT(w32k_bitblt(dst, 0, 0, 2, 1, 0, 0, 0, W32K_DSTINVERT));
    MT_EXPECT_EQ(d[1], 0x00EDCBA9u);
    /* Errors: unknown ROP, source needed but missing. */
    MT_EXPECT(!w32k_bitblt(dst, 0, 0, 2, 1, src, 0, 0, 0x12345678u));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
    MT_EXPECT(!w32k_bitblt(dst, 0, 0, 2, 1, 0, 0, 0, W32K_SRCCOPY));
    MT_EXPECT(w32k_delete_dc(src));
    MT_EXPECT(w32k_delete_dc(dst));
}

static void test_bitblt_overlap_and_clip(void)
{
    void *bits;
    w32k_handle dc, b;
    uint32_t *p;
    w32k_init();
    dc = mem_dc(6, 1, &bits, &b);
    p = (uint32_t *)bits;
    for (int i = 0; i < 6; ++i)
        p[i] = (uint32_t)(i + 1);
    /* Overlapping blit on one surface: copies the original values, not a smear. */
    MT_EXPECT(w32k_bitblt(dc, 2, 0, 4, 1, dc, 0, 0, W32K_SRCCOPY));
    MT_EXPECT_EQ(p[2], 1u);
    MT_EXPECT_EQ(p[3], 2u);
    MT_EXPECT_EQ(p[4], 3u);
    MT_EXPECT_EQ(p[5], 4u);
    /* Destination x = -2..0 is clipped to the surface: only x = 0 is written, from
     * source offset 2 (column 5, value 4). Nothing is written out of range. */
    MT_EXPECT(w32k_bitblt(dc, -2, 0, 3, 1, dc, 3, 0, W32K_SRCCOPY));
    MT_EXPECT_EQ(p[0], 4u);
    MT_EXPECT(w32k_delete_dc(dc));
}

static void test_stretchblt_nearest(void)
{
    void *sb, *db;
    w32k_handle src, dst;
    uint32_t *s, *d;
    w32k_init();
    src = mem_dc(2, 2, &sb, NULL);
    dst = mem_dc(4, 4, &db, NULL);
    s = (uint32_t *)sb;
    d = (uint32_t *)db;
    s[0] = 1;
    s[1] = 2;
    s[2] = 3;
    s[3] = 4;
    MT_EXPECT(w32k_stretchblt(dst, 0, 0, 4, 4, src, 0, 0, 2, 2, W32K_SRCCOPY));
    /* Each source pixel becomes a 2x2 block. */
    MT_EXPECT_EQ(d[0], 1u);
    MT_EXPECT_EQ(d[1], 1u);
    MT_EXPECT_EQ(d[2], 2u);
    MT_EXPECT_EQ(d[3], 2u);
    MT_EXPECT_EQ(d[4 * 2 + 0], 3u);
    MT_EXPECT_EQ(d[4 * 3 + 3], 4u);
    /* Shrink: sample the top-left 1x1 source pixel into the whole destination. */
    MT_EXPECT(w32k_stretchblt(dst, 0, 0, 4, 4, src, 0, 0, 1, 1, W32K_SRCCOPY));
    MT_EXPECT_EQ(d[4 * 3 + 3], 1u);
    MT_EXPECT(!w32k_stretchblt(dst, 0, 0, 0, 4, src, 0, 0, 2, 2, W32K_SRCCOPY));
    MT_EXPECT(w32k_delete_dc(src));
    MT_EXPECT(w32k_delete_dc(dst));
}

/* ---- Text --------------------------------------------------------------------- */

/* Counts pixels equal to colour c in the box [x0,x1) x [y0,y1) of a 32-wide surface. */
static int count_colour(const uint32_t *p, int x0, int x1, int y0, int y1, uint32_t c)
{
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            n += p[y * 32 + x] == c;
    return n;
}

/* The stroke font draws real glyphs: 'A' has a red apex on the cap line and leaves most
 * of its 8x16 cell on the opaque background. */
static void test_text_glyph_cells(void)
{
    void *bits;
    w32k_handle dc;
    uint32_t *p;
    w32k_size sz;
    int red;
    static const uint16_t text[] = {'A', ' ', 'B'};
    w32k_init();
    dc = mem_dc(32, 16, &bits, NULL);
    p = (uint32_t *)bits;
    MT_EXPECT(w32k_set_text_color(dc, 0x000000FFu) == 0);
    MT_EXPECT(w32k_set_bk_color(dc, 0x00FFFFFFu) == 0xFFFFFFu);
    MT_EXPECT(w32k_text_out(dc, 0, 0, text, 3));
    /* 'A' apex: the design apex sits on the cap line at the centre column. */
    red = count_colour(p, 0, 8, 0, 16, 0x00FF0000u);
    MT_EXPECT(red > 0);
    MT_EXPECT(red < 8 * 16 / 2);
    MT_EXPECT_EQ(p[3], 0x00FF0000u);
    /* Uncovered pixels in the cell keep the opaque background. */
    MT_EXPECT_EQ(p[0], 0x00FFFFFFu);
    /* The space keeps the opaque background across its whole cell. */
    MT_EXPECT_EQ(count_colour(p, 8, 16, 0, 16, 0x00FFFFFFu), 8 * 16);
    /* 'B' starts at column 16 and draws its stem there. */
    MT_EXPECT(count_colour(p, 16, 24, 0, 16, 0x00FF0000u) > 0);
    MT_EXPECT(w32k_get_text_extent(dc, text, 3, &sz));
    MT_EXPECT_EQ(sz.cx, 24);
    MT_EXPECT_EQ(sz.cy, 16);
    MT_EXPECT(w32k_set_bk_mode(dc, W32K_TRANSPARENT) == W32K_OPAQUE);
    MT_EXPECT(!w32k_set_bk_mode(dc, 9));
    MT_EXPECT(w32k_delete_dc(dc));
}

/* A transparent background leaves the uncovered pixels as they were. */
static void test_text_transparent_keeps_pixels(void)
{
    void *bits;
    w32k_handle dc;
    uint32_t *p;
    static const uint16_t text[] = {'A'};
    w32k_init();
    dc = mem_dc(32, 16, &bits, NULL);
    p = (uint32_t *)bits;
    for (int i = 0; i < 32 * 16; ++i)
        p[i] = 0x00123456u;
    MT_EXPECT(w32k_set_text_color(dc, 0x000000FFu) == 0);
    MT_EXPECT(w32k_set_bk_mode(dc, W32K_TRANSPARENT) == W32K_OPAQUE);
    MT_EXPECT(w32k_text_out(dc, 0, 0, text, 1));
    MT_EXPECT_EQ(p[0], 0x00123456u);
    MT_EXPECT_EQ(count_colour(p, 0, 8, 0, 16, 0x00123456u) < 8 * 16, 1);
    MT_EXPECT_EQ(count_colour(p, 0, 8, 0, 16, 0x00FF0000u) > 0, 1);
    MT_EXPECT(w32k_delete_dc(dc));
}

/* A text origin offsets the first cell: columns before the origin keep the background. */
static void test_text_clipped_to_dc(void)
{
    void *bits;
    w32k_handle dc;
    uint32_t *p;
    static const uint16_t text[] = {'M'};
    w32k_init();
    dc = mem_dc(32, 16, &bits, NULL);
    p = (uint32_t *)bits;
    MT_EXPECT(w32k_set_text_color(dc, 0x000000FFu) == 0);
    MT_EXPECT(w32k_text_out(dc, 4, 0, text, 1));
    /* 'M' starts at column 4, so columns 0..3 keep the cleared background. */
    MT_EXPECT_EQ(count_colour(p, 0, 4, 0, 16, 0x00FF0000u), 0);
    MT_EXPECT(w32k_delete_dc(dc));
}

/* ---- Objects ------------------------------------------------------------------ */

static void test_select_and_delete_rules(void)
{
    void *bits;
    w32k_handle dc, b, dcw, win_h, other;
    w32k_init();
    dc = mem_dc(4, 4, &bits, &b);
    /* Bitmap in use cannot be deleted and cannot be selected into a second DC. */
    MT_EXPECT(!w32k_delete_object(b));
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_BUSY);
    other = w32k_create_compatible_dc(0);
    MT_EXPECT(w32k_select_object(other, b) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_BUSY);
    /* Only memory DCs take a bitmap. */
    win_h = win(0, 0, 10, 10);
    dcw = w32k_get_dc(win_h);
    MT_EXPECT(w32k_select_object(dcw, b) == 0);
    MT_EXPECT(w32k_release_dc(win_h, dcw));
    MT_EXPECT(w32k_delete_dc(other));
    /* Stock objects are never deleted. */
    MT_EXPECT(w32k_delete_object(w32k_get_stock_object(W32K_BLACK_BRUSH)));
    MT_EXPECT(w32k_get_object_type(w32k_get_stock_object(W32K_BLACK_PEN)) == W32K_OBJ_PEN);
    MT_EXPECT(w32k_get_object_type(w32k_get_stock_object(W32K_SYSTEM_FONT)) == W32K_OBJ_FONT);
    MT_EXPECT(w32k_get_stock_object(99) == 0);
    /* Selecting a brush returns the previous one. */
    {
        w32k_handle br = w32k_create_solid_brush(0x0000FF00u);
        w32k_handle prev = w32k_select_object(dc, br);
        MT_EXPECT(prev == w32k_get_stock_object(W32K_WHITE_BRUSH));
        MT_EXPECT_EQ(w32k_get_object_type(br), W32K_OBJ_BRUSH);
        MT_EXPECT(w32k_select_object(dc, prev) == br);
        MT_EXPECT(w32k_delete_object(br));
    }
    /* The default bitmap of a memory DC belongs to the DC. */
    {
        w32k_handle dc2 = w32k_create_compatible_dc(0);
        w32k_handle b2 = w32k_create_dib_section(2, 2, 32, NULL);
        w32k_handle def;
        MT_ASSERT(dc2 != 0 && b2 != 0);
        def = w32k_select_object(dc2, b2);
        MT_ASSERT(def != 0);
        MT_EXPECT(!w32k_delete_object(def));           /* the DC owns it */
        MT_EXPECT(w32k_select_object(dc2, def) == b2);
        MT_EXPECT(w32k_delete_dc(dc2));
        MT_EXPECT(!w32k_delete_object(def));           /* freed with the DC */
        MT_EXPECT(w32k_delete_object(b2));
    }
    MT_EXPECT(w32k_delete_dc(dc));
    MT_EXPECT(w32k_delete_object(b));
    MT_EXPECT(!w32k_delete_dc(dc)); /* stale */
}

static void test_pen_and_font_objects(void)
{
    w32k_handle pen, font, dc;
    w32k_init();
    pen = w32k_create_pen(0, 2, 0x00112233u);
    font = w32k_create_font(-12);
    MT_ASSERT(pen != 0 && font != 0);
    MT_EXPECT_EQ(w32k_get_object_type(pen), W32K_OBJ_PEN);
    MT_EXPECT_EQ(w32k_get_object_type(font), W32K_OBJ_FONT);
    MT_EXPECT(w32k_create_pen(0, -1, 0) == 0);
    dc = w32k_get_dc(0);
    MT_EXPECT(w32k_select_object(dc, pen) != 0);
    MT_EXPECT(w32k_select_object(dc, font) != 0);
    MT_EXPECT(!w32k_delete_object(pen)); /* selected */
    MT_EXPECT(w32k_release_dc(0, dc));
    MT_EXPECT(w32k_delete_object(pen));
    MT_EXPECT(w32k_delete_object(font));
    MT_EXPECT_EQ(w32k_get_object_type(0), 0);
}

static void test_arena_reuse_after_free(void)
{
    /* Fill the pixel arena with DIBs, free them all, and fill again: the same
     * count must fit, which shows the freed blocks coalesce. */
    w32k_handle handles[64];
    int first = 0, second = 0;
    w32k_init();
    for (int i = 0; i < 64; ++i) {
        handles[i] = w32k_create_dib_section(1000, 1000, 32, NULL);
        if (!handles[i])
            break;
        ++first;
    }
    MT_EXPECT(first >= 2 && first < 64);
    for (int i = 0; i < first; ++i)
        MT_EXPECT(w32k_delete_object(handles[i]));
    for (int i = 0; i < 64; ++i) {
        handles[i] = w32k_create_dib_section(1000, 1000, 32, NULL);
        if (!handles[i])
            break;
        ++second;
    }
    MT_EXPECT_EQ(second, first);
    for (int i = 0; i < second; ++i)
        MT_EXPECT(w32k_delete_object(handles[i]));
}

int main(void)
{
    static const mt_case_t cases[] = {
        {"screen_dc_caps", test_screen_dc_caps},
        {"window_dc_origin_clip", test_window_dc_origin_and_clip},
        {"fill_rect_memory_dc", test_fill_rect_and_colors_on_memory_dc},
        {"dib_section_rules", test_dib_section_rules},
        {"bitblt_rops", test_bitblt_rops},
        {"bitblt_overlap_clip", test_bitblt_overlap_and_clip},
        {"stretchblt_nearest", test_stretchblt_nearest},
        {"text_glyph_cells", test_text_glyph_cells},
        {"text_transparent_keeps_pixels", test_text_transparent_keeps_pixels},
        {"text_origin_offsets_cell", test_text_clipped_to_dc},
        {"select_delete_rules", test_select_and_delete_rules},
        {"pen_font_objects", test_pen_and_font_objects},
        {"arena_reuse_after_free", test_arena_reuse_after_free},
    };
    setvbuf(stdout, NULL, _IONBF, 0);
    return mt_run("test_w32k_gdi", cases, sizeof(cases) / sizeof(cases[0]));
}
