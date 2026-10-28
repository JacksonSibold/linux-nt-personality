/* w32k_region.c - GDI regions: rectangle-list objects, the boolean combinations, the
 * shape constructors, and the DC clip region.
 *
 * A region is a set of pixels stored as disjoint rectangles (half-open, as RECT is).
 * CombineRgn works one horizontal band at a time. The band edges are every top and
 * bottom in both inputs; within a band each input is a sorted list of x intervals, and
 * the operation is applied to the elementary x segments of the two lists. Adjacent
 * segments with the same extent in the previous band are merged vertically, so the
 * result of a given set does not depend on how it was built (EqualRgn relies on this).
 *
 * Limits. A region holds at most W32K_RGN_MAX_RECTS rectangles; a combination that
 * would need more fails with ERROR_NOT_ENOUGH_MEMORY, and the destination is left
 * unchanged. The pool holds W32K_RGN_MAX_OBJS regions.
 *
 * Shape constructors rasterise their outline on the pixel grid with the centre rule:
 * a pixel is in an ellipse when its centre is inside it. CreatePolygonRgn uses the
 * same rule for its scanline fill.
 */
#include "w32k_int.h"

#define W32K_RGN_MAX_RECTS 512
#define W32K_RGN_MAX_OBJS 64
#define RGN_MAX_COORD 16384

#define RGN_AND 1
#define RGN_OR 2
#define RGN_XOR 3
#define RGN_DIFF 4
#define RGN_COPY 5

#define RGN_NULL 1
#define RGN_SIMPLE 2
#define RGN_COMPLEX 3

typedef struct w32k_region_obj {
    w32k_hdr hdr;
    uint32_t n;
    w32k_rect rc[W32K_RGN_MAX_RECTS];
} w32k_region_obj;

static w32k_region_obj rgn_arr[W32K_RGN_MAX_OBJS];
static w32k_pool pool_rgn;

/* Scratch for CombineRgn: the two inputs are copied before the output is written, so
 * a destination that is also an input is safe. */
static w32k_rect scratch_a[W32K_RGN_MAX_RECTS];
static w32k_rect scratch_b[W32K_RGN_MAX_RECTS];
static w32k_rect scratch_out[W32K_RGN_MAX_RECTS];

static w32k_region_obj *rgn_of(w32k_handle h)
{
    return (w32k_region_obj *)w32k_pool_get(&pool_rgn, h);
}

static int32_t i32min(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static int32_t i32max(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

void w32k_region_init(void)
{
    w32k_zero(rgn_arr, sizeof(rgn_arr));
    pool_rgn.base = (uint8_t *)rgn_arr;
    pool_rgn.elem_size = sizeof(rgn_arr[0]);
    pool_rgn.count = W32K_RGN_MAX_OBJS;
    pool_rgn.type = W32K_T_REGION;
}

uint32_t w32k_region_count_live(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < W32K_RGN_MAX_OBJS; ++i)
        n += rgn_arr[i].hdr.used != 0;
    return n;
}

int w32k_region_is(w32k_handle h)
{
    return rgn_of(h) != NULL;
}

/* ---- Rectangle-list helpers ------------------------------------------------------- */

/* Interval list for one input in the band [y0, y1): x intervals sorted by left. */
typedef struct band_iv {
    int32_t l, r;
} band_iv;

static uint32_t band_intervals(const w32k_rect *rc, uint32_t n, int32_t y0, int32_t y1, band_iv *out)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (rc[i].top <= y0 && rc[i].bottom >= y1) {
            out[k].l = rc[i].left;
            out[k].r = rc[i].right;
            ++k;
        }
    }
    /* Sort by left. */
    for (uint32_t i = 1; i < k; ++i) {
        band_iv v = out[i];
        uint32_t j = i;
        while (j > 0 && out[j - 1].l > v.l) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = v;
    }
    return k;
}

static int covered(const band_iv *iv, uint32_t k, int32_t x0, int32_t x1)
{
    for (uint32_t i = 0; i < k; ++i) {
        if (iv[i].l <= x0 && iv[i].r >= x1)
            return 1;
    }
    return 0;
}

static int apply_op(int op, int a, int b)
{
    switch (op) {
    case RGN_AND: return a && b;
    case RGN_OR: return a || b;
    case RGN_XOR: return a != b;
    case RGN_DIFF: return a && !b;
    default: return 0;
    }
}

/* Combines two disjoint rectangle lists. Returns the number of output rectangles, or -1
 * when the result does not fit in out. */
static int combine_lists(const w32k_rect *a, uint32_t na, const w32k_rect *b, uint32_t nb,
                         int op, w32k_rect *out, uint32_t cap)
{
    int32_t ys[4 * W32K_RGN_MAX_RECTS];
    uint32_t ny = 0;
    uint32_t nout = 0;
    uint32_t prev_start = 0, prev_end = 0;
    band_iv ia[W32K_RGN_MAX_RECTS], ib[W32K_RGN_MAX_RECTS];

    /* Distinct band edges, sorted. */
    for (uint32_t i = 0; i < na; ++i) {
        ys[ny++] = a[i].top;
        ys[ny++] = a[i].bottom;
    }
    for (uint32_t i = 0; i < nb; ++i) {
        ys[ny++] = b[i].top;
        ys[ny++] = b[i].bottom;
    }
    for (uint32_t i = 1; i < ny; ++i) {
        int32_t v = ys[i];
        uint32_t j = i;
        while (j > 0 && ys[j - 1] > v) {
            ys[j] = ys[j - 1];
            --j;
        }
        ys[j] = v;
    }
    {
        uint32_t u = 0;
        for (uint32_t i = 0; i < ny; ++i) {
            if (u == 0 || ys[u - 1] != ys[i])
                ys[u++] = ys[i];
        }
        ny = u;
    }

    for (uint32_t yi = 0; yi + 1 < ny; ++yi) {
        int32_t y0 = ys[yi], y1 = ys[yi + 1];
        uint32_t ka, kb;
        int32_t xs[4 * W32K_RGN_MAX_RECTS];
        uint32_t nx = 0;
        uint32_t cur_start = nout;
        w32k_rect seg[4 * W32K_RGN_MAX_RECTS];
        uint32_t nseg = 0;

        ka = band_intervals(a, na, y0, y1, ia);
        kb = band_intervals(b, nb, y0, y1, ib);
        for (uint32_t i = 0; i < ka; ++i) {
            xs[nx++] = ia[i].l;
            xs[nx++] = ia[i].r;
        }
        for (uint32_t i = 0; i < kb; ++i) {
            xs[nx++] = ib[i].l;
            xs[nx++] = ib[i].r;
        }
        for (uint32_t i = 1; i < nx; ++i) {
            int32_t v = xs[i];
            uint32_t j = i;
            while (j > 0 && xs[j - 1] > v) {
                xs[j] = xs[j - 1];
                --j;
            }
            xs[j] = v;
        }
        /* Elementary segments between consecutive distinct x values. */
        for (uint32_t xi = 0; xi + 1 < nx; ++xi) {
            int32_t x0 = xs[xi], x1 = xs[xi + 1];
            int in;
            if (x1 <= x0)
                continue;
            in = apply_op(op, covered(ia, ka, x0, x1), covered(ib, kb, x0, x1));
            if (!in)
                continue;
            if (nseg > 0 && seg[nseg - 1].right == x0) {
                seg[nseg - 1].right = x1; /* adjacent true segments form one interval */
            } else {
                seg[nseg].left = x0;
                seg[nseg].right = x1;
                seg[nseg].top = y0;
                seg[nseg].bottom = y1;
                ++nseg;
            }
        }
        /* Vertical merge: extend a rectangle of the previous band with the same extent. */
        for (uint32_t s = 0; s < nseg; ++s) {
            int merged = 0;
            for (uint32_t k = prev_start; k < prev_end; ++k) {
                if (out[k].bottom == y0 && out[k].left == seg[s].left && out[k].right == seg[s].right) {
                    out[k].bottom = y1;
                    merged = 1;
                    break;
                }
            }
            if (!merged) {
                if (nout >= cap)
                    return -1;
                out[nout++] = seg[s];
            }
        }
        prev_start = cur_start;
        prev_end = nout;
    }
    return (int)nout;
}

static int region_type_of(uint32_t n)
{
    if (n == 0)
        return RGN_NULL;
    if (n == 1)
        return RGN_SIMPLE;
    return RGN_COMPLEX;
}

static int region_bounds(const w32k_rect *rc, uint32_t n, w32k_rect *out)
{
    if (n == 0) {
        out->left = out->top = out->right = out->bottom = 0;
        return RGN_NULL;
    }
    *out = rc[0];
    for (uint32_t i = 1; i < n; ++i) {
        out->left = i32min(out->left, rc[i].left);
        out->top = i32min(out->top, rc[i].top);
        out->right = i32max(out->right, rc[i].right);
        out->bottom = i32max(out->bottom, rc[i].bottom);
    }
    return region_type_of(n);
}

/* Stores a rectangle list in a region. Returns the region type, or 0 on overflow. */
static int region_store(w32k_region_obj *r, const w32k_rect *rc, int n)
{
    if (n < 0 || (uint32_t)n > W32K_RGN_MAX_RECTS)
        return 0;
    for (int i = 0; i < n; ++i)
        r->rc[i] = rc[i];
    r->n = (uint32_t)n;
    return region_type_of((uint32_t)n);
}

/* Stores a rectangle list as a new region object. Returns the handle, or 0. */
static w32k_handle region_new_from(const w32k_rect *rc, uint32_t n)
{
    w32k_handle h;
    w32k_region_obj *r = (w32k_region_obj *)w32k_pool_alloc(&pool_rgn, &h);
    if (!r) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (!region_store(r, rc, (int)n)) {
        w32k_pool_free(&pool_rgn, r);
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return h;
}

/* Builds a region from rows that are each a single interval (scanline output), by
 * combining them with union so that identical rows merge into rectangles. */
static w32k_handle region_from_rows(const w32k_rect *rows, uint32_t n)
{
    static w32k_rect acc[W32K_RGN_MAX_RECTS];
    uint32_t na = 0;
    int m = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (rows[i].right <= rows[i].left || rows[i].bottom <= rows[i].top)
            continue;
        if (na >= W32K_RGN_MAX_RECTS) {
            w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        acc[na++] = rows[i];
    }
    m = combine_lists(acc, na, acc, 0, RGN_OR, scratch_out, W32K_RGN_MAX_RECTS);
    if (m < 0) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return region_new_from(scratch_out, (uint32_t)m);
}

/* ---- Creation ----------------------------------------------------------------------- */

w32k_handle w32k_create_rect_rgn(int32_t l, int32_t t, int32_t r, int32_t b)
{
    w32k_rect rc;
    rc.left = l;
    rc.top = t;
    rc.right = r;
    rc.bottom = b;
    if (r <= l || b <= t)
        return region_new_from(NULL, 0);
    return region_new_from(&rc, 1);
}

w32k_handle w32k_create_rect_rgn_indirect(const w32k_rect *rc)
{
    if (!rc) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return w32k_create_rect_rgn(rc->left, rc->top, rc->right, rc->bottom);
}

/* Ellipse membership: the pixel (x, y) is inside when its centre is inside the ellipse
 * inscribed in [l, r) x [t, b). Exact integer test, with doubled coordinates. */
static int ellipse_contains(int32_t l, int32_t t, int32_t r, int32_t b, int32_t x, int32_t y)
{
    int64_t w = (int64_t)(r - l), h = (int64_t)(b - t);
    int64_t dx = 2 * (int64_t)x + 1 - (int64_t)(l + r);
    int64_t dy = 2 * (int64_t)y + 1 - (int64_t)(t + b);
    return dx * dx * h * h + dy * dy * w * w <= w * w * h * h;
}

w32k_handle w32k_create_elliptic_rgn(int32_t l, int32_t t, int32_t r, int32_t b)
{
    w32k_rect rows[W32K_RGN_MAX_RECTS];
    uint32_t n = 0;
    if (r <= l || b <= t || r - l > RGN_MAX_COORD || b - t > RGN_MAX_COORD) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (int32_t y = t; y < b; ++y) {
        int32_t x = l;
        while (x < r && !ellipse_contains(l, t, r, b, x, y))
            ++x;
        if (x == r)
            continue;
        {
            int32_t xe = r - 1;
            while (xe > x && !ellipse_contains(l, t, r, b, xe, y))
                --xe;
            if (n >= W32K_RGN_MAX_RECTS) {
                w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            rows[n].left = x;
            rows[n].right = xe + 1;
            rows[n].top = y;
            rows[n].bottom = y + 1;
            ++n;
        }
    }
    return region_from_rows(rows, n);
}

/* Round rectangle membership: inside the rectangle and, in each corner square of size
 * ew x eh, inside the corner's ellipse quadrant. */
static int round_contains(int32_t l, int32_t t, int32_t r, int32_t b, int32_t ew, int32_t eh,
                          int32_t x, int32_t y)
{
    if (x < l || x >= r || y < t || y >= b)
        return 0;
    if (ew <= 0 || eh <= 0)
        return 1;
    if (x < l + ew / 2 && y < t + eh / 2)
        return ellipse_contains(l, t, l + ew, t + eh, x, y);
    if (x >= r - ew / 2 && y < t + eh / 2)
        return ellipse_contains(r - ew, t, r, t + eh, x, y);
    if (x < l + ew / 2 && y >= b - eh / 2)
        return ellipse_contains(l, b - eh, l + ew, b, x, y);
    if (x >= r - ew / 2 && y >= b - eh / 2)
        return ellipse_contains(r - ew, b - eh, r, b, x, y);
    return 1;
}

w32k_handle w32k_create_round_rect_rgn(int32_t l, int32_t t, int32_t r, int32_t b, int32_t ew, int32_t eh)
{
    w32k_rect rows[W32K_RGN_MAX_RECTS];
    uint32_t n = 0;
    if (r <= l || b <= t || r - l > RGN_MAX_COORD || b - t > RGN_MAX_COORD) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (int32_t y = t; y < b; ++y) {
        int32_t x = l;
        while (x < r && !round_contains(l, t, r, b, ew, eh, x, y))
            ++x;
        if (x == r)
            continue;
        {
            int32_t xe = r - 1;
            while (xe > x && !round_contains(l, t, r, b, ew, eh, xe, y))
                --xe;
            if (n >= W32K_RGN_MAX_RECTS) {
                w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            rows[n].left = x;
            rows[n].right = xe + 1;
            rows[n].top = y;
            rows[n].bottom = y + 1;
            ++n;
        }
    }
    return region_from_rows(rows, n);
}

/* floor(a / 2) and ceil(a / 2) for signed 64-bit a (C division truncates toward zero). */
static int64_t floor_half(int64_t a)
{
    return a >= 0 ? a / 2 : -((-a + 1) / 2);
}

static int64_t ceil_half(int64_t a)
{
    return -floor_half(-a);
}

/* Polygon fill. Alternate (even-odd) or winding fill of the polygon with the centre rule:
 * a pixel row is scanned at y + 0.5, the edge crossings are sorted, and the spans are
 * filled per the rule. Edges are the closed polygon. */
w32k_handle w32k_create_polygon_rgn(const w32k_point *pts, int n, int fill_winding)
{
    w32k_rect rows[W32K_RGN_MAX_RECTS];
    uint32_t nrows = 0;
    int32_t ymin, ymax;
    if (!pts || n < 3 || n > 256) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    ymin = pts[0].y;
    ymax = pts[0].y;
    for (int i = 1; i < n; ++i) {
        if (pts[i].y < ymin)
            ymin = pts[i].y;
        if (pts[i].y > ymax)
            ymax = pts[i].y;
    }
    for (int32_t y = ymin; y < ymax; ++y) {
        /* Crossings at y + 0.5: x in doubled units, with the winding direction. */
        int64_t xs[256];
        int dirs[256];
        int nc = 0;
        int64_t sy = 2 * (int64_t)y + 1;
        for (int i = 0; i < n; ++i) {
            const w32k_point *p = &pts[i];
            const w32k_point *q = &pts[(i + 1) % n];
            int64_t y0 = 2 * (int64_t)p->y, y1 = 2 * (int64_t)q->y;
            if (y0 == y1)
                continue;
            if (!((y0 <= sy && sy < y1) || (y1 <= sy && sy < y0)))
                continue;
            {
                int64_t x0 = 2 * (int64_t)p->x, x1 = 2 * (int64_t)q->x;
                int64_t xc = x0 + (x1 - x0) * (sy - y0) / (y1 - y0);
                if (nc < 256) {
                    xs[nc] = xc;
                    dirs[nc] = y1 > y0 ? 1 : -1;
                    ++nc;
                }
            }
        }
        /* Sort crossings by x. */
        for (int i = 1; i < nc; ++i) {
            int64_t vx = xs[i];
            int vd = dirs[i];
            int j = i;
            while (j > 0 && xs[j - 1] > vx) {
                xs[j] = xs[j - 1];
                dirs[j] = dirs[j - 1];
                --j;
            }
            xs[j] = vx;
            dirs[j] = vd;
        }
        /* Walk spans. A span is inside when the rule says so. */
        {
            int wind = 0;
            for (int i = 0; i + 1 < nc; ++i) {
                int inside;
                wind += dirs[i];
                inside = fill_winding ? (wind != 0) : ((i & 1) == 0);
                if (!inside)
                    continue;
                {
                    /* Pixel x is inside the span [lo, hi) (doubled units) when its centre
                     * 2x + 1 lies in it, so x runs over [ceil((lo - 1) / 2), ceil((hi - 1) / 2)). */
                    int64_t lo = xs[i], hi = xs[i + 1];
                    int64_t pl = ceil_half(lo - 1);
                    int64_t pr = ceil_half(hi - 1);
                    if (pr > pl) {
                        if (nrows >= W32K_RGN_MAX_RECTS) {
                            w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
                            return 0;
                        }
                        rows[nrows].left = (int32_t)pl;
                        rows[nrows].right = (int32_t)pr;
                        rows[nrows].top = y;
                        rows[nrows].bottom = y + 1;
                        ++nrows;
                    }
                }
            }
        }
    }
    return region_from_rows(rows, nrows);
}

/* ---- Operations ------------------------------------------------------------------ */

int w32k_set_rect_rgn(w32k_handle h, int32_t l, int32_t t, int32_t r, int32_t b)
{
    w32k_region_obj *dst = rgn_of(h);
    w32k_rect rc;
    if (!dst) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    rc.left = l;
    rc.top = t;
    rc.right = r;
    rc.bottom = b;
    if (r <= l || b <= t)
        dst->n = 0;
    else
        region_store(dst, &rc, 1);
    return 1;
}

/* Combines two regions into dst. Returns the region type of the result, or 0 on error. */
int w32k_combine_rgn(w32k_handle hdst, w32k_handle ha, w32k_handle hb, int mode)
{
    w32k_region_obj *dst = rgn_of(hdst), *a = rgn_of(ha), *b = rgn_of(hb);
    int m;
    uint32_t na, nb;
    if (!dst || !a || (mode != RGN_COPY && !b) || mode < RGN_AND || mode > RGN_COPY) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    na = a->n;
    for (uint32_t i = 0; i < na; ++i)
        scratch_a[i] = a->rc[i];
    nb = b ? b->n : 0;
    for (uint32_t i = 0; i < nb; ++i)
        scratch_b[i] = b->rc[i];
    if (mode == RGN_COPY) {
        m = (int)na;
        for (uint32_t i = 0; i < na; ++i)
            scratch_out[i] = scratch_a[i];
    } else {
        m = combine_lists(scratch_a, na, scratch_b, nb, mode, scratch_out, W32K_RGN_MAX_RECTS);
    }
    if (m < 0) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (!region_store(dst, scratch_out, m)) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return region_type_of((uint32_t)m);
}

int w32k_get_rgn_box(w32k_handle h, w32k_rect *out)
{
    w32k_region_obj *r = rgn_of(h);
    if (!r || !out) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    return region_bounds(r->rc, r->n, out);
}

int w32k_pt_in_region(w32k_handle h, int32_t x, int32_t y)
{
    w32k_region_obj *r = rgn_of(h);
    if (!r)
        return 0;
    for (uint32_t i = 0; i < r->n; ++i) {
        if (x >= r->rc[i].left && x < r->rc[i].right && y >= r->rc[i].top && y < r->rc[i].bottom)
            return 1;
    }
    return 0;
}

int w32k_region_contains(w32k_handle h, int32_t x, int32_t y)
{
    return w32k_pt_in_region(h, x, y);
}

/* RectInRegion: any pixel of the rectangle is in the region. */
int w32k_rect_in_region(w32k_handle h, const w32k_rect *rc)
{
    w32k_region_obj *r = rgn_of(h);
    if (!r || !rc)
        return 0;
    if (rc->right <= rc->left || rc->bottom <= rc->top)
        return 0;
    for (uint32_t i = 0; i < r->n; ++i) {
        if (rc->left < r->rc[i].right && rc->right > r->rc[i].left &&
            rc->top < r->rc[i].bottom && rc->bottom > r->rc[i].top)
            return 1;
    }
    return 0;
}

/* EqualRgn: the symmetric difference is empty. Both empty regions are equal. */
int w32k_equal_rgn(w32k_handle ha, w32k_handle hb)
{
    w32k_region_obj *a = rgn_of(ha), *b = rgn_of(hb);
    int m;
    if (!a || !b)
        return 0;
    if (a->n == b->n) {
        int same = 1;
        for (uint32_t i = 0; i < a->n && same; ++i)
            same = a->rc[i].left == b->rc[i].left && a->rc[i].top == b->rc[i].top &&
                   a->rc[i].right == b->rc[i].right && a->rc[i].bottom == b->rc[i].bottom;
        if (same)
            return 1;
    }
    for (uint32_t i = 0; i < a->n; ++i)
        scratch_a[i] = a->rc[i];
    for (uint32_t i = 0; i < b->n; ++i)
        scratch_b[i] = b->rc[i];
    m = combine_lists(scratch_a, a->n, scratch_b, b->n, RGN_XOR, scratch_out, W32K_RGN_MAX_RECTS);
    return m == 0;
}

int w32k_offset_rgn(w32k_handle h, int32_t dx, int32_t dy)
{
    w32k_region_obj *r = rgn_of(h);
    if (!r) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    for (uint32_t i = 0; i < r->n; ++i) {
        r->rc[i].left += dx;
        r->rc[i].right += dx;
        r->rc[i].top += dy;
        r->rc[i].bottom += dy;
    }
    return region_type_of(r->n);
}

int w32k_delete_region(w32k_handle h)
{
    w32k_region_obj *r = rgn_of(h);
    if (!r) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    w32k_pool_free(&pool_rgn, r);
    return 1;
}

/* Copies a region into a new object. Used for the DC clip, which owns its copy. */
w32k_handle w32k_region_dup(w32k_handle src)
{
    w32k_region_obj *r = rgn_of(src);
    if (!r) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    return region_new_from(r->rc, r->n);
}

/* Copies the rectangles of src into dst. Returns 0 if either is not a region. */
int w32k_region_copy_into(w32k_handle dst, w32k_handle src)
{
    w32k_region_obj *d = rgn_of(dst), *s = rgn_of(src);
    if (!d || !s)
        return 0;
    if (d == s)
        return 1;
    return region_store(d, s->rc, (int)s->n) != 0;
}

uint32_t w32k_region_rect_count(w32k_handle h)
{
    w32k_region_obj *r = rgn_of(h);
    return r ? r->n : 0;
}

int w32k_region_rect_at(w32k_handle h, uint32_t i, w32k_rect *out)
{
    w32k_region_obj *r = rgn_of(h);
    if (!r || i >= r->n || !out)
        return 0;
    *out = r->rc[i];
    return 1;
}
