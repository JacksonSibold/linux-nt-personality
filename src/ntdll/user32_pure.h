/*
 * user32_pure.h - pure logic behind user32 exports that need no model state:
 * RECT arithmetic, AdjustWindowRectEx, the default system colours, the case
 * mapping of CharUpperW and CharLowerW, and the US keyboard layout tables
 * (MapVirtualKey, VkKeyScan, ToUnicode, GetKeyNameText).
 *
 * The shims (user32_win32k.c) call these; tests/ntdll-host/test_user32_pure.c
 * runs them on the host. Values are the model's configured defaults for a
 * 96 dpi Windows 10 desktop and the US layout. They are not sampled from a
 * Windows system.
 */
#ifndef NTDLL_USER32_PURE_H
#define NTDLL_USER32_PURE_H

#include <stddef.h>
#include <stdint.h>

#include "win32k/w32k.h"

/* ---- RECT arithmetic (w32k_rect has the layout of RECT) ----------------------- */

static inline int u32p_is_rect_empty(const w32k_rect *r)
{
    return r->right <= r->left || r->bottom <= r->top;
}

static inline int u32p_equal_rect(const w32k_rect *a, const w32k_rect *b)
{
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

static inline int u32p_pt_in_rect(const w32k_rect *r, int32_t x, int32_t y)
{
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

static inline void u32p_offset_rect(w32k_rect *r, int32_t dx, int32_t dy)
{
    r->left += dx;
    r->right += dx;
    r->top += dy;
    r->bottom += dy;
}

/* InflateRect: positive values grow every side, negative values shrink. */
static inline void u32p_inflate_rect(w32k_rect *r, int32_t dx, int32_t dy)
{
    r->left -= dx;
    r->right += dx;
    r->top -= dy;
    r->bottom += dy;
}

/* IntersectRect. Returns 1 when the result is not empty; otherwise *dst is all zero. */
static inline int u32p_intersect_rect(w32k_rect *dst, const w32k_rect *a, const w32k_rect *b)
{
    w32k_rect r;
    r.left = a->left > b->left ? a->left : b->left;
    r.top = a->top > b->top ? a->top : b->top;
    r.right = a->right < b->right ? a->right : b->right;
    r.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (u32p_is_rect_empty(a) || u32p_is_rect_empty(b) || u32p_is_rect_empty(&r)) {
        dst->left = dst->top = dst->right = dst->bottom = 0;
        return 0;
    }
    *dst = r;
    return 1;
}

/* UnionRect. An empty input is ignored. Returns 0, with *dst zero, when both are empty. */
static inline int u32p_union_rect(w32k_rect *dst, const w32k_rect *a, const w32k_rect *b)
{
    int ea = u32p_is_rect_empty(a), eb = u32p_is_rect_empty(b);
    w32k_rect r;
    if (ea && eb) {
        dst->left = dst->top = dst->right = dst->bottom = 0;
        return 0;
    }
    if (ea) {
        *dst = *b;
        return 1;
    }
    if (eb) {
        *dst = *a;
        return 1;
    }
    r.left = a->left < b->left ? a->left : b->left;
    r.top = a->top < b->top ? a->top : b->top;
    r.right = a->right > b->right ? a->right : b->right;
    r.bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    *dst = r;
    return 1;
}

/* SubtractRect: a minus b, when the result is still a rectangle. b trims a only if it
 * spans a fully on one axis; otherwise *dst is a unchanged. Returns 1 when the result
 * is not empty. */
static inline int u32p_subtract_rect(w32k_rect *dst, const w32k_rect *a, const w32k_rect *b)
{
    w32k_rect r = *a, x;
    if (u32p_is_rect_empty(a)) {
        dst->left = dst->top = dst->right = dst->bottom = 0;
        return 0;
    }
    if (u32p_intersect_rect(&x, a, b)) {
        if (x.top == a->top && x.bottom == a->bottom) {
            if (x.left == a->left && x.right == a->right) {
                r.left = r.right;
            } else if (x.left == a->left) {
                r.left = x.right;
            } else if (x.right == a->right) {
                r.right = x.left;
            }
        } else if (x.left == a->left && x.right == a->right) {
            if (x.top == a->top)
                r.top = x.bottom;
            else if (x.bottom == a->bottom)
                r.bottom = x.top;
        }
    }
    *dst = r;
    return !u32p_is_rect_empty(&r);
}

/* ---- AdjustWindowRectEx ------------------------------------------------------- */

#define U32P_WS_BORDER 0x00800000u
#define U32P_WS_DLGFRAME 0x00400000u
#define U32P_WS_CAPTION 0x00C00000u
#define U32P_WS_THICKFRAME 0x00040000u
#define U32P_WS_MINIMIZE 0x20000000u
#define U32P_WS_EX_DLGMODALFRAME 0x00000001u
#define U32P_WS_EX_TOOLWINDOW 0x00000080u
#define U32P_WS_EX_CLIENTEDGE 0x00000200u
#define U32P_WS_EX_STATICEDGE 0x00020000u

/* The system metrics AdjustWindowRectEx reads. */
typedef struct u32p_metrics {
    int32_t cxborder, cyborder;
    int32_t cxsizeframe, cysizeframe;
    int32_t cxpadded;
    int32_t cxdlgframe, cydlgframe;
    int32_t cxedge, cyedge;
    int32_t cycaption, cysmcaption;
    int32_t cymenu;
} u32p_metrics;

/* Grows *rc, a client rectangle, to the window rectangle for the style. A minimised
 * window has no frame to add. Scroll bars are not counted: AdjustWindowRect ignores
 * WS_HSCROLL and WS_VSCROLL. */
static inline void u32p_adjust_window_rect(w32k_rect *rc, uint32_t style, int has_menu, uint32_t exstyle,
                                           const u32p_metrics *m)
{
    int32_t bx = 0, by = 0;
    if (style & U32P_WS_MINIMIZE)
        return;
    if (exstyle & U32P_WS_EX_CLIENTEDGE) {
        rc->left -= m->cxedge;
        rc->right += m->cxedge;
        rc->top -= m->cyedge;
        rc->bottom += m->cyedge;
    }
    if ((style & U32P_WS_THICKFRAME) && !(exstyle & U32P_WS_EX_DLGMODALFRAME)) {
        bx = m->cxsizeframe + m->cxpadded;
        by = m->cysizeframe + m->cxpadded;
    } else if ((style & U32P_WS_DLGFRAME) || (exstyle & U32P_WS_EX_DLGMODALFRAME)) {
        bx = m->cxdlgframe;
        by = m->cydlgframe;
    } else if (style & U32P_WS_BORDER) {
        bx = m->cxborder;
        by = m->cyborder;
    } else if (exstyle & U32P_WS_EX_STATICEDGE) {
        bx = m->cxborder;
        by = m->cyborder;
    }
    rc->left -= bx;
    rc->right += bx;
    rc->top -= by;
    rc->bottom += by;
    if ((style & U32P_WS_CAPTION) == U32P_WS_CAPTION)
        rc->top -= (exstyle & U32P_WS_EX_TOOLWINDOW) ? m->cysmcaption : m->cycaption;
    if (has_menu)
        rc->top -= m->cymenu;
}

/* ---- System colours (GetSysColor) ---------------------------------------------- */

#define U32P_COLOR_COUNT 31

/* RGB values of the Windows 10 default theme for COLOR_SCROLLBAR (0) to COLOR_MENUBAR (30).
 * Index 25 is not defined by Windows; it reads as the button face. */
static const uint32_t u32p_sys_rgb[U32P_COLOR_COUNT] = {
    0xC8C8C8, 0x000000, 0x99B4D1, 0xBFCDDB, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000,
    0x000000, 0x000000, 0xB4B4B4, 0xF4F7FC, 0xABABAB, 0x0078D7, 0xFFFFFF, 0xF0F0F0,
    0xA0A0A0, 0x6D6D6D, 0x000000, 0x000000, 0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000,
    0xFFFFE1, 0xF0F0F0, 0x0066CC, 0xB9D1EA, 0xD7E4F2, 0x3399FF, 0xF0F0F0,
};

/* COLORREF (0x00BBGGRR) of a system colour, or 0 with *ok = 0 when the index is out of range. */
static inline uint32_t u32p_sys_colorref(int index, int *ok)
{
    uint32_t rgb;
    if (index < 0 || index >= U32P_COLOR_COUNT) {
        if (ok)
            *ok = 0;
        return 0;
    }
    if (ok)
        *ok = 1;
    rgb = u32p_sys_rgb[index];
    return ((rgb >> 16) & 0xFFu) | (rgb & 0xFF00u) | ((rgb & 0xFFu) << 16);
}

/* ---- Case mapping (CharUpperW, CharLowerW) ------------------------------------- */

/* Simple one-to-one case mapping for ASCII, Latin-1, Greek and Cyrillic. Other code
 * points map to themselves. */
static inline uint16_t u32p_wupper(uint16_t c)
{
    if (c >= 'a' && c <= 'z')
        return (uint16_t)(c - 0x20);
    if (c < 0x80)
        return c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return (uint16_t)(c - 0x20);
    if (c == 0xFF)
        return 0x178;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2)
        return (uint16_t)(c - 0x20);
    if (c >= 0x430 && c <= 0x44F)
        return (uint16_t)(c - 0x20);
    if (c >= 0x450 && c <= 0x45F)
        return (uint16_t)(c - 0x50);
    return c;
}

static inline uint16_t u32p_wlower(uint16_t c)
{
    if (c >= 'A' && c <= 'Z')
        return (uint16_t)(c + 0x20);
    if (c < 0x80)
        return c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return (uint16_t)(c + 0x20);
    if (c == 0x178)
        return 0xFF;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2)
        return (uint16_t)(c + 0x20);
    if (c >= 0x410 && c <= 0x42F)
        return (uint16_t)(c + 0x20);
    if (c >= 0x400 && c <= 0x40F)
        return (uint16_t)(c + 0x50);
    return c;
}

/* ---- US keyboard layout --------------------------------------------------------- */

#define U32P_VK_BACK 0x08
#define U32P_VK_TAB 0x09
#define U32P_VK_RETURN 0x0D
#define U32P_VK_SHIFT 0x10
#define U32P_VK_CONTROL 0x11
#define U32P_VK_MENU 0x12
#define U32P_VK_CAPITAL 0x14
#define U32P_VK_ESCAPE 0x1B
#define U32P_VK_SPACE 0x20
#define U32P_VK_NUMPAD0 0x60
#define U32P_VK_MULTIPLY 0x6A
#define U32P_VK_ADD 0x6B
#define U32P_VK_SUBTRACT 0x6D
#define U32P_VK_DECIMAL 0x6E
#define U32P_VK_F1 0x70
#define U32P_VK_NUMLOCK 0x90
#define U32P_VK_SCROLL 0x91
#define U32P_VK_LSHIFT 0xA0
#define U32P_VK_RSHIFT 0xA1
#define U32P_VK_LCONTROL 0xA2
#define U32P_VK_LMENU 0xA4
#define U32P_VK_OEM_1 0xBA
#define U32P_VK_OEM_PLUS 0xBB
#define U32P_VK_OEM_COMMA 0xBC
#define U32P_VK_OEM_MINUS 0xBD
#define U32P_VK_OEM_PERIOD 0xBE
#define U32P_VK_OEM_2 0xBF
#define U32P_VK_OEM_3 0xC0
#define U32P_VK_OEM_4 0xDB
#define U32P_VK_OEM_5 0xDC
#define U32P_VK_OEM_6 0xDD
#define U32P_VK_OEM_7 0xDE

/* Set-1 scan code (make code, no E0 prefix) to virtual key, indexed 0..0x58. 0 = none.
 * The shared keys (Shift, Ctrl, Alt) map to the left-hand virtual key (VK_L*). */
static const uint8_t u32p_scan_to_vk[0x59] = {
    0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  0xBD, 0xBB, 0x08, 0x09, /* 0x00 */
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  'O',  'P',  0xDB, 0xDD, 0x0D, 0xA2, 'A',  'S',  /* 0x10 */
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  0xBA, 0xDE, 0xC0, 0xA0, 0xDC, 'Z',  'X',  'C',  'V',  /* 0x20 */
    'B',  'N',  'M',  0xBC, 0xBE, 0xBF, 0xA1, 0x6A, 0xA4, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74, /* 0x30 */
    0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x67, 0x68, 0x69, 0x6D, 0x64, 0x65, 0x66, 0x6B, 0x61, /* 0x40 */
    0x62, 0x63, 0x60, 0x6E, 0,    0,    0,    0x7A, 0x7B,                                            /* 0x50 */
};

/* MAPVK_VK_TO_VSC (0): scan code for a virtual key, 0 when none. The generic VK_SHIFT,
 * VK_CONTROL and VK_MENU map to the left-hand key's code. */
static inline uint32_t u32p_vk_to_scan(uint32_t vk)
{
    uint32_t i;
    if (vk == U32P_VK_SHIFT)
        vk = U32P_VK_LSHIFT;
    else if (vk == U32P_VK_CONTROL)
        vk = U32P_VK_LCONTROL;
    else if (vk == U32P_VK_MENU)
        vk = U32P_VK_LMENU;
    for (i = 1; i < sizeof(u32p_scan_to_vk); ++i) {
        if (u32p_scan_to_vk[i] == vk)
            return i;
    }
    return 0;
}

/* MAPVK_VSC_TO_VK (1) and MAPVK_VSC_TO_VK_EX (3): virtual key for a scan code. Code 1
 * folds the left/right pairs into VK_SHIFT, VK_CONTROL and VK_MENU. */
static inline uint32_t u32p_scan_to_vk_fn(uint32_t scan, int ex)
{
    uint32_t vk;
    if (scan >= sizeof(u32p_scan_to_vk))
        return 0;
    vk = u32p_scan_to_vk[scan];
    if (!ex) {
        if (vk == U32P_VK_LSHIFT || vk == U32P_VK_RSHIFT)
            return U32P_VK_SHIFT;
        if (vk == U32P_VK_LCONTROL)
            return U32P_VK_CONTROL;
        if (vk == U32P_VK_LMENU)
            return U32P_VK_MENU;
    }
    return vk;
}

/* The unshifted and shifted character of a key on the US layout; 0 when it makes none. */
static inline uint16_t u32p_vk_char(uint32_t vk, int shift)
{
    static const char oem_plain[] = ";=,-./`";
    static const char oem_shift[] = ":+<_>?~";
    static const char brk_plain[] = "[\\]'";
    static const char brk_shift[] = "{|}\"";
    static const char digit_shift[] = ")!@#$%^&*(";
    if (vk >= 'A' && vk <= 'Z')
        return (uint16_t)(shift ? vk : vk + 0x20);
    if (vk >= '0' && vk <= '9')
        return (uint16_t)(shift ? (uint16_t)digit_shift[vk - '0'] : (uint16_t)vk);
    if (vk >= U32P_VK_NUMPAD0 && vk <= U32P_VK_NUMPAD0 + 9)
        return (uint16_t)('0' + (vk - U32P_VK_NUMPAD0));
    switch (vk) {
    case U32P_VK_SPACE: return ' ';
    case U32P_VK_MULTIPLY: return '*';
    case U32P_VK_ADD: return '+';
    case U32P_VK_SUBTRACT: return '-';
    case U32P_VK_DECIMAL: return '.';
    case U32P_VK_RETURN: return '\r';
    case U32P_VK_TAB: return '\t';
    case U32P_VK_BACK: return 0x08;
    case U32P_VK_ESCAPE: return 0x1B;
    default: break;
    }
    if (vk >= U32P_VK_OEM_1 && vk <= U32P_VK_OEM_3 && vk - U32P_VK_OEM_1 < 7)
        return (uint16_t)(shift ? (uint16_t)oem_shift[vk - U32P_VK_OEM_1] : (uint16_t)oem_plain[vk - U32P_VK_OEM_1]);
    if (vk >= U32P_VK_OEM_4 && vk <= U32P_VK_OEM_7)
        return (uint16_t)(shift ? (uint16_t)brk_shift[vk - U32P_VK_OEM_4] : (uint16_t)brk_plain[vk - U32P_VK_OEM_4]);
    return 0;
}

/* VkKeyScan: low byte the virtual key, high byte the shift state (1 shift, 2 ctrl, 4 alt).
 * Returns -1 (both bytes 0xFF) when the layout has no key for the character. */
static inline int16_t u32p_vk_key_scan(uint16_t ch)
{
    uint32_t vk;
    for (vk = 0x08; vk <= 0xDE; ++vk) {
        if (u32p_vk_char(vk, 0) == ch && !(vk >= U32P_VK_NUMPAD0 && vk <= U32P_VK_DECIMAL))
            return (int16_t)vk;
        if (u32p_vk_char(vk, 1) == ch && u32p_vk_char(vk, 0) != ch)
            return (int16_t)(vk | 0x100);
    }
    return -1;
}

/* GetKeyNameText for the scan code in bits 16..23 of lParam (bit 24 is the extended flag).
 * Copies the name into out (max units with the terminator) and returns its length, or 0
 * when the key has no name. */
static inline int u32p_key_name(uint32_t lparam, uint16_t *out, int max)
{
    static const char *const names[0x59] = {
        0, "Esc", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "Backspace", "Tab",
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "Enter", "Ctrl", 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "Shift", 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, "Right Shift", "Num *", "Alt", "Space", "Caps Lock", "F1", "F2", "F3", "F4", "F5",
        "F6", "F7", "F8", "F9", "F10", "Num Lock", "Scroll Lock", "Num 7", "Num 8", "Num 9", "Num -", "Num 4",
        "Num 5", "Num 6", "Num +", "Num 1", "Num 2", "Num 3", "Num 0", "Num Del", 0, 0, 0, "F11", "F12",
    };
    uint32_t scan = (lparam >> 16) & 0xFFu;
    uint32_t vk;
    char one[2] = {0, 0};
    const char *s = 0;
    int n = 0;
    if (max <= 0)
        return 0;
    if (scan < 0x59) {
        s = names[scan];
        if (!s) {
            vk = u32p_scan_to_vk[scan];
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || (vk >= U32P_VK_OEM_1 && vk <= U32P_VK_OEM_7)) {
                uint16_t c = u32p_vk_char(vk, 0);
                if (c >= 'a' && c <= 'z')
                    c = (uint16_t)(c - 0x20);
                one[0] = (char)c;
                s = one[0] ? one : 0;
            }
        }
    }
    if (!s) {
        out[0] = 0;
        return 0;
    }
    while (s[n] && n < max - 1) {
        out[n] = (uint8_t)s[n];
        ++n;
    }
    out[n] = 0;
    return n;
}

#endif /* NTDLL_USER32_PURE_H */
