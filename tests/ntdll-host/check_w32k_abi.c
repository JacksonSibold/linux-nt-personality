/* check_w32k_abi.c - compile-time check that the win32k model's ABI structs and
 * the offsets in src/ntdll/win32k/w32k_abi.h match the MinGW Win32 headers.
 *
 * Build-only, never run (Windows Application Control blocks fresh binaries):
 *   make -C tests/ntdll-host w32k-abi
 * which runs x86_64-w64-mingw32-gcc -fsyntax-only on this file. A mismatch is a
 * compile error. */
#include <windows.h>
#include <stddef.h>

#include "w32k.h"
#include "win32k/w32k_abi.h"

/* The model's message and geometry types pass through to Win32 callers unchanged. */
_Static_assert(sizeof(MSG) == sizeof(w32k_msg), "MSG size");
_Static_assert(offsetof(MSG, hwnd) == offsetof(w32k_msg, hwnd), "MSG.hwnd");
_Static_assert(offsetof(MSG, message) == offsetof(w32k_msg, message), "MSG.message");
_Static_assert(offsetof(MSG, wParam) == offsetof(w32k_msg, wparam), "MSG.wParam");
_Static_assert(offsetof(MSG, lParam) == offsetof(w32k_msg, lparam), "MSG.lParam");
_Static_assert(offsetof(MSG, time) == offsetof(w32k_msg, time), "MSG.time");
_Static_assert(offsetof(MSG, pt) == offsetof(w32k_msg, pt), "MSG.pt");
_Static_assert(sizeof(POINT) == sizeof(w32k_point), "POINT size");
_Static_assert(sizeof(RECT) == sizeof(w32k_rect), "RECT size");
_Static_assert(sizeof(SIZE) == sizeof(w32k_size), "SIZE size");

/* WNDCLASSEXW */
_Static_assert(sizeof(WNDCLASSEXW) == sizeof(W32K_WNDCLASSEXW), "WNDCLASSEXW size");
_Static_assert(offsetof(WNDCLASSEXW, lpfnWndProc) == offsetof(W32K_WNDCLASSEXW, lpfnWndProc), "wc.proc");
_Static_assert(offsetof(WNDCLASSEXW, lpszClassName) == offsetof(W32K_WNDCLASSEXW, lpszClassName), "wc.name");
_Static_assert(offsetof(WNDCLASSEXW, hbrBackground) == offsetof(W32K_WNDCLASSEXW, hbrBackground), "wc.bg");

/* PAINTSTRUCT */
_Static_assert(sizeof(PAINTSTRUCT) == sizeof(W32K_PAINTSTRUCT), "PAINTSTRUCT size");
_Static_assert(offsetof(PAINTSTRUCT, hdc) == offsetof(W32K_PAINTSTRUCT, hdc), "ps.hdc");
_Static_assert(offsetof(PAINTSTRUCT, fErase) == offsetof(W32K_PAINTSTRUCT, fErase), "ps.fErase");
_Static_assert(offsetof(PAINTSTRUCT, rcPaint) == offsetof(W32K_PAINTSTRUCT, rcPaint), "ps.rcPaint");

/* DEVMODEW (display fields) */
_Static_assert(sizeof(DEVMODEW) == W32K_DEVMODEW_SIZE, "DEVMODEW size");
_Static_assert(offsetof(DEVMODEW, dmSize) == W32K_DM_OFF_SIZE, "dmSize");
_Static_assert(offsetof(DEVMODEW, dmFields) == W32K_DM_OFF_FIELDS, "dmFields");
_Static_assert(offsetof(DEVMODEW, dmBitsPerPel) == W32K_DM_OFF_BITSPERPEL, "dmBitsPerPel");
_Static_assert(offsetof(DEVMODEW, dmPelsWidth) == W32K_DM_OFF_PELSWIDTH, "dmPelsWidth");
_Static_assert(offsetof(DEVMODEW, dmPelsHeight) == W32K_DM_OFF_PELSHEIGHT, "dmPelsHeight");
_Static_assert(offsetof(DEVMODEW, dmDisplayFrequency) == W32K_DM_OFF_FREQUENCY, "dmDisplayFrequency");
_Static_assert(DM_BITSPERPEL == W32K_DM_BITSPERPEL, "DM_BITSPERPEL");
_Static_assert(DM_PELSWIDTH == W32K_DM_PELSWIDTH, "DM_PELSWIDTH");
_Static_assert(DM_PELSHEIGHT == W32K_DM_PELSHEIGHT, "DM_PELSHEIGHT");
_Static_assert(DM_DISPLAYFREQUENCY == W32K_DM_DISPLAYFREQUENCY, "DM_DISPLAYFREQUENCY");

/* BITMAPINFOHEADER */
_Static_assert(offsetof(BITMAPINFOHEADER, biWidth) == W32K_BMIH_WIDTH, "biWidth");
_Static_assert(offsetof(BITMAPINFOHEADER, biHeight) == W32K_BMIH_HEIGHT, "biHeight");
_Static_assert(offsetof(BITMAPINFOHEADER, biBitCount) == W32K_BMIH_BITCOUNT, "biBitCount");

/* Window messages and flags the shims pass through unchanged. */
_Static_assert(WM_PAINT == W32K_WM_PAINT && WM_TIMER == W32K_WM_TIMER, "WM_ values");
_Static_assert(WM_KEYDOWN == W32K_WM_KEYDOWN && WM_CHAR == W32K_WM_CHAR, "WM_KEY/CHAR values");
_Static_assert(WM_LBUTTONDOWN == W32K_WM_LBUTTONDOWN && WM_MOUSEWHEEL == W32K_WM_MOUSEWHEEL, "WM_MOUSE values");
_Static_assert(WM_CAPTURECHANGED == W32K_WM_CAPTURECHANGED, "WM_CAPTURECHANGED");
_Static_assert(WM_DISPLAYCHANGE == W32K_WM_DISPLAYCHANGE, "WM_DISPLAYCHANGE");
_Static_assert(WS_VISIBLE == W32K_WS_VISIBLE && WS_CHILD == W32K_WS_CHILD, "WS_ values");
_Static_assert(SW_SHOW == W32K_SW_SHOW && SW_HIDE == W32K_SW_HIDE, "SW_ values");
_Static_assert(SRCCOPY == W32K_SRCCOPY && PATCOPY == W32K_PATCOPY, "ROP values");
_Static_assert(CDS_TEST == W32K_CDS_TEST, "CDS_TEST");
_Static_assert(DISP_CHANGE_SUCCESSFUL == W32K_DISP_CHANGE_SUCCESSFUL, "DISP_CHANGE_SUCCESSFUL");
_Static_assert(DISP_CHANGE_BADMODE == W32K_DISP_CHANGE_BADMODE, "DISP_CHANGE_BADMODE");
_Static_assert(SM_CXSCREEN == W32K_SM_CXSCREEN && SM_CMONITORS == W32K_SM_CMONITORS, "SM_ values");
_Static_assert(CW_USEDEFAULT == (int)W32K_CW_USEDEFAULT, "CW_USEDEFAULT");

