/* test_w32k_region.c - GDI regions (w32k_region.c): rectangle creation and bounds, the
 * boolean combinations (exact shapes and rectangle counts), EqualRgn canonical form,
 * offsets and hit tests, the ellipse, round-rect and polygon constructors, the overflow
 * rule, and the DC clip (SelectClipRgn, GetClipRgn, GetClipBox) applied to drawing. */
#include "minitest.h"
#include "w32k.h"
#include "w32k_int.h"

#include <string.h>

static w32k_rect R(int32_t l, int32_t t, int32_t r, int32_t b)
{
    w32k_rect rc;
    rc.left = l;
    rc.top = t;
    rc.right = r;
    rc.bottom = b;
    return rc;
}

static w32k_handle rgn(int32_t l, int32_t t, int32_t r, int32_t b)
{
    w32k_handle h = w32k_create_rect_rgn(l, t, r, b);
    if (h == 0)
        mt_fail(__FILE__, __LINE__, "rgn: create_rect_rgn failed");
    return h;
}

static uint32_t count(w32k_handle h)
{
    return w32k_region_rect_count(h);
}

/* Number of pixels inside a region, by scanning its bounding box. */
static int area(w32k_handle h)
{
    w32k_rect b;
    int n = 0;
    if (!w32k_get_rgn_box(h, &b))
        return 0;
    for (int32_t y = b.top; y < b.bottom; ++y)
        for (int32_t x = b.left; x < b.right; ++x)
            n += w32k_pt_in_region(h, x, y);
    return n;
}

/* ---- Rectangles ------------------------------------------------------------------- */

static void rect_region_bounds_and_type(void)
{
    w32k_init();
    w32k_rect b;
    w32k_init();
    w32k_handle h = rgn(2, 3, 10, 9);
    MT_EXPECT_EQ(w32k_get_rgn_box(h, &b), W32K_SIMPLEREGION);
    MT_EXPECT(b.left == 2 && b.top == 3 && b.right == 10 && b.bottom == 9);
    MT_EXPECT_EQ(count(h), 1);
    MT_EXPECT_EQ(area(h), 8 * 6);
    MT_EXPECT_EQ(w32k_get_object_type(h), W32K_OBJ_REGION);
    /* Empty rectangles make a null region. */
    w32k_handle e = rgn(5, 5, 5, 9);
    MT_EXPECT_EQ(w32k_get_rgn_box(e, &b), W32K_NULLREGION);
    MT_EXPECT_EQ(count(e), 0);
    MT_EXPECT(w32k_delete_region(h));
    MT_EXPECT(w32k_delete_region(e));
}

static void half_open_hit_test(void)
{
    w32k_init();
    w32k_handle h = rgn(10, 10, 20, 20);
    MT_EXPECT(w32k_pt_in_region(h, 10, 10));
    MT_EXPECT(w32k_pt_in_region(h, 19, 19));
    MT_EXPECT(!w32k_pt_in_region(h, 20, 10));
    MT_EXPECT(!w32k_pt_in_region(h, 10, 20));
    MT_EXPECT(!w32k_pt_in_region(h, 9, 15));
    MT_EXPECT(w32k_region_contains(h, 15, 15));
    w32k_rect over = R(19, 19, 30, 30), away = R(20, 20, 30, 30);
    MT_EXPECT(w32k_rect_in_region(h, &over));
    MT_EXPECT(!w32k_rect_in_region(h, &away));
    MT_EXPECT(w32k_delete_region(h));
}

static void set_rect_and_offset(void)
{
    w32k_init();
    w32k_handle h = rgn(0, 0, 4, 4);
    w32k_rect b;
    MT_EXPECT(w32k_set_rect_rgn(h, 1, 1, 5, 2));
    MT_EXPECT_EQ(w32k_get_rgn_box(h, &b), W32K_SIMPLEREGION);
    MT_EXPECT(b.left == 1 && b.top == 1 && b.right == 5 && b.bottom == 2);
    MT_EXPECT_EQ(w32k_offset_rgn(h, 10, -1), W32K_SIMPLEREGION);
    MT_EXPECT(w32k_get_rgn_box(h, &b));
    MT_EXPECT(b.left == 11 && b.top == 0 && b.right == 15 && b.bottom == 1);
    MT_EXPECT(w32k_set_rect_rgn(h, 3, 3, 3, 9));
    MT_EXPECT_EQ(w32k_get_rgn_box(h, &b), W32K_NULLREGION);
    MT_EXPECT(w32k_delete_region(h));
}

/* ---- Combinations ----------------------------------------------------------------- */

static void combine_or_disjoint(void)
{
    w32k_init();
    w32k_handle a = rgn(0, 0, 4, 4), b = rgn(10, 0, 14, 4), d = rgn(0, 0, 1, 1);
    MT_EXPECT_EQ(w32k_combine_rgn(d, a, b, W32K_RGN_OR), W32K_COMPLEXREGION);
    MT_EXPECT_EQ(count(d), 2);
    MT_EXPECT_EQ(area(d), 32);
    MT_EXPECT(w32k_pt_in_region(d, 12, 2));
    MT_EXPECT(!w32k_pt_in_region(d, 7, 2));
    MT_EXPECT(w32k_delete_region(a) && w32k_delete_region(b) && w32k_delete_region(d));
}

static void combine_and_overlap(void)
{
    w32k_init();
    w32k_handle a = rgn(0, 0, 10, 10), b = rgn(5, 4, 20, 8), d = rgn(0, 0, 1, 1);
    w32k_rect box;
    MT_EXPECT_EQ(w32k_combine_rgn(d, a, b, W32K_RGN_AND), W32K_SIMPLEREGION);
    MT_EXPECT_EQ(w32k_get_rgn_box(d, &box), W32K_SIMPLEREGION);
    MT_EXPECT(box.left == 5 && box.top == 4 && box.right == 10 && box.bottom == 8);
    MT_EXPECT_EQ(area(d), 5 * 4);
    MT_EXPECT(w32k_delete_region(a) && w32k_delete_region(b) && w32k_delete_region(d));
}

/* A minus a centred square is a ring of four rectangles, and the bands are exact. */
static void combine_diff_ring(void)
{
    w32k_init();
    w32k_handle a = rgn(0, 0, 10, 10), b = rgn(3, 3, 7, 7), d = rgn(0, 0, 1, 1);
    MT_EXPECT_EQ(w32k_combine_rgn(d, a, b, W32K_RGN_DIFF), W32K_COMPLEXREGION);
    MT_EXPECT_EQ(count(d), 4);
    MT_EXPECT_EQ(area(d), 100 - 16);
    MT_EXPECT(!w32k_pt_in_region(d, 5, 5));
    MT_EXPECT(w32k_pt_in_region(d, 1, 5));
    MT_EXPECT(w32k_pt_in_region(d, 5, 1));
    MT_EXPECT(w32k_delete_region(a) && w32k_delete_region(b) && w32k_delete_region(d));
}

static void combine_xor_and_copy(void)
{
    w32k_init();
    w32k_handle a = rgn(0, 0, 6, 6), b = rgn(3, 0, 9, 6), d = rgn(0, 0, 1, 1);
    MT_EXPECT_EQ(w32k_combine_rgn(d, a, b, W32K_RGN_XOR), W32K_COMPLEXREGION);
    MT_EXPECT_EQ(area(d), 18 + 18); /* two 3x6 halves */
    MT_EXPECT(!w32k_pt_in_region(d, 4, 2));
    MT_EXPECT_EQ(w32k_combine_rgn(d, b, 0, W32K_RGN_COPY), W32K_SIMPLEREGION);
    MT_EXPECT_EQ(area(d), 6 * 6);
    MT_EXPECT(w32k_pt_in_region(d, 8, 5));
    /* XOR with itself is empty. */
    MT_EXPECT_EQ(w32k_combine_rgn(d, a, a, W32K_RGN_XOR), W32K_NULLREGION);
    MT_EXPECT_EQ(area(d), 0);
    MT_EXPECT(w32k_delete_region(a) && w32k_delete_region(b) && w32k_delete_region(d));
}

/* The destination may be one of the inputs. */
static void combine_into_input(void)
{
    w32k_init();
    w32k_handle a = rgn(0, 0, 4, 4), b = rgn(2, 2, 6, 6);
    MT_EXPECT_EQ(w32k_combine_rgn(a, a, b, W32K_RGN_OR), W32K_COMPLEXREGION);
    MT_EXPECT_EQ(area(a), 16 + 16 - 4);
    MT_EXPECT(w32k_delete_region(a) && w32k_delete_region(b));
}

/* Two shapes with the same pixels built differently are equal, and the result is one
 * rectangle: vertically adjacent bands with equal extents merge. */
static void canonical_equal(void)
{
    w32k_init();
    w32k_handle whole = rgn(0, 0, 10, 10);
    w32k_handle top = rgn(0, 0, 10, 5), bottom = rgn(0, 5, 10, 10);
    w32k_handle built = rgn(0, 0, 1, 1), other = rgn(0, 0, 1, 1);
    MT_EXPECT_EQ(w32k_combine_rgn(built, top, bottom, W32K_RGN_OR), W32K_SIMPLEREGION);
    MT_EXPECT_EQ(count(built), 1);
    MT_EXPECT(w32k_equal_rgn(whole, built));
    MT_EXPECT_EQ(w32k_combine_rgn(other, whole, rgn(20, 20, 21, 21), W32K_RGN_OR), W32K_COMPLEXREGION);
    MT_EXPECT(!w32k_equal_rgn(whole, other));
    MT_EXPECT(w32k_equal_rgn(other, other));
    MT_EXPECT(w32k_delete_region(whole) && w32k_delete_region(top) && w32k_delete_region(bottom));
    MT_EXPECT(w32k_delete_region(built) && w32k_delete_region(other));
}

/* A: 300 vertical cells in the band y 0..10. B: 300 cells in the band y 20..30. Their
 * union is 600 disjoint rectangles, over the limit of 512. The combination fails, and the
 * destination keeps its old rectangle. */
static void combine_overflow_leaves_destination(void)
{
    w32k_init();
    w32k_handle dst = rgn(0, 0, 1, 1);
    w32k_handle a = rgn(0, 0, 1, 10);
    w32k_handle b = rgn(0, 20, 1, 30);
    w32k_rect rc;
    for (int i = 1; i < 300; ++i) {
        w32k_handle cell = rgn(2 * i, 0, 2 * i + 1, 10);
        MT_ASSERT(w32k_combine_rgn(a, a, cell, W32K_RGN_OR) != 0);
        w32k_delete_region(cell);
    }
    for (int i = 1; i < 300; ++i) {
        w32k_handle cell = rgn(2 * i, 20, 2 * i + 1, 30);
        MT_ASSERT(w32k_combine_rgn(b, b, cell, W32K_RGN_OR) != 0);
        w32k_delete_region(cell);
    }
    MT_EXPECT_EQ(count(a), 300);
    MT_EXPECT_EQ(count(b), 300);
    w32k_set_error(0);
    MT_EXPECT_EQ(w32k_combine_rgn(dst, a, b, W32K_RGN_OR), 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_NOT_ENOUGH_MEMORY);
    MT_EXPECT_EQ(w32k_get_rgn_box(dst, &rc), W32K_SIMPLEREGION);
    MT_EXPECT(rc.left == 0 && rc.right == 1);
    w32k_delete_region(a);
    w32k_delete_region(b);
    w32k_delete_region(dst);
}

/* ---- Shapes ----------------------------------------------------------------------- */

static void ellipse_region_shape(void)
{
    w32k_init();
    w32k_handle h = w32k_create_elliptic_rgn(0, 0, 20, 20);
    w32k_rect box;
    MT_ASSERT(h != 0);
    MT_EXPECT(w32k_pt_in_region(h, 10, 10));
    MT_EXPECT(!w32k_pt_in_region(h, 0, 0));
    MT_EXPECT(!w32k_pt_in_region(h, 19, 19));
    MT_EXPECT(w32k_pt_in_region(h, 0, 10));
    MT_EXPECT_EQ(w32k_get_rgn_box(h, &box), W32K_COMPLEXREGION); /* rows differ in extent */
    MT_EXPECT(box.left == 0 && box.top == 0 && box.right == 20 && box.bottom == 20);
    /* Area of a circle of radius 10 is 314.16; the centre rule gives a value close to it. */
    {
        int a = area(h);
        MT_EXPECT(a > 300 && a < 330);
    }
    MT_EXPECT(w32k_delete_region(h));
}

static void round_rect_region_corners(void)
{
    w32k_init();
    w32k_handle h = w32k_create_round_rect_rgn(0, 0, 20, 20, 8, 8);
    MT_ASSERT(h != 0);
    MT_EXPECT(w32k_pt_in_region(h, 10, 0));
    MT_EXPECT(w32k_pt_in_region(h, 0, 10));
    MT_EXPECT(!w32k_pt_in_region(h, 0, 0));
    MT_EXPECT(!w32k_pt_in_region(h, 19, 19));
    MT_EXPECT(w32k_pt_in_region(h, 10, 10));
    /* Zero corner size is a plain rectangle. */
    w32k_handle plain = w32k_create_round_rect_rgn(0, 0, 20, 20, 0, 0);
    MT_EXPECT(w32k_pt_in_region(plain, 0, 0));
    MT_EXPECT(w32k_delete_region(h) && w32k_delete_region(plain));
}

static void polygon_triangle_and_square(void)
{
    w32k_init();
    static const w32k_point tri[] = {{0, 0}, {10, 0}, {0, 10}};
    static const w32k_point sq[] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    w32k_handle t = w32k_create_polygon_rgn(tri, 3, 0);
    w32k_handle s = w32k_create_polygon_rgn(sq, 4, 0);
    MT_ASSERT(t != 0 && s != 0);
    MT_EXPECT(w32k_pt_in_region(t, 1, 1));
    MT_EXPECT(!w32k_pt_in_region(t, 9, 9));
    /* A right triangle with legs 10 has about 50 pixels. */
    {
        int a = area(t);
        MT_EXPECT(a >= 45 && a <= 60);
    }
    MT_EXPECT_EQ(area(s), 100);
    MT_EXPECT(w32k_delete_region(t) && w32k_delete_region(s));
}

/* A pentagram (self-intersecting, the five vertices in star order). The centre pentagon
 * is crossed twice by a ray to its right, so the alternate rule leaves it out, while the
 * winding number there is 2 and the winding rule fills it. A point inside one tip is
 * filled under both rules. */
static void polygon_fill_rules_differ(void)
{
    w32k_init();
    static const w32k_point star[] = {{50, 0}, {79, 90}, {2, 35}, {98, 35}, {21, 90}};
    w32k_handle alt = w32k_create_polygon_rgn(star, 5, 0);
    w32k_handle wind = w32k_create_polygon_rgn(star, 5, 1);
    MT_ASSERT(alt != 0 && wind != 0);
    MT_EXPECT(!w32k_pt_in_region(alt, 50, 50));
    MT_EXPECT(w32k_pt_in_region(wind, 50, 50));
    MT_EXPECT(w32k_pt_in_region(alt, 50, 20));
    MT_EXPECT(w32k_pt_in_region(wind, 50, 20));
    MT_EXPECT(w32k_delete_region(alt) && w32k_delete_region(wind));
}

static void polygon_rejects_bad_input(void)
{
    w32k_init();
    static const w32k_point two[] = {{0, 0}, {1, 1}};
    w32k_set_error(0);
    MT_EXPECT(w32k_create_polygon_rgn(two, 2, 0) == 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_PARAMETER);
    MT_EXPECT(w32k_create_polygon_rgn(NULL, 4, 0) == 0);
}

/* ---- DC clip ---------------------------------------------------------------------- */

static void dc_clip_limits_fill(void)
{
    w32k_init();
    void *bits;
    w32k_handle dc, bmp, clip;
    uint32_t *p;
    w32k_init();
    dc = w32k_create_compatible_dc(0);
    bmp = w32k_create_dib_section(32, 16, 32, &bits);
    MT_ASSERT(dc != 0 && bmp != 0);
    MT_ASSERT(w32k_select_object(dc, bmp) != 0);
    p = (uint32_t *)bits;
    memset(p, 0, 32 * 16 * 4);
    clip = rgn(0, 0, 8, 8);
    MT_EXPECT_EQ(w32k_select_clip_rgn(dc, clip), W32K_SIMPLEREGION);
    {
        w32k_handle brush = w32k_create_solid_brush(0x00FF00u);
        w32k_rect whole = R(0, 0, 32, 16);
        MT_ASSERT(brush != 0);
        MT_ASSERT(w32k_fill_rect(dc, &whole, brush));
    }
    MT_EXPECT_EQ(p[0], 0x00FF00u);
    MT_EXPECT_EQ(p[7], 0x00FF00u);
    MT_EXPECT_EQ(p[8], 0);
    MT_EXPECT_EQ(p[31], 0); /* row 0, column 31: outside the clip */
    MT_EXPECT_EQ(p[8 * 32], 0);    /* row 8, outside the clip */
    /* Clear the clip: drawing reaches the rest of the surface again. */
    MT_EXPECT_EQ(w32k_select_clip_rgn(dc, 0), W32K_SIMPLEREGION);
    MT_EXPECT(w32k_get_clip_rgn(dc, clip) == 0);
    {
        w32k_handle brush = w32k_create_solid_brush(0x0000FFu);
        w32k_rect right = R(20, 0, 32, 16);
        MT_ASSERT(w32k_fill_rect(dc, &right, brush));
    }
    MT_EXPECT_EQ(p[20], 0xFF0000u); /* COLORREF 0x0000FF is pixel 0xFF0000 */
    MT_EXPECT(w32k_delete_region(clip));
    w32k_delete_dc(dc);
}

static void dc_clip_region_queries(void)
{
    w32k_init();
    w32k_handle dc, clip, got;
    w32k_rect box;
    w32k_init();
    dc = w32k_create_compatible_dc(0);
    MT_ASSERT(dc != 0);
    /* No clip: the box is the surface (1x1 memory DC). */
    MT_EXPECT_EQ(w32k_get_clip_box(dc, &box), W32K_SIMPLEREGION);
    clip = rgn(2, 0, 4, 1);
    got = rgn(0, 0, 1, 1);
    MT_EXPECT_EQ(w32k_select_clip_rgn(dc, clip), W32K_SIMPLEREGION);
    MT_EXPECT_EQ(w32k_get_clip_rgn(dc, got), 1);
    MT_EXPECT(w32k_equal_rgn(got, clip));
    /* Only the part inside the 1x1 surface remains: nothing. */
    MT_EXPECT_EQ(w32k_get_clip_box(dc, &box), W32K_NULLREGION);
    MT_EXPECT(w32k_delete_region(clip) && w32k_delete_region(got));
    w32k_delete_dc(dc);
}

static void dc_clip_rejects_bad_handles(void)
{
    w32k_init();
    w32k_handle dc = w32k_create_compatible_dc(0);
    w32k_set_error(0);
    MT_EXPECT_EQ(w32k_select_clip_rgn(dc, 0x7777), 0);
    MT_EXPECT_EQ(w32k_get_last_error(), W32K_ERROR_INVALID_HANDLE);
    MT_EXPECT(w32k_delete_dc(dc));
}

static void delete_region_object(void)
{
    w32k_init();
    w32k_handle h = rgn(0, 0, 3, 3);
    MT_EXPECT(w32k_delete_region(h));
    MT_EXPECT(!w32k_delete_region(h));
    MT_EXPECT_EQ(w32k_region_count_live(), 0);
}

static const mt_case_t cases[] = {
    MT_CASE(rect_region_bounds_and_type),
    MT_CASE(half_open_hit_test),
    MT_CASE(set_rect_and_offset),
    MT_CASE(combine_or_disjoint),
    MT_CASE(combine_and_overlap),
    MT_CASE(combine_diff_ring),
    MT_CASE(combine_xor_and_copy),
    MT_CASE(combine_into_input),
    MT_CASE(canonical_equal),
    MT_CASE(combine_overflow_leaves_destination),
    MT_CASE(ellipse_region_shape),
    MT_CASE(round_rect_region_corners),
    MT_CASE(polygon_triangle_and_square),
    MT_CASE(polygon_fill_rules_differ),
    MT_CASE(polygon_rejects_bad_input),
    MT_CASE(dc_clip_limits_fill),
    MT_CASE(dc_clip_region_queries),
    MT_CASE(dc_clip_rejects_bad_handles),
    MT_CASE(delete_region_object),
};

int main(void)
{
    return MT_RUN("w32k_region", cases);
}
