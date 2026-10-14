/*
 * test_k32_str_pure.c - host tests for src/ntdll/k32_str_pure.h: UTF-8
 * decoding and encoding, the Windows-1252 high table, surrogate pairs, the
 * invalid-sequence flags, buffer sizing and the default character.
 */
#include "minitest.h"
#include "k32_str_pure.h"

#include <string.h>

/* Multibyte to wide with the caller's buffer; returns the error and the count. */
static int to_wide(unsigned int cp, unsigned long flags, const char *s, size_t n,
                   WCHAR *dst, size_t cap, size_t *count)
{
    return k32str_mb_to_wc(cp, flags, (const unsigned char *)s, n, dst, cap, count);
}

static int to_narrow(unsigned int cp, unsigned long flags, const WCHAR *s, size_t n,
                     unsigned char *dst, size_t cap, unsigned char def, size_t *count, int *used)
{
    return k32str_wc_to_mb(cp, flags, s, n, dst, cap, def, count, used);
}

/* A 1-, 2-, 3- and 4-byte sequence each decode to their code point. */
static void utf8_decode_lengths(void)
{
    unsigned long cp = 0;
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"A", 1, &cp), 1);
    MT_EXPECT_EQ(cp, 0x41);
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\xC3\xA9", 2, &cp), 2);
    MT_EXPECT_EQ(cp, 0xE9);
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\xE2\x82\xAC", 3, &cp), 3);
    MT_EXPECT_EQ(cp, 0x20AC);
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\xF0\x9F\x98\x80", 4, &cp), 4);
    MT_EXPECT_EQ(cp, 0x1F600);
}

/* Overlong forms, surrogates, values above U+10FFFF, bad continuation bytes
 * and truncated sequences are invalid. */
static void utf8_decode_rejects(void)
{
    unsigned long cp = 0;
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\xC0\xAF", 2, &cp), 0);
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\xE0\x80\xAF", 3, &cp), 0);
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\xED\xA0\x80", 3, &cp), 0);
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\xF4\x90\x80\x80", 4, &cp), 0);
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\xC3\x28", 2, &cp), 0);
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\xE2\x82", 2, &cp), 0);
    MT_EXPECT_EQ(k32str_utf8_decode((const unsigned char *)"\x80", 1, &cp), 0);
}

/* A code point round trips through the encoder. */
static void utf8_encode_round_trip(void)
{
    unsigned char b[4];
    unsigned long cps[4] = { 0x41, 0xE9, 0x20AC, 0x1F600 };
    size_t lens[4] = { 1, 2, 3, 4 };
    for (int i = 0; i < 4; ++i) {
        size_t n = k32str_utf8_encode(cps[i], b);
        MT_EXPECT_EQ(n, lens[i]);
        unsigned long back = 0;
        MT_EXPECT_EQ(k32str_utf8_decode(b, n, &back), n);
        MT_EXPECT_EQ(back, cps[i]);
    }
}

/* ASCII is one wide character each; "h\xC3\xA9" is two; the count includes
 * no terminator when the caller passed an explicit length. */
static void mb_to_wc_utf8_counts(void)
{
    size_t need = 0;
    MT_EXPECT_EQ(to_wide(K32STR_CP_UTF8, 0, "hi", 2, NULL, 0, &need), 0);
    MT_EXPECT_EQ(need, 2);
    MT_EXPECT_EQ(to_wide(K32STR_CP_UTF8, 0, "h\xC3\xA9", 3, NULL, 0, &need), 0);
    MT_EXPECT_EQ(need, 2);
    /* U+1F600 needs a surrogate pair. */
    MT_EXPECT_EQ(to_wide(K32STR_CP_UTF8, 0, "\xF0\x9F\x98\x80", 4, NULL, 0, &need), 0);
    MT_EXPECT_EQ(need, 2);
}

/* The decoded surrogate pair is D83D DE00 for U+1F600. */
static void mb_to_wc_surrogate_pair(void)
{
    WCHAR w[4];
    size_t need = 0;
    MT_EXPECT_EQ(to_wide(K32STR_CP_UTF8, 0, "\xF0\x9F\x98\x80", 4, w, sizeof(w) / sizeof(w[0]), &need), 0);
    MT_EXPECT_EQ(need, 2);
    MT_EXPECT_EQ(w[0], 0xD83D);
    MT_EXPECT_EQ(w[1], 0xDE00);
}

/* With MB_ERR_INVALID_CHARS an invalid byte is ERROR_NO_UNICODE_TRANSLATION;
 * without it the byte becomes U+FFFD. */
static void mb_to_wc_invalid_bytes(void)
{
    WCHAR w[4];
    size_t need = 0;
    MT_EXPECT_EQ(to_wide(K32STR_CP_UTF8, K32STR_MB_ERR_INVALID_CHARS, "\xC3\x28", 2, w, 4, &need),
                 K32STR_ERROR_NO_UNICODE_TRANSLATION);
    MT_EXPECT_EQ(to_wide(K32STR_CP_UTF8, 0, "\xC3\x28", 2, w, 4, &need), 0);
    MT_EXPECT_EQ(need, 2);
    MT_EXPECT_EQ(w[0], K32STR_REPLACEMENT);
    MT_EXPECT_EQ(w[1], 0x28);
}

/* CP_ACP is Windows-1252: 0x80 is the euro sign, 0xE9 is e-acute. */
static void mb_to_wc_1252(void)
{
    WCHAR w[2];
    size_t need = 0;
    MT_EXPECT_EQ(to_wide(K32STR_CP_ACP, 0, "\x80", 1, w, 2, &need), 0);
    MT_EXPECT_EQ(w[0], 0x20AC);
    MT_EXPECT_EQ(to_wide(K32STR_CP_ACP, 0, "\xE9", 1, w, 2, &need), 0);
    MT_EXPECT_EQ(w[0], 0xE9);
    MT_EXPECT_EQ(to_wide(K32STR_CP_ACP, 0, "\x9F", 1, w, 2, &need), 0);
    MT_EXPECT_EQ(w[0], 0x0178);
}

/* A buffer that is too small fails with ERROR_INSUFFICIENT_BUFFER and
 * reports the needed count. */
static void mb_to_wc_buffer_too_small(void)
{
    WCHAR w[1];
    size_t need = 0;
    MT_EXPECT_EQ(to_wide(K32STR_CP_UTF8, 0, "ab", 2, w, 1, &need), K32STR_ERROR_INSUFFICIENT_BUFFER);
    MT_EXPECT_EQ(need, 2);
}

/* An unknown code page is ERROR_INVALID_PARAMETER. */
static void unknown_code_page(void)
{
    WCHAR w[2];
    size_t need = 0;
    MT_EXPECT_EQ(to_wide(65000, 0, "a", 1, w, 2, &need), K32STR_ERROR_INVALID_PARAMETER);
    unsigned char b[2];
    int used = 0;
    MT_EXPECT_EQ(to_narrow(12345, 0, w, 1, b, 2, '?', &need, &used), K32STR_ERROR_INVALID_PARAMETER);
}

/* Wide to Windows-1252: euro sign back to 0x80, a character with no mapping
 * takes the default and sets used_default. */
static void wc_to_mb_1252_and_default(void)
{
    unsigned char b[4];
    size_t need = 0;
    int used = 0;
    WCHAR euro[1] = { 0x20AC };
    MT_EXPECT_EQ(to_narrow(K32STR_CP_ACP, 0, euro, 1, b, sizeof(b), '?', &need, &used), 0);
    MT_EXPECT_EQ(need, 1);
    MT_EXPECT_EQ(b[0], 0x80);
    MT_EXPECT_EQ(used, 0);
    WCHAR cjk[1] = { 0x4E2D };
    MT_EXPECT_EQ(to_narrow(K32STR_CP_ACP, 0, cjk, 1, b, sizeof(b), '#', &need, &used), 0);
    MT_EXPECT_EQ(b[0], '#');
    MT_EXPECT_EQ(used, 1);
}

/* Wide to UTF-8 with a surrogate pair gives the 4-byte form; a lone surrogate
 * is U+FFFD unless WC_ERR_INVALID_CHARS is set. */
static void wc_to_mb_utf8(void)
{
    unsigned char b[8];
    size_t need = 0;
    int used = 0;
    WCHAR pair[2] = { 0xD83D, 0xDE00 };
    MT_EXPECT_EQ(to_narrow(K32STR_CP_UTF8, 0, pair, 2, b, sizeof(b), '?', &need, &used), 0);
    MT_EXPECT_EQ(need, 4);
    MT_EXPECT_EQ(b[0], 0xF0);
    MT_EXPECT_EQ(b[3], 0x80);
    WCHAR lone[1] = { 0xD800 };
    MT_EXPECT_EQ(to_narrow(K32STR_CP_UTF8, 0, lone, 1, b, sizeof(b), '?', &need, &used), 0);
    MT_EXPECT_EQ(need, 3);
    MT_EXPECT_EQ(b[0], 0xEF);
    MT_EXPECT_EQ(b[1], 0xBF);
    MT_EXPECT_EQ(b[2], 0xBD);
    MT_EXPECT_EQ(to_narrow(K32STR_CP_UTF8, K32STR_WC_ERR_INVALID_CHARS, lone, 1, b, sizeof(b), '?', &need, &used),
                 K32STR_ERROR_NO_UNICODE_TRANSLATION);
}

/* Counting mode (no buffer) and the terminator: a -1 length is handled by the
 * caller; here the counts for a string without terminator. */
static void count_only_mode(void)
{
    size_t need = 0;
    int used = 0;
    WCHAR w[3] = { 'a', 'b', 'c' };
    MT_EXPECT_EQ(to_narrow(K32STR_CP_UTF8, 0, w, 3, NULL, 0, '?', &need, &used), 0);
    MT_EXPECT_EQ(need, 3);
    MT_EXPECT_EQ(to_narrow(K32STR_CP_ACP, 0, w, 3, NULL, 0, '?', &need, &used), 0);
    MT_EXPECT_EQ(need, 3);
}

static const mt_case_t cases[] = {
    { "utf8_decode_lengths", utf8_decode_lengths },
    { "utf8_decode_rejects", utf8_decode_rejects },
    { "utf8_encode_round_trip", utf8_encode_round_trip },
    { "mb_to_wc_utf8_counts", mb_to_wc_utf8_counts },
    { "mb_to_wc_surrogate_pair", mb_to_wc_surrogate_pair },
    { "mb_to_wc_invalid_bytes", mb_to_wc_invalid_bytes },
    { "mb_to_wc_1252", mb_to_wc_1252 },
    { "mb_to_wc_buffer_too_small", mb_to_wc_buffer_too_small },
    { "unknown_code_page", unknown_code_page },
    { "wc_to_mb_1252_and_default", wc_to_mb_1252_and_default },
    { "wc_to_mb_utf8", wc_to_mb_utf8 },
    { "count_only_mode", count_only_mode },
};

int main(void)
{
    return MT_RUN("k32_str_pure", cases);
}
