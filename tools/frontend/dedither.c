/* Smooth dithering: see dedither.h. */
#include "dedither.h"

#include <stdbool.h>
#include <string.h>

enum {
    NEAR = 40,          /* each channel within this of the pixel's own (one level of 8 is 36) */
    MAX_COLOURS = 6,
    MW = 512, MH = 256, /* the largest picture */
    PX = 4, PY = 2,     /* the border around the picture's copy (never near anything) */
    OUT = 512,          /* the level index of the border */
};

/* The picture's copy with a border, and each pixel's colour as levels: 3 bits a channel,
 * the nearest of the console's 8 (the colours between them during a smooth fade go to the
 * nearest). Two colours are near when no channel is more than one level apart. */
static uint32_t pic[MH + 2 * PY][MW + 2 * PX];
static uint16_t lv[MH + 2 * PY][MW + 2 * PX];
static uint8_t near_lut[513][512 / 8 + 1];
static uint8_t edge[MH][MW], rows[MH][MW];

static void init_lut(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int a = 0; a < 512; a++)
        for (int b = 0; b < 512; b++) {
            bool n = true;
            for (int s = 0; s < 9; s += 3) {
                int d = (a >> s & 7) - (b >> s & 7);
                if (d < -1 || d > 1) n = false;
            }
            if (n) near_lut[a][b >> 3] |= (uint8_t)(1u << (b & 7));
        }
}

static inline bool near(int a, int b) { return near_lut[a][b >> 3] >> (b & 7) & 1; }

static inline int level(uint32_t c)
{
    int r = (int)(c >> 16 & 0xFF), g = (int)(c >> 8 & 0xFF), b = (int)(c & 0xFF);
    return ((r * 7 + 127) / 255) << 6 | ((g * 7 + 127) / 255) << 3 | ((b * 7 + 127) / 255);
}

static uint32_t blend(int x, int y)
{
    static const int wt[5] = {1, 2, 2, 2, 1};
    int cx = x + PX, cy = y + PY;
    uint32_t c = pic[cy][cx];
    int cl = lv[cy][cx];
    /* the 9 x 5 pixels around (x, y) near its colour; the blend's window is the middle 5 x 5 */
    bool ok[5][9];
    for (int j = 0; j < 5; j++) {
        const uint16_t *l = &lv[cy + j - 2][cx - 4];
        for (int i = 0; i < 9; i++) ok[j][i] = near(cl, l[i]);
    }
#define LV(j, i) lv[cy + (j) - 2][cx + (i) - 4]
#define PIX(j, i) pic[cy + (j) - 2][cx + (i) - 4]

    /* the colours in the window (as levels) and how many of each: counted in a table marked
     * for this pixel, not searched for */
    static uint32_t mark[513], stamp;
    static uint8_t count[513];
    int col[MAX_COLOURS], n = 0;
    stamp++;
    for (int j = 0; j < 5; j++)
        for (int i = 2; i < 7; i++) {
            if (!ok[j][i]) continue;
            int q = LV(j, i);
            if (mark[q] != stamp) {
                if (n == MAX_COLOURS) return c;
                mark[q] = stamp;
                count[q] = 0;
                col[n++] = q;
            }
            count[q]++;
        }
    if (n < 2) return c;

    /* every colour but the most common is single dots */
    int major = col[0];
    for (int k = 1; k < n; k++)
        if (count[col[k]] > count[major]) major = col[k];
    for (int j = 0; j < 5; j++)
        for (int i = 2; i < 7; i++) {
            int q = LV(j, i);
            if (!ok[j][i] || q == major) continue;
            if ((i > 2 && ok[j][i - 1] && LV(j, i - 1) == q) || (i < 6 && ok[j][i + 1] && LV(j, i + 1) == q) ||
                (j > 0 && ok[j - 1][i] && LV(j - 1, i) == q) || (j < 4 && ok[j + 1][i] && LV(j + 1, i) == q))
                return c;
        }

    /* the rows repeat every 4 pixels (9 of 10 pairs, of at least 10) */
    int pairs = 0, same = 0;
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 5; i++)
            if (ok[j][i] && ok[j][i + 4]) {
                pairs++;
                same += LV(j, i) == LV(j, i + 4);
            }
    if (pairs < 10 || same * 10 < pairs * 9) return c;

    unsigned acc[3] = {0, 0, 0}, tw = 0;
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 5; i++) {
            if (!ok[j][i + 2]) continue;
            uint32_t q = PIX(j, i + 2);
            unsigned wgt = (unsigned)(wt[i] * wt[j]);
            tw += wgt;
            for (int k = 0; k < 3; k++) acc[k] += (q >> (8 * k) & 0xFF) * wgt;
        }
#undef PIX
#undef LV
    uint32_t out = 0xFF000000u;
    for (int k = 0; k < 3; k++) out |= ((acc[k] + tw / 2) / tw) << (8 * k);
    return out;
}

void dedither(const uint32_t *src, uint32_t *dst, int w, int h, int pitch)
{
    init_lut();
    if (w > MW) w = MW;
    if (h > MH) h = MH;
    for (int y = 0; y < h + 2 * PY; y++)
        for (int x = 0; x < w + 2 * PX; x++) {
            int sx = x - PX, sy = y - PY;
            bool inside = sx >= 0 && sx < w && sy >= 0 && sy < h;
            pic[y][x] = inside ? src[sy * pitch + sx] : 0;
            lv[y][x] = inside ? (uint16_t)level(pic[y][x]) : OUT;
        }

    /* Most of a picture is flat colour or colours far apart, where nothing is blended. Only
     * pixels within 2 of one beside a near but different colour are looked at closely: a
     * dither pattern always has such pairs. */
    for (int y = 0; y < h; y++) {
        memset(edge[y], 0, (size_t)w);
        for (int x = 0; x < w; x++) {
            uint32_t c = pic[y + PY][x + PX];
            int l = lv[y + PY][x + PX];
            uint32_t r = pic[y + PY][x + PX + 1], d = pic[y + PY + 1][x + PX];
            if (r != c && near(l, lv[y + PY][x + PX + 1])) edge[y][x] = edge[y][x + 1 < w ? x + 1 : x] = 1;
            if (d != c && near(l, lv[y + PY + 1][x + PX])) edge[y][x] = edge[y + 1 < h ? y + 1 : y][x] = 1;
        }
    }
    for (int y = 0; y < h; y++)   /* spread 2 across */
        for (int x = 0; x < w; x++) {
            uint8_t m = 0;
            for (int i = x - 2; i <= x + 2 && !m; i++) m = i >= 0 && i < w && edge[y][i];
            rows[y][x] = m;
        }
    for (int y = 0; y < h; y++) {   /* and 2 up and down */
        uint32_t *o = &dst[y * pitch];
        for (int x = 0; x < w; x++) {
            uint8_t m = 0;
            for (int j = y - 2; j <= y + 2 && !m; j++) m = j >= 0 && j < h && rows[j][x];
            o[x] = m ? blend(x, y) : pic[y + PY][x + PX];
        }
    }
}
