/*
 * test_k32_con_pure.c - tests for src/ntdll/k32_con_pure.h: the screen buffer state, cell access, scrolling, the title, the
 * Ctrl handler list and the key records.
 */
#include "minitest.h"
#include "k32_con_pure.h"

#include <stdlib.h>
#include <string.h>

static kcon *new_con(void)
{
    kcon *c = (kcon *)malloc(sizeof *c);
    uint16_t *ch = (uint16_t *)malloc(sizeof(uint16_t) * KCON_MAX_W * KCON_MAX_H);
    uint16_t *at = (uint16_t *)malloc(sizeof(uint16_t) * KCON_MAX_W * KCON_MAX_H);
    kcon_init(c, ch, at);
    return c;
}

static void free_con(kcon *c)
{
    free(c->chars);
    free(c->attrs);
    free(c);
}

static kcon_coord C(int x, int y)
{
    kcon_coord p;
    p.x = (int16_t)x;
    p.y = (int16_t)y;
    return p;
}

static kcon_rect R(int l, int t, int r, int b)
{
    kcon_rect q;
    q.left = (int16_t)l;
    q.top = (int16_t)t;
    q.right = (int16_t)r;
    q.bottom = (int16_t)b;
    return q;
}

static void defaults(void)
{
    kcon *c = new_con();
    kcon_info i;
    kcon_get_info(c, &i);
    MT_EXPECT_EQ(i.size.x, KCON_DEFAULT_W);
    MT_EXPECT_EQ(i.size.y, KCON_DEFAULT_H);
    MT_EXPECT_EQ(i.attr, 7);
    MT_EXPECT_EQ(i.window.right, KCON_WIN_W - 1);
    MT_EXPECT_EQ(i.window.bottom, KCON_WIN_H - 1);
    MT_EXPECT_EQ(i.max_window.x, KCON_MAX_WIN_W > KCON_DEFAULT_W ? KCON_DEFAULT_W : KCON_MAX_WIN_W);
    MT_EXPECT_EQ(i.max_window.y, KCON_MAX_WIN_H);
    free_con(c);
}

static void resizing_the_buffer(void)
{
    kcon *c = new_con();
    size_t d;
    uint16_t v;
    kcon_info i;
    MT_EXPECT_EQ(kcon_fill_chars(c, C(5, 5), 'Q', 1, &d), 0);
    MT_EXPECT_EQ(kcon_set_size(c, C(10, 10)), KCON_ERR_INVALID_PARAMETER); /* smaller than the 120x30 window */
    MT_EXPECT_EQ(kcon_set_size(c, C(130, 40)), 0);
    kcon_read_chars(c, C(5, 5), &v, 1, &d);
    MT_EXPECT_EQ(v, 'Q'); /* kept */
    MT_EXPECT_EQ(kcon_set_size(c, C(KCON_MAX_W + 1, 40)), KCON_ERR_INVALID_PARAMETER);
    MT_EXPECT_EQ(kcon_set_size(c, C(0, 40)), KCON_ERR_INVALID_PARAMETER);
    c->cursor = C(129, 39);
    MT_EXPECT_EQ(kcon_set_window(c, 1, R(0, 0, 119, 29)), 0);
    MT_EXPECT_EQ(kcon_set_size(c, C(120, 30)), 0); /* the cursor is pulled back in */
    kcon_get_info(c, &i);
    MT_EXPECT_EQ(i.cursor.x, 119);
    MT_EXPECT_EQ(i.cursor.y, 29);
    kcon_read_chars(c, C(5, 5), &v, 1, &d);
    MT_EXPECT_EQ(v, 'Q');
    /* cells that come back after a shrink are blank */
    MT_EXPECT_EQ(kcon_set_size(c, C(130, 40)), 0);
    kcon_read_chars(c, C(125, 35), &v, 1, &d);
    MT_EXPECT_EQ(v, ' ');
    free_con(c);
}

static void window_rules(void)
{
    kcon *c = new_con();
    kcon_info i;
    MT_EXPECT_EQ(kcon_set_window(c, 1, R(0, 0, 79, 24)), 0);
    MT_EXPECT_EQ(kcon_set_window(c, 1, R(0, 0, 200, 24)), KCON_ERR_INVALID_PARAMETER); /* beyond the buffer */
    MT_EXPECT_EQ(kcon_set_window(c, 1, R(10, 0, 5, 24)), KCON_ERR_INVALID_PARAMETER);  /* empty */
    MT_EXPECT_EQ(kcon_set_window(c, 0, R(0, 10, 0, 10)), 0);                          /* scroll down by ten */
    kcon_get_info(c, &i);
    MT_EXPECT_EQ(i.window.top, 10);
    MT_EXPECT_EQ(i.window.bottom, 34);
    MT_EXPECT_EQ(kcon_set_window(c, 0, R(0, -20, 0, -20)), KCON_ERR_INVALID_PARAMETER); /* above the top */
    free_con(c);
}

static void cursor_moves_window(void)
{
    kcon *c = new_con();
    kcon_info i;
    MT_EXPECT_EQ(kcon_set_cursor(c, C(0, 100)), 0);
    kcon_get_info(c, &i);
    MT_EXPECT_EQ(i.window.bottom, 100); /* the window follows the cursor */
    MT_EXPECT_EQ(i.window.top, 100 - (KCON_WIN_H - 1));
    MT_EXPECT_EQ(kcon_set_cursor(c, C(0, 0)), 0);
    kcon_get_info(c, &i);
    MT_EXPECT_EQ(i.window.top, 0);
    MT_EXPECT_EQ(kcon_set_cursor(c, C(-1, 0)), KCON_ERR_INVALID_PARAMETER);
    MT_EXPECT_EQ(kcon_set_cursor(c, C(0, KCON_DEFAULT_H)), KCON_ERR_INVALID_PARAMETER);
    MT_EXPECT_EQ(kcon_set_cursor_info(c, 0, 1), KCON_ERR_INVALID_PARAMETER);
    MT_EXPECT_EQ(kcon_set_cursor_info(c, 101, 1), KCON_ERR_INVALID_PARAMETER);
    MT_EXPECT_EQ(kcon_set_cursor_info(c, 50, 0), 0);
    MT_EXPECT_EQ(c->cursor_visible, 0);
    free_con(c);
}

static void cell_access_wraps_rows_and_stops_at_the_end(void)
{
    kcon *c = new_con();
    size_t d;
    uint16_t out[8], text[4] = {'a', 'b', 'c', 'd'};
    MT_EXPECT_EQ(kcon_write_chars(c, C(118, 0), text, 4, &d), 0);
    MT_EXPECT_EQ((long)d, 4L);
    MT_EXPECT_EQ(kcon_read_chars(c, C(118, 0), out, 4, &d), 0);
    MT_EXPECT(out[0] == 'a' && out[1] == 'b' && out[2] == 'c' && out[3] == 'd');
    MT_EXPECT_EQ(c->chars[kcon_cell(0, 1)], 'c'); /* wrapped to the next row */
    MT_EXPECT_EQ(kcon_fill_chars(c, C(KCON_DEFAULT_W - 1, KCON_DEFAULT_H - 1), 'z', 100, &d), 0);
    MT_EXPECT_EQ((long)d, 1L); /* only the last cell is left */
    MT_EXPECT_EQ(kcon_fill_chars(c, C(KCON_DEFAULT_W, 0), 'z', 1, &d), KCON_ERR_INVALID_PARAMETER);
    MT_EXPECT_EQ((long)d, 0L);
    MT_EXPECT_EQ(kcon_fill_attrs(c, C(2, 2), 0x1F, 3, &d), 0);
    MT_EXPECT_EQ(kcon_read_attrs(c, C(1, 2), out, 5, &d), 0);
    MT_EXPECT(out[0] == 7 && out[1] == 0x1F && out[2] == 0x1F && out[3] == 0x1F && out[4] == 7);
    free_con(c);
}

static void line(kcon *c, int y, const char *s)
{
    size_t d;
    uint16_t w[64];
    size_t n = strlen(s);
    for (size_t i = 0; i < n; ++i)
        w[i] = (unsigned char)s[i];
    kcon_write_chars(c, C(0, y), w, n, &d);
}

static void row_is(kcon *c, int y, const char *s, int *ok)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; ++i)
        if (c->chars[kcon_cell((int)i, y)] != (unsigned char)s[i])
            *ok = 0;
}

static void scrolling(void)
{
    kcon *c = new_con();
    int ok = 1;
    line(c, 0, "AAAA");
    line(c, 1, "BBBB");
    line(c, 2, "CCCC");
    line(c, 3, "DDDD");
    /* scroll rows 1..3 up by one: B,C,D move to rows 0..2, row 3 is filled */
    MT_EXPECT_EQ(kcon_scroll(c, R(0, 1, 3, 3), NULL, C(0, 0), '.', 9), 0);
    row_is(c, 0, "BBBB", &ok);
    row_is(c, 1, "CCCC", &ok);
    row_is(c, 2, "DDDD", &ok);
    row_is(c, 3, "....", &ok);
    MT_EXPECT(ok);
    MT_EXPECT_EQ(c->attrs[kcon_cell(0, 3)], 9);
    /* scroll down by one with overlap, in the other direction */
    ok = 1;
    MT_EXPECT_EQ(kcon_scroll(c, R(0, 0, 3, 2), NULL, C(0, 1), '_', 7), 0);
    row_is(c, 0, "____", &ok);
    row_is(c, 1, "BBBB", &ok);
    row_is(c, 2, "CCCC", &ok);
    row_is(c, 3, "DDDD", &ok);
    MT_EXPECT(ok);
    /* sideways, with overlap */
    ok = 1;
    line(c, 5, "0123456");
    MT_EXPECT_EQ(kcon_scroll(c, R(0, 5, 4, 5), NULL, C(2, 5), ' ', 7), 0);
    row_is(c, 5, "  01234", &ok);
    MT_EXPECT(ok);
    /* a clip rectangle limits both the source and the destination */
    ok = 1;
    line(c, 6, "abcdef");
    kcon_rect clip = R(0, 6, 3, 6);
    MT_EXPECT_EQ(kcon_scroll(c, R(0, 6, 5, 6), &clip, C(1, 6), '-', 7), 0);
    row_is(c, 6, "-abcef", &ok); /* e and f lie outside the clip: not moved, not filled */
    MT_EXPECT(ok);
    MT_EXPECT_EQ(kcon_scroll(c, R(3, 0, 1, 0), NULL, C(0, 0), ' ', 7), KCON_ERR_INVALID_PARAMETER);
    free_con(c);
}

static void title(void)
{
    kcon *c = new_con();
    uint16_t t[6] = {'H', 'e', 'l', 'l', 'o', 0}, out[16];
    kcon_set_title(c, t, 5);
    MT_EXPECT_EQ((long)kcon_get_title(c, out, 16), 5L);
    MT_EXPECT_EQ(out[5], 0);
    MT_EXPECT_EQ((long)kcon_get_title(c, out, 3), 2L); /* cut, still terminated */
    MT_EXPECT(out[0] == 'H' && out[1] == 'e' && out[2] == 0);
    MT_EXPECT_EQ((long)kcon_get_title(c, out, 0), 0L);
    free_con(c);
}

static int g_calls[4], g_n;
static int h_a(uint32_t e) { g_calls[g_n++] = 1; return e == 2; }
static int h_b(uint32_t e) { g_calls[g_n++] = 2; return e == 1; }
static int h_c(uint32_t e) { (void)e; g_calls[g_n++] = 3; return 0; }

static void ctrl_handlers(void)
{
    kcon *c = new_con();
    g_n = 0;
    MT_EXPECT_EQ(kcon_dispatch(c, 0), 0); /* no handlers: default action */
    MT_EXPECT_EQ(kcon_set_handler(c, h_a, 1), 0);
    MT_EXPECT_EQ(kcon_set_handler(c, h_b, 1), 0);
    MT_EXPECT_EQ(kcon_set_handler(c, h_c, 1), 0);
    MT_EXPECT_EQ(kcon_dispatch(c, 1), 1); /* c (no), then b (yes) */
    MT_EXPECT(g_n == 2 && g_calls[0] == 3 && g_calls[1] == 2);
    g_n = 0;
    MT_EXPECT_EQ(kcon_dispatch(c, 2), 1); /* c, b, a */
    MT_EXPECT(g_n == 3 && g_calls[2] == 1);
    g_n = 0;
    MT_EXPECT_EQ(kcon_dispatch(c, 5), 0); /* nobody takes it */
    MT_EXPECT_EQ(g_n, 3);
    MT_EXPECT_EQ(kcon_set_handler(c, h_b, 0), 0);
    MT_EXPECT_EQ(kcon_set_handler(c, h_b, 0), KCON_ERR_INVALID_PARAMETER);
    g_n = 0;
    kcon_dispatch(c, 5);
    MT_EXPECT_EQ(g_n, 2);
    /* a NULL handler ignores Ctrl+C only */
    MT_EXPECT_EQ(kcon_set_handler(c, NULL, 1), 0);
    g_n = 0;
    MT_EXPECT_EQ(kcon_dispatch(c, 0), 1);
    MT_EXPECT_EQ(g_n, 0);
    MT_EXPECT_EQ(kcon_dispatch(c, 1), 0);
    MT_EXPECT_EQ(kcon_set_handler(c, NULL, 0), 0);
    free_con(c);
}

static void key_records(void)
{
    kcon *c = new_con();
    kcon_key_record r;
    MT_EXPECT_EQ((long)sizeof r, 20L);
    kcon_next_key(c, 1, 'a', &r);
    MT_EXPECT(r.event_type == 1 && r.key_down == 1 && r.vk == 'A' && r.ch == 'a' && r.ctrl_state == 0);
    kcon_next_key(c, 0, 0, &r); /* the matching key-up */
    MT_EXPECT(r.key_down == 0 && r.vk == 'A' && r.ch == 'a');
    kcon_next_key(c, 1, 'Q', &r);
    MT_EXPECT(r.vk == 'Q' && r.ch == 'Q' && (r.ctrl_state & KCON_SHIFT_PRESSED));
    kcon_next_key(c, 0, 0, &r);
    kcon_next_key(c, 1, '\n', &r);
    MT_EXPECT(r.vk == 0x0D && r.ch == '\r');
    kcon_next_key(c, 0, 0, &r);
    kcon_next_key(c, 1, 0x7F, &r);
    MT_EXPECT(r.vk == 8 && r.ch == 8);
    free_con(c);
}

static void char_info_rectangles(void)
{
    kcon *c = new_con();
    kcon_char_info src[6], back[6];
    kcon_rect rg;
    int i;
    for (i = 0; i < 6; ++i) {
        src[i].ch = (uint16_t)('a' + i);
        src[i].attr = (uint16_t)(0x10 + i);
        back[i].ch = 0;
        back[i].attr = 0;
    }
    /* a 3x2 array written at (2,1): the region is 3 wide, 2 high */
    rg = R(2, 1, 4, 2);
    MT_EXPECT_EQ(kcon_write_rect(c, src, C(3, 2), C(0, 0), &rg), 0);
    MT_EXPECT_EQ(c->chars[kcon_cell(2, 1)], 'a');
    MT_EXPECT_EQ(c->chars[kcon_cell(4, 2)], 'f');
    MT_EXPECT_EQ(c->attrs[kcon_cell(3, 2)], 0x14);
    rg = R(2, 1, 4, 2);
    MT_EXPECT_EQ(kcon_read_rect(c, back, C(3, 2), C(0, 0), &rg), 0);
    MT_EXPECT(back[0].ch == 'a' && back[5].ch == 'f' && back[3].attr == 0x13);
    /* a starting corner inside the array shifts what is taken */
    rg = R(10, 10, 11, 10);
    MT_EXPECT_EQ(kcon_write_rect(c, src, C(3, 2), C(1, 1), &rg), 0);
    MT_EXPECT_EQ(c->chars[kcon_cell(10, 10)], 'e'); /* element (1,1) */
    MT_EXPECT_EQ(c->chars[kcon_cell(11, 10)], 'f');
    /* the region is cut where the array ends */
    rg = R(0, 0, 9, 9);
    MT_EXPECT_EQ(kcon_write_rect(c, src, C(3, 2), C(0, 0), &rg), 0);
    MT_EXPECT_EQ(rg.right, 2);
    MT_EXPECT_EQ(rg.bottom, 1);
    /* a region partly off the buffer: the visible part is written and the array offset follows */
    rg = R(-1, 0, 1, 0);
    MT_EXPECT_EQ(kcon_write_rect(c, src, C(3, 2), C(0, 0), &rg), 0);
    MT_EXPECT_EQ(rg.left, 0);
    MT_EXPECT_EQ(c->chars[kcon_cell(0, 0)], 'b'); /* the second element */
    /* nothing visible */
    rg = R(500, 0, 501, 0);
    MT_EXPECT_EQ(kcon_write_rect(c, src, C(3, 2), C(0, 0), &rg), KCON_ERR_INVALID_PARAMETER);
    MT_EXPECT_EQ(kcon_write_rect(c, src, C(3, 2), C(3, 0), &rg), KCON_ERR_INVALID_PARAMETER);
    free_con(c);
}

static const mt_case_t cases[] = {
    MT_CASE(defaults),     MT_CASE(resizing_the_buffer), MT_CASE(window_rules),
    MT_CASE(cursor_moves_window), MT_CASE(cell_access_wraps_rows_and_stops_at_the_end), MT_CASE(scrolling),
    MT_CASE(title),        MT_CASE(ctrl_handlers),       MT_CASE(key_records), MT_CASE(char_info_rectangles),
};

int main(void)
{
    return MT_RUN("k32_con_pure", cases);
}
