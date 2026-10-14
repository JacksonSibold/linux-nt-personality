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
    (void)reason;
    (void)reserved;
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

#endif /* __MINGW32__ */
