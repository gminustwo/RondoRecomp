/* See vc_crypto.h. AES (FIPS-197), decryption only, with the usual lookup tables built at
 * first use; SHA-1 (FIPS 180-4). */
#include "vc_crypto.h"

#include <string.h>

static uint8_t sbox[256], inv_sbox[256];
static uint32_t td[4][256];   /* InvSubBytes + InvMixColumns, one table per byte position */
static int ready;

static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static uint8_t mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static void init(void)
{
    /* the S-box: the inverse in GF(2^8), then the affine map */
    for (int i = 0; i < 256; i++) {
        uint8_t inv = 0;
        for (int j = 1; j < 256 && i; j++)
            if (mul((uint8_t)i, (uint8_t)j) == 1) { inv = (uint8_t)j; break; }
        uint8_t s = inv, x = inv;
        for (int k = 0; k < 4; k++) {
            x = (uint8_t)((x << 1) | (x >> 7));
            s ^= x;
        }
        s ^= 0x63;
        sbox[i] = s;
        inv_sbox[s] = (uint8_t)i;
    }
    for (int i = 0; i < 256; i++) {
        uint8_t s = inv_sbox[i];
        uint32_t w = (uint32_t)mul(s, 14) | (uint32_t)mul(s, 9) << 8 | (uint32_t)mul(s, 13) << 16 | (uint32_t)mul(s, 11) << 24;
        for (int k = 0; k < 4; k++) td[k][i] = (w << (8 * k)) | (k ? w >> (32 - 8 * k) : 0);
    }
    ready = 1;
}

/* the decryption round keys, as columns (little-endian words: byte 0 is row 0) */
static void expand(const uint8_t key[16], uint32_t dk[44])
{
    uint32_t ek[44];
    for (int i = 0; i < 4; i++) ek[i] = (uint32_t)key[4 * i] | (uint32_t)key[4 * i + 1] << 8 | (uint32_t)key[4 * i + 2] << 16 | (uint32_t)key[4 * i + 3] << 24;
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint32_t t = ek[i - 1];
        if (i % 4 == 0) {
            t = (t >> 8) | (t << 24);
            t = (uint32_t)sbox[t & 255] | (uint32_t)sbox[(t >> 8) & 255] << 8 | (uint32_t)sbox[(t >> 16) & 255] << 16 | (uint32_t)sbox[t >> 24] << 24;
            t ^= rcon;
            rcon = xt(rcon);
        }
        ek[i] = ek[i - 4] ^ t;
    }
    /* equivalent inverse cipher: the round keys in reverse, the middle ones through InvMixColumns */
    for (int r = 0; r <= 10; r++)
        for (int c = 0; c < 4; c++) {
            uint32_t w = ek[4 * (10 - r) + c];
            if (r > 0 && r < 10)
                w = td[0][sbox[w & 255]] ^ td[1][sbox[(w >> 8) & 255]] ^ td[2][sbox[(w >> 16) & 255]] ^ td[3][sbox[w >> 24]];
            dk[4 * r + c] = w;
        }
}

static void decrypt_block(const uint32_t dk[44], const uint8_t in[16], uint8_t out[16])
{
    uint32_t s[4], t[4];
    for (int c = 0; c < 4; c++)
        s[c] = ((uint32_t)in[4 * c] | (uint32_t)in[4 * c + 1] << 8 | (uint32_t)in[4 * c + 2] << 16 | (uint32_t)in[4 * c + 3] << 24) ^ dk[c];
    for (int r = 1; r < 10; r++) {
        for (int c = 0; c < 4; c++)   /* InvShiftRows: row k comes from column c - k */
            t[c] = td[0][s[c] & 255] ^ td[1][(s[(c + 3) & 3] >> 8) & 255] ^ td[2][(s[(c + 2) & 3] >> 16) & 255] ^
                   td[3][s[(c + 1) & 3] >> 24] ^ dk[4 * r + c];
        memcpy(s, t, sizeof s);
    }
    for (int c = 0; c < 4; c++) {
        uint32_t w = (uint32_t)inv_sbox[s[c] & 255] | (uint32_t)inv_sbox[(s[(c + 3) & 3] >> 8) & 255] << 8 |
                     (uint32_t)inv_sbox[(s[(c + 2) & 3] >> 16) & 255] << 16 | (uint32_t)inv_sbox[s[(c + 1) & 3] >> 24] << 24;
        w ^= dk[40 + c];
        out[4 * c] = (uint8_t)w;
        out[4 * c + 1] = (uint8_t)(w >> 8);
        out[4 * c + 2] = (uint8_t)(w >> 16);
        out[4 * c + 3] = (uint8_t)(w >> 24);
    }
}

void aes128_cbc_decrypt(const uint8_t key[16], uint8_t iv[16], uint8_t *buf, size_t len)
{
    if (!ready) init();
    uint32_t dk[44];
    expand(key, dk);
    uint8_t prev[16], plain[16];
    for (size_t o = 0; o + 16 <= len; o += 16) {
        memcpy(prev, buf + o, 16);
        decrypt_block(dk, buf + o, plain);
        for (int i = 0; i < 16; i++) buf[o + i] = plain[i] ^ iv[i];
        memcpy(iv, prev, 16);
    }
}

/* ---- SHA-1 ---- */

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void sha1(const void *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    const uint8_t *p = data;
    size_t o = 0;
    for (; o + 64 <= len; o += 64) sha1_block(h, p + o);
    uint8_t tail[128] = {0};
    size_t rest = len - o;
    memcpy(tail, p + o, rest);
    tail[rest] = 0x80;
    size_t blocks = rest + 9 > 64 ? 2 : 1;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t b = 0; b < blocks; b++) sha1_block(h, tail + 64 * b);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}
