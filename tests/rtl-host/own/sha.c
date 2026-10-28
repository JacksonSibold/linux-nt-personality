/*
 * sha.c - host test for src/ntdll/rtl/sha.c: SHA-1 and SHA-256 against the examples of FIPS 180-4 and the NIST
 * example values, including the message lengths around the padding boundary (55, 56, 63, 64 bytes) and a long one.
 * Original code of this repository; uses the Wine test macros of include/wine/test.h.
 */
#include <stdlib.h>

#include "windef.h"
#include "winnt.h"
#include "wine/test.h"
#include "rtl_sha.h"

static void check(void (*fn)(const BYTE *, SIZE_T, BYTE *), unsigned int len, const char *msg, SIZE_T msglen, const char *want, const char *what)
{
    BYTE out[32];
    char hex[65];
    unsigned int i;

    fn((const BYTE *)msg, msglen, out);
    for (i = 0; i < len; i++) sprintf(hex + 2 * i, "%02x", out[i]);
    ok(!strcmp(hex, want), "%s of %lu bytes: got %s, expected %s\n", what, (unsigned long)msglen, hex, want);
}

static void test_known_vectors(void)
{
    static const char two_blocks[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

    check(rtl_sha1, 20, "", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1");
    check(rtl_sha1, 20, "abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1");
    check(rtl_sha1, 20, two_blocks, strlen(two_blocks), "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "sha1");
    check(rtl_sha256, 32, "", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256");
    check(rtl_sha256, 32, "abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256");
    check(rtl_sha256, 32, two_blocks, strlen(two_blocks), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256");
}

static void test_million_a(void)
{
    char *msg = malloc(1000000);

    memset(msg, 'a', 1000000);
    check(rtl_sha1, 20, msg, 1000000, "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "sha1");
    check(rtl_sha256, 32, msg, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256");
    free(msg);
}

static void test_padding_boundaries(void)
{
    /* digests of 55, 56, 63 and 64 repetitions of 'a', from the reference implementations */
    static const struct { unsigned int len; const char *sha1, *sha256; } tests[] =
    {
        { 55, "c1c8bbdc22796e28c0e15163d20899b65621d65a", "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
        { 56, "c2db330f6083854c99d4b5bfb6e8f29f201be699", "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
        { 63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5", "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
        { 64, "0098ba824b5c16427bd7a1122a5a442a25ec644d", "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
    };
    char msg[64];
    unsigned int i;

    memset(msg, 'a', sizeof(msg));
    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        check(rtl_sha1, 20, msg, tests[i].len, tests[i].sha1, "sha1");
        check(rtl_sha256, 32, msg, tests[i].len, tests[i].sha256, "sha256");
    }
}

START_TEST(sha)
{
    test_known_vectors();
    test_padding_boundaries();
    test_million_a();
}
