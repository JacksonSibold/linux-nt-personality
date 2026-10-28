/*
 * k32_con_pure.h - an in-process model of a console screen buffer: the size, window, cursor and attributes that
 * GetConsoleScreenBufferInfo reports, the character and attribute cells that the Fill/Read/WriteConsoleOutput functions
 * address, scrolling, the title, the Ctrl handler list, and the key records ReadConsoleInput builds from input bytes.
 *
 * No console host is reached: WriteConsole still sends its bytes to the standard output file; this model only keeps the
 * state a program can query and set. The cells are not updated by WriteConsole.
 * Host tests: tests/ntdll-host/test_k32_con_pure.c.
 */
#ifndef K32_CON_PURE_H
#define K32_CON_PURE_H

#include <stddef.h>
#include <stdint.h>

#define KCON_MAX_W 256
#define KCON_MAX_H 512
#define KCON_DEFAULT_W 120
#define KCON_DEFAULT_H 300
#define KCON_WIN_W 120
#define KCON_WIN_H 30
#define KCON_MAX_WIN_W 240
#define KCON_MAX_WIN_H 100
#define KCON_TITLE_MAX 1024
#define KCON_HANDLERS 64
#define KCON_DEFAULT_ATTR 0x0007

typedef struct {
    int16_t x, y;
} kcon_coord;

typedef struct {
    int16_t left, top, right, bottom;
} kcon_rect;

typedef struct {
    kcon_coord size;
    kcon_coord cursor;
    uint16_t attr;
    kcon_rect window;
    kcon_coord max_window;
} kcon_info;

typedef int (*kcon_handler)(uint32_t event);

typedef struct {
    int16_t w, h;
    kcon_coord cursor;
    uint16_t attr;
    kcon_rect window;
    uint32_t cursor_size;
    int cursor_visible;
    uint16_t *chars; /* KCON_MAX_W * KCON_MAX_H cells, rows KCON_MAX_W apart */
    uint16_t *attrs;
    uint16_t title[KCON_TITLE_MAX];
    size_t title_len;
    kcon_handler handlers[KCON_HANDLERS];
    unsigned nhandlers;
    int ignore_ctrl_c;
    int key_up_pending; /* ReadConsoleInput: the key-up record that follows a key-down */
    uint8_t key_pending_ch;
} kcon;

#define KCON_ERR_INVALID_PARAMETER 87u

static inline size_t kcon_cell(int x, int y) { return (size_t)y * KCON_MAX_W + (size_t)x; }

/* The caller supplies the two cell arrays, KCON_MAX_W * KCON_MAX_H elements each. */
static inline void kcon_init(kcon *c, uint16_t *chars, uint16_t *attrs)
{
    size_t i;
    unsigned k;
    c->w = KCON_DEFAULT_W;
    c->h = KCON_DEFAULT_H;
    c->cursor.x = 0;
    c->cursor.y = 0;
    c->attr = KCON_DEFAULT_ATTR;
    c->window.left = 0;
    c->window.top = 0;
    c->window.right = KCON_WIN_W - 1;
    c->window.bottom = KCON_WIN_H - 1;
    c->cursor_size = 25;
    c->cursor_visible = 1;
    c->chars = chars;
    c->attrs = attrs;
    for (i = 0; i < (size_t)KCON_MAX_W * KCON_MAX_H; ++i) {
        chars[i] = ' ';
        attrs[i] = KCON_DEFAULT_ATTR;
    }
    c->title_len = 0;
    c->title[0] = 0;
    for (k = 0; k < KCON_HANDLERS; ++k)
        c->handlers[k] = NULL;
    c->nhandlers = 0;
    c->ignore_ctrl_c = 0;
    c->key_up_pending = 0;
    c->key_pending_ch = 0;
}

static inline int kcon_in_buffer(const kcon *c, kcon_coord p) { return p.x >= 0 && p.y >= 0 && p.x < c->w && p.y < c->h; }

static inline void kcon_get_info(const kcon *c, kcon_info *i)
{
    int mw = c->w < KCON_MAX_WIN_W ? c->w : KCON_MAX_WIN_W;
    int mh = c->h < KCON_MAX_WIN_H ? c->h : KCON_MAX_WIN_H;
    i->size.x = c->w;
    i->size.y = c->h;
    i->cursor = c->cursor;
    i->attr = c->attr;
    i->window = c->window;
    i->max_window.x = (int16_t)mw;
    i->max_window.y = (int16_t)mh;
}

static inline kcon_coord kcon_largest_window(const kcon *c)
{
    kcon_coord r;
    r.x = (int16_t)(c->w < KCON_MAX_WIN_W ? c->w : KCON_MAX_WIN_W);
    r.y = (int16_t)(c->h < KCON_MAX_WIN_H ? c->h : KCON_MAX_WIN_H);
    return r;
}

/* The buffer cannot be smaller than the window, or larger than the model holds. Cells that remain keep their contents;
 * new ones are blank in the current attribute. */
static inline uint32_t kcon_set_size(kcon *c, kcon_coord s)
{
    int x, y;
    int ww = c->window.right - c->window.left + 1, wh = c->window.bottom - c->window.top + 1;
    if (s.x < 1 || s.y < 1 || s.x > KCON_MAX_W || s.y > KCON_MAX_H || s.x < ww || s.y < wh)
        return KCON_ERR_INVALID_PARAMETER;
    for (y = 0; y < KCON_MAX_H; ++y)
        for (x = 0; x < KCON_MAX_W; ++x) {
            if (x >= s.x || y >= s.y) {
                c->chars[kcon_cell(x, y)] = ' ';
                c->attrs[kcon_cell(x, y)] = c->attr;
            } else if (x >= c->w || y >= c->h) {
                c->chars[kcon_cell(x, y)] = ' ';
                c->attrs[kcon_cell(x, y)] = c->attr;
            }
        }
    c->w = s.x;
    c->h = s.y;
    if (c->cursor.x >= c->w)
        c->cursor.x = (int16_t)(c->w - 1);
    if (c->cursor.y >= c->h)
        c->cursor.y = (int16_t)(c->h - 1);
    if (c->window.right >= c->w) {
        c->window.left = (int16_t)(c->window.left - (c->window.right - (c->w - 1)));
        c->window.right = (int16_t)(c->w - 1);
        if (c->window.left < 0)
            c->window.left = 0;
    }
    if (c->window.bottom >= c->h) {
        c->window.top = (int16_t)(c->window.top - (c->window.bottom - (c->h - 1)));
        c->window.bottom = (int16_t)(c->h - 1);
        if (c->window.top < 0)
            c->window.top = 0;
    }
    return 0;
}

/* Absolute: the rectangle is the window. Relative: its members are added to the window's. Either way the window must lie in
 * the buffer, be at least one cell, and not exceed the largest window. */
static inline uint32_t kcon_set_window(kcon *c, int absolute, kcon_rect r)
{
    kcon_rect n;
    if (absolute) {
        n = r;
    } else {
        n.left = (int16_t)(c->window.left + r.left);
        n.top = (int16_t)(c->window.top + r.top);
        n.right = (int16_t)(c->window.right + r.right);
        n.bottom = (int16_t)(c->window.bottom + r.bottom);
    }
    if (n.left < 0 || n.top < 0 || n.right >= c->w || n.bottom >= c->h || n.right < n.left || n.bottom < n.top)
        return KCON_ERR_INVALID_PARAMETER;
    if (n.right - n.left + 1 > KCON_MAX_WIN_W || n.bottom - n.top + 1 > KCON_MAX_WIN_H)
        return KCON_ERR_INVALID_PARAMETER;
    c->window = n;
    return 0;
}

/* The window moves to keep the cursor in view. */
static inline uint32_t kcon_set_cursor(kcon *c, kcon_coord p)
{
    if (!kcon_in_buffer(c, p))
        return KCON_ERR_INVALID_PARAMETER;
    c->cursor = p;
    if (p.x < c->window.left || p.x > c->window.right) {
        int w = c->window.right - c->window.left;
        c->window.left = (int16_t)(p.x < c->window.left ? p.x : p.x - w);
        c->window.right = (int16_t)(c->window.left + w);
    }
    if (p.y < c->window.top || p.y > c->window.bottom) {
        int h = c->window.bottom - c->window.top;
        c->window.top = (int16_t)(p.y < c->window.top ? p.y : p.y - h);
        c->window.bottom = (int16_t)(c->window.top + h);
    }
    return 0;
}

static inline uint32_t kcon_set_cursor_info(kcon *c, uint32_t size, int visible)
{
    if (size < 1 || size > 100)
        return KCON_ERR_INVALID_PARAMETER;
    c->cursor_size = size;
    c->cursor_visible = visible != 0;
    return 0;
}

/* How many cells from `at`, in reading order, fit before the end of the buffer. */
static inline size_t kcon_span(const kcon *c, kcon_coord at, size_t want)
{
    size_t left = (size_t)c->w * (size_t)c->h - ((size_t)at.y * (size_t)c->w + (size_t)at.x);
    return want < left ? want : left;
}

static inline void kcon_step(const kcon *c, kcon_coord *p)
{
    if (++p->x >= c->w) {
        p->x = 0;
        ++p->y;
    }
}

/* Fill, Write and Read all walk the cells in reading order from a start cell and stop at the end of the buffer; *done is how
 * many were handled. */
static inline uint32_t kcon_fill_chars(kcon *c, kcon_coord at, uint16_t ch, size_t n, size_t *done)
{
    size_t k, m;
    *done = 0;
    if (!kcon_in_buffer(c, at))
        return KCON_ERR_INVALID_PARAMETER;
    m = kcon_span(c, at, n);
    for (k = 0; k < m; ++k, kcon_step(c, &at))
        c->chars[kcon_cell(at.x, at.y)] = ch;
    *done = m;
    return 0;
}

static inline uint32_t kcon_fill_attrs(kcon *c, kcon_coord at, uint16_t attr, size_t n, size_t *done)
{
    size_t k, m;
    *done = 0;
    if (!kcon_in_buffer(c, at))
        return KCON_ERR_INVALID_PARAMETER;
    m = kcon_span(c, at, n);
    for (k = 0; k < m; ++k, kcon_step(c, &at))
        c->attrs[kcon_cell(at.x, at.y)] = attr;
    *done = m;
    return 0;
}

static inline uint32_t kcon_write_chars(kcon *c, kcon_coord at, const uint16_t *s, size_t n, size_t *done)
{
    size_t k, m;
    *done = 0;
    if (!kcon_in_buffer(c, at))
        return KCON_ERR_INVALID_PARAMETER;
    m = kcon_span(c, at, n);
    for (k = 0; k < m; ++k, kcon_step(c, &at))
        c->chars[kcon_cell(at.x, at.y)] = s[k];
    *done = m;
    return 0;
}

static inline uint32_t kcon_write_attrs(kcon *c, kcon_coord at, const uint16_t *s, size_t n, size_t *done)
{
    size_t k, m;
    *done = 0;
    if (!kcon_in_buffer(c, at))
        return KCON_ERR_INVALID_PARAMETER;
    m = kcon_span(c, at, n);
    for (k = 0; k < m; ++k, kcon_step(c, &at))
        c->attrs[kcon_cell(at.x, at.y)] = s[k];
    *done = m;
    return 0;
}

static inline uint32_t kcon_read_chars(const kcon *c, kcon_coord at, uint16_t *out, size_t n, size_t *done)
{
    size_t k, m;
    *done = 0;
    if (!kcon_in_buffer(c, at))
        return KCON_ERR_INVALID_PARAMETER;
    m = kcon_span(c, at, n);
    for (k = 0; k < m; ++k, kcon_step(c, &at))
        out[k] = c->chars[kcon_cell(at.x, at.y)];
    *done = m;
    return 0;
}

static inline uint32_t kcon_read_attrs(const kcon *c, kcon_coord at, uint16_t *out, size_t n, size_t *done)
{
    size_t k, m;
    *done = 0;
    if (!kcon_in_buffer(c, at))
        return KCON_ERR_INVALID_PARAMETER;
    m = kcon_span(c, at, n);
    for (k = 0; k < m; ++k, kcon_step(c, &at))
        out[k] = c->attrs[kcon_cell(at.x, at.y)];
    *done = m;
    return 0;
}

static inline int kcon_rect_has(kcon_rect r, int x, int y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

/* ScrollConsoleScreenBuffer: the cells of `src` (limited to `clip` and the buffer) move so that src's upper left lands on
 * `dest`; destination cells outside clip are untouched; source cells the move does not cover are filled. */
static inline uint32_t kcon_scroll(kcon *c, kcon_rect src, const kcon_rect *clip, kcon_coord dest, uint16_t fill_ch, uint16_t fill_attr)
{
    kcon_rect lim, d;
    int dx, dy, x, y, x0, x1, xs, y0, y1, ys;
    if (src.right < src.left || src.bottom < src.top)
        return KCON_ERR_INVALID_PARAMETER;
    lim.left = 0;
    lim.top = 0;
    lim.right = (int16_t)(c->w - 1);
    lim.bottom = (int16_t)(c->h - 1);
    if (clip) {
        if (clip->left > lim.left) lim.left = clip->left;
        if (clip->top > lim.top) lim.top = clip->top;
        if (clip->right < lim.right) lim.right = clip->right;
        if (clip->bottom < lim.bottom) lim.bottom = clip->bottom;
    }
    dx = dest.x - src.left;
    dy = dest.y - src.top;
    d.left = (int16_t)(src.left + dx);
    d.top = (int16_t)(src.top + dy);
    d.right = (int16_t)(src.right + dx);
    d.bottom = (int16_t)(src.bottom + dy);
    if (dy > 0) { y0 = src.bottom; y1 = src.top - 1; ys = -1; } else { y0 = src.top; y1 = src.bottom + 1; ys = 1; }
    if (dx > 0) { x0 = src.right; x1 = src.left - 1; xs = -1; } else { x0 = src.left; x1 = src.right + 1; xs = 1; }
    for (y = y0; y != y1; y += ys)
        for (x = x0; x != x1; x += xs) {
            int tx = x + dx, ty = y + dy;
            if (!kcon_rect_has(lim, x, y) || !kcon_rect_has(lim, tx, ty))
                continue;
            c->chars[kcon_cell(tx, ty)] = c->chars[kcon_cell(x, y)];
            c->attrs[kcon_cell(tx, ty)] = c->attrs[kcon_cell(x, y)];
        }
    for (y = src.top; y <= src.bottom; ++y)
        for (x = src.left; x <= src.right; ++x)
            if (kcon_rect_has(lim, x, y) && !kcon_rect_has(d, x, y)) {
                c->chars[kcon_cell(x, y)] = fill_ch;
                c->attrs[kcon_cell(x, y)] = fill_attr;
            }
    return 0;
}

/* ---- Rectangles of CHAR_INFO cells (WriteConsoleOutput / ReadConsoleOutput) ---------------------------------------------- */

typedef struct {
    uint16_t ch, attr;
} kcon_char_info;

/* The part of the region that lies in the buffer and in the caller's array of `bsz` cells starting at `bcoord`. Returns 0 and
 * an empty rectangle when nothing is left. */
static inline int kcon_clip_region(const kcon *c, kcon_coord bsz, kcon_coord bcoord, kcon_rect *rg)
{
    int l = rg->left, t = rg->top;
    int r = rg->right, b = rg->bottom;
    if (bcoord.x < 0 || bcoord.y < 0 || bcoord.x >= bsz.x || bcoord.y >= bsz.y || r < l || b < t)
        goto none;
    if (r > l + (bsz.x - bcoord.x) - 1) r = l + (bsz.x - bcoord.x) - 1;
    if (b > t + (bsz.y - bcoord.y) - 1) b = t + (bsz.y - bcoord.y) - 1;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > c->w - 1) r = c->w - 1;
    if (b > c->h - 1) b = c->h - 1;
    if (r < l || b < t)
        goto none;
    {
        kcon_rect o = *rg;
        rg->left = (int16_t)l;
        rg->top = (int16_t)t;
        rg->right = (int16_t)r;
        rg->bottom = (int16_t)b;
        (void)o;
    }
    return 1;
none:
    rg->right = (int16_t)(rg->left - 1);
    rg->bottom = (int16_t)(rg->top - 1);
    return 0;
}

static inline uint32_t kcon_write_rect(kcon *c, const kcon_char_info *src, kcon_coord bsz, kcon_coord bcoord, kcon_rect *rg)
{
    int x, y, ox = rg->left, oy = rg->top;
    if (!kcon_clip_region(c, bsz, bcoord, rg))
        return KCON_ERR_INVALID_PARAMETER;
    for (y = rg->top; y <= rg->bottom; ++y)
        for (x = rg->left; x <= rg->right; ++x) {
            const kcon_char_info *ci = &src[(size_t)(bcoord.y + (y - oy)) * (size_t)bsz.x + (size_t)(bcoord.x + (x - ox))];
            c->chars[kcon_cell(x, y)] = ci->ch;
            c->attrs[kcon_cell(x, y)] = ci->attr;
        }
    return 0;
}

static inline uint32_t kcon_read_rect(const kcon *c, kcon_char_info *dst, kcon_coord bsz, kcon_coord bcoord, kcon_rect *rg)
{
    int x, y, ox = rg->left, oy = rg->top;
    if (!kcon_clip_region(c, bsz, bcoord, rg))
        return KCON_ERR_INVALID_PARAMETER;
    for (y = rg->top; y <= rg->bottom; ++y)
        for (x = rg->left; x <= rg->right; ++x) {
            kcon_char_info *ci = &dst[(size_t)(bcoord.y + (y - oy)) * (size_t)bsz.x + (size_t)(bcoord.x + (x - ox))];
            ci->ch = c->chars[kcon_cell(x, y)];
            ci->attr = c->attrs[kcon_cell(x, y)];
        }
    return 0;
}

/* ---- Title ---------------------------------------------------------------------------------------------------------------- */

static inline void kcon_set_title(kcon *c, const uint16_t *s, size_t n)
{
    size_t i;
    if (n >= KCON_TITLE_MAX)
        n = KCON_TITLE_MAX - 1;
    for (i = 0; i < n; ++i)
        c->title[i] = s[i];
    c->title[n] = 0;
    c->title_len = n;
}

/* Copies up to size-1 characters and terminates; returns the number copied. */
static inline size_t kcon_get_title(const kcon *c, uint16_t *out, size_t size)
{
    size_t i, n;
    if (size == 0)
        return 0;
    n = c->title_len < size - 1 ? c->title_len : size - 1;
    for (i = 0; i < n; ++i)
        out[i] = c->title[i];
    out[n] = 0;
    return n;
}

/* ---- Ctrl handlers -------------------------------------------------------------------------------------------------------- */

/* A NULL handler adds or removes the "ignore Ctrl+C" state. A removal that finds nothing is an error. */
static inline uint32_t kcon_set_handler(kcon *c, kcon_handler h, int add)
{
    unsigned i;
    if (!h) {
        c->ignore_ctrl_c = add != 0;
        return 0;
    }
    if (add) {
        if (c->nhandlers >= KCON_HANDLERS)
            return KCON_ERR_INVALID_PARAMETER;
        c->handlers[c->nhandlers++] = h;
        return 0;
    }
    for (i = c->nhandlers; i > 0; --i)
        if (c->handlers[i - 1] == h) {
            for (; i < c->nhandlers; ++i)
                c->handlers[i - 1] = c->handlers[i];
            c->handlers[--c->nhandlers] = NULL;
            return 0;
        }
    return KCON_ERR_INVALID_PARAMETER;
}

/* Handlers run newest first until one returns nonzero. Returns 1 when the event was handled (or is Ctrl+C and ignored), 0
 * when the default action (ending the process) should follow. */
static inline int kcon_dispatch(kcon *c, uint32_t event)
{
    unsigned i;
    if (event == 0 && c->ignore_ctrl_c)
        return 1;
    for (i = c->nhandlers; i > 0; --i)
        if (c->handlers[i - 1](event))
            return 1;
    return 0;
}

/* ---- Input records ---------------------------------------------------------------------------------------------------------- */

#define KCON_KEY_EVENT 1u
#define KCON_SHIFT_PRESSED 0x0010u

typedef struct {
    uint16_t event_type;
    uint16_t pad;
    int32_t key_down;
    uint16_t repeat;
    uint16_t vk;
    uint16_t scan;
    uint16_t ch;
    uint32_t ctrl_state;
} kcon_key_record; /* the 20-byte INPUT_RECORD for a key event */

static inline uint16_t kcon_vk_of(uint8_t ch, uint32_t *ctrl)
{
    *ctrl = 0;
    if (ch >= 'a' && ch <= 'z')
        return (uint16_t)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') {
        *ctrl = KCON_SHIFT_PRESSED;
        return ch;
    }
    if (ch >= '0' && ch <= '9')
        return ch;
    switch (ch) {
    case ' ': return 0x20;
    case '\r': case '\n': return 0x0D;
    case '\t': return 0x09;
    case 8: case 0x7F: return 0x08;
    case 27: return 0x1B;
    case ',': return 0xBC;
    case '.': return 0xBE;
    case '-': return 0xBD;
    case '=': return 0xBB;
    case '/': return 0xBF;
    case ';': return 0xBA;
    case '\'': return 0xDE;
    case '[': return 0xDB;
    case ']': return 0xDD;
    case '\\': return 0xDC;
    case '`': return 0xC0;
    default: return 0;
    }
}

static inline void kcon_make_key(kcon_key_record *r, uint8_t ch, int down)
{
    uint32_t ctrl;
    r->event_type = KCON_KEY_EVENT;
    r->pad = 0;
    r->key_down = down;
    r->repeat = 1;
    r->vk = kcon_vk_of(ch, &ctrl);
    r->scan = 0;
    r->ch = ch == '\n' ? '\r' : (ch == 0x7F ? 8 : ch);
    r->ctrl_state = ctrl;
}

/* One input byte becomes a key-down record and a key-up record. Returns the next record: the caller passes the byte when
 * key_up_pending is clear and nothing otherwise. */
static inline void kcon_next_key(kcon *c, int have_byte, uint8_t byte, kcon_key_record *r)
{
    if (c->key_up_pending) {
        c->key_up_pending = 0;
        kcon_make_key(r, c->key_pending_ch, 0);
        return;
    }
    (void)have_byte;
    c->key_pending_ch = byte;
    c->key_up_pending = 1;
    kcon_make_key(r, byte, 1);
}

#endif
