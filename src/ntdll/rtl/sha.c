/*
 * SHA-1 and SHA-256
 *
 * Wine's ntdll takes these from the SymCrypt library for RtlDeriveCapabilitySidsFromName and the app container SID code.
 * This repository does not carry SymCrypt, so the two functions are written here from FIPS 180-4. Original code of this
 * repository; tested against the FIPS 180 examples in tests/rtl-host/test_sha.c.
 *
 * The functions take the whole message at once, as the callers do.
 */
#include <stdarg.h>

#include "windef.h"
#include "winnt.h"
#include "rtl_sha.h"

static inline UINT32 rol(UINT32 x, int n) { return (x << n) | (x >> (32 - n)); }
static inline UINT32 ror(UINT32 x, int n) { return (x >> n) | (x << (32 - n)); }

static inline UINT32 load_be32(const BYTE *p)
{
    return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | p[3];
}

static inline void store_be32(BYTE *p, UINT32 v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* Feeds one 64-byte block, or the padded tail, to compress(). The padding is the same for both hashes. */
static void hash_message(UINT32 *state, void (*compress)(UINT32 *, const BYTE *), const BYTE *data, SIZE_T len)
{
    BYTE tail[128];
    SIZE_T i, rest = len % 64, pad_len = (rest < 56) ? 64 : 128;
    UINT64 bits = (UINT64)len * 8;

    for (i = 0; i + 64 <= len; i += 64) compress(state, data + i);
    for (i = 0; i < rest; i++) tail[i] = data[len - rest + i];
    tail[rest] = 0x80;
    for (i = rest + 1; i < pad_len - 8; i++) tail[i] = 0;
    for (i = 0; i < 8; i++) tail[pad_len - 1 - i] = (BYTE)(bits >> (8 * i));
    for (i = 0; i < pad_len; i += 64) compress(state, tail + i);
}

static void sha1_block(UINT32 *h, const BYTE *p)
{
    UINT32 w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f, k, t;
    int i;

    for (i = 0; i < 16; i++) w[i] = load_be32(p + 4 * i);
    for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++)
    {
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void rtl_sha1( const BYTE *data, SIZE_T len, BYTE *result )
{
    UINT32 h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    int i;

    hash_message( h, sha1_block, data, len );
    for (i = 0; i < 5; i++) store_be32( result + 4 * i, h[i] );
}

static const UINT32 k256[64] =
{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(UINT32 *h, const BYTE *p)
{
    UINT32 w[64], v[8], t1, t2, s0, s1;
    int i;

    for (i = 0; i < 16; i++) w[i] = load_be32(p + 4 * i);
    for (i = 16; i < 64; i++)
    {
        s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 8; i++) v[i] = h[i];
    for (i = 0; i < 64; i++)
    {
        s1 = ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25);
        t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k256[i] + w[i];
        s0 = ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22);
        t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) h[i] += v[i];
}

void rtl_sha256( const BYTE *data, SIZE_T len, BYTE *result )
{
    UINT32 h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    int i;

    hash_message( h, sha256_block, data, len );
    for (i = 0; i < 8; i++) store_be32( result + 4 * i, h[i] );
}
