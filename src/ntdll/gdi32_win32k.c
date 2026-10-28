/* gdi32_win32k.c - gdi32.dll exports backed by the win32k model (src/ntdll/win32k).
 *
 * Same conventions as user32_win32k.c. CreateDIBSection reads the bitmap size and
 * depth from a BITMAPINFOHEADER; only 32 bpp is accepted, as in the model.
 * Fonts are not exported: the model has font handles, but without the LOGFONT
 * path there is no way to create one from a Win32 caller yet.
 */
#ifdef __MINGW32__
#include "win32k/w32k_abi.h"
#include <stdint.h>

typedef int32_t BOOL_T;

static int32_t bmih_i32(const void *bmi, size_t off)
{
    const uint8_t *b = (const uint8_t *)bmi + off;
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

static uint32_t bmih_u16(const void *bmi, size_t off)
{
    const uint8_t *b = (const uint8_t *)bmi + off;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8);
}

/* DLL entry points, as in user32.c (the link uses DllMainCRTStartup). */
BOOL_T DllMain(void *h, uint32_t reason, void *reserved)
{
    (void)h;
    (void)reserved;
    /* DLL_PROCESS_ATTACH: the model lives in win32u.dll; init is idempotent. */
    if (reason == 1)
        w32k_init_once();
    return 1;
}

BOOL_T __stdcall _DllMainCRTStartup(void *h, uint32_t reason, void *reserved)
{
    return DllMain(h, reason, reserved);
}

BOOL_T __stdcall DllMainCRTStartup(void *h, uint32_t reason, void *reserved)
{
    return DllMain(h, reason, reserved);
}

uintptr_t CreateCompatibleDC(uintptr_t dc)
{
    return w32k_create_compatible_dc(dc);
}

BOOL_T DeleteDC(uintptr_t dc)
{
    return w32k_delete_dc(dc) != 0;
}

uintptr_t CreateDIBSection(uintptr_t dc, const void *bmi, uint32_t usage, void **bits, void *section,
                           uint32_t offset)
{
    (void)dc;
    (void)usage;
    (void)section;
    (void)offset;
    if (!bmi)
        return 0;
    return w32k_create_dib_section(bmih_i32(bmi, W32K_BMIH_WIDTH), bmih_i32(bmi, W32K_BMIH_HEIGHT),
                                   bmih_u16(bmi, W32K_BMIH_BITCOUNT), bits);
}

uintptr_t CreateSolidBrush(uint32_t color)
{
    return w32k_create_solid_brush(color);
}

uintptr_t CreatePen(int32_t style, int32_t width, uint32_t color)
{
    return w32k_create_pen((uint32_t)style, width, color);
}

uintptr_t SelectObject(uintptr_t dc, uintptr_t obj)
{
    return w32k_select_object(dc, obj);
}

BOOL_T DeleteObject(uintptr_t obj)
{
    return w32k_delete_object(obj) != 0;
}

uint32_t GetObjectType(uintptr_t obj)
{
    return w32k_get_object_type(obj);
}

uintptr_t GetStockObject(int32_t index)
{
    return w32k_get_stock_object(index);
}

BOOL_T BitBlt(uintptr_t dst, int32_t x, int32_t y, int32_t w, int32_t h, uintptr_t src, int32_t sx,
              int32_t sy, uint32_t rop)
{
    return w32k_bitblt(dst, x, y, w, h, src, sx, sy, rop) != 0;
}

BOOL_T StretchBlt(uintptr_t dst, int32_t x, int32_t y, int32_t w, int32_t h, uintptr_t src,
                  int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t rop)
{
    return w32k_stretchblt(dst, x, y, w, h, src, sx, sy, sw, sh, rop) != 0;
}

uint32_t SetPixel(uintptr_t dc, int32_t x, int32_t y, uint32_t color)
{
    return w32k_set_pixel(dc, x, y, color);
}

uint32_t GetPixel(uintptr_t dc, int32_t x, int32_t y)
{
    return w32k_get_pixel(dc, x, y);
}

BOOL_T TextOutW(uintptr_t dc, int32_t x, int32_t y, const uint16_t *s, int32_t len)
{
    return w32k_text_out(dc, x, y, s, len) != 0;
}

BOOL_T GetTextExtentPoint32W(uintptr_t dc, const uint16_t *s, int32_t len, w32k_size *sz)
{
    return w32k_get_text_extent(dc, s, len, sz) != 0;
}

uint32_t SetTextColor(uintptr_t dc, uint32_t color)
{
    return w32k_set_text_color(dc, color);
}

uint32_t GetTextColor(uintptr_t dc)
{
    return w32k_get_text_color(dc);
}

uint32_t SetBkColor(uintptr_t dc, uint32_t color)
{
    return w32k_set_bk_color(dc, color);
}

uint32_t GetBkColor(uintptr_t dc)
{
    return w32k_get_bk_color(dc);
}

int32_t SetBkMode(uintptr_t dc, int32_t mode)
{
    return w32k_set_bk_mode(dc, mode);
}

int32_t GetDeviceCaps(uintptr_t dc, int32_t index)
{
    return w32k_get_device_caps(dc, index);
}

/* ---- Fonts ----------------------------------------------------------------------- */

/* LOGFONTW has the same x64 layout as w32k_logfont (wingdi.h field order), so it is passed
 * through. */
uintptr_t CreateFontIndirectW(const w32k_logfont *lf)
{
    return w32k_create_font_indirect(lf);
}

uintptr_t CreateFontW(int32_t height, int32_t width, int32_t escapement, int32_t orientation,
                      int32_t weight, uint32_t italic, uint32_t underline, uint32_t strikeout,
                      uint32_t charset, uint32_t out_prec, uint32_t clip_prec, uint32_t quality,
                      uint32_t pitch_family, const uint16_t *face)
{
    w32k_logfont lf;
    uint8_t *raw = (uint8_t *)&lf;
    size_t i;
    for (i = 0; i < sizeof(lf); ++i)
        raw[i] = 0;
    lf.height = height;
    lf.width = width;
    lf.escapement = escapement;
    lf.orientation = orientation;
    lf.weight = weight;
    lf.italic = (uint8_t)italic;
    lf.underline = (uint8_t)underline;
    lf.strikeout = (uint8_t)strikeout;
    lf.charset = (uint8_t)charset;
    lf.out_precision = (uint8_t)out_prec;
    lf.clip_precision = (uint8_t)clip_prec;
    lf.quality = (uint8_t)quality;
    lf.pitch_family = (uint8_t)pitch_family;
    if (face) {
        for (i = 0; i < 31 && face[i]; ++i)
            lf.face[i] = face[i];
    }
    return w32k_create_font_indirect(&lf);
}

static void tm_put(uint8_t *p, size_t off, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        p[off + (size_t)i] = (uint8_t)(v >> (8 * i));
}

/* TEXTMETRICW (x64, 60 bytes). First, last, default and break characters are UTF-16 units.
 * tmPitchAndFamily: TMPF_FIXED_PITCH (1) means variable pitch, as in Win32. */
BOOL_T GetTextMetricsW(uintptr_t dc, void *out)
{
    w32k_tm t;
    uint8_t *p = (uint8_t *)out;
    size_t i;
    if (!p || !w32k_get_text_metrics(dc, &t))
        return 0;
    for (i = 0; i < 60; ++i)
        p[i] = 0;
    tm_put(p, 0, (uint32_t)t.height, 4);
    tm_put(p, 4, (uint32_t)t.ascent, 4);
    tm_put(p, 8, (uint32_t)t.descent, 4);
    tm_put(p, 12, (uint32_t)t.internal_leading, 4);
    tm_put(p, 16, (uint32_t)t.external_leading, 4);
    tm_put(p, 20, (uint32_t)t.ave_char_width, 4);
    tm_put(p, 24, (uint32_t)t.max_char_width, 4);
    tm_put(p, 28, (uint32_t)t.weight, 4);
    tm_put(p, 36, 96, 4);
    tm_put(p, 40, 96, 4);
    tm_put(p, 44, t.first_char, 2);
    tm_put(p, 46, t.last_char, 2);
    tm_put(p, 48, t.default_char, 2);
    tm_put(p, 50, t.break_char, 2);
    p[52] = t.italic;
    p[53] = t.underlined;
    p[54] = t.struckout;
    p[55] = t.pitch_fixed ? 0 : 1;
    p[56] = 0; /* ANSI_CHARSET */
    return 1;
}

/* ---- Regions ----------------------------------------------------------------------- */

uintptr_t CreateRectRgn(int32_t l, int32_t t, int32_t r, int32_t b)
{
    return w32k_create_rect_rgn(l, t, r, b);
}

uintptr_t CreateRectRgnIndirect(const w32k_rect *rc)
{
    return w32k_create_rect_rgn_indirect(rc);
}

uintptr_t CreateEllipticRgn(int32_t l, int32_t t, int32_t r, int32_t b)
{
    return w32k_create_elliptic_rgn(l, t, r, b);
}

uintptr_t CreateRoundRectRgn(int32_t l, int32_t t, int32_t r, int32_t b, int32_t ew, int32_t eh)
{
    return w32k_create_round_rect_rgn(l, t, r, b, ew, eh);
}

/* Fill modes: ALTERNATE (1) is even-odd, WINDING (2) is the nonzero rule. */
uintptr_t CreatePolygonRgn(const w32k_point *pts, int32_t n, int32_t fill_mode)
{
    return w32k_create_polygon_rgn(pts, n, fill_mode == 2);
}

BOOL_T SetRectRgn(uintptr_t rgn, int32_t l, int32_t t, int32_t r, int32_t b)
{
    return w32k_set_rect_rgn(rgn, l, t, r, b) != 0;
}

int32_t CombineRgn(uintptr_t dst, uintptr_t a, uintptr_t b, int32_t mode)
{
    return w32k_combine_rgn(dst, a, b, mode);
}

int32_t GetRgnBox(uintptr_t rgn, w32k_rect *out)
{
    return w32k_get_rgn_box(rgn, out);
}

BOOL_T PtInRegion(uintptr_t rgn, int32_t x, int32_t y)
{
    return w32k_pt_in_region(rgn, x, y) != 0;
}

BOOL_T RectInRegion(uintptr_t rgn, const w32k_rect *rc)
{
    return w32k_rect_in_region(rgn, rc) != 0;
}

BOOL_T EqualRgn(uintptr_t a, uintptr_t b)
{
    return w32k_equal_rgn(a, b) != 0;
}

int32_t OffsetRgn(uintptr_t rgn, int32_t dx, int32_t dy)
{
    return w32k_offset_rgn(rgn, dx, dy);
}

int32_t SelectClipRgn(uintptr_t dc, uintptr_t rgn)
{
    return w32k_select_clip_rgn(dc, rgn);
}

int32_t GetClipRgn(uintptr_t dc, uintptr_t rgn)
{
    return w32k_get_clip_rgn(dc, rgn);
}

int32_t GetClipBox(uintptr_t dc, w32k_rect *out)
{
    return w32k_get_clip_box(dc, out);
}

#endif /* __MINGW32__ */
