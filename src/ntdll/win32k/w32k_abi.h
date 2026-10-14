/* w32k_abi.h - Win32 ABI layouts and field offsets used by the user32 and gdi32
 * shims (user32_win32k.c, gdi32_win32k.c). The offsets follow the public x64
 * definitions of DEVMODEW, BITMAPINFOHEADER and friends. They are checked at
 * compile time against the MinGW headers by tests/ntdll-host/check_w32k_abi.c. */
#ifndef W32K_ABI_H
#define W32K_ABI_H

#include "w32k.h"
#include <stdint.h>

/* WNDCLASSEXW (x64). */
typedef struct W32K_WNDCLASSEXW {
    uint32_t cbSize;
    uint32_t style;
    w32k_wndproc lpfnWndProc;
    int32_t cbClsExtra;
    int32_t cbWndExtra;
    const void *hInstance;
    uintptr_t hIcon;
    uintptr_t hCursor;
    uintptr_t hbrBackground;
    const uint16_t *lpszMenuName;
    const uint16_t *lpszClassName;
    uintptr_t hIconSm;
} W32K_WNDCLASSEXW;

/* PAINTSTRUCT (x64, 72 bytes). */
typedef struct W32K_PAINTSTRUCT {
    uintptr_t hdc;
    int32_t fErase;
    w32k_rect rcPaint;
    int32_t fRestore;
    int32_t fIncUpdate;
    uint8_t rgbReserved[32];
} W32K_PAINTSTRUCT;

/* DEVMODEW: display fields only. */
#define W32K_DEVMODEW_SIZE 220
#define W32K_DM_OFF_SIZE 68
#define W32K_DM_OFF_FIELDS 72
#define W32K_DM_OFF_BITSPERPEL 168
#define W32K_DM_OFF_PELSWIDTH 172
#define W32K_DM_OFF_PELSHEIGHT 176
#define W32K_DM_OFF_FREQUENCY 184
#define W32K_DM_BITSPERPEL 0x00040000u
#define W32K_DM_PELSWIDTH 0x00080000u
#define W32K_DM_PELSHEIGHT 0x00100000u
#define W32K_DM_DISPLAYFREQUENCY 0x00400000u

/* BITMAPINFOHEADER fields. */
#define W32K_BMIH_WIDTH 4
#define W32K_BMIH_HEIGHT 8
#define W32K_BMIH_BITCOUNT 14

#endif /* W32K_ABI_H */
