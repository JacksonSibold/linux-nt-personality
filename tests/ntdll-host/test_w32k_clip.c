/* test_w32k_clip.c - clipboard text conversions (w32k_clip.c): the Windows-1252 table,
 * terminators and capacity, surrogate pairs, the OEM rule, and the model's synthesised
 * CF_TEXT / CF_OEMTEXT / CF_UNICODETEXT formats. */
#include "minitest.h"
#include "w32k.h"
#include "win32k/w32k_clip.h"

#include <string.h>

/* ---- Code page table ----------------------------------------------------------- */

static void cp1252_bytes_to_wide(void)
{
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0x41), 0x41);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0x7F), 0x7F);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0x80), 0x20AC);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0x82), 0x201A);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0x93), 0x201C);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0x9F), 0x0178);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0x81), 0xFFFD);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0x9D), 0xFFFD);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0xA0), 0xA0);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0xE9), 0xE9);
    MT_EXPECT_EQ(w32k_clip_cp1252_to_wide(0xFF), 0xFF);
}

static void wide_to_cp1252_units(void)
{
    MT_EXPECT_EQ(w32k_clip_wide_to_cp1252(0x41), 0x41);
    MT_EXPECT_EQ(w32k_clip_wide_to_cp1252(0xE9), 0xE9);
    MT_EXPECT_EQ(w32k_clip_wide_to_cp1252(0x20AC), 0x80);
    MT_EXPECT_EQ(w32k_clip_wide_to_cp1252(0x0178), 0x9F);
    MT_EXPECT_EQ(w32k_clip_wide_to_cp1252(0x4E2D), -1);
    MT_EXPECT_EQ(w32k_clip_wide_to_cp1252(0xFFFD), -1);
    MT_EXPECT_EQ(w32k_clip_wide_to_cp1252(0x0100), -1);
}

/* Every 1252 byte round-trips through UTF-16 except the five undefined ones. */
static void cp1252_round_trip_all(void)
{
    for (unsigned b = 0; b < 256; ++b) {
        uint16_t u = w32k_clip_cp1252_to_wide((uint8_t)b);
        if (u == 0xFFFD && b >= 0x80 && b <= 0x9F) {
            MT_EXPECT_EQ(w32k_clip_wide_to_cp1252(u), -1);
            continue;
        }
        MT_EXPECT_EQ(w32k_clip_wide_to_cp1252(u), (int)b);
    }
}

/* ---- Text to wide ------------------------------------------------------------- */

static void text_to_wide_basic(void)
{
    static const uint8_t src[] = {'H', 'i', 0x80};
    uint16_t dst[8];
    size_t n = w32k_clip_text_to_wide(src, sizeof(src), dst, 8);
    MT_EXPECT_EQ(n, 4);
    MT_EXPECT_EQ(dst[0], 'H');
    MT_EXPECT_EQ(dst[1], 'i');
    MT_EXPECT_EQ(dst[2], 0x20AC);
    MT_EXPECT_EQ(dst[3], 0);
}

static void text_to_wide_stops_at_nul(void)
{
    static const uint8_t src[] = {'a', 'b', 0, 'c', 'd'};
    uint16_t dst[8];
    MT_EXPECT_EQ(w32k_clip_text_to_wide(src, sizeof(src), dst, 8), 3);
    MT_EXPECT_EQ(dst[2], 0);
}

static void text_to_wide_capacity(void)
{
    static const uint8_t src[] = {'a', 'b', 'c'};
    uint16_t dst[8];
    /* Three characters need four units with the terminator. */
    MT_EXPECT_EQ(w32k_clip_text_to_wide(src, 3, dst, 3), 0);
    MT_EXPECT_EQ(w32k_clip_text_to_wide(src, 3, dst, 4), 4);
    MT_EXPECT_EQ(w32k_clip_text_to_wide(src, 0, dst, 1), 1);
    MT_EXPECT_EQ(dst[0], 0);
}

/* ---- Wide to text ------------------------------------------------------------- */

static void wide_to_text_basic(void)
{
    static const uint16_t src[] = {0x20AC, 'x', 0};
    uint8_t dst[8];
    size_t n = w32k_clip_wide_to_text(src, 3, dst, sizeof(dst), 0);
    MT_EXPECT_EQ(n, 3);
    MT_EXPECT_EQ(dst[0], 0x80);
    MT_EXPECT_EQ(dst[1], 'x');
    MT_EXPECT_EQ(dst[2], 0);
}

static void wide_to_text_unit_bound(void)
{
    /* Without a NUL in the source, the unit count bounds the conversion. */
    static const uint16_t src[] = {'a', 'b', 'c'};
    uint8_t dst[8];
    MT_EXPECT_EQ(w32k_clip_wide_to_text(src, 2, dst, sizeof(dst), 0), 3);
    MT_EXPECT_EQ(dst[1], 'b');
    MT_EXPECT_EQ(dst[2], 0);
}

static void wide_to_text_capacity(void)
{
    static const uint16_t src[] = {'a', 'b', 0};
    uint8_t dst[8];
    MT_EXPECT_EQ(w32k_clip_wide_to_text(src, 3, dst, 2, 0), 0); /* needs 3 bytes */
    MT_EXPECT_EQ(w32k_clip_wide_to_text(src, 3, dst, 3, 0), 3);
}

static void wide_to_text_unmappable_is_question(void)
{
    static const uint16_t src[] = {0x4E2D, 0xFFFD, 'z', 0};
    uint8_t dst[8];
    size_t n = w32k_clip_wide_to_text(src, 4, dst, sizeof(dst), 0);
    MT_EXPECT_EQ(n, 4);
    MT_EXPECT_EQ(dst[0], '?');
    MT_EXPECT_EQ(dst[1], '?');
    MT_EXPECT_EQ(dst[2], 'z');
}

static void wide_to_text_surrogate_pair(void)
{
    /* U+1F600 is the pair D83D DE00: one character, one '?'. */
    static const uint16_t src[] = {0xD83D, 0xDE00, 'a', 0};
    uint8_t dst[8];
    size_t n = w32k_clip_wide_to_text(src, 4, dst, sizeof(dst), 0);
    MT_EXPECT_EQ(n, 3);
    MT_EXPECT_EQ(dst[0], '?');
    MT_EXPECT_EQ(dst[1], 'a');
}

static void wide_to_text_lone_surrogates(void)
{
    static const uint16_t src[] = {0xDE00, 'b', 0xD83D, 0};
    uint8_t dst[8];
    size_t n = w32k_clip_wide_to_text(src, 4, dst, sizeof(dst), 0);
    MT_EXPECT_EQ(n, 4);
    MT_EXPECT_EQ(dst[0], '?');
    MT_EXPECT_EQ(dst[1], 'b');
    MT_EXPECT_EQ(dst[2], '?');
}

static void wide_to_oem_is_ascii_only(void)
{
    static const uint16_t src[] = {0x00E9, 'A', 0x20AC, 0};
    uint8_t dst[8];
    size_t n = w32k_clip_wide_to_text(src, 4, dst, sizeof(dst), 1);
    MT_EXPECT_EQ(n, 4); /* three characters and the terminator */
    MT_EXPECT_EQ(dst[0], '?');
    MT_EXPECT_EQ(dst[1], 'A');
    MT_EXPECT_EQ(dst[2], '?');
}

static void wide_units_length(void)
{
    static const uint16_t s[] = {'a', 'b', 0, 'c'};
    MT_EXPECT_EQ(w32k_clip_wide_units(s, 4), 2);
    MT_EXPECT_EQ(w32k_clip_wide_units(s, 1), 1);
    MT_EXPECT_EQ(w32k_clip_wide_units(s, 0), 0);
}

/* ---- Model: synthesised formats ------------------------------------------------ */

static void put_wide(uint32_t fmt, const uint16_t *units, size_t n)
{
    MT_ASSERT(w32k_set_clipboard_data(fmt, units, n * 2));
}

static void model_synthesises_text_from_unicode(void)
{
    /* "h" U+00E9 NUL stored as CF_UNICODETEXT. */
    static const uint16_t uni[] = {'h', 0x00E9, 0};
    size_t n = 0;
    const uint8_t *p;
    w32k_init();
    MT_ASSERT(w32k_open_clipboard(0));
    MT_ASSERT(w32k_empty_clipboard());
    put_wide(W32K_CF_UNICODETEXT, uni, 3);
    MT_EXPECT(w32k_is_clipboard_format_available(W32K_CF_TEXT));
    MT_EXPECT(w32k_is_clipboard_format_available(W32K_CF_OEMTEXT));
    MT_EXPECT_EQ(w32k_count_clipboard_formats(), 1);
    p = w32k_get_clipboard_data(W32K_CF_TEXT, &n);
    MT_ASSERT(p != NULL);
    MT_EXPECT_EQ(n, 3);
    MT_EXPECT_EQ(p[0], 'h');
    MT_EXPECT_EQ(p[1], 0xE9);
    MT_EXPECT_EQ(p[2], 0);
    MT_EXPECT(w32k_close_clipboard());
}

static void model_synthesises_unicode_from_text(void)
{
    static const char txt[] = "a\x80" "b";
    size_t n = 0;
    const uint8_t *p;
    w32k_init();
    MT_ASSERT(w32k_open_clipboard(0));
    MT_ASSERT(w32k_empty_clipboard());
    MT_ASSERT(w32k_set_clipboard_data(W32K_CF_TEXT, txt, 4));
    MT_EXPECT(w32k_is_clipboard_format_available(W32K_CF_UNICODETEXT));
    p = w32k_get_clipboard_data(W32K_CF_UNICODETEXT, &n);
    MT_ASSERT(p != NULL);
    MT_EXPECT_EQ(n, 8); /* 'a', U+20AC, 'b', NUL as UTF-16 */
    MT_EXPECT_EQ(p[0], 'a');
    MT_EXPECT_EQ(p[1], 0);
    MT_EXPECT_EQ(p[2], 0xAC);
    MT_EXPECT_EQ(p[3], 0x20);
    MT_EXPECT_EQ(p[4], 'b');
    MT_EXPECT_EQ(p[6], 0);
    MT_EXPECT(w32k_close_clipboard());
}

static void model_stored_format_wins(void)
{
    static const uint16_t uni[] = {'u', 0};
    static const char ansi[] = "s";
    size_t n = 0;
    const uint8_t *p;
    w32k_init();
    MT_ASSERT(w32k_open_clipboard(0));
    MT_ASSERT(w32k_empty_clipboard());
    put_wide(W32K_CF_UNICODETEXT, uni, 2);
    MT_ASSERT(w32k_set_clipboard_data(W32K_CF_TEXT, ansi, 2));
    /* CF_TEXT was stored, so it is returned as stored, not converted. */
    p = w32k_get_clipboard_data(W32K_CF_TEXT, &n);
    MT_ASSERT(p != NULL);
    MT_EXPECT_EQ(n, 2);
    MT_EXPECT_EQ(p[0], 's');
    MT_EXPECT_EQ(w32k_count_clipboard_formats(), 2);
    MT_EXPECT(w32k_close_clipboard());
}

static void model_synthesis_needs_open(void)
{
    static const uint16_t uni[] = {'x', 0};
    w32k_init();
    MT_ASSERT(w32k_open_clipboard(0));
    put_wide(W32K_CF_UNICODETEXT, uni, 2);
    MT_EXPECT(w32k_close_clipboard());
    MT_EXPECT(w32k_get_clipboard_data(W32K_CF_TEXT, NULL) == NULL);
    MT_EXPECT_EQ(w32k_get_last_error(), 1418); /* ERROR_CLIPBOARD_NOT_OPEN */
}

static void model_no_source_no_synthesis(void)
{
    size_t n = 99;
    w32k_init();
    MT_ASSERT(w32k_open_clipboard(0));
    MT_ASSERT(w32k_empty_clipboard());
    MT_ASSERT(w32k_set_clipboard_data(W32K_CF_BITMAP, "bm", 2));
    MT_EXPECT(!w32k_is_clipboard_format_available(W32K_CF_TEXT));
    MT_EXPECT(w32k_get_clipboard_data(W32K_CF_TEXT, &n) == NULL);
    MT_EXPECT_EQ(n, 99);
    MT_EXPECT(w32k_close_clipboard());
}

static void model_oem_from_unicode(void)
{
    static const uint16_t uni[] = {0x00E9, 'k', 0};
    size_t n = 0;
    const uint8_t *p;
    w32k_init();
    MT_ASSERT(w32k_open_clipboard(0));
    MT_ASSERT(w32k_empty_clipboard());
    put_wide(W32K_CF_UNICODETEXT, uni, 3);
    p = w32k_get_clipboard_data(W32K_CF_OEMTEXT, &n);
    MT_ASSERT(p != NULL);
    MT_EXPECT_EQ(n, 3);
    MT_EXPECT_EQ(p[0], '?');
    MT_EXPECT_EQ(p[1], 'k');
    MT_EXPECT_EQ(p[2], 0);
    MT_EXPECT(w32k_close_clipboard());
}

static const mt_case_t cases[] = {
    MT_CASE(cp1252_bytes_to_wide),
    MT_CASE(wide_to_cp1252_units),
    MT_CASE(cp1252_round_trip_all),
    MT_CASE(text_to_wide_basic),
    MT_CASE(text_to_wide_stops_at_nul),
    MT_CASE(text_to_wide_capacity),
    MT_CASE(wide_to_text_basic),
    MT_CASE(wide_to_text_unit_bound),
    MT_CASE(wide_to_text_capacity),
    MT_CASE(wide_to_text_unmappable_is_question),
    MT_CASE(wide_to_text_surrogate_pair),
    MT_CASE(wide_to_text_lone_surrogates),
    MT_CASE(wide_to_oem_is_ascii_only),
    MT_CASE(wide_units_length),
    MT_CASE(model_synthesises_text_from_unicode),
    MT_CASE(model_synthesises_unicode_from_text),
    MT_CASE(model_stored_format_wins),
    MT_CASE(model_synthesis_needs_open),
    MT_CASE(model_no_source_no_synthesis),
    MT_CASE(model_oem_from_unicode),
};

int main(void)
{
    return MT_RUN("w32k_clip", cases);
}
