/* Title art: see titleart.h. */
#include "titleart.h"

#include <string.h>

bool titleart_parse(TitleArt *t, const uint32_t *px, int w, int h)
{
    t->loaded = false;
    if (w != TITLEART_W || h < TITLEART_FILE_H) return false;
    const uint32_t *meta = px + TITLEART_H * w;
    if ((meta[0] & 0xFFFFFF) != 0x525331) return false;   /* 'R' 'S' '1' */
    for (int y = 0; y < TITLEART_H; y++)
        for (int k = 0; k < 2 * TITLEART_SIDE; k++) {
            int x = k < TITLEART_SIDE ? k : TITLEART_SIDE + 256 + (k - TITLEART_SIDE);
            uint32_t c = px[y * w + x];
            t->side[y][k] = (c >> 24) == 0 ? TITLEART_NONE : (uint16_t)((c >> 16 & 0xFF) | (c >> 8 & 1) << 8);
        }
    for (int n = 0; n < TITLEART_ROWS * TITLEART_COLS; n++) {
        uint32_t c = meta[1 + n];
        t->layout[n / TITLEART_COLS][n % TITLEART_COLS] = (uint16_t)((c >> 16 & 0xFF) | (c >> 8 & 0xFF) << 8);
    }
    t->shown = false;
    return t->loaded = true;
}

static int map_w(const Vdc *d) { static const int w[4] = {32, 64, 128, 128}; return w[(d->reg[9] >> 4) & 3]; }
static int map_h(const Vdc *d) { return (d->reg[9] & 0x40) ? 64 : 32; }

void titleart_layout(const Vdc *d, uint16_t out[TITLEART_ROWS][TITLEART_COLS])
{
    const uint16_t *rs = vdc_row_scroll(), *my = vdc_row_map_y();
    int mw = map_w(d), mh = map_h(d), rows = d->fb_h < PCE_FB_MAX_H ? d->fb_h : PCE_FB_MAX_H;
    for (int ty = 0; ty < TITLEART_ROWS; ty++) {
        int y = ty * 8 + 4;
        if (y >= rows) y = rows - 1;
        int r = (my[y] & (mh * 8 - 1)) >> 3, first = rs[y] >> 3;
        for (int c = 0; c < TITLEART_COLS; c++) out[ty][c] = d->vram[r * mw + ((first + c) & (mw - 1))];
    }
}

void titleart_entries(const Vdc *d, uint16_t *out)
{
    const uint16_t *rs = vdc_row_scroll(), *my = vdc_row_map_y();
    int mw = map_w(d), mh = map_h(d), rows = d->fb_h < PCE_FB_MAX_H ? d->fb_h : PCE_FB_MAX_H;
    for (int y = 0; y < TITLEART_H; y++)
        for (int x = 0; x < 256; x++) {
            if (y >= rows) { out[y * 256 + x] = 0; continue; }
            int py = my[y] & (mh * 8 - 1), px = (rs[y] + x) & (mw * 8 - 1);
            uint16_t e = d->vram[(py >> 3) * mw + (px >> 3)];
            uint32_t base = ((uint32_t)(e & 0x7FF) * 16 + (py & 7)) & 0x7FFF;
            uint16_t p01 = d->vram[base], p23 = d->vram[(base + 8) & 0x7FFF];
            int bit = 7 - (px & 7);
            int c = ((p01 >> bit) & 1) | ((p01 >> (8 + bit)) & 1) << 1 | ((p23 >> bit) & 1) << 2 | ((p23 >> (8 + bit)) & 1) << 3;
            out[y * 256 + x] = c ? (uint16_t)((e >> 12) << 4 | c) : 0;
        }
}

bool titleart_compose(TitleArt *t, const Pce *p, const uint32_t *fb, int h, int pitch, int ext, uint32_t *out)
{
    if (!t->loaded || h < 200 || ext <= 0 || p->vdc.fb_h < 200) return t->shown = false;
    if (ext > TITLEART_SIDE) ext = TITLEART_SIDE;
    if (h > TITLEART_H) h = TITLEART_H;

    /* the screen: its map entries stand where the file's do (most of them: a prompt
     * blinks, a stage map's name box changes; a little less once shown) */
    uint16_t now[TITLEART_ROWS][TITLEART_COLS];
    titleart_layout(&p->vdc, now);
    int same = 0;
    for (int r = 0; r < TITLEART_ROWS; r++)
        for (int c = 0; c < TITLEART_COLS; c++) same += now[r][c] == t->layout[r][c];
    int all = TITLEART_ROWS * TITLEART_COLS;
    t->shown = same * 100 >= all * (t->shown ? 75 : 85);
    if (!t->shown) return false;

    /* the palette can be up before the picture is (the title's colours are set, then its
     * background switched on): while the middle is all black, so are the sides */
    bool lit = false;
    for (int y = 4; y < h && !lit; y += 8)
        for (int x = 4; x < 256 && !lit; x += 8) lit = (fb[y * pitch + x] & 0xFFFFFF) != 0;
    int w = 256 + 2 * ext;
    for (int y = 0; y < h; y++) {
        uint32_t *o = out + y * pitch;
        memcpy(o + ext, fb + y * pitch, 256 * sizeof *o);
        /* the screen's first column can be a shade darker than the rest (unseen at a
         * screen's edge, a line beside the art): drawn as the next */
        o[ext] = o[ext + 1];
        /* (the colours this line was drawn with: the lightning changes them during the picture) */
        const uint32_t *pal = vdc_line_palette(y);
        for (int x = 0; x < ext; x++) {
            uint16_t l = t->side[y][TITLEART_SIDE - ext + x], r = t->side[y][TITLEART_SIDE + x];
            o[x] = l == TITLEART_NONE || !lit ? 0xFF000000u : pal[l & 0x1FF];
            o[w - ext + x] = r == TITLEART_NONE || !lit ? 0xFF000000u : pal[r & 0x1FF];
        }
    }
    return true;
}
