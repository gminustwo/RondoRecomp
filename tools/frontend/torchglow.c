/* See torchglow.h. */
#include "torchglow.h"

#include "pce/pce.h"

#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* A wall torch, as the game draws it (Stage 3's dungeon, area $18): a flame of 2 x 2 pixels
 * in pale yellow (9-bit $1FC: R 7, G 7, B 4) on a sconce, whose dark brown ($0B3: R 3, G 2,
 * B 1) shows either side of the flame's lower row. Found by those exact colours, so a fade
 * (other colours) or anything else pale yellow (without the sconce) is left alone. */
#define FLAME 0xFFFFFF91u
#define SCONCE 0xFF6D4824u
#define MAX_TORCHES 16
#define REACH 36      /* the light's largest radius, pixels */

static float clampf(float v) { return v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v; }

/* smooth noise over time: a value every 4 frames, eased between; 0..1 */
static float hash01(uint32_t n)
{
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return (float)(n & 0xFFFF) / 65535.0f;
}
static float noise(uint32_t seed, uint64_t frame)
{
    uint32_t k = (uint32_t)(frame / 4);
    float t = (float)(frame % 4) / 4.0f;
    t = t * t * (3.0f - 2.0f * t);
    float a = hash01(seed * 7919u + k), b = hash01(seed * 7919u + k + 1);
    return a + (b - a) * t;
}

/* A blue candle's flame, as the game draws it: its tip a white or pink pixel ($1FF / $0FB)
 * with a blue either side ($007 / $00F: 0024B6 / 006DFF) and a blue below; 3 lines down the
 * candle's pale blue-grey body ($1B4: 91B6B6). Its animation moves the tip a pixel. */
#define CANDLE_BODY 0xFF91B6B6u
#define MAX_CANDLES 16
static bool flame_blue(uint32_t p) { return (p >> 16 & 0xFF) == 0 && (p & 0xFF) >= 0xB6; }

int candle_glow(uint32_t *img, int w, int h, int pitch, uint64_t frame, int world_x)
{
    int cx[MAX_CANDLES], cy[MAX_CANDLES], n = 0;
    for (int y = 0; y + 3 < h && n < MAX_CANDLES; y++)
        for (int x = 1; x + 1 < w && n < MAX_CANDLES; x++) {
            uint32_t t = img[y * pitch + x];
            if (t != 0xFFFFFFFFu && t != 0xFFFF6D6Du) continue;
            if (!flame_blue(img[y * pitch + x - 1]) || !flame_blue(img[y * pitch + x + 1]) || !flame_blue(img[(y + 1) * pitch + x]))
                continue;
            if (img[(y + 3) * pitch + x] != CANDLE_BODY) continue;
            cx[n] = x;
            cy[n] = y;
            n++;
        }
    for (int c = 0; c < n; c++) {
        uint32_t seed = (uint32_t)((cx[c] + world_x) >> 2) * 977u + (uint32_t)(cy[c] >> 3) + 5u;
        float f = 0.6f + 0.4f * noise(seed, frame);
        float rad = 13.0f * f;
        int x0 = cx[c] - 14, x1 = cx[c] + 14, y0 = cy[c] - 14, y1 = cy[c] + 14;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w - 1) x1 = w - 1;
        if (y1 > h - 1) y1 = h - 1;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float dx = (float)(x - cx[c]), dy = (float)(y - cy[c]) * 1.1f, d = sqrtf(dx * dx + dy * dy);
                if (d >= rad) continue;
                float a = (1.0f - d / rad) * (1.0f - d / rad) * 0.8f * f;
                uint32_t p = img[y * pitch + x];
                float r = (float)(p >> 16 & 0xFF) + 60.0f * a, g = (float)(p >> 8 & 0xFF) + 150.0f * a, b = (float)(p & 0xFF) + 255.0f * a;
                img[y * pitch + x] = 0xFF000000u | (uint32_t)clampf(r) << 16 | (uint32_t)clampf(g) << 8 | (uint32_t)clampf(b);
            }
    }
    return n;
}

/* Alcove glow: the same wall torches where an alcove of lit red brick is painted round them
 * (the castle's halls, area $04: dark arches whose bricks are the game's dark reds $240000
 * and $480000; not $482400, which the floor stones use too), often half behind the next column of wall, only the flame's one column
 * showing. Found by a flame pixel with the sconce's brown below one side of it; only those
 * three reds round it are lit (brighter and warmer towards the flame, out to 60 px, the light
 * flickering), so the light keeps to the painted alcove (not the stone round it, nor the
 * HUD's brighter reds). skip_full: torches with their whole flame showing left to Torch glow. */
static bool brick_red(uint32_t p) { return p == 0xFF240000u || p == 0xFF480000u; }

int alcove_glow(uint32_t *img, int w, int h, int pitch, uint64_t frame, int world_x, bool skip_full)
{
    int fx[MAX_TORCHES], fy[MAX_TORCHES], n = 0;
    for (int y = 0; y + 1 < h && n < MAX_TORCHES; y++)
        for (int x = 1; x + 1 < w && n < MAX_TORCHES; x++) {
            if (img[y * pitch + x] != FLAME || img[(y + 1) * pitch + x] != FLAME) continue;
            if (img[(y + 1) * pitch + x - 1] != SCONCE && img[(y + 1) * pitch + x + 1] != SCONCE) continue;
            bool dup = false;
            for (int k = 0; k < n; k++) dup |= x - fx[k] < 4 && fx[k] - x < 4 && y - fy[k] < 4 && fy[k] - y < 4;
            if (dup) continue;
            bool full = x + 2 < w && img[y * pitch + x + 1] == FLAME && img[(y + 1) * pitch + x + 1] == FLAME &&
                        img[(y + 1) * pitch + x - 1] == SCONCE && img[(y + 1) * pitch + x + 2] == SCONCE;
            if (skip_full && full) {
                x += 2;
                continue;
            }
            fx[n] = x;
            fy[n] = y;
            n++;
        }
    for (int t = 0; t < n; t++) {
        float cx = (float)fx[t] + 0.5f, cy = (float)fy[t] + 0.5f;
        uint32_t seed = (uint32_t)((fx[t] + world_x) >> 3) * 131u + (uint32_t)(fy[t] >> 3) + 17u;
        float f = 0.55f + 0.45f * noise(seed, frame);
        int x0 = fx[t] - 61, x1 = fx[t] + 61, y0 = fy[t] - 61, y1 = fy[t] + 61;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w - 1) x1 = w - 1;
        if (y1 > h - 1) y1 = h - 1;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                uint32_t p = img[y * pitch + x];
                if (!brick_red(p)) continue;
                float dx = (float)x - cx, dy = ((float)y - cy) * 0.9f, d = sqrtf(dx * dx + dy * dy);
                if (d >= 60.0f) continue;
                float a = powf(1.0f - d / 60.0f, 1.2f) * f;
                float r = (float)(p >> 16 & 0xFF) * (1.0f + 2.2f * a) + 40.0f * a;
                float g = (float)(p >> 8 & 0xFF) * (1.0f + 1.5f * a) + 18.0f * a;
                img[y * pitch + x] = 0xFF000000u | (uint32_t)clampf(r) << 16 | (uint32_t)clampf(g) << 8 | (p & 0xFF);
            }
    }
    return n;
}

/* Moon glow: the moon (Stage 5's docks, area $26: a disc about 30 px across in four pale
 * blues, $91B6DA, $9191FF, $B6DAFF, $6D6DDA, that nothing else on its screens uses) gets a
 * faint rim of light: the sky's blues (no red or green above $24, blue $24 or more) within
 * 9 px of its edge lightened a little, breathing very slowly. The mast and posts in front stay
 * as they are. Its middle and size are the moon pixels' bounds; a moon mostly behind something
 * (fewer than 120 of its pixels) is left alone. */
static bool moon_px(uint32_t p)
{
    return p == 0xFF91B6DAu || p == 0xFF9191FFu || p == 0xFFB6DAFFu || p == 0xFF6D6DDAu;
}

int moon_glow(uint32_t *img, int w, int h, int pitch, uint64_t frame)
{
    int x0 = w, x1 = -1, y0 = h, y1 = -1, n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (moon_px(img[y * pitch + x])) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
                n++;
            }
    if (n < 120 || x1 - x0 > 48 || y1 - y0 > 48) return 0;
    /* (its size from its width and its middle from its bottom: clouds cross its top, and a few
     * of their pixels above it are in its colours, so its height is not its own; with the
     * middle worked out from the height the ring sat inside its lower left, a dark gap left
     * between them) */
    float r0 = (float)(x1 - x0 + 1) * 0.5f;
    float cx = (float)(x0 + x1) * 0.5f, cy = (float)y1 + 0.5f - r0;
    float br = 0.95f + 0.05f * sinf((float)(frame % 160) * 6.2831853f / 160.0f);
    float reach = 9.0f * br;
    int lx = (int)(cx - r0 - reach) - 1, hx = (int)(cx + r0 + reach) + 1;
    int ly = (int)(cy - r0 - reach) - 1, hy = (int)(cy + r0 + reach) + 1;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (hx > w - 1) hx = w - 1;
    if (hy > h - 1) hy = h - 1;
    for (int y = ly; y <= hy; y++)
        for (int x = lx; x <= hx; x++) {
            uint32_t p = img[y * pitch + x];
            int r = p >> 16 & 0xFF, g = p >> 8 & 0xFF, b = p & 0xFF;
            if (r > 0x24 || g > 0x24 || b < 0x24) continue;   /* (the sky's blues only) */
            float dx = (float)x - cx, dy = (float)y - cy, e = sqrtf(dx * dx + dy * dy) - r0;
            if (e < -1.5f || e >= reach) continue;   /* (sky up to its very edge: no gap) */
            if (e < 0.0f) e = 0.0f;
            float a = (1.0f - e / reach) * (1.0f - e / reach) * 0.22f * br;
            img[y * pitch + x] = 0xFF000000u | (uint32_t)clampf((float)r + 120.0f * a) << 16 |
                                 (uint32_t)clampf((float)g + 150.0f * a) << 8 | (uint32_t)clampf((float)b + 190.0f * a);
        }
    return 1;
}

/* Shield shine: the wall of heraldic shields (Stage 5's docks, area $26): each shield's olive
 * rim ($6D6D48, $484824) catches the light as a glint runs round it, now and then (3-7 s apart),
 * on one shield of those in view at random (now and then two). A shield
 * is found by its rim's upper part: a connected run of those two colours some 20-26 px wide
 * and 14-24 tall; the shield is that wide and 40 tall from its top. Only the rim's pixels
 * change, the face and the stone round it are left as they are. */
#define SHIELD_RIM_A 0xFF6D6D48u
#define SHIELD_RIM_B 0xFF484824u
#define MAX_SHIELDS 24
#define SHINE_BLOCK 300   /* frames (5 s) */
#define SHINE_GAP 180     /* the least time between two glints' starts (3 s) */
static bool rim_px(uint32_t p) { return p == SHIELD_RIM_A || p == SHIELD_RIM_B; }

int shield_shine(uint32_t *img, int w, int h, int pitch, uint64_t frame)
{
    static uint8_t seen[PCE_FB_MAX_W * PCE_FB_MAX_H];
    static int stack[PCE_FB_MAX_W * PCE_FB_MAX_H];
    if (w > PCE_FB_MAX_W || h > PCE_FB_MAX_H) return 0;
    memset(seen, 0, (size_t)w * (size_t)h);
    int bx0[MAX_SHIELDS], by0[MAX_SHIELDS], bx1[MAX_SHIELDS], n = 0;
    for (int y = 0; y < h && n < MAX_SHIELDS; y++)
        for (int x = 0; x < w && n < MAX_SHIELDS; x++) {
            if (seen[y * w + x] || !rim_px(img[y * pitch + x])) continue;
            int sp = 0, x0 = x, x1 = x, y0 = y, y1 = y, count = 0;
            stack[sp++] = y * w + x;
            seen[y * w + x] = 1;
            while (sp) {
                int q = stack[--sp], qx = q % w, qy = q / w;
                count++;
                if (qx < x0) x0 = qx;
                if (qx > x1) x1 = qx;
                if (qy < y0) y0 = qy;
                if (qy > y1) y1 = qy;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = qx + dx, ny = qy + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || seen[ny * w + nx]) continue;
                        if (!rim_px(img[ny * pitch + nx])) continue;
                        seen[ny * w + nx] = 1;
                        stack[sp++] = ny * w + nx;
                    }
            }
            int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            if (bw >= 20 && bw <= 26 && bh >= 14 && bh <= 24 && count >= 30) {
                bx0[n] = x0;
                by0[n] = y0;
                bx1[n] = x1;
                n++;
            }
        }
    /* when: one glint (48 frames) in each SHINE_BLOCK frames, starting at a random point in
     * the block's first SHINE_BLOCK - SHINE_GAP frames, so glints start SHINE_GAP (3 s) to
     * 2 * SHINE_BLOCK - SHINE_GAP (7 s) apart, at no set beat */
    uint64_t blk = frame / SHINE_BLOCK;
    int start = (int)(hash01((uint32_t)blk * 2654435761u ^ 0x5D1E1Du) * (float)(SHINE_BLOCK - SHINE_GAP));
    int age = (int)(frame % SHINE_BLOCK) - start;
    if (age < 0 || age >= 48) return n;
    float t = (float)age / 48.0f;
    /* which: one of the shields in view, picked at random (a third of the time two), when the
     * glint starts; followed by their X from frame to frame while it runs, as the wall scrolls */
    static uint64_t pick_blk = UINT64_MAX;
    static int pick_x[2], npick;
    if (n == 0) return 0;
    if (pick_blk != blk) {
        pick_blk = blk;
        int a = (int)(hash01((uint32_t)blk * 374761393u ^ 0xA11Cu) * (float)n);
        if (a >= n) a = n - 1;
        pick_x[0] = bx0[a];
        npick = 1;
        if (n >= 2 && hash01((uint32_t)blk * 668265263u ^ 0x2B0Du) < 0.33f) {
            int b = (a + 1 + (int)(hash01((uint32_t)blk * 3266489917u) * (float)(n - 1))) % n;
            pick_x[1] = bx0[b];
            npick = 2;
        }
    }
    bool lit[MAX_SHIELDS] = {0};
    for (int k = 0; k < npick; k++) {
        int best = -1, bd = 9;
        for (int s = 0; s < n; s++) {
            int d = abs(bx0[s] - pick_x[k]);
            if (d < bd) {
                bd = d;
                best = s;
            }
        }
        if (best >= 0) {
            lit[best] = true;
            pick_x[k] = bx0[best];
        }
    }
    for (int s = 0; s < n; s++) {
        if (!lit[s]) continue;
        float sw = (float)(bx1[s] - bx0[s] + 1), sh = 40.0f;
        for (int y = by0[s]; y < by0[s] + 41 && y < h; y++)
            for (int x = bx0[s] - 2; x <= bx1[s] + 2 && x < w; x++) {
                if (x < 0) continue;
                uint32_t p = img[y * pitch + x];
                if (!rim_px(p)) continue;
                float u = ((float)(x - bx0[s]) / sw) - 0.5f, v = ((float)(y - by0[s]) / sh) - 0.5f;
                float ang = atan2f(v, u) / 6.2831853f;
                if (ang < 0.0f) ang += 1.0f;
                float d = fabsf(ang - t);
                if (d > 0.5f) d = 1.0f - d;
                float a = expf(-(d / 0.05f) * (d / 0.05f));
                if (a < 0.01f) continue;
                float r = (float)(p >> 16 & 0xFF), g = (float)(p >> 8 & 0xFF), b = (float)(p & 0xFF);
                float k = 0.5f + (r + g + b) / 765.0f;
                r += (255.0f - r) * a * k;
                g += (245.0f - g) * a * k;
                b += (220.0f - b) * a * k;
                img[y * pitch + x] = 0xFF000000u | (uint32_t)clampf(r) << 16 | (uint32_t)clampf(g) << 8 | (uint32_t)clampf(b);
            }
    }
    return n;
}

int torch_glow(const uint32_t *src, uint32_t *dst, int w, int h, int pitch, uint64_t frame, int world_x)
{
    if (src != dst)
        for (int y = 0; y < h; y++) memcpy(&dst[y * pitch], &src[y * pitch], (size_t)w * sizeof *dst);
    int tx[MAX_TORCHES], ty[MAX_TORCHES], n = 0;
    for (int y = 0; y + 1 < h && n < MAX_TORCHES; y++) {
        const uint32_t *r0 = &src[y * pitch], *r1 = &src[(y + 1) * pitch];
        for (int x = 1; x + 2 < w && n < MAX_TORCHES; x++) {
            if (r0[x] != FLAME || r0[x + 1] != FLAME || r1[x] != FLAME || r1[x + 1] != FLAME) continue;
            if (r1[x - 1] != SCONCE || r1[x + 2] != SCONCE) continue;
            tx[n] = x;
            ty[n] = y;
            n++;
            x += 2;
        }
    }
    for (int t = 0; t < n; t++) {
        /* the flame's middle; its flicker, its own by where it stands in the level */
        float cx = (float)tx[t] + 1.0f, cy = (float)ty[t] + 1.0f;
        uint32_t seed = (uint32_t)((tx[t] + world_x) >> 3) * 131u + (uint32_t)(ty[t] >> 3);
        float f = 0.65f + 0.35f * noise(seed, frame);
        float rad = (float)REACH * f, core_rad = 7.0f * f;
        int x0 = (int)(cx - REACH) - 1, x1 = (int)(cx + REACH) + 1;
        int y0 = (int)(cy - REACH) - 1, y1 = (int)(cy + REACH) + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > w - 1) x1 = w - 1;
        if (y1 > h - 1) y1 = h - 1;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x - cx, dy = ((float)y - cy) * 1.15f;
                float d = sqrtf(dx * dx + dy * dy);
                if (d >= rad) continue;
                /* the wall lit: brighter, warmer, towards the flame; and a small bright core */
                float a = powf(1.0f - d / rad, 1.6f) * f;
                uint32_t p = dst[y * pitch + x];
                float r = (float)(p >> 16 & 0xFF), g = (float)(p >> 8 & 0xFF), b = (float)(p & 0xFF);
                r = r * (1.0f + 2.6f * a) + 70.0f * a;
                g = g * (1.0f + 1.7f * a) + 25.0f * a;
                b = b * (1.0f + 0.6f * a);
                if (d < core_rad) {
                    float c = (1.0f - d / core_rad) * (1.0f - d / core_rad) * 0.6f;
                    r += 255.0f * c;
                    g += 170.0f * c;
                    b += 60.0f * c;
                }
                dst[y * pitch + x] = 0xFF000000u | (uint32_t)clampf(r) << 16 | (uint32_t)clampf(g) << 8 | (uint32_t)clampf(b);
            }
    }
    return n;
}

/* Sail sway: the tattered purple sails hanging over Stage 5's docks move in the wind. A sail is
 * the run of its six purples joined to the picture's top row (so the HUD's purple sub-weapon
 * icon, not joined to it, stays as it is); each column's bottom is its lowest such pixel. Tall
 * sails (reaching below SAIL_TALL) sway slowly, their lower part moving most; short ones keep
 * still and their lower half flutters, quick and small. Where a sail moves off, the night sky
 * ($000024) shows. Phases go by the level's X, so sails keep their motion as they scroll. */
#define SAIL_SKY 0xFF000024u
#define SAIL_TALL 56
static bool sail_px(uint32_t p)
{
    return p == 0xFF48246Du || p == 0xFF480048u || p == 0xFF6D2491u || p == 0xFF9148B6u ||
           p == 0xFFB66DFFu || p == 0xFF240048u;
}

int sail_sway(uint32_t *img, int w, int h, int pitch, uint64_t frame, int world_x)
{
    static uint8_t mask[PCE_FB_MAX_W * PCE_FB_MAX_H];
    static int stack[PCE_FB_MAX_W * PCE_FB_MAX_H];
    static uint32_t row[PCE_FB_MAX_W];
    static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    int bot[PCE_FB_MAX_W], tall[PCE_FB_MAX_W];
    if (w > PCE_FB_MAX_W || h > PCE_FB_MAX_H) return 0;
    int hh = h < 128 ? h : 128;   /* (sails hang no lower than this) */
    memset(mask, 0, (size_t)w * (size_t)hh);
    int sp = 0, n = 0;
    for (int x = 0; x < w; x++)
        if (sail_px(img[x])) {
            mask[x] = 1;
            stack[sp++] = x;
        }
    while (sp) {
        int q = stack[--sp], qx = q % w, qy = q / w;
        n++;
        for (int k = 0; k < 4; k++) {
            int nx = qx + d4[k][0], ny = qy + d4[k][1];
            if (nx < 0 || ny < 0 || nx >= w || ny >= hh || mask[ny * w + nx]) continue;
            if (!sail_px(img[ny * pitch + nx])) continue;
            mask[ny * w + nx] = 1;
            stack[sp++] = ny * w + nx;
        }
    }
    if (n < 200) return 0;
    for (int x = 0; x < w; x++) {
        bot[x] = -1;
        for (int y = hh - 1; y >= 0; y--)
            if (mask[y * w + x]) {
                bot[x] = y;
                break;
            }
    }
    /* a sail: columns with sail, gaps of up to 6 joined; tall if any of it reaches SAIL_TALL */
    for (int x = 0; x < w;) {
        if (bot[x] < 0) {
            tall[x] = 0;
            x++;
            continue;
        }
        int e = x, deep = bot[x];
        for (int k = x; k < w && k <= e + 6; k++)
            if (bot[k] >= 0) {
                e = k;
                if (bot[k] > deep) deep = bot[k];
            }
        for (int k = x; k <= e; k++) tall[k] = deep >= SAIL_TALL;
        x = e + 1;
    }
    float t = (float)(frame % 144) / 144.0f;   /* sway: 2.4 s; flutter twice as quick */
    for (int y = 0; y < hh; y++) {
        uint32_t *line = &img[y * pitch];
        memcpy(row, line, (size_t)w * sizeof *row);
        for (int x = 0; x < w; x++) {
            /* the nearest sail column's motion (6 px either side: a moved pixel may come from
             * just beside its sail) */
            int c = -1;
            for (int d = 0; d <= 6 && c < 0; d++) {
                if (x - d >= 0 && bot[x - d] >= 0) c = x - d;
                else if (x + d < w && bot[x + d] >= 0) c = x + d;
            }
            if (c < 0) continue;
            float dx;
            if (tall[c]) {
                float depth = (float)y / 70.0f;
                if (depth > 1.0f) depth = 1.0f;
                dx = 3.0f * sinf(6.2831853f * t + (float)(c + world_x) * 6.2831853f / 400.0f) * depth * sqrtf(depth);
            } else {
                int b = -1;
                for (int k = c - 3; k <= c + 3; k++)
                    if (k >= 0 && k < w && bot[k] > b) b = bot[k];
                float half = (float)b * 0.5f, f = half > 0.0f ? ((float)y - half) / half : 0.0f;
                if (f <= 0.0f) continue;
                if (f > 1.0f) f = 1.0f;
                dx = 2.0f * sinf(6.2831853f * 2.0f * t + (float)(x + world_x) / 5.0f) * f;
            }
            int sx = x - (int)lroundf(dx);
            bool from = sx >= 0 && sx < w && mask[y * w + sx], here = mask[y * w + x];
            if (from) line[x] = row[sx];
            else if (here) line[x] = SAIL_SKY;
        }
    }
    return 1;
}

/* Night sky: the stars over Stage 5's docks twinkle faintly, and now and then (rarely) a small,
 * dim shooting star crosses the sky. A star, as the game draws it: a plus of 3 x 3 pixels on
 * the night sky ($000024), its middle pale ($B6B691) with dull arms ($6D4848), or (a fainter
 * one) dull with dark-blue arms ($000048); its corners sky. Each star keeps its own twinkle
 * (stars are followed from frame to frame, so one keeps its phase as the sky scrolls). Both
 * are drawn only while at least NS_MIN stars show or did in the last half second (none in a
 * fade or anywhere else), and the
 * shooting star only over the sky's own dark blues (behind sails, ropes and the moon). */
#define NS_MAX 64
#define NS_MIN 3
#define NS_STREAK_FRAMES 29      /* a shooting star's flight (about half a second) */
#define NS_STREAK_LEN 10
#define NS_STREAK_CHANCE 3600    /* one in this many frames of starry sky: once a minute on average */
#define NS_STREAK_GAP 1200       /* the least time between two (20 s) */
static const uint32_t ns_level[5] = {0xFF000024u, 0xFF000048u, 0xFF6D4848u, 0xFFB6B691u, 0xFFFFFFE0u};
static int ns_force = -1;

void night_sky_streak_at(int frame) { ns_force = frame; }

static uint32_t ns_mix(uint32_t a, uint32_t b, float t)
{
    float r = (float)(a >> 16 & 0xFF) + ((float)(b >> 16 & 0xFF) - (float)(a >> 16 & 0xFF)) * t;
    float g = (float)(a >> 8 & 0xFF) + ((float)(b >> 8 & 0xFF) - (float)(a >> 8 & 0xFF)) * t;
    float bl = (float)(a & 0xFF) + ((float)(b & 0xFF) - (float)(a & 0xFF)) * t;
    return 0xFF000000u | (uint32_t)clampf(r) << 16 | (uint32_t)clampf(g) << 8 | (uint32_t)clampf(bl);
}
static uint32_t ns_at(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 4.0f) v = 4.0f;
    int i = (int)v;
    if (i > 3) i = 3;
    return ns_mix(ns_level[i], ns_level[i + 1], v - (float)i);
}

int night_sky(uint32_t *img, int w, int h, int pitch, uint64_t frame)
{
    static int px[NS_MAX], py[NS_MAX], np;
    static float pph[NS_MAX];
    static uint32_t seq;
    static uint64_t last_frame = UINT64_MAX, streak_start, streak_end;
    static bool streak;
    static float sx0, sy0, svx, svy;
    int sx[NS_MAX], sy[NS_MAX], sb[NS_MAX], n = 0;
    float sph[NS_MAX];
    const uint32_t sky = ns_level[0];
    for (int y = 1; y < h - 1 && n < NS_MAX; y++)
        for (int x = 1; x < w - 1 && n < NS_MAX; x++) {
            uint32_t c = img[y * pitch + x], arm;
            if (c == ns_level[3]) arm = ns_level[2];
            else if (c == ns_level[2]) arm = ns_level[1];
            else continue;
            if (img[(y - 1) * pitch + x] != arm || img[(y + 1) * pitch + x] != arm ||
                img[y * pitch + x - 1] != arm || img[y * pitch + x + 1] != arm) continue;
            if (img[(y - 1) * pitch + x - 1] != sky || img[(y - 1) * pitch + x + 1] != sky ||
                img[(y + 1) * pitch + x - 1] != sky || img[(y + 1) * pitch + x + 1] != sky) continue;
            sx[n] = x;
            sy[n] = y;
            sb[n] = c == ns_level[3] ? 3 : 2;
            n++;
        }
    /* each star's twinkle phase: the one it had last frame (the nearest within 6 px), or a new one */
    for (int i = 0; i < n; i++) {
        int best = -1, bd = 7;
        for (int j = 0; j < np; j++) {
            int d = abs(px[j] - sx[i]) + abs(py[j] - sy[i]);
            if (d < bd) { bd = d; best = j; }
        }
        sph[i] = best >= 0 ? pph[best] : hash01(++seq * 2654435761u ^ (uint32_t)frame);
    }
    for (int i = 0; i < n; i++) {
        px[i] = sx[i];
        py[i] = sy[i];
        pph[i] = sph[i];
    }
    np = n;
    bool new_frame = frame != last_frame;
    last_frame = frame;
    /* (the game blinks its stars itself, swapping their colours every 6 frames: they are found
     * only while lit, so the sky counts as starry while stars were found in the last 30 frames) */
    static uint64_t seen_at;
    static bool seen;
    if (n >= NS_MIN) {
        seen = true;
        seen_at = frame;
    } else if (!seen || frame - seen_at > 30) {
        seen = false;
        streak = false;
        return 0;
    }
    /* twinkle: a quarter of a step either way of the star's own brightness, every 2.4 s */
    for (int i = 0; i < n; i++) {
        float v = (float)sb[i] + 0.3f * sinf(6.2831853f * ((float)(frame % 144) / 144.0f + sph[i]));
        int x = sx[i], y = sy[i];
        img[y * pitch + x] = ns_at(v);
        uint32_t a = ns_at(v - 1.0f);
        img[(y - 1) * pitch + x] = a;
        img[(y + 1) * pitch + x] = a;
        img[y * pitch + x - 1] = a;
        img[y * pitch + x + 1] = a;
    }
    /* the shooting star: rare, at a random point of the upper sky, down to the left or right */
    if (new_frame && !streak && (frame >= streak_end + NS_STREAK_GAP || streak_end == 0)) {
        bool go = ns_force >= 0 ? frame >= (uint64_t)ns_force
                                : hash01((uint32_t)frame * 2246822519u ^ 0x51A7u) * NS_STREAK_CHANCE < 1.0f;
        if (go) {
            ns_force = -1;
            float r1 = hash01((uint32_t)frame * 3266489917u), r2 = hash01((uint32_t)frame * 668265263u);
            float r3 = hash01((uint32_t)frame * 374761393u);
            streak = true;
            streak_start = frame;
            sx0 = (float)w * (0.2f + 0.6f * r1);
            sy0 = 10.0f + 40.0f * r2;
            svx = r3 < 0.5f ? -100.0f : 100.0f;
            svy = 40.0f;
        }
    }
    if (streak) {
        float u = (float)(frame - streak_start) / (float)NS_STREAK_FRAMES;
        if (u > 1.0f) {
            streak = false;
            streak_end = frame;
        } else {
            float fade = powf(sinf(3.14159265f * u), 0.7f), d = sqrtf(svx * svx + svy * svy);
            float hx = sx0 + svx * u, hy = sy0 + svy * u;
            for (int i = 0; i < NS_STREAK_LEN; i++) {
                int x = (int)lroundf(hx - svx / d * (float)i), y = (int)lroundf(hy - svy / d * (float)i);
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                uint32_t p = img[y * pitch + x];
                int r = p >> 16 & 0xFF, g = p >> 8 & 0xFF, b = p & 0xFF;
                if (r > 0x24 || g > 0x24 || b < 0x24) continue;   /* (the sky's blues only) */
                float a = 0.55f * fade * powf(1.0f - (float)i / NS_STREAK_LEN, 1.5f);
                img[y * pitch + x] = 0xFF000000u | (uint32_t)clampf((float)r + 255.0f * a) << 16 |
                                     (uint32_t)clampf((float)g + 255.0f * a) << 8 | (uint32_t)clampf((float)b + 224.0f * a);
            }
        }
    }
    return 1;
}

/* Sail sway on Stage 5's ship (area $32, Death): the grey sails furled and hanging under its yards move
 * in the wind, in one of nine styles picked at random each time they come into view after a
 * while away (entering the area, loading a state): 1 a barely-there lean, 2 a gentle sway with
 * a livelier hem, 3 only the hem stirring, 4 still air with now and then a soft gust, 5 a slow
 * ripple along the cloth, 7 waves running down it, 8 a sway with a fluttering hem, 9 gusty,
 * 10 a storm (the numbers of the mock-ups they came from).
 * A sail: a connected run of the background's greys (sprites left out: the mast and its
 * metal bands, Richter, the HUD), less each stair step crossing it (a step: its top bar,
 * $DADADA $919191 $B6B6B6 x4 $919191 $DADADA, or 5 px or more of $FFFFFF / $DADADA, and
 * the 9 rows of stem under it), 120 px or more,
 * hanging from a yard (brown above it).
 * Each column of a sail hangs from its top: the further down, the more it moves. Each sail
 * moves on its own: the phases come from where it is in the level (smoothly, so nothing jumps
 * as the picture scrolls), the two swings have unrelated periods, and gusts reach each at its
 * own moment. Where cloth moves off, what is behind it shows (vdc_behind_pixels: the mast
 * where a sail hangs in front of it, else the night sky). */
#define RIG_MAX 32
#define RIG_SKY 0xFF000024u
static const int RIG_STYLES[9] = {1, 2, 3, 4, 5, 7, 8, 9, 10};
static int rig_force = -1;
void rigging_sway_style(int style) { rig_force = style; }

static bool rig_grey(uint32_t p)
{
    int r = p >> 16 & 0xFF, g = p >> 8 & 0xFF, b = p & 0xFF;
    return r == g && g == b && r > 0;
}

static float rig_gust(float ph, float width)
{
    float d = ph - floorf(ph) - 0.5f;   /* (-0.5..0.5) */
    return expf(-(d / width) * (d / width));
}

int rigging_sway(uint32_t *img, int w, int h, int pitch, uint64_t frame, int world_x, int world_y,
                 const uint8_t *sprite, const uint32_t *behind)
{
    static uint8_t mask[PCE_FB_MAX_W * PCE_FB_MAX_H];
    static int8_t comp[PCE_FB_MAX_W * PCE_FB_MAX_H];
    static int stack[PCE_FB_MAX_W * PCE_FB_MAX_H];
    static int16_t top[RIG_MAX][PCE_FB_MAX_W], bot[RIG_MAX][PCE_FB_MAX_W];
    static uint32_t row[PCE_FB_MAX_W];
    static uint64_t last_seen;
    static bool seen_once;
    static int style = 1;
    if (w > PCE_FB_MAX_W || h > PCE_FB_MAX_H) return 0;
    /* the greys of the background, less the stair steps */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            mask[y * w + x] = !sprite[y * PCE_FB_MAX_W + x] && rig_grey(img[y * pitch + x]);
    /* (a step's top as the ship draws it: $DADADA $919191 $B6B6B6 x4 $919191 $DADADA) */
    static const uint32_t STEP_TOP[8] = {0xFFDADADAu, 0xFF919191u, 0xFFB6B6B6u, 0xFFB6B6B6u,
                                         0xFFB6B6B6u, 0xFFB6B6B6u, 0xFF919191u, 0xFFDADADAu};
    for (int y = 0; y < h; y++)
        for (int x = 0; x + 8 <= w; x++) {
            if (img[y * pitch + x] != STEP_TOP[0] || memcmp(&img[y * pitch + x], STEP_TOP, sizeof STEP_TOP)) continue;
            for (int yy = y - 1; yy <= y + 9 && yy < h; yy++)
                for (int xx = x - 2; xx <= x + 9 && xx < w; xx++)
                    if (yy >= 0 && xx >= 0) mask[yy * w + xx] = 0;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w;) {
            uint32_t p = img[y * pitch + x];
            if (p != 0xFFFFFFFFu && p != 0xFFDADADAu) {
                x++;
                continue;
            }
            int e = x;
            while (e < w && (img[y * pitch + e] == 0xFFFFFFFFu || img[y * pitch + e] == 0xFFDADADAu)) e++;
            if (e - x >= 5)
                for (int yy = y - 1; yy <= y + 9 && yy < h; yy++)
                    for (int xx = x - 2; xx <= e + 1 && xx < w; xx++)
                        if (yy >= 0 && xx >= 0) mask[yy * w + xx] = 0;
            x = e;
        }
    /* the sails: connected runs of 120 px or more */
    memset(comp, -1, (size_t)w * (size_t)h);
    int n = 0, bx0[RIG_MAX], bx1[RIG_MAX], by0[RIG_MAX], by1[RIG_MAX];
    for (int y = 0; y < h && n < RIG_MAX; y++)
        for (int x = 0; x < w && n < RIG_MAX; x++) {
            if (!mask[y * w + x] || comp[y * w + x] >= 0) continue;
            int sp = 0, cnt = 0, x0 = x, x1 = x, y0 = y, y1 = y;
            stack[sp++] = y * w + x;
            comp[y * w + x] = (int8_t)n;
            while (sp) {
                int q = stack[--sp], qx = q % w, qy = q / w;
                cnt++;
                if (qx < x0) x0 = qx;
                if (qx > x1) x1 = qx;
                if (qy < y0) y0 = qy;
                if (qy > y1) y1 = qy;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = qx + dx, ny = qy + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        int k = ny * w + nx;
                        if (!mask[k] || comp[k] >= 0) continue;
                        comp[k] = (int8_t)n;
                        stack[sp++] = k;
                    }
            }
            if (cnt < 120) {   /* (too small: not a sail; its pixels left as they are) */
                for (int yy = y0; yy <= y1; yy++)
                    for (int xx = x0; xx <= x1; xx++)
                        if (comp[yy * w + xx] == n) comp[yy * w + xx] = RIG_MAX;   /* (seen, none) */
                continue;
            }
            bx0[n] = x0, bx1[n] = x1, by0[n] = y0, by1[n] = y1;
            for (int xx = 0; xx < w; xx++) top[n][xx] = bot[n][xx] = -1;
            for (int yy = y0; yy <= y1; yy++)
                for (int xx = x0; xx <= x1; xx++)
                    if (comp[yy * w + xx] == n) {
                        if (top[n][xx] < 0) top[n][xx] = (int16_t)yy;
                        bot[n][xx] = (int16_t)yy;
                    }
            /* a sail hangs from a yard: brown (the yard's wood) within 3 px above the top of a
             * fifth of its columns or more, or its top cut off by the picture's (its yard above
             * it); not the deck's railing, grey too, with nothing brown above */
            int cols = 0, hung = 0;
            for (int xx = x0; xx <= x1; xx++) {
                if (top[n][xx] < 0) continue;
                cols++;
                for (int k = 1; k <= 3; k++) {
                    int yy = top[n][xx] - k;
                    if (yy < 0) break;
                    uint32_t q = img[yy * pitch + xx];
                    int qr = q >> 16 & 0xFF, qg = q >> 8 & 0xFF, qb = q & 0xFF;
                    if (qr > qb + 0x20 && qr >= qg && qr > 0x40) {
                        hung++;
                        break;
                    }
                }
            }
            if (y0 > 0 && hung * 5 < cols) {
                for (int yy = y0; yy <= y1; yy++)
                    for (int xx = x0; xx <= x1; xx++)
                        if (comp[yy * w + xx] == n) comp[yy * w + xx] = RIG_MAX;   /* (seen, none) */
                continue;
            }
            n++;
        }
    if (n == 0) return 0;
    /* a style for this visit */
    if (rig_force >= 0) style = rig_force;
    else if (!seen_once || frame - last_seen > 120)
        style = RIG_STYLES[(int)(hash01((uint32_t)frame * 2654435761u ^ 0xA11Du) * 8.999f)];
    seen_once = true;
    last_seen = frame;
    const float TAU = 6.2831853f, t = (float)frame;
    for (int y = 0; y < h; y++) {
        uint32_t *line = &img[y * pitch];
        memcpy(row, line, (size_t)w * sizeof *row);
        for (int x = 0; x < w; x++) {
            /* the sail this pixel belongs to, or is next to (within 4 px of its columns) */
            int c = comp[y * w + x];
            if (c < 0 || c >= RIG_MAX) {
                c = -1;
                for (int s = 0; s < n && c < 0; s++)
                    if (x >= bx0[s] - 4 && x <= bx1[s] + 4 && y >= by0[s] && y <= by1[s] + 1) c = s;
                if (c < 0) continue;
            }
            int k = x;
            if (top[c][k] < 0)
                for (int dd = 1; dd <= 4; dd++) {
                    if (x - dd >= 0 && top[c][x - dd] >= 0) { k = x - dd; break; }
                    if (x + dd < w && top[c][x + dd] >= 0) { k = x + dd; break; }
                }
            if (top[c][k] < 0) continue;
            float hgt = (float)(bot[c][k] - top[c][k]);
            if (hgt < 1.0f) hgt = 1.0f;
            float d = ((float)y - (float)top[c][k]) / hgt;
            if (d <= 0.0f) continue;
            if (d > 1.0f) d = 1.0f;
            /* this spot's own phases and strength (smooth across the level) */
            float wx = (float)(x + world_x), wy = (float)(y + world_y);
            float p1 = 0.5f * sinf(wx / 150.0f + wy / 97.0f) + 0.5f * sinf(wy / 61.0f - wx / 233.0f);
            float p2 = 0.5f * sinf(wx / 121.0f - wy / 83.0f + 1.3f) + 0.5f * sinf(wx / 197.0f + 2.1f);
            float p3 = 0.5f * sinf(wy / 71.0f + wx / 173.0f + 0.7f) + 0.5f;
            float g0 = 0.5f * sinf(wx / 109.0f + wy / 139.0f + 2.9f) + 0.5f;
            float wgt = 0.6f + 0.4f * (0.5f + 0.5f * sinf(wx / 89.0f + wy / 113.0f));
            float sway = sinf(TAU * (t / 380.0f + p1)) + 0.35f * sinf(TAU * (t / 167.0f + p2));
            float d14 = powf(d, 1.4f), d15 = powf(d, 1.5f), d3 = d * d * d, dx;
            switch (style) {
            case 1: dx = 1.0f * wgt * sway * d15; break;
            case 2: dx = 1.4f * wgt * sway * d14 + 0.4f * sinf(TAU * (t / 144.0f + p3) + wx / 7.0f) * d3; break;
            case 3: dx = 1.3f * wgt * sinf(TAU * (t / 167.0f + p3) + wx / 9.0f) * d3; break;
            case 4: dx = (0.4f * sway + 1.8f * rig_gust(t / 430.0f + g0, 0.09f) * sinf(TAU * (t / 108.0f + p2))) * wgt * d15; break;
            case 5: dx = 1.2f * wgt * sinf(TAU * (t / 380.0f + p1) - wx / 18.0f) * d14; break;
            case 7: dx = 2.0f * wgt * sinf(TAU * (t / 216.0f + p1) - d * 3.0f) * powf(d, 1.2f); break;
            case 8: dx = 2.0f * wgt * sway * d14 + 1.0f * sinf(TAU * (t / 72.0f + p3) + wx / 5.0f) * d3; break;
            case 9: {
                float g = fmaxf(rig_gust(t / 430.0f + g0, 0.12f), rig_gust(t / 430.0f + g0 + 0.5f, 0.08f));
                dx = (0.6f * sway + 3.2f * g * sinf(TAU * (t / 86.0f + p2) - d * 2.0f)) * wgt * powf(d, 1.3f);
                break;
            }
            default: dx = (3.0f * sway + 1.5f * sinf(TAU * (t / 54.0f + p3) + wx / 4.0f - d * 3.0f)) * wgt * powf(d, 1.2f); break;
            }
            int sx = x - (int)lroundf(dx);
            if (sx >= 0 && sx < w && comp[y * w + sx] == c) line[x] = row[sx];
            else if (comp[y * w + x] == c) line[x] = behind ? behind[y * PCE_FB_MAX_W + x] : RIG_SKY;
        }
    }
    return n;
}
