/* w32k_evdev.h - decoding of Linux evdev records into virtual-key input events.
 *
 * A record is struct input_event on LP64 (24 bytes, little-endian):
 *   0..7   timeval.tv_sec   (int64)
 *   8..15  timeval.tv_usec  (int64)
 *   16..17 type  (EV_KEY, EV_REL, EV_SYN)
 *   18..19 code  (KEY_*, BTN_*, REL_*, SYN_*)
 *   20..23 value (int32)
 * The codes are the values from linux/input-event-codes.h. Only the subset the
 * model uses is mapped; other codes decode but map to VK 0.
 */
#ifndef W32K_EVDEV_H
#define W32K_EVDEV_H

#include <stdint.h>

#define W32K_EVDEV_RECORD_SIZE 24u

#define W32K_EV_SYN 0x00
#define W32K_EV_KEY 0x01
#define W32K_EV_REL 0x02

#define W32K_SYN_REPORT 0x00
#define W32K_REL_X 0x00
#define W32K_REL_Y 0x01
#define W32K_REL_WHEEL 0x08

#define W32K_BTN_LEFT 0x110
#define W32K_BTN_RIGHT 0x111
#define W32K_BTN_MIDDLE 0x112

typedef struct w32k_evdev_ev {
    uint16_t type;
    uint16_t code;
    int32_t value;
} w32k_evdev_ev;

static inline void w32k_evdev_decode(const uint8_t *p, w32k_evdev_ev *ev)
{
    ev->type = (uint16_t)(p[16] | (p[17] << 8));
    ev->code = (uint16_t)(p[18] | (p[19] << 8));
    ev->value = (int32_t)((uint32_t)p[20] | ((uint32_t)p[21] << 8) |
                          ((uint32_t)p[22] << 16) | ((uint32_t)p[23] << 24));
}

/* Linux KEY_* code to virtual-key code. Returns 0 for unmapped codes. */
static inline uint32_t w32k_evdev_key_to_vk(uint16_t code)
{
    /* Letters: KEY_Q..KEY_P (16..25), KEY_A..KEY_L (30..38), KEY_Z..KEY_M (44..50). */
    static const uint8_t row_q[10] = {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'};
    static const uint8_t row_a[9] = {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L'};
    static const uint8_t row_z[7] = {'Z', 'X', 'C', 'V', 'B', 'N', 'M'};
    if (code >= 16 && code <= 25)
        return row_q[code - 16];
    if (code >= 30 && code <= 38)
        return row_a[code - 30];
    if (code >= 44 && code <= 50)
        return row_z[code - 44];
    if (code >= 2 && code <= 10)
        return (uint32_t)('1' + (code - 2));
    switch (code) {
    case 1: return 0x1B;          /* KEY_ESC -> VK_ESCAPE */
    case 11: return '0';          /* KEY_0 */
    case 12: return 0xBD;         /* KEY_MINUS -> VK_OEM_MINUS */
    case 13: return 0xBB;         /* KEY_EQUAL -> VK_OEM_PLUS */
    case 14: return 0x08;         /* KEY_BACKSPACE */
    case 15: return 0x09;         /* KEY_TAB */
    case 26: return 0xDB;         /* KEY_LEFTBRACE */
    case 27: return 0xDD;         /* KEY_RIGHTBRACE */
    case 28: return 0x0D;         /* KEY_ENTER */
    case 29: return 0xA2;         /* KEY_LEFTCTRL -> VK_LCONTROL */
    case 39: return 0xBA;         /* KEY_SEMICOLON */
    case 40: return 0xDE;         /* KEY_APOSTROPHE */
    case 41: return 0xC0;         /* KEY_GRAVE */
    case 42: return 0xA0;         /* KEY_LEFTSHIFT -> VK_LSHIFT */
    case 43: return 0xDC;         /* KEY_BACKSLASH */
    case 51: return 0xBC;         /* KEY_COMMA */
    case 52: return 0xBE;         /* KEY_DOT */
    case 53: return 0xBF;         /* KEY_SLASH */
    case 54: return 0xA1;         /* KEY_RIGHTSHIFT -> VK_RSHIFT */
    case 56: return 0xA4;         /* KEY_LEFTALT -> VK_LMENU */
    case 57: return 0x20;         /* KEY_SPACE */
    case 58: return 0x14;         /* KEY_CAPSLOCK */
    case 97: return 0xA3;         /* KEY_RIGHTCTRL -> VK_RCONTROL */
    case 100: return 0xA5;        /* KEY_RIGHTALT -> VK_RMENU */
    case 102: return 0x24;        /* KEY_HOME */
    case 103: return 0x26;        /* KEY_UP */
    case 104: return 0x21;        /* KEY_PAGEUP -> VK_PRIOR */
    case 105: return 0x25;        /* KEY_LEFT */
    case 106: return 0x27;        /* KEY_RIGHT */
    case 107: return 0x23;        /* KEY_END */
    case 108: return 0x28;        /* KEY_DOWN */
    case 109: return 0x22;        /* KEY_PAGEDOWN -> VK_NEXT */
    case 110: return 0x2D;        /* KEY_INSERT */
    case 111: return 0x2E;        /* KEY_DELETE */
    default: break;
    }
    if (code >= 59 && code <= 68)
        return (uint32_t)(0x70 + (code - 59));   /* KEY_F1..KEY_F10 -> VK_F1..VK_F10 */
    if (code == 87)
        return 0x7A;                             /* KEY_F11 */
    if (code == 88)
        return 0x7B;                             /* KEY_F12 */
    return 0;
}

#endif /* W32K_EVDEV_H */
