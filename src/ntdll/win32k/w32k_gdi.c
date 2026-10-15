/* w32k_gdi.c - GDI objects (DC, bitmap, brush, pen, font), the software 32bpp
 * surface, and the drawing operations used by the model.
 *
 * Pixels are stored as 0x00RRGGBB in top-down rows, which is BGRA byte order
 * on little-endian. COLORREF values (0x00BBGGRR) are converted at the API
 * boundary. The screen surface is a fixed 1920x1080 framebuffer; window DCs
 * draw into it with the window's client origin and clip. Pixel memory comes
 * from a fixed arena (first fit, coalescing), so the code needs no libc.
 *
 * TextOut is a placeholder: each non-space character fills one 8x16 cell in the
 * text colour. There is no font rasteriser in this model.
 */
#include "w32k_int.h"

#define GDI_ARENA_BYTES (24u * 1024u * 1024u)
#define GDI_FB_W 1920
#define GDI_FB_H 1080
#define GDI_CELL_W 8
#define GDI_CELL_H 16
#define GDI_MAX_DC 256
#define GDI_MAX_BMP 256
#define GDI_MAX_BRUSH 256
#define GDI_MAX_PEN 256
#define GDI_MAX_FONT 128

typedef struct gdi_blk {
    uint32_t size;
    uint32_t used;
} gdi_blk;

typedef struct w32k_bitmap {
    w32k_hdr hdr;
    int32_t w, h;
    uint32_t *bits;
    int owned_by_dc;
} w32k_bitmap;

typedef struct w32k_dc {
    w32k_hdr hdr;
    int memory;
    w32k_handle hwnd;
    w32k_handle surf;
    w32k_handle default_bmp;
    w32k_handle brush, pen, font;
    int32_t org_x, org_y;
    w32k_rect clip;
    uint32_t text_color;
    uint32_t bk_color;
    int bk_mode;
} w32k_dc;

typedef struct w32k_brush {
    w32k_hdr hdr;
    uint32_t color;
    int null_brush;
} w32k_brush;

typedef struct w32k_pen {
    w32k_hdr hdr;
    uint32_t style;
    int32_t width;
    uint32_t color;
} w32k_pen;

typedef struct w32k_font {
    w32k_hdr hdr;
    int32_t height;
} w32k_font;

static uint64_t gdi_arena[GDI_ARENA_BYTES / 8u];
static w32k_bitmap bmp_arr[GDI_MAX_BMP];
static w32k_dc dc_arr[GDI_MAX_DC];
static w32k_brush brush_arr[GDI_MAX_BRUSH];
static w32k_pen pen_arr[GDI_MAX_PEN];
static w32k_font font_arr[GDI_MAX_FONT];

static w32k_pool pool_bmp;
static w32k_pool pool_dc;
static w32k_pool pool_brush;
static w32k_pool pool_pen;
static w32k_pool pool_font;

static w32k_handle framebuffer_h;

/* ---- Arena ------------------------------------------------------------------ */

static uint8_t *arena_base(void)
{
    return (uint8_t *)gdi_arena;
}

static void arena_reset(void)
{
    gdi_blk *b = (gdi_blk *)arena_base();
    b->size = GDI_ARENA_BYTES - (uint32_t)sizeof(gdi_blk);
    b->used = 0;
}

static void *arena_alloc(size_t n)
{
    uint8_t *p = arena_base();
    uint8_t *end = arena_base() + GDI_ARENA_BYTES;
    size_t need = (n + 7u) & ~(size_t)7u;
    if (need == 0)
        need = 8;
    while (p + sizeof(gdi_blk) <= end) {
        gdi_blk *b = (gdi_blk *)p;
        if (!b->used && b->size >= need) {
            size_t rest = b->size - need;
            if (rest >= sizeof(gdi_blk) + 8u) {
                gdi_blk *nb = (gdi_blk *)(p + sizeof(gdi_blk) + need);
                nb->size = (uint32_t)(rest - sizeof(gdi_blk));
                nb->used = 0;
                b->size = (uint32_t)need;
            }
            b->used = 1;
            return p + sizeof(gdi_blk);
        }
        p += sizeof(gdi_blk) + b->size;
    }
    return NULL;
}

static void arena_free(void *ptr)
{
    uint8_t *target = (uint8_t *)ptr - sizeof(gdi_blk);
    uint8_t *p = arena_base();
    uint8_t *end = arena_base() + GDI_ARENA_BYTES;
    gdi_blk *prev = NULL;
    while (p + sizeof(gdi_blk) <= end) {
        gdi_blk *b = (gdi_blk *)p;
        if (p == target) {
            b->used = 0;
            break;
        }
        prev = b;
        p += sizeof(gdi_blk) + b->size;
    }
    /* Coalesce this free block with following free blocks, then with the previous one. */
    {
        gdi_blk *cur = (gdi_blk *)target;
        while (1) {
            uint8_t *nx = (uint8_t *)cur + sizeof(gdi_blk) + cur->size;
            gdi_blk *nb;
            if (nx + sizeof(gdi_blk) > end)
                break;
            nb = (gdi_blk *)nx;
            if (nb->used)
                break;
            cur->size += (uint32_t)(sizeof(gdi_blk) + nb->size);
        }
        if (prev && !prev->used) {
            prev->size += (uint32_t)(sizeof(gdi_blk) + cur->size);
        }
    }
}

/* ---- Helpers ---------------------------------------------------------------- */

static w32k_bitmap *bmp_of(w32k_handle h)
{
    return (w32k_bitmap *)w32k_pool_get(&pool_bmp, h);
}

static w32k_dc *dc_of(w32k_handle h)
{
    return (w32k_dc *)w32k_pool_get(&pool_dc, h);
}

static w32k_brush *brush_of(w32k_handle h)
{
    return (w32k_brush *)w32k_pool_get(&pool_brush, h);
}

static w32k_pen *pen_of(w32k_handle h)
{
    return (w32k_pen *)w32k_pool_get(&pool_pen, h);
}

static w32k_font *font_of(w32k_handle h)
{
    return (w32k_font *)w32k_pool_get(&pool_font, h);
}

static uint32_t colorref_to_pixel(uint32_t c)
{
    return ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
}

static uint32_t pixel_to_colorref(uint32_t p)
{
    return ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
}

static w32k_handle new_bitmap(int32_t w, int32_t h, int owned)
{
    w32k_handle hh;
    w32k_bitmap *b;
    size_t bytes;
    if (w <= 0 || h <= 0)
        return 0;
    bytes = (size_t)w * (size_t)h * 4u;
    b = (w32k_bitmap *)w32k_pool_alloc(&pool_bmp, &hh);
    if (!b)
        return 0;
    b->bits = (uint32_t *)arena_alloc(bytes);
    if (!b->bits) {
        w32k_pool_free(&pool_bmp, b);
        return 0;
    }
    w32k_zero(b->bits, bytes);
    b->w = w;
    b->h = h;
    b->owned_by_dc = owned;
    return hh;
}

/* Surface, origin and clip used for drawing with a DC. */
static w32k_bitmap *dc_surface(w32k_dc *dc)
{
    return bmp_of(dc->surf);
}

static int in_clip(const w32k_dc *dc, int32_t x, int32_t y)
{
    return x >= dc->clip.left && x < dc->clip.right && y >= dc->clip.top && y < dc->clip.bottom;
}

static void put_pixel(w32k_bitmap *s, int32_t x, int32_t y, uint32_t pix)
{
    s->bits[(size_t)y * (size_t)s->w + (size_t)x] = pix;
}

static uint32_t get_pixel_raw(const w32k_bitmap *s, int32_t x, int32_t y)
{
    return s->bits[(size_t)y * (size_t)s->w + (size_t)x];
}

static uint32_t rop_apply(uint32_t rop, uint32_t src, uint32_t dst, uint32_t brush)
{
    uint32_t r;
    switch (rop) {
    case W32K_SRCCOPY: r = src; break;
    case W32K_SRCPAINT: r = src | dst; break;
    case W32K_SRCAND: r = src & dst; break;
    case W32K_SRCINVERT: r = src ^ dst; break;
    case W32K_NOTSRCCOPY: r = ~src; break;
    case W32K_PATCOPY: r = brush; break;
    case W32K_BLACKNESS: r = 0; break;
    case W32K_WHITENESS: r = 0x00FFFFFFu; break;
    case W32K_DSTINVERT: r = ~dst; break;
    default: return 0xFFFFFFFFu;
    }
    return r & 0x00FFFFFFu;
}

static int rop_needs_source(uint32_t rop)
{
    switch (rop) {
    case W32K_SRCCOPY:
    case W32K_SRCPAINT:
    case W32K_SRCAND:
    case W32K_SRCINVERT:
    case W32K_NOTSRCCOPY:
        return 1;
    default:
        return 0;
    }
}

static int rop_known(uint32_t rop)
{
    return rop_apply(rop, 0, 0, 0) != 0xFFFFFFFFu;
}

/* ---- Init and stock objects ------------------------------------------------- */

void w32k_gdi_init(void)
{
    w32k_zero(bmp_arr, sizeof(bmp_arr));
    w32k_zero(dc_arr, sizeof(dc_arr));
    w32k_zero(brush_arr, sizeof(brush_arr));
    w32k_zero(pen_arr, sizeof(pen_arr));
    w32k_zero(font_arr, sizeof(font_arr));

    pool_bmp.base = (uint8_t *)bmp_arr;
    pool_bmp.elem_size = sizeof(bmp_arr[0]);
    pool_bmp.count = GDI_MAX_BMP;
    pool_bmp.type = W32K_T_BITMAP;

    pool_dc.base = (uint8_t *)dc_arr;
    pool_dc.elem_size = sizeof(dc_arr[0]);
    pool_dc.count = GDI_MAX_DC;
    pool_dc.type = W32K_T_DC;

    pool_brush.base = (uint8_t *)brush_arr;
    pool_brush.elem_size = sizeof(brush_arr[0]);
    pool_brush.count = GDI_MAX_BRUSH;
    pool_brush.type = W32K_T_BRUSH;

    pool_pen.base = (uint8_t *)pen_arr;
    pool_pen.elem_size = sizeof(pen_arr[0]);
    pool_pen.count = GDI_MAX_PEN;
    pool_pen.type = W32K_T_PEN;

    pool_font.base = (uint8_t *)font_arr;
    pool_font.elem_size = sizeof(font_arr[0]);
    pool_font.count = GDI_MAX_FONT;
    pool_font.type = W32K_T_FONT;

    arena_reset();
    framebuffer_h = new_bitmap(GDI_FB_W, GDI_FB_H, 0);

    {
        w32k_handle h;
        w32k_brush *b;
        int i;
        /* Brush stock objects: white, light gray, gray, dark gray, black, null. */
        static const uint32_t brush_colors[6] = {0xFFFFFFu, 0xC0C0C0u, 0x808080u, 0x404040u, 0x000000u, 0};
        for (i = 0; i < 6; ++i) {
            b = (w32k_brush *)w32k_pool_alloc(&pool_brush, &h);
            b->color = brush_colors[i];
            b->null_brush = (i == 5);
            w32k_g.stock_objects[i] = h;
        }
        /* Pens: white, black, null (stock indices 6, 7, 8). */
        {
            w32k_pen *p;
            p = (w32k_pen *)w32k_pool_alloc(&pool_pen, &h);
            p->color = 0xFFFFFFu;
            p->width = 1;
            w32k_g.stock_objects[6] = h;
            p = (w32k_pen *)w32k_pool_alloc(&pool_pen, &h);
            p->color = 0;
            p->width = 1;
            w32k_g.stock_objects[7] = h;
            p = (w32k_pen *)w32k_pool_alloc(&pool_pen, &h);
            p->style = 5; /* PS_NULL */
            p->width = 1;
            w32k_g.stock_objects[8] = h;
        }
        {
            w32k_font *f = (w32k_font *)w32k_pool_alloc(&pool_font, &h);
            f->height = GDI_CELL_H;
            w32k_g.stock_objects[9] = h; /* SYSTEM_FONT */
        }
    }
}

void w32k_gdi_reset(void)
{
    w32k_gdi_init();
}

w32k_handle w32k_framebuffer(void)
{
    return framebuffer_h;
}

/* stock_objects slots: 0..5 brushes (white, ltgray, gray, dkgray, black, null),
 * 6..8 pens (white, black, null), 9 system font. GetStockObject maps the WH
 * indices onto these slots. */
w32k_handle w32k_get_stock_object(int index)
{
    switch (index) {
    case W32K_WHITE_BRUSH: return w32k_g.stock_objects[0];
    case 1: return w32k_g.stock_objects[1];
    case 2: return w32k_g.stock_objects[2];
    case 3: return w32k_g.stock_objects[3];
    case W32K_BLACK_BRUSH: return w32k_g.stock_objects[4];
    case W32K_NULL_BRUSH: return w32k_g.stock_objects[5];
    case W32K_WHITE_PEN: return w32k_g.stock_objects[6];
    case W32K_BLACK_PEN: return w32k_g.stock_objects[7];
    case W32K_NULL_PEN: return w32k_g.stock_objects[8];
    case W32K_SYSTEM_FONT: return w32k_g.stock_objects[9];
    default:
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
}

/* ---- DCs -------------------------------------------------------------------- */

w32k_handle w32k_get_dc(w32k_handle hwnd)
{
    w32k_handle h;
    w32k_dc *dc;
    w32k_bitmap *fb = bmp_of(framebuffer_h);
    w32k_rect fbr;
    if (!fb)
        return 0;
    fbr.left = 0;
    fbr.top = 0;
    fbr.right = fb->w;
    fbr.bottom = fb->h;
    dc = (w32k_dc *)w32k_pool_alloc(&pool_dc, &h);
    if (!dc) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    dc->surf = framebuffer_h;
    dc->text_color = 0;
    dc->bk_color = 0xFFFFFFu;
    dc->bk_mode = W32K_OPAQUE;
    dc->brush = w32k_g.stock_objects[0];
    dc->pen = w32k_g.stock_objects[7];
    dc->font = w32k_g.stock_objects[9];
    if (hwnd) {
        w32k_rect cr;
        w32k_window *w = w32k_window_checked(hwnd);
        if (!w) {
            w32k_pool_free(&pool_dc, dc);
            w32k_set_error(W32K_ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        dc->hwnd = hwnd;
        dc->org_x = w->rect.left;
        dc->org_y = w->rect.top;
        cr = w->rect;
        /* An off-screen window gets an empty clip (intersect zeroes it on failure). */
        w32k_rect_intersect(&dc->clip, &cr, &fbr);
    } else {
        dc->clip = fbr;
    }
    return h;
}

int w32k_release_dc(w32k_handle hwnd, w32k_handle dch)
{
    w32k_dc *dc = dc_of(dch);
    (void)hwnd;
    if (!dc || dc->memory)
        return 0;
    w32k_pool_free(&pool_dc, dc);
    return 1;
}

w32k_handle w32k_create_compatible_dc(w32k_handle dch)
{
    w32k_handle h, bmp;
    w32k_dc *dc, *src = dc_of(dch);
    (void)src;
    bmp = new_bitmap(1, 1, 1);
    if (!bmp) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    dc = (w32k_dc *)w32k_pool_alloc(&pool_dc, &h);
    if (!dc) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    dc->memory = 1;
    dc->surf = bmp;
    dc->default_bmp = bmp;
    dc->brush = w32k_g.stock_objects[0];
    dc->pen = w32k_g.stock_objects[6];
    dc->font = w32k_g.stock_objects[9];
    dc->clip.left = 0;
    dc->clip.top = 0;
    dc->clip.right = 1;
    dc->clip.bottom = 1;
    dc->text_color = 0;
    dc->bk_color = 0xFFFFFFu;
    dc->bk_mode = W32K_OPAQUE;
    return h;
}

int w32k_delete_dc(w32k_handle dch)
{
    w32k_dc *dc = dc_of(dch);
    w32k_bitmap *def;
    if (!dc || !dc->memory) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    def = bmp_of(dc->default_bmp);
    if (def) {
        arena_free(def->bits);
        w32k_pool_free(&pool_bmp, def);
    }
    w32k_pool_free(&pool_dc, dc);
    return 1;
}

/* ---- Object creation -------------------------------------------------------- */

w32k_handle w32k_create_dib_section(int32_t w, int32_t h, uint32_t bpp, void **bits)
{
    w32k_handle hh;
    w32k_bitmap *b;
    if (bpp != 32 || w <= 0 || h == 0) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (h < 0)
        h = -h;
    hh = new_bitmap(w, h, 0);
    if (!hh) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    b = bmp_of(hh);
    if (bits)
        *bits = b->bits;
    return hh;
}

w32k_handle w32k_create_solid_brush(uint32_t color)
{
    w32k_handle h;
    w32k_brush *b = (w32k_brush *)w32k_pool_alloc(&pool_brush, &h);
    if (!b) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    b->color = color & 0x00FFFFFFu;
    return h;
}

w32k_handle w32k_create_pen(uint32_t style, int32_t width, uint32_t color)
{
    w32k_handle h;
    w32k_pen *p;
    if (width < 0) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    p = (w32k_pen *)w32k_pool_alloc(&pool_pen, &h);
    if (!p) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    p->style = style;
    p->width = width;
    p->color = color & 0x00FFFFFFu;
    return h;
}

w32k_handle w32k_create_font(int32_t height)
{
    w32k_handle h;
    w32k_font *f = (w32k_font *)w32k_pool_alloc(&pool_font, &h);
    if (!f) {
        w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    f->height = height < 0 ? -height : height;
    return h;
}

static w32k_handle dc_handle_at(uint32_t i)
{
    w32k_dc *d = (w32k_dc *)w32k_pool_index(&pool_dc, i);
    return ((uint32_t)W32K_T_DC << 24) | (((uint32_t)d->hdr.gen & 0xFFu) << 16) | (i + 1u);
}

/* Returns 1 if the bitmap is selected into some memory DC other than except_dc. */
static int bitmap_selected_elsewhere(w32k_handle bmp, w32k_handle except_dc)
{
    for (uint32_t i = 0; i < GDI_MAX_DC; ++i) {
        w32k_dc *d = (w32k_dc *)w32k_pool_index(&pool_dc, i);
        if (d->hdr.used && d->memory && d->surf == bmp && dc_handle_at(i) != except_dc)
            return 1;
    }
    return 0;
}

static int object_selected(w32k_handle obj)
{
    for (uint32_t i = 0; i < GDI_MAX_DC; ++i) {
        w32k_dc *d = (w32k_dc *)w32k_pool_index(&pool_dc, i);
        if (!d->hdr.used)
            continue;
        if (d->brush == obj || d->pen == obj || d->font == obj || d->surf == obj)
            return 1;
    }
    return 0;
}

w32k_handle w32k_select_object(w32k_handle dch, w32k_handle obj)
{
    w32k_dc *dc = dc_of(dch);
    if (!dc || !obj) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (brush_of(obj)) {
        w32k_handle prev = dc->brush;
        dc->brush = obj;
        return prev;
    }
    if (pen_of(obj)) {
        w32k_handle prev = dc->pen;
        dc->pen = obj;
        return prev;
    }
    if (font_of(obj)) {
        w32k_handle prev = dc->font;
        dc->font = obj;
        return prev;
    }
    if (bmp_of(obj)) {
        w32k_bitmap *b = bmp_of(obj);
        w32k_handle prev;
        if (!dc->memory) {
            /* Only memory DCs take a bitmap; the screen surface is fixed. */
            w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (bitmap_selected_elsewhere(obj, dch) || obj == framebuffer_h) {
            w32k_set_error(W32K_ERROR_BUSY);
            return 0;
        }
        prev = dc->surf;
        (void)b;
        dc->surf = obj;
        dc->clip.left = 0;
        dc->clip.top = 0;
        dc->clip.right = b->w;
        dc->clip.bottom = b->h;
        return prev;
    }
    w32k_set_error(W32K_ERROR_INVALID_HANDLE);
    return 0;
}

int w32k_delete_object(w32k_handle obj)
{
    w32k_bitmap *b;
    for (int i = 0; i < 6; ++i) {
        if (w32k_g.stock_objects[i] == obj)
            return 1; /* stock objects are never deleted */
    }
    for (int i = 6; i < 10; ++i) {
        if (w32k_g.stock_objects[i] == obj)
            return 1;
    }
    if (object_selected(obj)) {
        w32k_set_error(W32K_ERROR_BUSY);
        return 0;
    }
    b = bmp_of(obj);
    if (b) {
        if (b->owned_by_dc || obj == framebuffer_h) {
            w32k_set_error(W32K_ERROR_ACCESS_DENIED);
            return 0;
        }
        arena_free(b->bits);
        w32k_pool_free(&pool_bmp, b);
        return 1;
    }
    if (brush_of(obj)) {
        w32k_pool_free(&pool_brush, brush_of(obj));
        return 1;
    }
    if (pen_of(obj)) {
        w32k_pool_free(&pool_pen, pen_of(obj));
        return 1;
    }
    if (font_of(obj)) {
        w32k_pool_free(&pool_font, font_of(obj));
        return 1;
    }
    w32k_set_error(W32K_ERROR_INVALID_HANDLE);
    return 0;
}

uint32_t w32k_get_object_type(w32k_handle obj)
{
    if (brush_of(obj))
        return W32K_OBJ_BRUSH;
    if (pen_of(obj))
        return W32K_OBJ_PEN;
    if (font_of(obj))
        return W32K_OBJ_FONT;
    if (bmp_of(obj))
        return W32K_OBJ_BITMAP;
    if (dc_of(obj))
        return W32K_OBJ_DC;
    return 0;
}

uint32_t *w32k_bitmap_bits(w32k_handle bitmap, int32_t *w, int32_t *h)
{
    w32k_bitmap *b = bmp_of(bitmap);
    if (!b)
        return NULL;
    if (w)
        *w = b->w;
    if (h)
        *h = b->h;
    return b->bits;
}

/* ---- Drawing ---------------------------------------------------------------- */

static uint32_t dc_brush_pixel(const w32k_dc *dc, w32k_handle brush)
{
    w32k_brush *b = brush_of(brush ? brush : dc->brush);
    if (!b || b->null_brush)
        return 0;
    return colorref_to_pixel(b->color);
}

int w32k_fill_rect(w32k_handle dch, const w32k_rect *rc, w32k_handle brush)
{
    w32k_dc *dc = dc_of(dch);
    w32k_bitmap *s;
    w32k_rect dr, cl;
    w32k_brush *b;
    uint32_t pix;
    int32_t y, x;
    if (!dc || !rc) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    s = dc_surface(dc);
    if (!s)
        return 0;
    b = brush_of(brush ? brush : dc->brush);
    if (!b)
        return 0;
    if (b->null_brush)
        return 1;
    pix = colorref_to_pixel(b->color);
    dr.left = rc->left + dc->org_x;
    dr.top = rc->top + dc->org_y;
    dr.right = rc->right + dc->org_x;
    dr.bottom = rc->bottom + dc->org_y;
    if (!w32k_rect_intersect(&cl, &dr, &dc->clip))
        return 1;
    for (y = cl.top; y < cl.bottom; ++y)
        for (x = cl.left; x < cl.right; ++x)
            put_pixel(s, x, y, pix);
    return 1;
}

uint32_t w32k_set_pixel(w32k_handle dch, int32_t x, int32_t y, uint32_t color)
{
    w32k_dc *dc = dc_of(dch);
    w32k_bitmap *s;
    int32_t dx, dy;
    if (!dc)
        return W32K_CLR_INVALID;
    s = dc_surface(dc);
    if (!s)
        return W32K_CLR_INVALID;
    dx = x + dc->org_x;
    dy = y + dc->org_y;
    if (!in_clip(dc, dx, dy))
        return W32K_CLR_INVALID;
    put_pixel(s, dx, dy, colorref_to_pixel(color));
    return color & 0x00FFFFFFu;
}

uint32_t w32k_get_pixel(w32k_handle dch, int32_t x, int32_t y)
{
    w32k_dc *dc = dc_of(dch);
    w32k_bitmap *s;
    int32_t dx, dy;
    if (!dc)
        return W32K_CLR_INVALID;
    s = dc_surface(dc);
    if (!s)
        return W32K_CLR_INVALID;
    dx = x + dc->org_x;
    dy = y + dc->org_y;
    if (!in_clip(dc, dx, dy))
        return W32K_CLR_INVALID;
    return pixel_to_colorref(get_pixel_raw(s, dx, dy));
}

int w32k_bitblt(w32k_handle dsth, int32_t x, int32_t y, int32_t w, int32_t h, w32k_handle srch,
                int32_t sx, int32_t sy, uint32_t rop)
{
    w32k_dc *dst = dc_of(dsth);
    w32k_dc *src = srch ? dc_of(srch) : NULL;
    w32k_bitmap *ds, *ss = NULL;
    uint32_t *tmp = NULL;
    uint32_t brush_pix;
    int32_t i, j;
    w32k_rect dr, cl;
    if (!dst || w <= 0 || h <= 0) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!rop_known(rop)) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (rop_needs_source(rop) && !src) {
        w32k_set_error(W32K_ERROR_INVALID_HANDLE);
        return 0;
    }
    ds = dc_surface(dst);
    if (src) {
        ss = dc_surface(src);
        if (!ss)
            return 0;
    }
    brush_pix = dc_brush_pixel(dst, 0);
    dr.left = x + dst->org_x;
    dr.top = y + dst->org_y;
    dr.right = dr.left + w;
    dr.bottom = dr.top + h;
    if (!w32k_rect_intersect(&cl, &dr, &dst->clip))
        return 1;
    /* Copy the source region first so overlapping blits on one surface are safe. */
    if (src && rop_needs_source(rop)) {
        tmp = (uint32_t *)arena_alloc((size_t)w * (size_t)h * 4u);
        if (!tmp) {
            w32k_set_error(W32K_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        for (j = 0; j < h; ++j) {
            for (i = 0; i < w; ++i) {
                int32_t spx = sx + src->org_x + i;
                int32_t spy = sy + src->org_y + j;
                uint32_t v = 0;
                if (spx >= 0 && spy >= 0 && spx < ss->w && spy < ss->h)
                    v = get_pixel_raw(ss, spx, spy);
                tmp[(size_t)j * (size_t)w + (size_t)i] = v;
            }
        }
    }
    for (int32_t py = cl.top; py < cl.bottom; ++py) {
        for (int32_t px = cl.left; px < cl.right; ++px) {
            int32_t i2 = px - dr.left;
            int32_t j2 = py - dr.top;
            uint32_t s = tmp ? tmp[(size_t)j2 * (size_t)w + (size_t)i2] : 0;
            uint32_t d = get_pixel_raw(ds, px, py);
            put_pixel(ds, px, py, rop_apply(rop, s, d, brush_pix));
        }
    }
    if (tmp)
        arena_free(tmp);
    return 1;
}

int w32k_stretchblt(w32k_handle dsth, int32_t x, int32_t y, int32_t w, int32_t h, w32k_handle srch,
                    int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t rop)
{
    w32k_dc *dst = dc_of(dsth);
    w32k_dc *src = srch ? dc_of(srch) : NULL;
    w32k_bitmap *ds, *ss;
    uint32_t brush_pix;
    w32k_rect dr, cl;
    if (!dst || !src || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!rop_known(rop) || !rop_needs_source(rop)) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    ds = dc_surface(dst);
    ss = dc_surface(src);
    if (!ds || !ss)
        return 0;
    brush_pix = dc_brush_pixel(dst, 0);
    dr.left = x + dst->org_x;
    dr.top = y + dst->org_y;
    dr.right = dr.left + w;
    dr.bottom = dr.top + h;
    if (!w32k_rect_intersect(&cl, &dr, &dst->clip))
        return 1;
    /* Nearest-neighbour mapping. Read the source through a temporary copy. */
    for (int32_t py = cl.top; py < cl.bottom; ++py) {
        for (int32_t px = cl.left; px < cl.right; ++px) {
            int32_t ox = px - dr.left;
            int32_t oy = py - dr.top;
            int32_t spx = sx + src->org_x + (int32_t)(((int64_t)ox * sw) / w);
            int32_t spy = sy + src->org_y + (int32_t)(((int64_t)oy * sh) / h);
            uint32_t s = 0, d;
            if (spx >= 0 && spy >= 0 && spx < ss->w && spy < ss->h)
                s = get_pixel_raw(ss, spx, spy);
            d = get_pixel_raw(ds, px, py);
            put_pixel(ds, px, py, rop_apply(rop, s, d, brush_pix));
        }
    }
    return 1;
}

int w32k_text_out(w32k_handle dch, int32_t x, int32_t y, const uint16_t *s, int len)
{
    w32k_dc *dc = dc_of(dch);
    w32k_bitmap *surf;
    uint32_t fg, bg;
    if (!dc || (!s && len > 0) || len < 0) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    surf = dc_surface(dc);
    if (!surf)
        return 0;
    fg = colorref_to_pixel(dc->text_color);
    bg = colorref_to_pixel(dc->bk_color);
    for (int i = 0; i < len; ++i) {
        w32k_rect cell;
        int32_t cx = x + dc->org_x + i * GDI_CELL_W;
        int32_t cy = y + dc->org_y;
        cell.left = cx;
        cell.top = cy;
        cell.right = cx + GDI_CELL_W;
        cell.bottom = cy + GDI_CELL_H;
        if (dc->bk_mode == W32K_OPAQUE) {
            w32k_rect cl;
            if (w32k_rect_intersect(&cl, &cell, &dc->clip)) {
                for (int32_t py = cl.top; py < cl.bottom; ++py)
                    for (int32_t px = cl.left; px < cl.right; ++px)
                        put_pixel(surf, px, py, bg);
            }
        }
        if (s[i] != ' ') {
            w32k_rect cl;
            if (w32k_rect_intersect(&cl, &cell, &dc->clip)) {
                for (int32_t py = cl.top; py < cl.bottom; ++py)
                    for (int32_t px = cl.left; px < cl.right; ++px)
                        put_pixel(surf, px, py, fg);
            }
        }
    }
    return 1;
}

int w32k_get_text_extent(w32k_handle dch, const uint16_t *s, int len, w32k_size *sz)
{
    (void)dch;
    (void)s;
    if (!sz || len < 0) {
        w32k_set_error(W32K_ERROR_INVALID_PARAMETER);
        return 0;
    }
    sz->cx = len * GDI_CELL_W;
    sz->cy = GDI_CELL_H;
    return 1;
}

uint32_t w32k_set_text_color(w32k_handle dch, uint32_t color)
{
    w32k_dc *dc = dc_of(dch);
    uint32_t prev;
    if (!dc)
        return W32K_CLR_INVALID;
    prev = dc->text_color;
    dc->text_color = color & 0x00FFFFFFu;
    return prev;
}

uint32_t w32k_get_text_color(w32k_handle dch)
{
    w32k_dc *dc = dc_of(dch);
    return dc ? dc->text_color : W32K_CLR_INVALID;
}

uint32_t w32k_set_bk_color(w32k_handle dch, uint32_t color)
{
    w32k_dc *dc = dc_of(dch);
    uint32_t prev;
    if (!dc)
        return W32K_CLR_INVALID;
    prev = dc->bk_color;
    dc->bk_color = color & 0x00FFFFFFu;
    return prev;
}

uint32_t w32k_get_bk_color(w32k_handle dch)
{
    w32k_dc *dc = dc_of(dch);
    return dc ? dc->bk_color : W32K_CLR_INVALID;
}

int w32k_set_bk_mode(w32k_handle dch, int mode)
{
    w32k_dc *dc = dc_of(dch);
    int prev;
    if (!dc || (mode != W32K_TRANSPARENT && mode != W32K_OPAQUE))
        return 0;
    prev = dc->bk_mode;
    dc->bk_mode = mode;
    return prev;
}

int w32k_get_device_caps(w32k_handle dch, int index)
{
    w32k_dc *dc = dc_of(dch);
    w32k_bitmap *s;
    if (!dc)
        return 0;
    s = dc_surface(dc);
    switch (index) {
    case W32K_HORZRES: return s ? s->w : 0;
    case W32K_VERTRES: return s ? s->h : 0;
    case W32K_BITSPIXEL: return 32;
    case W32K_PLANES: return 1;
    case W32K_LOGPIXELSX: return 96;
    case W32K_LOGPIXELSY: return 96;
    default: return 0;
    }
}
