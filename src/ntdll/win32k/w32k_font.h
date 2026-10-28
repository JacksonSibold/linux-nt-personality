/* w32k_font.h - font specification, metrics and glyph rasterisation (see w32k_font.c). */
#ifndef W32K_FONT_H
#define W32K_FONT_H

#include <stddef.h>
#include <stdint.h>

/* Largest font height in pixels, and the largest glyph cell the rasteriser fills. */
#define W32K_FONT_MAX_H 128
#define W32K_FONT_MAX_CELL_W 160

/* The LOGFONTW layout (wingdi.h), field for field. The shim copies into this. */
typedef struct w32k_logfont {
    int32_t height;
    int32_t width;
    int32_t escapement;
    int32_t orientation;
    int32_t weight;
    uint8_t italic;
    uint8_t underline;
    uint8_t strikeout;
    uint8_t charset;
    uint8_t out_precision;
    uint8_t clip_precision;
    uint8_t quality;
    uint8_t pitch_family;
    uint16_t face[32];
} w32k_logfont;

/* What the rasteriser needs from a font. Built from a LOGFONT by w32k_font_spec_from_logfont. */
typedef struct w32k_fontspec {
    int32_t height;     /* cell height in pixels (the em height the strokes are fitted to) */
    int32_t weight;     /* FW_* value; 600 and above is drawn with a heavier stroke */
    int italic;
    int underline;
    int strikeout;
    int fixed;          /* fixed pitch: every glyph advances by the same amount */
} w32k_fontspec;

/* TEXTMETRICW fields the model reports (the rest are zero). */
typedef struct w32k_tm {
    int32_t height, ascent, descent, internal_leading, external_leading;
    int32_t ave_char_width, max_char_width;
    int32_t weight;
    uint8_t italic, underlined, struckout, pitch_fixed;
    uint8_t first_char, last_char, default_char, break_char;
} w32k_tm;

/* Converts a LOGFONT to a spec. A zero height gets the 16-pixel system size; a negative
 * height is a character height (em size), a positive one a cell height. Both take the
 * magnitude here. */
void w32k_font_spec_from_logfont(const w32k_logfont *lf, w32k_fontspec *spec);

/* The default 8x16 system font spec (fixed pitch, weight 400). */
void w32k_font_system_spec(w32k_fontspec *spec);

/* Ascent (baseline row from the top of the cell) for a spec. */
int32_t w32k_font_ascent(const w32k_fontspec *spec);

void w32k_font_metrics(const w32k_fontspec *spec, w32k_tm *tm);

/* Advance in pixels of character ch (a UTF-16 unit). Unmapped characters use the
 * default character '?'. */
int32_t w32k_font_advance(const w32k_fontspec *spec, uint16_t ch);

/* Rasterises ch into cov: row-major coverage bytes (0 or 255), stride `stride`, cell
 * height spec->height, width w32k_font_advance(). cov must hold stride*height bytes and
 * stride must be at least the advance. Returns the advance, or 0 for a bad spec. */
int32_t w32k_font_glyph(const w32k_fontspec *spec, uint16_t ch, uint8_t *cov, int32_t stride);

/* The text extent of len characters: the sum of advances, and the cell height. */
int32_t w32k_font_text_width(const w32k_fontspec *spec, const uint16_t *s, int32_t len);

#endif /* W32K_FONT_H */
