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
 * any other is not.
 *
 * LZMA is the LZMA SDK's decoder (third_party/lzma/LzmaDec.c); Deflate and FLAC are decoded
 * here. */
#include "chd.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "LzmaDec.h"
#include "disc.h"

#define SECTOR 2352
#define SUBCODE 96
#define FLAC_MAX 65536      /* the longest FLAC block */
#define CACHE 4             /* hunks kept decoded (CD audio and data can take turns) */

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
enum {
    CODEC_CDLZ = TAG('c', 'd', 'l', 'z'), CODEC_CDZL = TAG('c', 'd', 'z', 'l'), CODEC_CDFL = TAG('c', 'd', 'f', 'l'),
    CODEC_LZMA = TAG('l', 'z', 'm', 'a'), CODEC_ZLIB = TAG('z', 'l', 'i', 'b'),
};

/* what the map says a hunk is (and, while it is decoded, the shorthands for runs and repeats) */
enum { MAP_NONE = 4, MAP_SELF, MAP_PARENT, MAP_RLE_SMALL, MAP_RLE_LARGE, MAP_SELF_0, MAP_SELF_1,
       MAP_PARENT_SELF, MAP_PARENT_0, MAP_PARENT_1 };

/* a Huffman code for Deflate: how many codes of each length, then the symbols in code order */
typedef struct InfHuff { int16_t count[16]; int16_t symbol[288]; } InfHuff;

struct Chd {
    FILE *f;
    uint32_t codec[4];
    uint64_t logical, map_at, meta_at;
    uint32_t hunk_bytes, hunks, frames_per_hunk;
    bool compressed;
    uint8_t *map;           /* compressed: 12 bytes a hunk (type, length, offset, CRC-16); else 4 */
    uint8_t *comp, *work;   /* a compressed hunk; the codecs' working space */
    uint8_t *slot[CACHE];   /* hunks read lately: which, and when */
    uint32_t slot_hunk[CACHE], slot_used[CACHE], tick;
    int32_t *pcm[2];
    InfHuff fixed_len, fixed_dist;
    uint16_t crc_table[256];
    uint8_t gf_mul[256], gf_div[256];   /* ECC: times 2 in GF(2^8), and its inverse of (x ^ 2x) */
    ChdTrack tracks[99];
    int n_tracks;
    char err[256];
};

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
    return at <= LONG_MAX && fseek(c->f, (long)at, SEEK_SET) == 0 && fread(buf, 1, n, c->f) == n;
}

/* CRC-16-CCITT (initial value FFFF), what the map keeps of each hunk and of itself */
static uint16_t crc16(const Chd *c, const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) crc = (uint16_t)(crc << 8 ^ c->crc_table[(crc >> 8) ^ *p++]);
    return crc;
}

/* ---- bits, most significant first (the map, FLAC) ------------------------------------------ */

typedef struct Bits {
    const uint8_t *start, *p, *end;
    uint64_t acc;           /* the next bits, from the top; zeros below them */
    int n;                  /* how many */
    size_t over;            /* bytes read past the end (as zeros) */
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

static bool damaged(Chd *c, const char *what)
{
    snprintf(c->err, sizeof c->err, "the .chd is damaged (%s)", what);
    return false;
}

static bool read_map(Chd *c)
{
    size_t n = c->hunks;
    if (!c->compressed) {   /* an offset (in hunks) per hunk; 0: all zeros */
        c->map = malloc(n * 4);
        return c->map && read_at(c, c->map_at, c->map, n * 4) ? true : damaged(c, "its map cannot be read");
    }
    uint8_t h[16];
    if (!read_at(c, c->map_at, h, sizeof h)) return damaged(c, "its map cannot be read");
    uint32_t bytes = be32(h);
    uint64_t at = be48(h + 4);
    uint16_t want = be16(h + 10);
    int len_bits = h[12], self_bits = h[13];
    uint8_t *src = bytes <= c->hunks * 16u + 4096 ? malloc(bytes ? bytes : 1) : NULL;
    c->map = calloc(n, 12);
    if (!src || !c->map) { free(src); return damaged(c, "its map cannot be read"); }
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
    if (!ok || crc16(c, c->map, n * 12) != want) return damaged(c, "its map does not check out");
    return true;
}

/* ---- Deflate (RFC 1951), raw: as zlib's inflate with negative window bits ------------------- */

typedef struct Inflate {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint32_t bits;          /* bits not yet used, from the bottom */
    int nbits;
    uint8_t *out;
    size_t out_len, out_pos;
    bool bad;               /* ran out of input */
} Inflate;

static int inf_bits(Inflate *s, int need)   /* need: 0 to 16 */
{
    while (s->nbits < need) {
        if (s->in_pos >= s->in_len) { s->bad = true; return 0; }
        s->bits |= (uint32_t)s->in[s->in_pos++] << s->nbits;
        s->nbits += 8;
    }
    int v = (int)(s->bits & ((1u << need) - 1));
    s->bits >>= need;
    s->nbits -= need;
    return v;
}

/* canonical codes: the shortest first, in symbol order within a length */
static int inf_decode(Inflate *s, const InfHuff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_bits(s, 1);
        if (s->bad) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* <0: too many codes; 0: complete; >0: incomplete (codes left over) */
static int inf_build(InfHuff *h, const uint8_t *length, int n)
{
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    int16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = (int16_t)i;
    return left;
}

static bool inf_codes(Inflate *s, const InfHuff *lc, const InfHuff *dc)
{
    static const int16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                         67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                          1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const int8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                          11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = inf_decode(s, lc);
        if (sym < 0) return false;
        if (sym < 256) {
            if (s->out_pos >= s->out_len) return false;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = (size_t)(len_base[sym] + inf_bits(s, len_extra[sym]));
            int d = inf_decode(s, dc);
            if (d < 0 || d >= 30) return false;
            size_t dist = (size_t)(dist_base[d] + inf_bits(s, dist_extra[d]));
            if (s->bad || dist > s->out_pos || len > s->out_len - s->out_pos) return false;
            for (size_t i = 0; i < len; i++, s->out_pos++) s->out[s->out_pos] = s->out[s->out_pos - dist];
        }
    }
}

static bool inf_stored(Inflate *s)
{
    s->bits = 0;            /* (fewer than 8 bits are left over: to the next byte) */
    s->nbits = 0;
    if (s->in_len - s->in_pos < 4) return false;
    const uint8_t *p = s->in + s->in_pos;
    size_t len = (size_t)(p[0] | p[1] << 8);
    if (len != (size_t)(~(p[2] | p[3] << 8) & 0xFFFF)) return false;
    s->in_pos += 4;
    if (len > s->in_len - s->in_pos || len > s->out_len - s->out_pos) return false;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return true;
}

static bool inf_dynamic(Inflate *s)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = inf_bits(s, 5) + 257, ndist = inf_bits(s, 5) + 1, ncode = inf_bits(s, 4) + 4;
    if (s->bad || nlen > 286 || ndist > 30) return false;
    uint8_t lengths[320] = {0};
    for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)inf_bits(s, 3);
    InfHuff lc, dc;
    if (s->bad || inf_build(&lc, lengths, 19) != 0) return false;
    for (int i = 0; i < nlen + ndist;) {
        int sym = inf_decode(s, &lc);
        if (sym < 0) return false;
        if (sym < 16) { lengths[i++] = (uint8_t)sym; continue; }
        int len = 0, rep;
        if (sym == 16) {
            if (!i) return false;
            len = lengths[i - 1];
            rep = 3 + inf_bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + inf_bits(s, 3);
        } else {
            rep = 11 + inf_bits(s, 7);
        }
        if (s->bad || i + rep > nlen + ndist) return false;
        while (rep--) lengths[i++] = (uint8_t)len;
    }
    if (!lengths[256]) return false;
    /* incomplete codes only when there is one code */
    int left = inf_build(&lc, lengths, nlen);
    if (left < 0 || (left > 0 && nlen - lc.count[0] != 1)) return false;
    left = inf_build(&dc, lengths + nlen, ndist);
    if (left < 0 || (left > 0 && ndist - dc.count[0] != 1)) return false;
    return inf_codes(s, &lc, &dc);
}

static bool unzip(Chd *c, const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t want)
{
    Inflate s = {.in = src, .in_len = len, .out = dst, .out_len = want};
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (s.bad) return false;
        bool ok = type == 0 ? inf_stored(&s) : type == 1 ? inf_codes(&s, &c->fixed_len, &c->fixed_dist)
                : type == 2 ? inf_dynamic(&s) : false;
        if (!ok || s.bad) return false;
    } while (!last);
    return s.out_pos == want;
}

/* ---- the codecs ---------------------------------------------------------------------------- */

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
static void sector_rebuild(const Chd *c, uint8_t *s)
{
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
                a = c->gf_mul[a];
            }
            a = c->gf_div[c->gf_mul[a] ^ b];
            out[major] = a;
            out[major + majors] = a ^ b;
        }
    }
    if (mode2) memcpy(s + 12, addr, 4);
}

/* the sectors (from `sectors`) and their subcode (from `sub`) back into frames */
static void frames_join(const Chd *c, const uint8_t *sectors, const uint8_t *sub, uint8_t *dst)
{
    for (uint32_t i = 0; i < c->frames_per_hunk; i++) {
        memcpy(dst + i * CHD_FRAME, sectors + i * SECTOR, SECTOR);
        memcpy(dst + i * CHD_FRAME + SECTOR, sub + i * SUBCODE, SUBCODE);
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
        if (src[i / 8] >> (i % 8) & 1) sector_rebuild(c, dst + i * CHD_FRAME);
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
    if (i >= c->hunks) return damaged(c, "its track list goes past its data");
    if (!c->compressed) {
        uint64_t at = (uint64_t)be32(c->map + 4 * (size_t)i) * c->hunk_bytes;
        if (!at) memset(dst, 0, c->hunk_bytes);
        return !at || read_at(c, at, dst, c->hunk_bytes) ? true : damaged(c, what);
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
        return at < i && depth < 8 ? read_hunk(c, (uint32_t)at, dst, depth + 1) : damaged(c, what);
    default:
        ok = false;
    }
    return ok && crc16(c, dst, c->hunk_bytes) == be16(m + 10) ? true : damaged(c, what);
}

const uint8_t *chd_frame(Chd *c, uint64_t frame)
{
    uint32_t h = (uint32_t)(frame / c->frames_per_hunk);
    int use = 0;
    for (int i = 0; i < CACHE; i++) {
        if (c->slot_hunk[i] == h) { use = i; goto found; }
        if (c->slot_used[i] < c->slot_used[use]) use = i;
    }
    c->slot_hunk[use] = UINT32_MAX;
    if (frame >= c->logical / CHD_FRAME) {
        damaged(c, "its track list goes past its data");
        return NULL;
    }
    if (!read_hunk(c, h, c->slot[use], 0)) return NULL;
    c->slot_hunk[use] = h;
found:
    c->slot_used[use] = ++c->tick;
    return c->slot[use] + frame % c->frames_per_hunk * CHD_FRAME;
}

bool chd_verify(Chd *c, void (*progress)(int pct, void *ctx), void *ctx)
{
    int last = -1;
    c->slot_hunk[0] = UINT32_MAX;   /* read into */
    for (uint32_t h = 0; h < c->hunks; h++) {
        if (!read_hunk(c, h, c->slot[0], 0)) return false;
        int pct = (int)((uint64_t)(h + 1) * 100 / c->hunks);
        if (progress && pct != last) progress(last = pct, ctx);
    }
    return true;
}

/* ---- opening one ---------------------------------------------------------------------------- */

void chd_close(Chd *c)
{
    if (!c) return;
    if (c->f) fclose(c->f);
    free(c->map);
    free(c->comp);
    free(c->work);
    for (int i = 0; i < CACHE; i++) free(c->slot[i]);
    free(c->pcm[0]);
    free(c->pcm[1]);
    free(c);
}

int chd_track_count(const Chd *c) { return c->n_tracks; }
const ChdTrack *chd_track(const Chd *c, int i) { return i >= 0 && i < c->n_tracks ? &c->tracks[i] : NULL; }
const char *chd_error(const Chd *c) { return c->err; }

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
        if (!read_at(c, at, h, sizeof h)) return damaged(c, "its track list cannot be read");
        uint32_t tag = be32(h), len = be24(h + 5);
        if (tag == TAG('C', 'H', 'T', '2') || tag == TAG('C', 'H', 'T', 'R')) {
            char text[256], v[32];
            if (len >= sizeof text || !read_at(c, at + 16, text, len)) return damaged(c, "its track list cannot be read");
            text[len] = 0;
            if (c->n_tracks == 99) return damaged(c, "it lists more than 99 tracks");
            ChdTrack *t = &c->tracks[c->n_tracks++];
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
                snprintf(c->err, sizeof c->err, "%s: track %d is %s, which a PC Engine CD does not have", name, t->number, v);
                return false;
            }
            if (t->frames <= 0 || t->pregap < 0 || t->postgap < 0 || (t->pregap_stored && t->pregap > t->frames))
                return damaged(c, "its track list does not add up");
        } else if (tag == TAG('C', 'H', 'G', 'D')) {
            snprintf(c->err, sizeof c->err, "%s is a Dreamcast GD-ROM, not a PC Engine CD", name);
            return false;
        }
        at = be64(h + 8);
    }
    if (!c->n_tracks) {
        snprintf(c->err, sizeof c->err, "%s is not a CD image (it has no track list)", name);
        return false;
    }
    for (int i = 1; i < c->n_tracks; i++)   /* in order */
        for (int j = i; j > 0 && c->tracks[j].number < c->tracks[j - 1].number; j--) {
            ChdTrack t = c->tracks[j];
            c->tracks[j] = c->tracks[j - 1];
            c->tracks[j - 1] = t;
        }
    uint64_t frame = 0;
    for (int i = 0; i < c->n_tracks; i++) {
        c->tracks[i].first = frame;
        frame += (uint64_t)(c->tracks[i].frames + 3) / 4 * 4;
    }
    return frame * CHD_FRAME <= c->logical ? true : damaged(c, "its track list goes past its data");
}

static void make_tables(Chd *c)
{
    for (int i = 0; i < 256; i++) {
        uint16_t r = (uint16_t)(i << 8);
        for (int k = 0; k < 8; k++) r = (uint16_t)(r & 0x8000 ? r << 1 ^ 0x1021 : r << 1);
        c->crc_table[i] = r;
        int j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
        c->gf_mul[i] = (uint8_t)j;
        c->gf_div[i ^ j] = (uint8_t)i;
    }
    uint8_t len[288];
    for (int i = 0; i < 288; i++) len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    inf_build(&c->fixed_len, len, 288);
    for (int i = 0; i < 30; i++) len[i] = 5;
    inf_build(&c->fixed_dist, len, 30);
}

Chd *chd_open(const char *path, char *err, int err_len)
{
    const char *name = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (bs > name) name = bs;
    name = name ? name + 1 : path;
    Chd *c = calloc(1, sizeof *c);
    if (!c) { snprintf(err, (size_t)err_len, "not enough memory"); return NULL; }
    make_tables(c);
    for (int i = 0; i < CACHE; i++) c->slot_hunk[i] = UINT32_MAX;
    c->f = disc_fopen(path, "rb");
    uint8_t h[124];
    if (!c->f) {
        snprintf(c->err, sizeof c->err, "cannot open %s", path);
        goto fail;
    }
    if (fread(h, 1, 16, c->f) != 16 || memcmp(h, "MComprHD", 8)) {
        snprintf(c->err, sizeof c->err, "%s is not a CHD file", name);
        goto fail;
    }
    if (be32(h + 12) != 5) {
        snprintf(c->err, sizeof c->err, "%s is a version %u CHD, which RondoRecomp cannot read: chdman (chdman copy) "
                 "turns it into a version 5 one", name, be32(h + 12));
        goto fail;
    }
    if (be32(h + 8) < sizeof h || !read_at(c, 0, h, sizeof h)) {
        damaged(c, "its header cannot be read");
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
        snprintf(c->err, sizeof c->err, "%s holds only what differs from another .chd (its parent), which RondoRecomp "
                 "cannot read: chdman (chdman copy) makes it whole", name);
        goto fail;
    }
    if (!c->hunk_bytes || c->hunk_bytes % CHD_FRAME || c->hunk_bytes > (1 << 20)) {
        snprintf(c->err, sizeof c->err, "%s is not a CD image", name);
        goto fail;
    }
    for (int i = 0; i < 4; i++) {
        uint32_t k = c->codec[i];
        if (!k || k == CODEC_CDLZ || k == CODEC_CDZL || k == CODEC_CDFL || k == CODEC_LZMA || k == CODEC_ZLIB) continue;
        char cc[5] = {(char)(k >> 24), (char)(k >> 16), (char)(k >> 8), (char)k, 0};
        for (int j = 0; j < 4; j++) if (cc[j] < 32 || cc[j] > 126) cc[j] = '?';
        snprintf(c->err, sizeof c->err, "%s is compressed with %s, which RondoRecomp cannot read; chdman's own choice "
                 "for CDs (cdlz, cdzl, cdfl) works: chdman copy -c cdlz,cdzl,cdfl", name, cc);
        goto fail;
    }
    c->frames_per_hunk = c->hunk_bytes / CHD_FRAME;
    uint64_t hunks = (c->logical + c->hunk_bytes - 1) / c->hunk_bytes;
    if (!hunks || hunks > (1u << 24)) {
        damaged(c, "its size is out of range");
        goto fail;
    }
    c->hunks = (uint32_t)hunks;
    c->comp = malloc(c->hunk_bytes);
    c->work = malloc(c->hunk_bytes);
    bool mem = c->comp && c->work;
    for (int i = 0; i < CACHE; i++) mem = (c->slot[i] = malloc(c->hunk_bytes)) && mem;
    c->pcm[0] = malloc(FLAC_MAX * sizeof(int32_t));
    c->pcm[1] = malloc(FLAC_MAX * sizeof(int32_t));
    if (!mem || !c->pcm[0] || !c->pcm[1]) {
        snprintf(c->err, sizeof c->err, "not enough memory");
        goto fail;
    }
    if (read_map(c) && read_tracks(c, name)) return c;
fail:
    snprintf(err, (size_t)err_len, "%s", c->err);
    chd_close(c);
    return NULL;
}
