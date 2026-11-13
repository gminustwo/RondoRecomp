/* See chd.h. What is inside a .chd (version 5, as MAME's src/lib/util/chd.cpp, chdcodec.cpp
 * and cdrom.cpp read it):
 *
 * A 124-byte header (big-endian): "MComprHD", its length, the version, four codecs, the size
 * of the data, where the map and the metadata are, the size of a hunk (what is compressed on
 * its own) and SHA-1s, the last one a parent's (a .chd that holds only what differs from
 * another). For a CD a hunk is a whole number of frames of 2448 bytes: a 2352-byte sector
 * (audio as big-endian samples) and 96 bytes of subcode.
 *
 * The map gives each hunk's codec (Huffman-coded, with runs), then its compressed length,
 * where it is and a CRC-16 of the hunk, or which earlier hunk it repeats. The CD codecs
 * compress a hunk's sectors and its subcode separately. cdlz (LZMA) and cdzl (Deflate) start
 * with a bit per frame whose sync header and ECC were left out, to be rebuilt, and the length
 * of the sectors' part; cdfl holds the sectors as FLAC frames (16-bit stereo, 44.1 kHz) and
 * the subcode as Deflate after them.
 *
 * The metadata lists the tracks ("CHT2": TRACK:2 TYPE:MODE1_RAW SUBTYPE:NONE FRAMES:10374
 * PREGAP:225 PGTYPE:VMODE1_RAW PGSUB:NONE POSTGAP:0). Each track's frames are padded to a
 * multiple of 4 in the .chd; a pregap whose type starts with V is stored (counted in FRAMES),
 * any other is not. */
#include "chd.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "LzmaDec.h"
#include "setup.h"

#define FRAME 2448        /* a CD frame in a .chd: a sector and its subcode */
#define SECTOR 2352
#define SUBCODE 96
#define FLAC_MAX 65536    /* the longest FLAC block */

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
enum {
    CODEC_CDLZ = TAG('c', 'd', 'l', 'z'), CODEC_CDZL = TAG('c', 'd', 'z', 'l'), CODEC_CDFL = TAG('c', 'd', 'f', 'l'),
    CODEC_LZMA = TAG('l', 'z', 'm', 'a'), CODEC_ZLIB = TAG('z', 'l', 'i', 'b'),
};

/* what the map says a hunk is (and, while it is decoded, the shorthands for runs and repeats) */
enum { MAP_NONE = 4, MAP_SELF, MAP_PARENT, MAP_RLE_SMALL, MAP_RLE_LARGE, MAP_SELF_0, MAP_SELF_1,
       MAP_PARENT_SELF, MAP_PARENT_0, MAP_PARENT_1 };

typedef struct Track {
    int number;
    int frames, pregap, postgap;
    int size;             /* bytes of each frame it uses: 2352, or 2048 (MODE1) */
    bool audio, pregap_stored;
    const char *cue_type;
    uint64_t first;       /* its first frame in the .chd */
} Track;

typedef struct Chd {
    SDL_IOStream *f;
    uint32_t codec[4];
    uint64_t logical, map_at, meta_at;
    uint32_t hunk_bytes, hunks, frames_per_hunk;
    bool compressed;
    uint8_t *map;         /* compressed: 12 bytes a hunk (type, length, offset, CRC-16); else 4 */
    uint8_t *comp, *work; /* a compressed hunk; the codecs' working space */
    uint8_t *hunk;        /* the hunk last read: `cached` */
    uint32_t cached;
    int32_t *pcm[2];
    z_stream z;
    bool z_ready;
    Track tracks[99];
    int n_tracks;
} Chd;

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | be24(p + 1); }
static uint64_t be48(const uint8_t *p) { return (uint64_t)be16(p) << 32 | be32(p + 2); }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static void put_be(uint8_t *p, uint64_t v, int bytes)
{
    while (bytes--) { p[bytes] = (uint8_t)v; v >>= 8; }
}

static bool read_at(Chd *c, uint64_t at, void *buf, size_t n)
{
    return SDL_SeekIO(c->f, (Sint64)at, SDL_IO_SEEK_SET) == (Sint64)at && SDL_ReadIO(c->f, buf, n) == n;
}

/* CRC-16-CCITT (initial value FFFF), what the map keeps of each hunk and of itself */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    static uint16_t table[256];
    if (!table[1])
        for (int i = 0; i < 256; i++) {
            uint16_t c = (uint16_t)(i << 8);
            for (int k = 0; k < 8; k++) c = (uint16_t)(c & 0x8000 ? c << 1 ^ 0x1021 : c << 1);
            table[i] = c;
        }
    uint16_t crc = 0xFFFF;
    while (n--) crc = (uint16_t)(crc << 8 ^ table[(crc >> 8) ^ *p++]);
    return crc;
}

/* ---- bits, most significant first (the map, FLAC) ------------------------------------------ */

typedef struct Bits {
    const uint8_t *start, *p, *end;
    uint64_t acc;         /* the next bits, from the top; zeros below them */
    int n;                /* how many */
    size_t over;          /* bytes read past the end (as zeros) */
} Bits;

static void bits_init(Bits *b, const uint8_t *p, size_t len)
{
    memset(b, 0, sizeof *b);
    b->start = b->p = p;
    b->end = p + len;
}

static void bits_fill(Bits *b)
{
    while (b->n <= 56) {
        uint64_t byte = 0;
        if (b->p < b->end) byte = *b->p++;
        else b->over++;
        b->acc |= byte << (56 - b->n);
        b->n += 8;
    }
}

static uint32_t bits_get(Bits *b, int k)   /* k: 0 to 32 */
{
    if (!k) return 0;
    if (b->n < k) bits_fill(b);
    uint32_t v = (uint32_t)(b->acc >> (64 - k));
    b->acc <<= k;
    b->n -= k;
    return v;
}

static int32_t bits_signed(Bits *b, int k)   /* k: 1 to 32 */
{
    uint32_t v = bits_get(b, k);
    return k < 32 ? (int32_t)(v << (32 - k)) >> (32 - k) : (int32_t)v;
}

/* zeros, then a one: how many zeros */
static uint32_t bits_unary(Bits *b)
{
    uint32_t q = 0;
    for (;;) {
        if (!b->n) bits_fill(b);
        if (b->acc) {
            int z = __builtin_clzll(b->acc);
            b->acc <<= z;
            b->acc <<= 1;
            b->n -= z + 1;
            return q + (uint32_t)z;
        }
        if (b->over > 8) return q;   /* past the end: the hunk's CRC fails */
        q += (uint32_t)b->n;
        b->n = 0;
    }
}

static void bits_align(Bits *b) { bits_get(b, b->n & 7); }

/* bytes used so far (after bits_align) */
static size_t bits_offset(const Bits *b) { return (size_t)(b->p - b->start) + b->over - (size_t)(b->n / 8); }

/* ---- the map ------------------------------------------------------------------------------- */

/* The Huffman code of the hunks' types: 16 codes of at most 8 bits, their lengths stored
 * with runs, codes assigned as MAME does (the longest lengths get the lowest codes). */
typedef struct Huff { uint8_t len[16]; uint16_t lookup[256]; /* code << 5 | length */ } Huff;

static bool huff_import(Huff *h, Bits *b)
{
    int n = 0;
    while (n < 16) {
        int v = (int)bits_get(b, 4);
        if (v != 1) { h->len[n++] = (uint8_t)v; continue; }
        v = (int)bits_get(b, 4);
        if (v == 1) { h->len[n++] = 1; continue; }
        int rep = (int)bits_get(b, 4) + 3;
        if (n + rep > 16) return false;
        while (rep--) h->len[n++] = (uint8_t)v;
    }
    uint32_t start[33] = {0}, at = 0;
    for (int i = 0; i < 16; i++) {
        if (h->len[i] > 8) return false;
        start[h->len[i]]++;
    }
    for (int l = 32; l > 0; l--) {
        uint32_t next = (at + start[l]) >> 1;
        if (l != 1 && next * 2 != at + start[l]) return false;
        start[l] = at;
        at = next;
    }
    memset(h->lookup, 0, sizeof h->lookup);
    for (int i = 0; i < 16; i++) {
        if (!h->len[i]) continue;
        int shift = 8 - h->len[i];
        uint32_t code = start[h->len[i]]++;
        if ((code + 1) << shift > 256) return false;
        for (uint32_t j = code << shift; j < (code + 1) << shift; j++) h->lookup[j] = (uint16_t)(i << 5 | h->len[i]);
    }
    return true;
}

static int huff_one(const Huff *h, Bits *b)
{
    if (b->n < 8) bits_fill(b);
    uint16_t e = h->lookup[b->acc >> 56];
    b->acc <<= e & 31;
    b->n -= e & 31;
    return e >> 5;
}

static bool damaged(const char *what)
{
    snprintf(g_err, sizeof g_err, "the .chd is damaged (%s)", what);
    return false;
}

static bool read_map(Chd *c)
{
    size_t n = c->hunks;
    if (!c->compressed) {   /* an offset (in hunks) per hunk; 0: all zeros */
        c->map = malloc(n * 4);
        return c->map && read_at(c, c->map_at, c->map, n * 4) ? true : damaged("its map cannot be read");
    }
    uint8_t h[16];
    if (!read_at(c, c->map_at, h, sizeof h)) return damaged("its map cannot be read");
    uint32_t bytes = be32(h);
    uint64_t at = be48(h + 4);
    uint16_t want = be16(h + 10);
    int len_bits = h[12], self_bits = h[13];
    uint8_t *src = malloc(bytes ? bytes : 1);
    c->map = calloc(n, 12);
    if (!src || !c->map) { free(src); snprintf(g_err, sizeof g_err, "not enough memory"); return false; }
    Bits b;
    bits_init(&b, src, bytes);
    Huff hf;
    bool ok = len_bits <= 32 && self_bits <= 32 && read_at(c, c->map_at + 16, src, bytes) && huff_import(&hf, &b);
    /* the types, with runs of the one before */
    int last = 0, rep = 0;
    for (size_t i = 0; ok && i < n; i++) {
        if (rep) { c->map[i * 12] = (uint8_t)last; rep--; continue; }
        int v = huff_one(&hf, &b);
        if (v == MAP_RLE_SMALL) {
            c->map[i * 12] = (uint8_t)last;
            rep = 2 + huff_one(&hf, &b);
        } else if (v == MAP_RLE_LARGE) {
            c->map[i * 12] = (uint8_t)last;
            rep = 2 + 16 + (huff_one(&hf, &b) << 4);
            rep += huff_one(&hf, &b);
        } else {
            c->map[i * 12] = (uint8_t)(last = v);
        }
    }
    /* then each hunk's length, place and CRC */
    uint64_t last_self = 0;
    for (size_t i = 0; ok && i < n; i++) {
        uint8_t *m = c->map + i * 12;
        uint64_t off = at;
        uint32_t len = 0;
        uint16_t crc = 0;
        switch (m[0]) {
        case 0: case 1: case 2: case 3:
            len = bits_get(&b, len_bits);
            at += len;
            crc = (uint16_t)bits_get(&b, 16);
            break;
        case MAP_NONE:
            len = c->hunk_bytes;
            at += len;
            crc = (uint16_t)bits_get(&b, 16);
            break;
        case MAP_SELF:
            last_self = off = bits_get(&b, self_bits);
            break;
        case MAP_SELF_1:
            last_self++;
            /* fall through */
        case MAP_SELF_0:
            m[0] = MAP_SELF;
            off = last_self;
            break;
        default:   /* a parent's hunks: not in a .chd without one */
            ok = false;
        }
        put_be(m + 1, len, 3);
        put_be(m + 4, off, 6);
        put_be(m + 10, crc, 2);
    }
    free(src);
    if (!ok || crc16(c->map, n * 12) != want) return damaged("its map does not check out");
    return true;
}

/* ---- the codecs ---------------------------------------------------------------------------- */

static bool unzip(Chd *c, const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t want)   /* raw Deflate */
{
    if (!c->z_ready) {
        memset(&c->z, 0, sizeof c->z);
        if (inflateInit2(&c->z, -MAX_WBITS) != Z_OK) return false;
        c->z_ready = true;
    } else if (inflateReset(&c->z) != Z_OK) {
        return false;
    }
    c->z.next_in = (Bytef *)src;
    c->z.avail_in = len;
    c->z.next_out = dst;
    c->z.avail_out = want;
    return inflate(&c->z, Z_FINISH) == Z_STREAM_END && c->z.total_out == want;
}

static void *lz_alloc(ISzAllocPtr p, size_t n) { (void)p; return malloc(n); }
static void lz_free(ISzAllocPtr p, void *a) { (void)p; free(a); }
static const ISzAlloc lz_allocator = {lz_alloc, lz_free};

static bool unlzma(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t want)
{
    /* chdman stores no properties: lc 3, lp 0, pb 2 (its level 9) and a dictionary no larger
     * than a hunk (any size that large will do: the output is the dictionary) */
    static const Byte props[LZMA_PROPS_SIZE] = {93, 0, 0, 0, 4};
    SizeT out = want, in = len;
    ELzmaStatus status;
    return LzmaDecode(dst, &out, src, &in, props, LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &status, &lz_allocator) == SZ_OK &&
           out == want;
}

/* A Mode 1 sector's sync header and P and Q parity (ECMA-130), which cdlz and cdzl leave out
 * when they can be rebuilt. */
static void sector_rebuild(uint8_t *s)
{
    static uint8_t f[256], back[256];
    if (!f[1])
        for (int i = 0; i < 256; i++) {
            int j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
            f[i] = (uint8_t)j;
            back[i ^ j] = (uint8_t)i;
        }
    static const uint8_t sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
    memcpy(s, sync, sizeof sync);
    uint8_t addr[4];
    bool mode2 = s[15] == 2;   /* (Mode 2's parity leaves the address out) */
    if (mode2) { memcpy(addr, s + 12, 4); memset(s + 12, 0, 4); }
    static const int shape[2][5] = {{86, 24, 2, 86, 2076}, {52, 43, 86, 88, 2248}};   /* P, then Q */
    for (int pq = 0; pq < 2; pq++) {
        int majors = shape[pq][0], minors = shape[pq][1], size = majors * minors;
        uint8_t *out = s + shape[pq][4];
        for (int major = 0; major < majors; major++) {
            int index = (major >> 1) * shape[pq][2] + (major & 1);
            uint8_t a = 0, b = 0;
            for (int minor = 0; minor < minors; minor++) {
                uint8_t t = s[12 + index];
                index += shape[pq][3];
                if (index >= size) index -= size;
                a ^= t;
                b ^= t;
                a = f[a];
            }
            a = back[f[a] ^ b];
            out[major] = a;
            out[major + majors] = a ^ b;
        }
    }
    if (mode2) memcpy(s + 12, addr, 4);
}

/* the sectors (from `sectors`) and their subcode (from `sub`) back into frames */
static void frames_join(Chd *c, const uint8_t *sectors, const uint8_t *sub, uint8_t *dst)
{
    for (uint32_t i = 0; i < c->frames_per_hunk; i++) {
        memcpy(dst + i * FRAME, sectors + i * SECTOR, SECTOR);
        memcpy(dst + i * FRAME + SECTOR, sub + i * SUBCODE, SUBCODE);
    }
}

/* cdlz, cdzl */
static bool cd_codec(Chd *c, uint32_t codec, const uint8_t *src, uint32_t len, uint8_t *dst)
{
    uint32_t frames = c->frames_per_hunk;
    uint32_t ecc_bytes = (frames + 7) / 8, len_bytes = c->hunk_bytes < 65536 ? 2 : 3, head = ecc_bytes + len_bytes;
    if (len < head) return false;
    uint32_t base = be16(src + ecc_bytes);
    if (len_bytes > 2) base = base << 8 | src[ecc_bytes + 2];
    if (base > len - head) return false;
    uint8_t *sectors = c->work, *sub = c->work + frames * SECTOR;
    bool ok = codec == CODEC_CDLZ ? unlzma(src + head, base, sectors, frames * SECTOR)
                                  : unzip(c, src + head, base, sectors, frames * SECTOR);
    if (!ok || !unzip(c, src + head + base, len - head - base, sub, frames * SUBCODE)) return false;
    frames_join(c, sectors, sub, dst);
    for (uint32_t i = 0; i < frames; i++)
        if (src[i / 8] >> (i % 8) & 1) sector_rebuild(dst + i * FRAME);
    return true;
}

/* FLAC: just what libFLAC writes for chdman (no stream header: 16-bit stereo frames) */

static bool residual(Bits *b, int32_t *x, int n, int order)
{
    int method = (int)bits_get(b, 2);
    if (method > 1) return false;
    int param_bits = method ? 5 : 4, escape = method ? 31 : 15;
    int porder = (int)bits_get(b, 4), per = n >> porder;
    if (per << porder != n || per < order) return false;
    int i = order;
    for (int p = 0; p < 1 << porder; p++) {
        int k = (int)bits_get(b, param_bits), end = (p + 1) * per;
        if (k == escape) {
            int raw = (int)bits_get(b, 5);
            for (; i < end; i++) x[i] = raw ? bits_signed(b, raw) : 0;
        } else {
            for (; i < end; i++) {
                uint32_t v = bits_unary(b) << k;
                v |= bits_get(b, k);
                x[i] = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);
            }
        }
    }
    return true;
}

static bool subframe(Bits *b, int32_t *x, int n, int bps)
{
    if (bits_get(b, 1)) return false;
    int type = (int)bits_get(b, 6), wasted = 0;
    if (bits_get(b, 1)) {
        wasted = (int)bits_unary(b) + 1;
        bps -= wasted;
    }
    if (bps < 1 || bps > 32) return false;
    if (type == 0) {                                   /* constant */
        int32_t v = bits_signed(b, bps);
        for (int i = 0; i < n; i++) x[i] = v;
    } else if (type == 1) {                            /* verbatim */
        for (int i = 0; i < n; i++) x[i] = bits_signed(b, bps);
    } else if (type >= 8 && type <= 12) {              /* fixed predictor */
        int order = type - 8;
        if (order > n) return false;
        for (int i = 0; i < order; i++) x[i] = bits_signed(b, bps);
        if (!residual(b, x, n, order)) return false;
        for (int i = order; i < n; i++) {
            int64_t p = 0;
            switch (order) {
            case 1: p = x[i - 1]; break;
            case 2: p = 2 * (int64_t)x[i - 1] - x[i - 2]; break;
            case 3: p = 3 * (int64_t)x[i - 1] - 3 * (int64_t)x[i - 2] + x[i - 3]; break;
            case 4: p = 4 * (int64_t)x[i - 1] - 6 * (int64_t)x[i - 2] + 4 * (int64_t)x[i - 3] - x[i - 4]; break;
            }
            x[i] += (int32_t)p;
        }
    } else if (type >= 32) {                           /* linear prediction */
        int order = type - 31;
        if (order > n) return false;
        for (int i = 0; i < order; i++) x[i] = bits_signed(b, bps);
        int precision = (int)bits_get(b, 4) + 1, shift = bits_signed(b, 5);
        if (precision == 16 || shift < 0) return false;
        int32_t coef[32];
        for (int i = 0; i < order; i++) coef[i] = bits_signed(b, precision);
        if (!residual(b, x, n, order)) return false;
        for (int i = order; i < n; i++) {
            int64_t s = 0;
            for (int j = 0; j < order; j++) s += (int64_t)coef[j] * x[i - 1 - j];
            x[i] += (int32_t)(s >> shift);
        }
    } else {
        return false;
    }
    if (wasted)
        for (int i = 0; i < n; i++) x[i] = (int32_t)((uint32_t)x[i] << wasted);
    return true;
}

/* one frame into l and r: how many samples, 0 if it is not a stereo FLAC frame */
static int flac_frame(Bits *b, int32_t *l, int32_t *r)
{
    if (bits_get(b, 15) != 0x7FFC) return 0;          /* sync code, a reserved 0 */
    bits_get(b, 1);                                    /* fixed or variable block size */
    int bs_code = (int)bits_get(b, 4), rate_code = (int)bits_get(b, 4);
    int chan = (int)bits_get(b, 4), size_code = (int)bits_get(b, 3);
    bits_get(b, 1);
    uint32_t x = bits_get(b, 8);                       /* frame number, coded like UTF-8 */
    int more = 0;
    if (x >= 0x80) {
        if (x >= 0xFE || (x & 0xC0) == 0x80) return 0;
        while (x & (0x40u >> more)) more++;
    }
    while (more--) bits_get(b, 8);
    int n;
    if (bs_code == 0) return 0;
    else if (bs_code == 1) n = 192;
    else if (bs_code <= 5) n = 576 << (bs_code - 2);
    else if (bs_code == 6) n = (int)bits_get(b, 8) + 1;
    else if (bs_code == 7) n = (int)bits_get(b, 16) + 1;
    else n = 256 << (bs_code - 8);
    if (rate_code == 12) bits_get(b, 8);
    else if (rate_code == 13 || rate_code == 14) bits_get(b, 16);
    else if (rate_code == 15) return 0;
    bits_get(b, 8);                                    /* CRC-8 (the hunk's CRC is checked) */
    static const int sizes[8] = {16, 8, 12, 0, 16, 20, 24, 32};   /* 0: the stream's, 16 */
    int bps = sizes[size_code];
    /* two channels: independent (1), left/side (8), side/right (9) or mid/side (10); the side
     * channel has a bit more */
    if (!bps || (chan != 1 && (chan < 8 || chan > 10)) || n > FLAC_MAX) return 0;
    if (!subframe(b, l, n, bps + (chan == 9)) || !subframe(b, r, n, bps + (chan == 8 || chan == 10))) return 0;
    bits_align(b);
    bits_get(b, 16);                                   /* CRC-16 */
    for (int i = 0; i < n; i++) {
        int32_t a = l[i], s = r[i];
        if (chan == 8) {
            r[i] = a - s;
        } else if (chan == 9) {
            l[i] = a + s;
        } else if (chan == 10) {
            int32_t m = a * 2 + (s & 1);
            l[i] = (m + s) >> 1;
            r[i] = (m - s) >> 1;
        }
    }
    return n;
}

/* cdfl */
static bool cd_flac(Chd *c, const uint8_t *src, uint32_t len, uint8_t *dst)
{
    uint32_t frames = c->frames_per_hunk, samples = frames * SECTOR / 4;
    uint8_t *sectors = c->work, *sub = c->work + frames * SECTOR;
    Bits b;
    bits_init(&b, src, len);
    for (uint32_t done = 0; done < samples;) {
        int n = flac_frame(&b, c->pcm[0], c->pcm[1]);
        if (n <= 0 || done + (uint32_t)n > samples) return false;
        uint8_t *o = sectors + done * 4;
        for (int i = 0; i < n; i++, o += 4) {   /* big-endian, as the .chd keeps audio */
            o[0] = (uint8_t)(c->pcm[0][i] >> 8);
            o[1] = (uint8_t)c->pcm[0][i];
            o[2] = (uint8_t)(c->pcm[1][i] >> 8);
            o[3] = (uint8_t)c->pcm[1][i];
        }
        done += (uint32_t)n;
    }
    size_t at = bits_offset(&b);
    if (at > len || !unzip(c, src + at, len - (uint32_t)at, sub, frames * SUBCODE)) return false;
    frames_join(c, sectors, sub, dst);
    return true;
}

static bool decode(Chd *c, uint32_t codec, const uint8_t *src, uint32_t len, uint8_t *dst)
{
    switch (codec) {
    case CODEC_CDLZ: case CODEC_CDZL: return cd_codec(c, codec, src, len, dst);
    case CODEC_CDFL: return cd_flac(c, src, len, dst);
    case CODEC_ZLIB: return unzip(c, src, len, dst, c->hunk_bytes);
    case CODEC_LZMA: return unlzma(src, len, dst, c->hunk_bytes);
    }
    return false;
}

static bool read_hunk(Chd *c, uint32_t i, uint8_t *dst, int depth)
{
    char what[64];
    snprintf(what, sizeof what, "hunk %u of %u does not check out", i, c->hunks);
    if (i >= c->hunks) return damaged("its track list goes past its data");
    if (!c->compressed) {
        uint64_t at = (uint64_t)be32(c->map + 4 * (size_t)i) * c->hunk_bytes;
        if (!at) memset(dst, 0, c->hunk_bytes);
        return !at || read_at(c, at, dst, c->hunk_bytes) ? true : damaged(what);
    }
    const uint8_t *m = c->map + 12 * (size_t)i;
    uint32_t len = be24(m + 1);
    uint64_t at = be48(m + 4);
    bool ok;
    switch (m[0]) {
    case 0: case 1: case 2: case 3:
        ok = len <= c->hunk_bytes && read_at(c, at, c->comp, len) && decode(c, c->codec[m[0]], c->comp, len, dst);
        break;
    case MAP_NONE:
        ok = read_at(c, at, dst, c->hunk_bytes);
        break;
    case MAP_SELF:   /* the same as an earlier one */
        return at < i && depth < 8 ? read_hunk(c, (uint32_t)at, dst, depth + 1) : damaged(what);
    default:
        ok = false;
    }
    return ok && crc16(dst, c->hunk_bytes) == be16(m + 10) ? true : damaged(what);
}

/* frame `frame` of the .chd */
static const uint8_t *read_frame(Chd *c, uint64_t frame)
{
    uint32_t h = (uint32_t)(frame / c->frames_per_hunk);
    if (h != c->cached) {
        c->cached = UINT32_MAX;
        if (!read_hunk(c, h, c->hunk, 0)) return NULL;
        c->cached = h;
    }
    return c->hunk + frame % c->frames_per_hunk * FRAME;
}

/* ---- opening one ---------------------------------------------------------------------------- */

static void chd_close(Chd *c)
{
    if (c->f) SDL_CloseIO(c->f);
    if (c->z_ready) inflateEnd(&c->z);
    free(c->map);
    free(c->comp);
    free(c->work);
    free(c->hunk);
    free(c->pcm[0]);
    free(c->pcm[1]);
    free(c);
}

/* "KEY:value" from a track's line in the metadata */
static const char *field(const char *text, const char *key, char *out, size_t n)
{
    const char *p = strstr(text, key);
    out[0] = 0;
    if (!p) return out;
    p += strlen(key);
    size_t len = strcspn(p, " \t\r\n");
    snprintf(out, n, "%.*s", (int)(len < n ? len : n - 1), p);
    return out;
}

static bool read_tracks(Chd *c, const char *name)
{
    uint64_t at = c->meta_at;
    for (int guard = 0; at && guard < 1000; guard++) {
        uint8_t h[16];
        if (!read_at(c, at, h, sizeof h)) return damaged("its track list cannot be read");
        uint32_t tag = be32(h), len = be24(h + 5);
        if (tag == TAG('C', 'H', 'T', '2') || tag == TAG('C', 'H', 'T', 'R')) {
            char text[256], v[32];
            if (len >= sizeof text || !read_at(c, at + 16, text, len)) return damaged("its track list cannot be read");
            text[len] = 0;
            if (c->n_tracks == 99) return damaged("it lists more than 99 tracks");
            Track *t = &c->tracks[c->n_tracks++];
            memset(t, 0, sizeof *t);
            t->number = atoi(field(text, "TRACK:", v, sizeof v));
            t->frames = atoi(field(text, "FRAMES:", v, sizeof v));
            t->pregap = atoi(field(text, "PREGAP:", v, sizeof v));
            t->postgap = atoi(field(text, "POSTGAP:", v, sizeof v));
            t->pregap_stored = field(text, "PGTYPE:", v, sizeof v)[0] == 'V';
            field(text, "TYPE:", v, sizeof v);
            t->size = 2352;
            if (!strcmp(v, "AUDIO")) t->audio = true, t->cue_type = "AUDIO";
            else if (!strcmp(v, "MODE1_RAW")) t->cue_type = "MODE1/2352";
            else if (!strcmp(v, "MODE1")) t->cue_type = "MODE1/2048", t->size = 2048;
            else {
                snprintf(g_err, sizeof g_err, "%s: track %d is %s, which a PC Engine CD does not have", name, t->number, v);
                return false;
            }
            if (t->frames <= 0 || t->pregap < 0 || t->postgap < 0 || (t->pregap_stored && t->pregap > t->frames))
                return damaged("its track list does not add up");
        } else if (tag == TAG('C', 'H', 'G', 'D')) {
            snprintf(g_err, sizeof g_err, "%s is a Dreamcast GD-ROM, not a PC Engine CD", name);
            return false;
        }
        at = be64(h + 8);
    }
    if (!c->n_tracks) {
        snprintf(g_err, sizeof g_err, "%s is not a CD image (it has no track list)", name);
        return false;
    }
    for (int i = 1; i < c->n_tracks; i++)   /* in order */
        for (int j = i; j > 0 && c->tracks[j].number < c->tracks[j - 1].number; j--) {
            Track t = c->tracks[j];
            c->tracks[j] = c->tracks[j - 1];
            c->tracks[j - 1] = t;
        }
    uint64_t frame = 0;
    for (int i = 0; i < c->n_tracks; i++) {
        c->tracks[i].first = frame;
        frame += (uint64_t)(c->tracks[i].frames + 3) / 4 * 4;
    }
    return frame * FRAME <= c->logical ? true : damaged("its track list goes past its data");
}

static Chd *chd_open(const char *path)
{
    const char *name = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (bs > name) name = bs;
    name = name ? name + 1 : path;
    Chd *c = calloc(1, sizeof *c);
    if (!c) { snprintf(g_err, sizeof g_err, "not enough memory"); return NULL; }
    c->cached = UINT32_MAX;
    c->f = SDL_IOFromFile(path, "rb");
    uint8_t h[124];
    if (!c->f) {
        snprintf(g_err, sizeof g_err, "cannot open %s", path);
        goto fail;
    }
    if (SDL_ReadIO(c->f, h, 16) != 16 || memcmp(h, "MComprHD", 8)) {
        snprintf(g_err, sizeof g_err, "%s is not a CHD file", name);
        goto fail;
    }
    if (be32(h + 12) != 5) {
        snprintf(g_err, sizeof g_err, "%s is a version %u CHD, which Setup cannot read: chdman (chdman copy) turns it "
                 "into a version 5 one", name, be32(h + 12));
        goto fail;
    }
    if (be32(h + 8) < sizeof h || !read_at(c, 0, h, sizeof h)) {
        damaged("its header cannot be read");
        goto fail;
    }
    for (int i = 0; i < 4; i++) c->codec[i] = be32(h + 16 + 4 * i);
    c->logical = be64(h + 32);
    c->map_at = be64(h + 40);
    c->meta_at = be64(h + 48);
    c->hunk_bytes = be32(h + 56);
    c->compressed = c->codec[0] != 0;
    static const uint8_t none[20];
    if (memcmp(h + 104, none, sizeof none)) {
        snprintf(g_err, sizeof g_err, "%s holds only what differs from another .chd (its parent), which Setup cannot "
                 "read: chdman (chdman copy) makes it whole", name);
        goto fail;
    }
    if (!c->hunk_bytes || c->hunk_bytes % FRAME || c->hunk_bytes > (1 << 20)) {
        snprintf(g_err, sizeof g_err, "%s is not a CD image", name);
        goto fail;
    }
    for (int i = 0; i < 4; i++) {
        uint32_t k = c->codec[i];
        if (!k || k == CODEC_CDLZ || k == CODEC_CDZL || k == CODEC_CDFL || k == CODEC_LZMA || k == CODEC_ZLIB) continue;
        char cc[5] = {(char)(k >> 24), (char)(k >> 16), (char)(k >> 8), (char)k, 0};
        for (int j = 0; j < 4; j++) if (cc[j] < 32 || cc[j] > 126) cc[j] = '?';
        snprintf(g_err, sizeof g_err, "%s is compressed with %s, which Setup cannot read; chdman's own choice for CDs "
                 "(cdlz, cdzl, cdfl) works: chdman copy -c cdlz,cdzl,cdfl", name, cc);
        goto fail;
    }
    c->frames_per_hunk = c->hunk_bytes / FRAME;
    uint64_t hunks = (c->logical + c->hunk_bytes - 1) / c->hunk_bytes;
    if (!hunks || hunks > (1u << 24)) {
        damaged("its size is out of range");
        goto fail;
    }
    c->hunks = (uint32_t)hunks;
    c->comp = malloc(c->hunk_bytes);
    c->work = malloc(c->hunk_bytes);
    c->hunk = malloc(c->hunk_bytes);
    c->pcm[0] = malloc(FLAC_MAX * sizeof(int32_t));
    c->pcm[1] = malloc(FLAC_MAX * sizeof(int32_t));
    if (!c->comp || !c->work || !c->hunk || !c->pcm[0] || !c->pcm[1]) {
        snprintf(g_err, sizeof g_err, "not enough memory");
        goto fail;
    }
    if (read_map(c) && read_tracks(c, name)) return c;
fail:
    chd_close(c);
    return NULL;
}

static const Track *first_data(const Chd *c)
{
    for (int i = 0; i < c->n_tracks; i++)
        if (!c->tracks[i].audio) return &c->tracks[i];
    return NULL;
}

/* a data track's sector (counted from its INDEX 01): its 2048 bytes */
static bool read_data(Chd *c, const Track *t, int sector, uint8_t *out)
{
    int index = sector + (t->pregap_stored ? t->pregap : 0);
    const uint8_t *f = index < t->frames ? read_frame(c, t->first + (uint64_t)index) : NULL;
    if (!f) return false;
    memcpy(out, f + (t->size == 2352 ? 16 : 0), 2048);
    return true;
}

DiscKind chd_identify(const char *path, uint64_t *fingerprint)
{
    if (fingerprint) *fingerprint = 0;
    Chd *c = chd_open(path);
    if (!c) return DISC_INVALID;
    const Track *t = first_data(c);
    uint8_t s1[2048], s2[2048];
    DiscKind kind = DISC_INVALID;
    if (!t) snprintf(g_err, sizeof g_err, "the .chd has no data track: it is not a PC Engine CD");
    else if (read_data(c, t, 1, s1) && read_data(c, t, 2, s2)) {
        kind = discid_identify_sectors(s1, s2, fingerprint);
        if (kind == DISC_INVALID) snprintf(g_err, sizeof g_err, "the .chd is not a PC Engine CD");
    }
    chd_close(c);
    return kind;
}

static void msf(char *out, size_t n, int frames)
{
    snprintf(out, n, "%02d:%02d:%02d", frames / 75 / 60, frames / 75 % 60, frames % 75);
}

bool chd_import(const char *path, const char *stage, char *cue_out, size_t cue_out_len)
{
    Chd *c = chd_open(path);
    if (!c) return false;
    /* the tracks are named after the .chd, as a Redump set is: "<name> (Track 02).bin" */
    char base[400];
    const char *name = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (bs > name) name = bs;
    snprintf(base, sizeof base, "%s", name ? name + 1 : path);
    size_t bl = strlen(base);
    if (bl > 4 && !SDL_strcasecmp(base + bl - 4, ".chd")) base[bl - 4] = 0;
    for (char *p = base; *p; p++) if (*p == '"') *p = '\'';

    remove_tree(stage);
    char cue_name[420];
    snprintf(cue_name, sizeof cue_name, "%s.cue", base);
    join(cue_out, cue_out_len, stage, cue_name);
    SDL_IOStream *cue = SDL_CreateDirectory(stage) ? SDL_IOFromFile(cue_out, "w") : NULL;
    if (!cue) {
        snprintf(g_err, sizeof g_err, "cannot write %s", cue_out);
        chd_close(c);
        return false;
    }
    SDL_IOprintf(cue, "REM made by RondoRecomp Setup from %s.chd\n", base);

    enum { BATCH = 256 };   /* frames written at once */
    uint8_t *buf = malloc((size_t)BATCH * SECTOR);
    uint64_t total = 0, done = 0;
    for (int i = 0; i < c->n_tracks; i++) total += (uint64_t)(c->tracks[i].frames + c->tracks[i].postgap);
    int data = 0, audio = 0, last_pct = -1;
    bool ok = buf != NULL;
    if (!ok) snprintf(g_err, sizeof g_err, "not enough memory");
    for (int i = 0; i < c->n_tracks && ok; i++) {
        const Track *t = &c->tracks[i];
        char file[480], dest[1500], m[16];
        snprintf(file, sizeof file, "%s (Track %02d).bin", base, t->number);
        join(dest, sizeof dest, stage, file);
        SDL_IOprintf(cue, "FILE \"%s\" BINARY\n  TRACK %02d %s\n", file, t->number, t->cue_type);
        if (t->pregap && !t->pregap_stored) {
            msf(m, sizeof m, t->pregap);
            SDL_IOprintf(cue, "    PREGAP %s\n", m);
        }
        if (t->pregap && t->pregap_stored) {
            msf(m, sizeof m, t->pregap);
            SDL_IOprintf(cue, "    INDEX 00 00:00:00\n    INDEX 01 %s\n", m);
        } else {
            SDL_IOprintf(cue, "    INDEX 01 00:00:00\n");
        }
        if (t->audio) audio++;
        else data++;
        SDL_IOStream *out = SDL_IOFromFile(dest, "wb");
        if (!out) { snprintf(g_err, sizeof g_err, "cannot write %s", dest); ok = false; break; }
        /* A postgap (silence the .chd does not store) is written out as zeros rather than as
         * a POSTGAP line, which the game's cue reader does not count: the tracks after it
         * stay where they are on the disc. */
        int n = 0, frames = t->frames + t->postgap;
        for (int f = 0; f < frames && ok; f++) {
            uint8_t *o = buf + (size_t)n * (size_t)t->size;
            if (f < t->frames) {
                const uint8_t *s = read_frame(c, t->first + (uint64_t)f);
                if (!s) { ok = false; break; }
                if (t->audio) {   /* little-endian samples, as in a .bin */
                    for (int k = 0; k < SECTOR; k += 2) { o[k] = s[k + 1]; o[k + 1] = s[k]; }
                } else {
                    memcpy(o, s, (size_t)t->size);
                }
            } else {
                memset(o, 0, (size_t)t->size);
            }
            if (++n == BATCH || f + 1 == frames) {
                size_t bytes = (size_t)n * (size_t)t->size;
                if (SDL_WriteIO(out, buf, bytes) != bytes) { snprintf(g_err, sizeof g_err, "cannot write %s", dest); ok = false; }
                n = 0;
            }
            int pct = (int)(++done * 100 / total);
            if (pct != last_pct) {
                setup_progress_hook("Unpacking the .chd", pct);
                last_pct = pct;
            }
        }
        if (!SDL_CloseIO(out) && ok) { snprintf(g_err, sizeof g_err, "cannot write %s", dest); ok = false; }
    }
    if (!SDL_CloseIO(cue) && ok) { snprintf(g_err, sizeof g_err, "cannot write %s", cue_out); ok = false; }
    if (ok) say("  %d tracks: %d data, %d audio\n", c->n_tracks, data, audio);
    free(buf);
    chd_close(c);
    return ok;
}
