/* test_w32k_font.c - the stroke font engine (w32k_font.c): LOGFONT to spec, metrics,
 * advances, glyph coverage in the cell, weight, italic, underline and strikeout, and the
 * lowercase rule. Pure: no model state is needed. */
#include "minitest.h"
#include "w32k_font.h"

#include <string.h>

#define STRIDE 160

static uint8_t cov[W32K_FONT_MAX_H * STRIDE];

static w32k_fontspec spec_h(int32_t h, int fixed)
{
    w32k_fontspec s;
    s.height = h;
    s.weight = 400;
    s.italic = 0;
    s.underline = 0;
    s.strikeout = 0;
    s.fixed = fixed;
    return s;
}

static int32_t count_cov(int32_t h, int32_t adv)
{
    int32_t n = 0;
    for (int32_t y = 0; y < h; ++y)
        for (int32_t x = 0; x < adv; ++x)
            n += cov[y * STRIDE + x] != 0;
    return n;
}

static int32_t row_cov(int32_t y, int32_t adv)
{
    int32_t n = 0;
    for (int32_t x = 0; x < adv; ++x)
        n += cov[y * STRIDE + x] != 0;
    return n;
}

/* Mean x of covered pixels in rows [y0, y1). Used to check italic slant. */
static int32_t mean_x(int32_t y0, int32_t y1, int32_t adv)
{
    int32_t sum = 0, n = 0;
    for (int32_t y = y0; y < y1; ++y)
        for (int32_t x = 0; x < adv; ++x)
            if (cov[y * STRIDE + x]) {
                sum += x;
                ++n;
            }
    return n ? sum / n : -1;
}

/* ---- Spec and LOGFONT ----------------------------------------------------------- */

static void spec_from_logfont_rules(void)
{
    w32k_logfont lf;
    w32k_fontspec s;
    memset(&lf, 0, sizeof(lf));
    w32k_font_spec_from_logfont(&lf, &s);
    MT_EXPECT_EQ(s.height, 16);          /* zero height: system size */
    MT_EXPECT_EQ(s.weight, 400);         /* zero weight: normal */
    MT_EXPECT_EQ(s.fixed, 0);
    lf.height = -12;
    lf.weight = 700;
    lf.italic = 1;
    lf.underline = 1;
    lf.strikeout = 1;
    w32k_font_spec_from_logfont(&lf, &s);
    MT_EXPECT_EQ(s.height, 12);          /* negative height: em size, magnitude used */
    MT_EXPECT_EQ(s.weight, 700);
    MT_EXPECT_EQ(s.italic, 1);
    MT_EXPECT_EQ(s.underline, 1);
    MT_EXPECT_EQ(s.strikeout, 1);
    lf.height = 9999;
    lf.pitch_family = 1; /* FIXED_PITCH */
    w32k_font_spec_from_logfont(&lf, &s);
    MT_EXPECT_EQ(s.height, W32K_FONT_MAX_H);
    MT_EXPECT_EQ(s.fixed, 1);
    lf.pitch_family = 2; /* VARIABLE_PITCH */
    w32k_font_spec_from_logfont(&lf, &s);
    MT_EXPECT_EQ(s.fixed, 0);
}

static void system_spec_is_8x16(void)
{
    w32k_fontspec s;
    w32k_tm tm;
    w32k_font_system_spec(&s);
    w32k_font_metrics(&s, &tm);
    MT_EXPECT_EQ(tm.height, 16);
    MT_EXPECT_EQ(tm.ave_char_width, 8);
    MT_EXPECT_EQ(tm.max_char_width, 8);
    MT_EXPECT_EQ(w32k_font_advance(&s, 'A'), 8);
    MT_EXPECT_EQ(w32k_font_advance(&s, 'i'), 8);
    MT_EXPECT_EQ(w32k_font_advance(&s, 0x4E2D), 8);
}

static void metrics_values(void)
{
    w32k_fontspec s = spec_h(16, 1);
    w32k_tm tm;
    w32k_font_metrics(&s, &tm);
    MT_EXPECT_EQ(tm.ascent, 11);
    MT_EXPECT_EQ(tm.descent, 5);
    MT_EXPECT_EQ(tm.ascent + tm.descent, 16);
    MT_EXPECT_EQ(tm.first_char, 32);
    MT_EXPECT_EQ(tm.last_char, 126);
    MT_EXPECT_EQ(tm.break_char, 32);
    MT_EXPECT_EQ(tm.default_char, '?');
    MT_EXPECT_EQ(tm.weight, 400);
    MT_EXPECT_EQ(w32k_font_ascent(&s), 11);

    s = spec_h(32, 0);
    w32k_font_metrics(&s, &tm);
    MT_EXPECT_EQ(tm.height, 32);
    // Baseline row is (10*32+7)/14 = 23; the descent is the rest of the 32-row cell.
    MT_EXPECT_EQ(tm.ascent, 23);
    MT_EXPECT_EQ(tm.descent, 9);
    MT_EXPECT(tm.max_char_width > tm.ave_char_width);
    MT_EXPECT(tm.ave_char_width > 0);
}

static void proportional_advances(void)
{
    w32k_fontspec s = spec_h(16, 0);
    int32_t i_adv = w32k_font_advance(&s, 'I');
    int32_t m_adv = w32k_font_advance(&s, 'M');
    int32_t sp_adv = w32k_font_advance(&s, ' ');
    MT_EXPECT(i_adv < m_adv);
    MT_EXPECT(sp_adv > 0);
    MT_EXPECT(i_adv >= 1);
    /* Unmapped characters use the default character's advance. */
    MT_EXPECT_EQ(w32k_font_advance(&s, 0x4E2D), w32k_font_advance(&s, '?'));
    /* Lowercase advances equal the capital's. */
    MT_EXPECT_EQ(w32k_font_advance(&s, 'a'), w32k_font_advance(&s, 'A'));
}

static void text_width_sums(void)
{
    w32k_fontspec s = spec_h(16, 1);
    static const uint16_t txt[] = {'A', 'B', 'C'};
    MT_EXPECT_EQ(w32k_font_text_width(&s, txt, 3), 24);
    MT_EXPECT_EQ(w32k_font_text_width(&s, txt, 0), 0);
    MT_EXPECT_EQ(w32k_font_text_width(&s, NULL, 3), 0);
}

/* ---- Coverage ------------------------------------------------------------------- */

static void every_printable_glyph_draws(void)
{
    w32k_fontspec s = spec_h(16, 1);
    int32_t adv;
    for (uint16_t c = 33; c <= 126; ++c) {
        memset(cov, 0, sizeof(cov));
        adv = w32k_font_glyph(&s, c, cov, STRIDE);
        MT_ASSERT(adv == 8);
        if (count_cov(16, adv) == 0) {
            printf("    no coverage for code %u\n", (unsigned)c);
            MT_EXPECT(0);
        }
    }
    memset(cov, 0, sizeof(cov));
    MT_EXPECT_EQ(w32k_font_glyph(&s, ' ', cov, STRIDE), 8);
    MT_EXPECT_EQ(count_cov(16, 8), 0);
}

static void glyphs_stay_in_cell(void)
{
    static const int32_t heights[] = {8, 16, 24, 32, 64};
    for (unsigned hi = 0; hi < sizeof(heights) / sizeof(heights[0]); ++hi) {
        for (int fixed = 0; fixed < 2; ++fixed) {
            w32k_fontspec s = spec_h(heights[hi], fixed);
            for (uint16_t c = 32; c <= 126; ++c) {
                int32_t adv = w32k_font_advance(&s, c);
                /* Fill the whole stride with a sentinel: nothing outside adv x h changes. */
                memset(cov, 0x5A, sizeof(cov));
                MT_EXPECT_EQ(w32k_font_glyph(&s, c, cov, STRIDE), adv);
                for (int32_t y = 0; y < heights[hi]; ++y) {
                    for (int32_t x = adv; x < STRIDE; ++x) {
                        if (cov[y * STRIDE + x] != 0x5A) {
                            printf("    code %u wrote outside cell at %d,%d\n", (unsigned)c, (int)x, (int)y);
                            MT_EXPECT(0);
                            return;
                        }
                    }
                }
                for (int32_t y = heights[hi]; y < W32K_FONT_MAX_H; ++y)
                    MT_EXPECT_EQ(cov[y * STRIDE], 0x5A);
            }
        }
    }
}

static void stride_too_small_rejected(void)
{
    w32k_fontspec s = spec_h(16, 1);
    MT_EXPECT_EQ(w32k_font_glyph(&s, 'A', cov, 4), 0);
    MT_EXPECT_EQ(w32k_font_glyph(&s, 'A', NULL, STRIDE), 0);
}

/* The vertical stroke of 'I' is in the middle column: centre of the fixed cell. */
static void capital_i_shape(void)
{
    w32k_fontspec s = spec_h(16, 1);
    int32_t adv;
    memset(cov, 0, sizeof(cov));
    adv = w32k_font_glyph(&s, 'I', cov, STRIDE);
    MT_EXPECT_EQ(adv, 8);
    /* Serifs top and bottom at the cap line and the baseline. */
    MT_EXPECT(row_cov(0, adv) > 0);
    MT_EXPECT(row_cov(10, adv) > 0);
    /* Middle rows have one stroke, near the centre, and nothing at the sides. */
    MT_EXPECT(cov[6 * STRIDE + 3] || cov[6 * STRIDE + 4]);
    MT_EXPECT_EQ(cov[6 * STRIDE + 0], 0);
    MT_EXPECT_EQ(cov[6 * STRIDE + 7], 0);
}

static void bold_is_heavier(void)
{
    w32k_fontspec n = spec_h(16, 0), b = spec_h(16, 0);
    int32_t normal, bold;
    b.weight = 700;
    memset(cov, 0, sizeof(cov));
    w32k_font_glyph(&n, 'O', cov, STRIDE);
    normal = count_cov(16, w32k_font_advance(&n, 'O'));
    memset(cov, 0, sizeof(cov));
    w32k_font_glyph(&b, 'O', cov, STRIDE);
    bold = count_cov(16, w32k_font_advance(&b, 'O'));
    MT_EXPECT(bold > normal);
    MT_EXPECT(normal > 0);
}

static void italic_slants_right_at_top(void)
{
    w32k_fontspec s = spec_h(32, 0);
    int32_t adv;
    s.italic = 1;
    memset(cov, 0, sizeof(cov));
    adv = w32k_font_glyph(&s, 'I', cov, STRIDE);
    int32_t top = mean_x(0, 10, adv);
    int32_t bottom = mean_x(22, 30, adv);
    MT_ASSERT(top >= 0 && bottom >= 0);
    MT_EXPECT(top > bottom);

    w32k_fontspec u = spec_h(32, 0);
    memset(cov, 0, sizeof(cov));
    adv = w32k_font_glyph(&u, 'I', cov, STRIDE);
    /* Upright: the same rows have the same mean x within one pixel. */
    int32_t utop = mean_x(0, 10, adv), ubot = mean_x(22, 30, adv);
    MT_EXPECT(utop - ubot <= 1 && ubot - utop <= 1);
}

static void underline_and_strikeout(void)
{
    w32k_fontspec s = spec_h(16, 1);
    int32_t adv;
    memset(cov, 0, sizeof(cov));
    adv = w32k_font_glyph(&s, '-', cov, STRIDE);
    int32_t plain_row13 = row_cov(13, adv);
    MT_EXPECT_EQ(plain_row13, 0);
    s.underline = 1;
    memset(cov, 0, sizeof(cov));
    adv = w32k_font_glyph(&s, '-', cov, STRIDE);
    MT_EXPECT_EQ(row_cov(13, adv), 8); /* asc+2 = 13 across the whole cell */
    s.underline = 0;
    s.strikeout = 1;
    memset(cov, 0, sizeof(cov));
    adv = w32k_font_glyph(&s, ' ', cov, STRIDE);
    MT_EXPECT_EQ(row_cov(6, adv), 8); /* asc*5/8 = 6 */
}

static void lowercase_is_reduced_capital(void)
{
    w32k_fontspec s = spec_h(16, 1);
    int32_t adv;
    /* Lowercase x sits in the x-height band: rows 0..2 stay empty. */
    memset(cov, 0, sizeof(cov));
    adv = w32k_font_glyph(&s, 'x', cov, STRIDE);
    MT_EXPECT_EQ(row_cov(0, adv), 0);
    MT_EXPECT_EQ(row_cov(1, adv), 0);
    MT_EXPECT_EQ(row_cov(2, adv), 0);
    MT_EXPECT(count_cov(16, adv) > 0);
    /* The capital X reaches the cap line. */
    memset(cov, 0, sizeof(cov));
    w32k_font_glyph(&s, 'X', cov, STRIDE);
    MT_EXPECT(row_cov(0, adv) > 0 || row_cov(1, adv) > 0);
}

static void heights_scale_strokes(void)
{
    w32k_fontspec small = spec_h(12, 1), big = spec_h(48, 1);
    int32_t a = w32k_font_advance(&small, 'O'), b = w32k_font_advance(&big, 'O');
    memset(cov, 0, sizeof(cov));
    w32k_font_glyph(&small, 'O', cov, STRIDE);
    int32_t ns = count_cov(12, a);
    memset(cov, 0, sizeof(cov));
    w32k_font_glyph(&big, 'O', cov, STRIDE);
    int32_t nb = count_cov(48, b);
    MT_EXPECT(nb > ns * 4 - 8);
    MT_EXPECT(b > a);
}

static void deterministic_rendering(void)
{
    static uint8_t a[W32K_FONT_MAX_H * STRIDE];
    w32k_fontspec s = spec_h(20, 0);
    s.italic = 1;
    s.weight = 700;
    memset(cov, 0, sizeof(cov));
    w32k_font_glyph(&s, 'g', cov, STRIDE);
    memcpy(a, cov, sizeof(a));
    memset(cov, 0, sizeof(cov));
    w32k_font_glyph(&s, 'g', cov, STRIDE);
    MT_EXPECT(memcmp(a, cov, sizeof(a)) == 0);
}

static const mt_case_t cases[] = {
    MT_CASE(spec_from_logfont_rules),
    MT_CASE(system_spec_is_8x16),
    MT_CASE(metrics_values),
    MT_CASE(proportional_advances),
    MT_CASE(text_width_sums),
    MT_CASE(every_printable_glyph_draws),
    MT_CASE(glyphs_stay_in_cell),
    MT_CASE(stride_too_small_rejected),
    MT_CASE(capital_i_shape),
    MT_CASE(bold_is_heavier),
    MT_CASE(italic_slants_right_at_top),
    MT_CASE(underline_and_strikeout),
    MT_CASE(lowercase_is_reduced_capital),
    MT_CASE(heights_scale_strokes),
    MT_CASE(deterministic_rendering),
};

int main(void)
{
    return MT_RUN("w32k_font", cases);
}
