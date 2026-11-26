/* PC Engine video: VCE + VDC. See video.h. */
#include "video.h"

#include <string.h>


/* VDC register numbers */
enum {
    R_MAWR = 0x00, R_MARR = 0x01, R_VWR = 0x02, R_CR = 0x05, R_RCR = 0x06,
    R_BXR = 0x07, R_BYR = 0x08, R_MWR = 0x09, R_HSR = 0x0A, R_HDR = 0x0B,
    R_VSR = 0x0C, R_VDR = 0x0D, R_VCR = 0x0E, R_DCR = 0x0F, R_SOUR = 0x10,
    R_DESR = 0x11, R_LENR = 0x12, R_DVSSR = 0x13,
};

/* ---- VCE ----------------------------------------------------------------- */

static void vce_update(Vce *v, int i)
{
    uint16_t c = v->pal[i];
    uint32_t b = c & 7, r = (c >> 3) & 7, g = (c >> 6) & 7;
    if (v->ctrl & 0x80) {
        uint32_t y = (r * 30 + g * 59 + b * 11) / 100;
        r = g = b = y;
    }
    r = r * 255 / 7; g = g * 255 / 7; b = b * 255 / 7;
    v->argb[i] = 0xFF000000u | r << 16 | g << 8 | b;
}

void vce_reset(Vce *v)
{
    memset(v, 0, sizeof *v);
    for (int i = 0; i < 512; i++) vce_update(v, i);
}

void vce_write(Vce *v, uint16_t off, uint8_t val)
{
    switch (off & 7) {
    case 0:
        v->ctrl = val;
        for (int i = 0; i < 512; i++) vce_update(v, i);
        break;
    case 2: v->addr = (uint16_t)((v->addr & 0x100) | val); break;
    case 3: v->addr = (uint16_t)((v->addr & 0x0FF) | (val & 1) << 8); break;
    case 4:
        v->pal[v->addr] = (uint16_t)((v->pal[v->addr] & 0x100) | val);
        vce_update(v, v->addr);
        break;
    case 5:
        v->pal[v->addr] = (uint16_t)((v->pal[v->addr] & 0x0FF) | (val & 1) << 8);
        vce_update(v, v->addr);
        v->addr = (v->addr + 1) & 0x1FF;
        break;
    }
}

uint8_t vce_read(Vce *v, uint16_t off)
{
    switch (off & 7) {
    case 4: return (uint8_t)v->pal[v->addr];
    case 5: {
        uint8_t r = (uint8_t)(0xFE | v->pal[v->addr] >> 8);
        v->addr = (v->addr + 1) & 0x1FF;
        return r;
    }
    default: return 0xFF;
    }
}

/* ---- VDC registers --------------------------------------------------------- */

static int vram_inc(const Vdc *d)
{
    static const int inc[4] = {1, 32, 64, 128};
    return inc[(d->reg[R_CR] >> 11) & 3];
}

/* A status flag is only latched when its interrupt is enabled. Rondo depends on
 * this: with raster IRQs disabled, a stray RR bit arriving with VD would send its
 * vblank handler down the raster-only path and the frame logic would never run. */
static void raise(Vdc *d, uint8_t flag, bool enabled)
{
    if (!enabled) return;
    d->status |= flag;
    d->irq = true;
}

static void vram_dma(Vdc *d)
{
    uint16_t dcr = d->reg[R_DCR];
    int sinc = (dcr & 0x04) ? -1 : 1, dinc = (dcr & 0x08) ? -1 : 1;
    uint16_t src = d->reg[R_SOUR], dst = d->reg[R_DESR];
    uint32_t len = (uint32_t)d->reg[R_LENR] + 1;
    for (uint32_t i = 0; i < len; i++) {
        if (dst < 0x8000) d->vram[dst] = d->vram[src & 0x7FFF];
        src = (uint16_t)(src + sinc);
        dst = (uint16_t)(dst + dinc);
    }
    d->reg[R_SOUR] = src;
    d->reg[R_DESR] = dst;
    d->reg[R_LENR] = 0xFFFF;
    raise(d, VDC_ST_DV, (dcr & 0x02) != 0);
}

void vdc_reset(Vdc *d)
{
    memset(d, 0, sizeof *d);
    /* standard 256x240 timing, as left by the System Card */
    d->reg[R_MWR] = 0x0010;
    d->reg[R_HSR] = 0x0202;
    d->reg[R_HDR] = 0x041F;
    d->reg[R_VSR] = 0x0F02;
    d->reg[R_VDR] = 0x00EF;
    d->reg[R_VCR] = 0x0003;
    d->fb_w = 256;
    d->fb_h = 240;
}

void vdc_write(Vdc *d, uint16_t off, uint8_t val)
{
    switch (off & 3) {
    case 0:
        d->ar = val & 0x1F;
        return;
    case 1:
        return;
    case 2:
        d->reg[d->ar] = (uint16_t)((d->reg[d->ar] & 0xFF00) | val);
        if (d->ar == R_BYR) { d->bg_y = d->reg[R_BYR] & 0x1FF; }
        return;
    case 3:
        d->reg[d->ar] = (uint16_t)((d->reg[d->ar] & 0x00FF) | val << 8);
        switch (d->ar) {
        case R_MARR:
            d->read_latch = d->vram[d->reg[R_MARR] & 0x7FFF];
            break;
        case R_VWR:
            if (d->reg[R_MAWR] < 0x8000) d->vram[d->reg[R_MAWR]] = d->reg[R_VWR];
            d->reg[R_MAWR] = (uint16_t)(d->reg[R_MAWR] + vram_inc(d));
            break;
        case R_BYR:
            d->bg_y = d->reg[R_BYR] & 0x1FF;
            break;
        case R_LENR:
            vram_dma(d);
            break;
        case R_DVSSR:
            d->satb_pending = true;
            break;
        }
        return;
    }
}

uint8_t vdc_read(Vdc *d, uint16_t off)
{
    switch (off & 3) {
    case 0: {
        uint8_t s = d->status;
        d->status = 0;
        d->irq = false;
        return s;
    }
    case 2:
        return d->ar == R_VWR ? (uint8_t)d->read_latch : 0;
    case 3:
        if (d->ar == R_VWR) {
            uint8_t v = (uint8_t)(d->read_latch >> 8);
            d->reg[R_MARR] = (uint16_t)(d->reg[R_MARR] + vram_inc(d));
            d->read_latch = d->vram[d->reg[R_MARR] & 0x7FFF];
            return v;
        }
        return 0;
    default:
        return 0;
    }
}

/* ---- timing ---------------------------------------------------------------- */

void vdc_line_start(Vdc *d, int line)
{
    if (line == 0) {
        int vsw = d->reg[R_VSR] & 0x1F;
        int vds = d->reg[R_VSR] >> 8;
        int vdw = d->reg[R_VDR] & 0x1FF;
        d->display_start = vsw + 1 + vds + 2;
        d->display_end = d->display_start + vdw + 1;
        if (d->display_end > PCE_LINES_PER_FRAME) d->display_end = PCE_LINES_PER_FRAME;
        d->first_line = true;
        int w = ((d->reg[R_HDR] & 0x7F) + 1) * 8;
        d->fb_w = w > PCE_FB_MAX_W ? PCE_FB_MAX_W : w;
        int h = d->display_end - d->display_start;
        d->fb_h = h > PCE_FB_MAX_H ? PCE_FB_MAX_H : (h < 0 ? 0 : h);
    }

    /* The raster counter is $40 on the picture's first line and counts every line until the
     * next picture starts, through vertical blank: a compare can match below the picture.
     * (Rondo of Blood sets $40 + the picture's height, the first line after it, to put Richter's
     * palette back after drawing his reflection in the water blue.) */
    int rc = (line >= d->display_start ? line - d->display_start : line + PCE_LINES_PER_FRAME - d->display_start) + 0x40;
    if ((d->reg[R_RCR] & 0x3FF) == rc)
        raise(d, VDC_ST_RR, (d->reg[R_CR] & 0x04) != 0);
}

void vdc_vblank(Vdc *d)
{
    /* After the line's raster compare, as on the console: a game reads the status once per
     * interrupt and handles one cause, so the two must come one after the other. */
    uint16_t dcr = d->reg[R_DCR];
    if (d->satb_pending || (dcr & 0x10)) {
        uint16_t src = d->reg[R_DVSSR];
        for (int i = 0; i < 256; i++) d->satb[i] = d->vram[(src + i) & 0x7FFF];
        d->satb_pending = false;
        raise(d, VDC_ST_DS, (dcr & 0x01) != 0);
    }
    raise(d, VDC_ST_VD, (d->reg[R_CR] & 0x08) != 0);
}

/* ---- rendering ------------------------------------------------------------- */

static uint16_t row_scroll[PCE_FB_MAX_H];
const uint16_t *vdc_row_scroll(void) { return row_scroll; }
static uint8_t sprite_px[PCE_FB_MAX_W * PCE_FB_MAX_H];
const uint8_t *vdc_sprite_pixels(void) { return sprite_px; }
static uint32_t behind_px[PCE_FB_MAX_W * PCE_FB_MAX_H];
const uint32_t *vdc_behind_pixels(void) { return behind_px; }
static uint64_t sprite_hide;
void vdc_set_sprite_hide(uint64_t mask) { sprite_hide = mask; }

static void render_bg(Vdc *d, int y, int w, uint16_t *out)
{
    static const int map_w[4] = {32, 64, 128, 128};
    uint16_t mwr = d->reg[R_MWR];
    int mw = map_w[(mwr >> 4) & 3];
    int mh = (mwr & 0x40) ? 64 : 32;
    int py = y & (mh * 8 - 1);
    int row = py & 7;
    int sx = d->reg[R_BXR] & 0x3FF;
    const uint16_t *map = &d->vram[(py >> 3) * mw];
    for (int x = 0; x < w; x++) {
        int px = (sx + x) & (mw * 8 - 1);
        uint16_t e = map[px >> 3];
        uint32_t base = ((uint32_t)(e & 0x7FF) * 16 + row) & 0x7FFF;
        uint16_t p01 = d->vram[base], p23 = d->vram[(base + 8) & 0x7FFF];
        int bit = 7 - (px & 7);
        int c = ((p01 >> bit) & 1) | ((p01 >> (8 + bit)) & 1) << 1 |
                ((p23 >> bit) & 1) << 2 | ((p23 >> (8 + bit)) & 1) << 3;
        out[x] = c ? (uint16_t)((e >> 12) << 4 | c) : 0;
    }
}

/* out: 0 = none, else 0x100 | palette<<4 | color, bit 15 = in front of BG. */
static void render_sprites(Vdc *d, int y, int w, uint16_t *out)
{
    int count = 0;
    bool spr0_drawn[PCE_FB_MAX_W];
    memset(spr0_drawn, 0, sizeof spr0_drawn);
    for (int i = 0; i < 64; i++) {
        const uint16_t *s = &d->satb[i * 4];
        int sy = (s[0] & 0x3FF) - 64;
        uint16_t attr = s[3];
        int cgy = (attr >> 12) & 3;
        int h = cgy == 0 ? 16 : cgy == 1 ? 32 : 64;
        if (y < sy || y >= sy + h) continue;
        if (++count > 16) {   /* the hardware shows 16 sprites per line */
            raise(d, VDC_ST_OR, (d->reg[R_CR] & 0x02) != 0);
            break;
        }
        int sw = (attr & 0x0100) ? 32 : 16;
        int sx = (s[1] & 0x3FF) - 32;
        int pattern = (s[2] >> 1) & 0x3FF;
        if (sw == 32) pattern &= ~1;
        if (h == 32) pattern &= ~2;
        if (h == 64) pattern &= ~6;
        int r = y - sy;
        if (attr & 0x8000) r = h - 1 - r;
        bool xflip = (attr & 0x0800) != 0;
        uint16_t pal = (uint16_t)(0x100 | (attr & 0xF) << 4);
        uint16_t front = (attr & 0x80) ? 0x8000 : 0;
        bool hidden = (sprite_hide >> i) & 1;
        for (int c = 0; c < sw; c++) {
            int x = sx + c;
            if (x < 0 || x >= w) continue;
            int cc = xflip ? sw - 1 - c : c;
            int cell = pattern + (r >> 4) * 2 + (cc >> 4);
            uint32_t a = ((uint32_t)cell * 64 + (r & 15)) & 0x7FFF;
            int bit = 15 - (cc & 15);
            int col = ((d->vram[a] >> bit) & 1) | ((d->vram[(a + 16) & 0x7FFF] >> bit) & 1) << 1 |
                      ((d->vram[(a + 32) & 0x7FFF] >> bit) & 1) << 2 |
                      ((d->vram[(a + 48) & 0x7FFF] >> bit) & 1) << 3;
            if (!col) continue;
            if (i == 0) spr0_drawn[x] = true;
            else if (spr0_drawn[x]) raise(d, VDC_ST_CR, (d->reg[R_CR] & 0x01) != 0);
            if (!out[x] && !hidden) out[x] = (uint16_t)(front | pal | col);
        }
    }
}

void vdc_line_end(Vdc *d, const Vce *vce, int line)
{
    if (line < d->display_start || line >= d->display_end) return;
    int row = line - d->display_start;

    int y;
    if (d->first_line) {
        y = d->reg[R_BYR] & 0x1FF;
        d->first_line = false;
    } else {
        y = (d->bg_y + 1) & 0x1FF;
    }
    d->bg_y = y;

    if (row >= d->fb_h) return;
    int w = d->fb_w;
    uint16_t bg[PCE_FB_MAX_W], sp[PCE_FB_MAX_W];
    memset(bg, 0, sizeof bg);
    memset(sp, 0, sizeof sp);
    uint16_t cr = d->reg[R_CR];
    row_scroll[row] = d->reg[R_BXR] & 0x3FF;
    if (cr & 0x80) render_bg(d, y, w, bg);
    if (cr & 0x40) render_sprites(d, row, w, sp);

    uint32_t *dst = &d->fb[row * PCE_FB_MAX_W];
    uint8_t *spx = &sprite_px[row * PCE_FB_MAX_W];
    uint32_t *bhx = &behind_px[row * PCE_FB_MAX_W];
    for (int x = 0; x < w; x++) {
        uint16_t s = sp[x];
        spx[x] = s && ((s & 0x8000) || !bg[x]);
        /* (behind: a sprite's pixel the background hides, else the background colour) */
        bhx[x] = s && !spx[x] ? vce->argb[s & 0x1FF] : vce->argb[0];
        if (spx[x]) dst[x] = vce->argb[s & 0x1FF];
        else if (bg[x]) dst[x] = vce->argb[bg[x]];
        else dst[x] = vce->argb[0];
    }
}
