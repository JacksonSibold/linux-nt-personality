/* w32k_font.c - font specification, metrics and the glyph rasteriser.
 *
 * The model has one font engine: a stroke font. Each printable ASCII character is a
 * set of polylines on a 9 x 14 design grid (x 0..8, y 0..13; the cap line is y = 0,
 * the baseline y = 10, the descender line y = 13). A glyph is fitted to the requested
 * cell height and drawn with a square pen whose width grows with the height and with
 * the weight. The system font (8x16, fixed pitch) is this engine at height 16.
 *
 * The outlines are authored here for this model. They are not a copy of any vendor
 * font. Lowercase letters are reduced capitals: each lowercase character is the
 * capital with its y coordinates mapped onto the x-height (y 3..10). They have no
 * ascender or descender of their own.
 *
 * Coverage is binary (0 or 255). There is no antialiasing and no hinting. The stroke
 * pen is a square of the stroke width, stamped along each Bresenham step, so a thick
 * stroke has no gaps at diagonals.
 */
#include "w32k_font.h"

#define PU (-1)   /* pen up: the next pair starts a new polyline */
#define END 127   /* end of a glyph */

/* Stroke data: pairs of (x, y), one glyph after another, each followed by END.
 * Entry order: codes 32..96 (65 entries), then 123..126 (4 entries). Lowercase codes
 * 97..122 use the capital entries (see stroke_entry). */
static const int8_t g_strokes[] = {
    /* 32 ' ' */   END,
    /* 33 '!' */   1,0, 1,7, PU,0, 1,9, 1,10, END,
    /* 34 '"' */   1,0, 1,3, PU,0, 3,0, 3,3, END,
    /* 35 '#' */   2,0, 1,10, PU,0, 5,0, 4,10, PU,0, 0,3, 6,3, PU,0, 0,7, 6,7, END,
    /* 36 '$' */   6,2, 5,1, 3,0, 1,0, 0,1, 0,3, 2,5, 5,6, 7,7, 7,9, 6,10, 4,10, 2,10, 0,9, PU,0, 4,0, 4,10, END,
    /* 37 '%' */   0,0, 2,0, 2,2, 0,2, 0,0, PU,0, 6,8, 8,8, 8,10, 6,10, 6,8, PU,0, 7,0, 1,10, END,
    /* 38 '&' */   6,10, 2,4, 0,2, 2,0, 4,0, 5,2, 3,5, 0,9, 1,10, 6,10, END,
    /* 39 ''' */   1,0, 1,2, END,
    /* 40 '(' */   3,0, 1,3, 0,6, 1,10, 3,13, END,
    /* 41 ')' */   1,0, 3,3, 4,6, 3,10, 1,13, END,
    /* 42 '*' */   3,1, 3,7, PU,0, 1,2, 5,6, PU,0, 5,2, 1,6, END,
    /* 43 '+' */   3,3, 3,9, PU,0, 0,6, 6,6, END,
    /* 44 ',' */   1,9, 1,10, 0,12, END,
    /* 45 '-' */   0,6, 4,6, END,
    /* 46 '.' */   1,9, 1,10, END,
    /* 47 '/' */   6,0, 0,13, END,
    /* 48 '0' */   1,0, 6,0, 7,1, 7,9, 6,10, 1,10, 0,9, 0,1, 1,0, END,
    /* 49 '1' */   1,2, 3,0, 3,10, PU,0, 1,10, 5,10, END,
    /* 50 '2' */   0,2, 1,0, 6,0, 7,2, 7,4, 0,10, 7,10, END,
    /* 51 '3' */   0,1, 1,0, 6,0, 7,1, 7,4, 6,5, 3,5, PU,0, 6,5, 7,6, 7,9, 6,10, 1,10, 0,9, END,
    /* 52 '4' */   5,10, 5,0, 0,7, 7,7, END,
    /* 53 '5' */   7,0, 1,0, 0,5, 4,4, 6,5, 7,7, 7,9, 6,10, 1,10, 0,9, END,
    /* 54 '6' */   6,0, 2,1, 0,4, 0,9, 1,10, 6,10, 7,8, 7,6, 6,5, 1,5, 0,6, END,
    /* 55 '7' */   0,0, 7,0, 3,10, END,
    /* 56 '8' */   1,5, 0,4, 0,1, 1,0, 6,0, 7,1, 7,4, 6,5, 1,5, 0,6, 0,9, 1,10, 6,10, 7,9, 7,6, 6,5, END,
    /* 57 '9' */   7,5, 7,1, 6,0, 1,0, 0,1, 0,4, 1,5, 6,5, 7,6, 7,9, 6,10, 1,10, 0,9, END,
    /* 58 ':' */   1,4, 1,5, PU,0, 1,9, 1,10, END,
    /* 59 ';' */   1,4, 1,5, PU,0, 1,9, 1,10, 0,12, END,
    /* 60 '<' */   5,2, 1,6, 5,10, END,
    /* 61 '=' */   0,4, 5,4, PU,0, 0,8, 5,8, END,
    /* 62 '>' */   1,2, 5,6, 1,10, END,
    /* 63 '?' */   0,1, 1,0, 5,0, 6,1, 6,3, 3,5, 3,7, PU,0, 3,9, 3,10, END,
    /* 64 '@' */   8,5, 8,2, 7,0, 1,0, 0,2, 0,8, 1,10, 7,10, 8,8, END,
    /* 65 'A' */   0,10, 3,0, 6,10, PU,0, 1,6, 5,6, END,
    /* 66 'B' */   0,0, 0,10, 5,10, 6,9, 6,6, 5,5, 0,5, PU,0, 0,0, 4,0, 5,1, 5,4, 4,5, END,
    /* 67 'C' */   6,1, 5,0, 2,0, 0,2, 0,8, 2,10, 5,10, 6,9, END,
    /* 68 'D' */   0,0, 0,10, 4,10, 6,8, 6,2, 4,0, 0,0, END,
    /* 69 'E' */   6,0, 0,0, 0,10, 6,10, PU,0, 0,5, 4,5, END,
    /* 70 'F' */   6,0, 0,0, 0,10, PU,0, 0,5, 4,5, END,
    /* 71 'G' */   6,1, 5,0, 2,0, 0,2, 0,8, 2,10, 5,10, 6,8, 6,6, 3,6, END,
    /* 72 'H' */   0,0, 0,10, PU,0, 6,0, 6,10, PU,0, 0,5, 6,5, END,
    /* 73 'I' */   0,0, 2,0, PU,0, 1,0, 1,10, PU,0, 0,10, 2,10, END,
    /* 74 'J' */   5,0, 5,8, 4,10, 2,10, 0,8, END,
    /* 75 'K' */   0,0, 0,10, PU,0, 6,0, 0,6, PU,0, 2,4, 6,10, END,
    /* 76 'L' */   0,0, 0,10, 5,10, END,
    /* 77 'M' */   0,10, 0,0, 4,6, 8,0, 8,10, END,
    /* 78 'N' */   0,10, 0,0, 6,10, 6,0, END,
    /* 79 'O' */   2,0, 6,0, 8,2, 8,8, 6,10, 2,10, 0,8, 0,2, 2,0, END,
    /* 80 'P' */   0,10, 0,0, 5,0, 6,1, 6,4, 5,5, 0,5, END,
    /* 81 'Q' */   2,0, 6,0, 8,2, 8,8, 6,10, 2,10, 0,8, 0,2, 2,0, PU,0, 5,8, 8,13, END,
    /* 82 'R' */   0,10, 0,0, 5,0, 6,1, 6,4, 5,5, 0,5, PU,0, 3,5, 6,10, END,
    /* 83 'S' */   6,1, 5,0, 2,0, 0,1, 0,4, 1,5, 5,5, 6,6, 6,9, 5,10, 1,10, 0,9, END,
    /* 84 'T' */   0,0, 6,0, PU,0, 3,0, 3,10, END,
    /* 85 'U' */   0,0, 0,8, 1,10, 5,10, 6,8, 6,0, END,
    /* 86 'V' */   0,0, 3,10, 6,0, END,
    /* 87 'W' */   0,0, 2,10, 4,3, 6,10, 8,0, END,
    /* 88 'X' */   0,0, 6,10, PU,0, 6,0, 0,10, END,
    /* 89 'Y' */   0,0, 3,5, 6,0, PU,0, 3,5, 3,10, END,
    /* 90 'Z' */   0,0, 6,0, 0,10, 6,10, END,
    /* 91 '[' */   3,0, 1,0, 1,13, 3,13, END,
    /* 92 '\' */   0,0, 6,13, END,
    /* 93 ']' */   1,0, 3,0, 3,13, 1,13, END,
    /* 94 '^' */   1,4, 3,1, 5,4, END,
    /* 95 '_' */   0,13, 6,13, END,
    /* 96 '`' */   1,0, 2,2, END,
    /* 123 '{' */  3,0, 2,1, 2,4, 0,6, 2,8, 2,11, 3,13, END,
    /* 124 '|' */  1,0, 1,13, END,
    /* 125 '}' */  1,0, 2,1, 2,4, 4,6, 2,8, 2,11, 1,13, END,
    /* 126 '~' */  0,6, 2,4, 4,8, 6,6, END,
};

/* Design advance per entry, in the same order as g_strokes: codes 32..96, then 123..126. */
static const uint8_t g_advance[69] = {
    /* 32..47 */  4, 3, 4, 7, 7, 8, 8, 3, 4, 4, 6, 7, 3, 5, 3, 5,
    /* 48..63 */  7, 5, 7, 7, 7, 7, 7, 7, 7, 7, 3, 3, 6, 6, 6, 6,
    /* 64..79 */  9, 7, 7, 7, 7, 6, 6, 7, 7, 3, 6, 7, 6, 8, 7, 8,
    /* 80..95 */  7, 8, 7, 7, 7, 7, 7, 9, 7, 7, 7, 4, 5, 4, 6, 6,
    /* 96, 123..126 */ 3, 4, 3, 4, 6,
};

#define ENTRY_COUNT 69
#define FIRST_CODE 32
#define LAST_MAIN_CODE 96

/* Returns the index in g_strokes of the entry for character code c (32..126). */
static int stroke_entry(uint16_t c)
{
    if (c >= 97 && c <= 122)
        c = (uint16_t)(c - 32); /* lowercase: the capital's entry */
    if (c >= FIRST_CODE && c <= LAST_MAIN_CODE)
        return (int)(c - FIRST_CODE);
    if (c >= 123 && c <= 126)
        return (int)(65 + (c - 123));
    return -1;
}

/* Start of entry k in g_strokes (the k-th END marker begins entry k+1). */
static size_t entry_start(int k)
{
    size_t i = 0;
    int seen = 0;
    while (seen < k && i < sizeof(g_strokes)) {
        if (g_strokes[i] == END)
            ++seen;
        ++i;
    }
    return i;
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int32_t absi(int32_t v)
{
    return v < 0 ? -v : v;
}

/* Design x (0..8) to pixels: 7/8 of the height-scaled cell, rounded. */
static int32_t px_x(int32_t x, int32_t h)
{
    return (x * 7 * h + 64) / 128;
}

/* Design y (0..13) to pixels, rounded, so y = 13 lands on the last row of a 14-unit cell. */
static int32_t px_y(int32_t y, int32_t h)
{
    return (y * h + 7) / 14;
}

/* Lowercase mapping: capitals' y in 0..10 onto the x-height band 3..10. */
static int32_t lower_y(int32_t y)
{
    if (y <= 10)
        return 3 + (y * 7 + 5) / 10;
    return y;
}

static int is_lower(uint16_t c)
{
    return c >= 97 && c <= 122;
}

static int bold_weight(int32_t weight)
{
    return weight >= 600;
}

/* Stroke width in pixels for a height and weight. */
static int32_t stroke_width(int32_t h, int32_t weight)
{
    int32_t bw = h / 12;
    if (bw < 1)
        bw = 1;
    if (bold_weight(weight)) {
        int32_t extra = h / 16;
        bw += extra < 1 ? 1 : extra;
    }
    return bw;
}

static void stamp(uint8_t *cov, int32_t stride, int32_t cw, int32_t ch, int32_t x, int32_t y, int32_t bw)
{
    int32_t x0 = x - bw / 2;
    int32_t y0 = y - bw / 2;
    for (int32_t j = 0; j < bw; ++j) {
        int32_t py = y0 + j;
        if (py < 0 || py >= ch)
            continue;
        for (int32_t i = 0; i < bw; ++i) {
            int32_t px = x0 + i;
            if (px >= 0 && px < cw)
                cov[py * stride + px] = 255;
        }
    }
}

/* Bresenham line from (x0,y0) to (x1,y1) with the square pen at every step. */
static void draw_line(uint8_t *cov, int32_t stride, int32_t cw, int32_t ch,
                      int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t bw)
{
    int32_t dx = absi(x1 - x0);
    int32_t dy = -absi(y1 - y0);
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        stamp(cov, stride, cw, ch, x0, y0, bw);
        if (x0 == x1 && y0 == y1)
            break;
        {
            int32_t e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static int32_t spec_height(const w32k_fontspec *spec)
{
    return clampi(spec->height, 4, W32K_FONT_MAX_H);
}

/* ---- Spec, metrics ------------------------------------------------------------ */

void w32k_font_spec_from_logfont(const w32k_logfont *lf, w32k_fontspec *spec)
{
    int32_t h;
    if (!spec)
        return;
    spec->height = 16;
    spec->weight = 400;
    spec->italic = 0;
    spec->underline = 0;
    spec->strikeout = 0;
    spec->fixed = 0;
    if (!lf)
        return;
    h = lf->height;
    if (h == 0)
        h = 16;
    spec->height = clampi(absi(h), 4, W32K_FONT_MAX_H);
    spec->weight = lf->weight == 0 ? 400 : lf->weight;
    spec->italic = lf->italic != 0;
    spec->underline = lf->underline != 0;
    spec->strikeout = lf->strikeout != 0;
    spec->fixed = (lf->pitch_family & 3u) == 1u;
}

void w32k_font_system_spec(w32k_fontspec *spec)
{
    if (!spec)
        return;
    spec->height = 16;
    spec->weight = 400;
    spec->italic = 0;
    spec->underline = 0;
    spec->strikeout = 0;
    spec->fixed = 1;
}

int32_t w32k_font_ascent(const w32k_fontspec *spec)
{
    return px_y(10, spec_height(spec));
}

int32_t w32k_font_advance(const w32k_fontspec *spec, uint16_t ch)
{
    int32_t h = spec_height(spec);
    int e;
    uint8_t adv_units;
    if (spec->fixed)
        return (h + 1) / 2;
    e = stroke_entry(ch);
    if (e < 0)
        e = stroke_entry('?');
    adv_units = g_advance[e];
    return px_x(adv_units, h) + 1;
}

void w32k_font_metrics(const w32k_fontspec *spec, w32k_tm *tm)
{
    int32_t h;
    int32_t ave = 0, max = 0;
    if (!tm)
        return;
    h = spec_height(spec);
    tm->height = h;
    tm->ascent = px_y(10, h);
    tm->descent = h - tm->ascent;
    tm->internal_leading = 0;
    tm->external_leading = 0;
    tm->weight = spec->weight;
    tm->italic = spec->italic != 0;
    tm->underlined = spec->underline != 0;
    tm->struckout = spec->strikeout != 0;
    tm->pitch_fixed = spec->fixed != 0;
    tm->first_char = 32;
    tm->last_char = 126;
    tm->default_char = '?';
    tm->break_char = 32;
    if (spec->fixed) {
        ave = max = (h + 1) / 2;
    } else {
        int32_t sum = 0;
        for (uint16_t c = 97; c <= 122; ++c)
            sum += w32k_font_advance(spec, c);
        ave = (sum + 13) / 26;
        for (uint16_t c = 32; c <= 126; ++c) {
            int32_t a = w32k_font_advance(spec, c);
            if (a > max)
                max = a;
        }
    }
    tm->ave_char_width = ave;
    tm->max_char_width = max;
}

int32_t w32k_font_text_width(const w32k_fontspec *spec, const uint16_t *s, int32_t len)
{
    int32_t w = 0;
    if (!spec || !s || len < 0)
        return 0;
    for (int32_t i = 0; i < len; ++i)
        w += w32k_font_advance(spec, s[i]);
    return w;
}

/* ---- Glyph rasterisation -------------------------------------------------------- */

int32_t w32k_font_glyph(const w32k_fontspec *spec, uint16_t ch, uint8_t *cov, int32_t stride)
{
    int32_t h, adv, asc, bw, ox = 0, cw, i;
    int e;
    size_t p;
    int lower;
    if (!spec || !cov)
        return 0;
    h = spec_height(spec);
    adv = w32k_font_advance(spec, ch);
    if (adv <= 0 || stride < adv)
        return 0;
    for (i = 0; i < h; ++i)
        for (int32_t j = 0; j < adv; ++j)
            cov[i * stride + j] = 0;
    asc = px_y(10, h);
    bw = stroke_width(h, spec->weight);
    cw = adv;
    e = stroke_entry(ch);
    if (e < 0)
        e = stroke_entry('?');
    if (spec->fixed) {
        /* Centre the glyph in the fixed cell using its design advance. */
        int32_t design = px_x(g_advance[e], h) + 1;
        ox = (adv - design) / 2;
        if (ox < 0)
            ox = 0;
    }
    lower = is_lower(ch);
    p = entry_start(e);
    {
        int pen_down = 0;
        int32_t px0 = 0, py0 = 0;
        while (p + 1 < sizeof(g_strokes)) {
            int8_t xv = g_strokes[p];
            int8_t yv;
            int32_t X, Y;
            if (xv == END)
                break;
            yv = g_strokes[p + 1];
            p += 2;
            if (xv == PU) {
                pen_down = 0;
                continue;
            }
            X = px_x(xv, h) + ox;
            Y = px_y(lower ? lower_y(yv) : yv, h);
            if (spec->italic)
                X += (asc - Y) / 4;
            if (pen_down)
                draw_line(cov, stride, cw, h, px0, py0, X, Y, bw);
            else
                stamp(cov, stride, cw, h, X, Y, bw);
            px0 = X;
            py0 = Y;
            pen_down = 1;
        }
    }
    if (spec->underline) {
        int32_t uy = asc + 2;
        if (uy > h - 1)
            uy = h - 1;
        draw_line(cov, stride, cw, h, 0, uy, adv - 1, uy, 1);
    }
    if (spec->strikeout) {
        int32_t sy = (asc * 5) / 8;
        draw_line(cov, stride, cw, h, 0, sy, adv - 1, sy, 1);
    }
    return adv;
}
