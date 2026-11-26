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

/* Background map cells written since the last vdc_bat_written_clear (host-side; see
 * video.h), whether or not the value changed */
static uint8_t bat_written[VDC_BAT_WATCH];
const uint8_t *vdc_bat_written(void) { return bat_written; }
void vdc_bat_written_clear(void) { memset(bat_written, 0, sizeof bat_written); }

/* ---- VCE ----------------------------------------------------------------- */

/* Smooth fades (host-side, outside the machine state; see video.h). fade_argb is the
 * palette the picture is drawn with while they are on: worked out at each picture's start,
 * and an entry the game writes after that shows as written for the rest of the picture
 * (raster palette changes, such as Richter's reflection in the water, stay exact). */
static bool fade_on;
static uint32_t fade_argb[512];
const uint32_t *vce_shown_argb(const Vce *v) { return fade_on ? fade_argb : v->argb; }
/* the colours each 16 lines of the last picture were drawn with (vdc_line_palette) */
static uint32_t line_pal[(PCE_FB_MAX_H + 15) / 16][512];
const uint32_t *vdc_line_palette(int row)
{
    if (row < 0) row = 0;
    if (row >= PCE_FB_MAX_H) row = PCE_FB_MAX_H - 1;
    return line_pal[row >> 4];
}
static unsigned fade_frame;
static struct FadeEntry {
    uint16_t val;           /* the 9-bit value last seen at a picture's start */
    uint32_t raw;           /* its colour */
    uint32_t from, to;      /* blending from -> to over len pictures from start */
    unsigned start, last;   /* when the blend started; when val last changed */
    uint8_t len;
    bool stepped;           /* val's last change was a fade step (each channel 1 level at most) */
    int8_t dir[3];          /* the way each channel last moved (0 not yet) */
} fade[512];

void vce_set_smooth_fades(bool on)
{
    if (on && !fade_on) memset(fade, 0, sizeof fade);   /* starts from what is there now */
    fade_on = on;
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned t /* 0-256 */)
{
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        int ca = (int)(a >> s & 0xFF), cb = (int)(b >> s & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * (int)t / 256) << s;
    }
    return out;
}

/* The blend's first picture is already 1/len of the way: the last step shows len - 1
 * pictures after the game's */
static uint32_t fade_shown(const struct FadeEntry *e, unsigned now)
{
    unsigned k = now - e->start + 1;
    return k < e->len ? mix(e->from, e->to, k * 256u / e->len) : e->to;
}

/* At a picture's start. The game fades by stepping each channel of each colour one level
 * (of 8) every few pictures; a step that follows another the same way is blended in over as
 * many pictures as the last one took, so a fade passes through 256 levels a channel instead
 * of 8. It ends a step's length (at most 16 pictures) less one after the game's own. Any
 * other change shows at once, as does the first step of a fade (its pace is not known yet). */
static void fade_begin(const Vce *v)
{
    unsigned now = ++fade_frame;
    for (int i = 0; i < 512; i++) {
        struct FadeEntry *e = &fade[i];
        uint32_t raw = v->argb[i];
        uint32_t shown = fade_shown(e, now);
        if (v->pal[i] != e->val || e->last == 0) {
            int d[3], step = 1;
            for (int c = 0; c < 3; c++) {
                d[c] = (v->pal[i] >> (3 * c) & 7) - (e->val >> (3 * c) & 7);
                if (d[c] < -1 || d[c] > 1) step = 0;
            }
            unsigned gap = now - e->last;
            bool same_way = step;
            for (int c = 0; c < 3; c++)
                if (d[c] && e->dir[c] && (d[c] > 0) != (e->dir[c] > 0)) same_way = false;
            if (e->last && e->stepped && same_way && gap >= 2 && gap <= 16) {
                e->from = shown;
                e->len = (uint8_t)gap;
            } else {
                e->from = raw;
                e->len = 0;
            }
            e->to = raw;
            e->start = now;
            for (int c = 0; c < 3; c++) e->dir[c] = !step ? 0 : d[c] ? (int8_t)d[c] : e->dir[c];
            e->stepped = step;
            e->val = v->pal[i];
            e->last = now;
            e->raw = raw;
            shown = fade_shown(e, now);
        } else if (raw != e->raw) {
            /* the same value in another colour (grayscale turned on or off): at once */
            e->raw = e->from = e->to = shown = raw;
            e->len = 0;
        }
        fade_argb[i] = shown;
    }
}

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
    fade_argb[i] = v->argb[i];
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
        if (dst < VDC_BAT_WATCH) bat_written[dst] = 1;
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
            if (d->reg[R_MAWR] < VDC_BAT_WATCH) bat_written[d->reg[R_MAWR]] = 1;
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

/* Widescreen's shift (host-side, see video.h): asked for, and latched for the frame. */
static int wide_shift, fb_shift;
void vdc_set_wide_shift(int shift) { wide_shift = shift; }
int vdc_fb_shift(void) { return fb_shift; }

/* Side tiles (see video.h): map entries drawn past the original 256 pixels instead of the
 * map's own, and the map row each displayed line was drawn from. */
static const uint16_t *side_left, *side_right;
static uint16_t row_map_y[PCE_FB_MAX_H];
void vdc_set_side_tiles(const uint16_t *left, const uint16_t *right)
{
    side_left = left;
    side_right = right;
}
const uint16_t *vdc_row_map_y(void) { return row_map_y; }
static uint8_t sprite_px[PCE_FB_MAX_W * PCE_FB_MAX_H];
const uint8_t *vdc_sprite_pixels(void) { return sprite_px; }
static uint32_t behind_px[PCE_FB_MAX_W * PCE_FB_MAX_H];
const uint32_t *vdc_behind_pixels(void) { return behind_px; }
static bool side_real;
void vdc_set_side_real(bool on) { side_real = on; }
static int side_sprites;   /* VDC_SIDE_LEFT | VDC_SIDE_RIGHT: sprites drawn there after all */
void vdc_set_side_sprites(int sides) { side_sprites = sides; }
static int side_fade;   /* VDC_SIDE_LEFT | VDC_SIDE_RIGHT */
void vdc_set_side_fade(int sides) { side_fade = sides; }
static int side_flip;
void vdc_set_side_flip(int sides) { side_flip = sides; }
static const uint16_t *map_fill;
static bool map_fill_all;
void vdc_set_map_fill(const uint16_t *fill) { map_fill = fill; }
void vdc_set_map_fill_all(bool all) { map_fill_all = all; }
static const uint16_t *map_patch;
void vdc_set_map_patch(const uint16_t *patch) { map_patch = patch; }
static const uint16_t *side_vram;
static int side_vram_side;
static const int16_t *side_vram_dx, *side_vram_dy;
/* (a jump in a line's scroll from the frame before, seen as the line is drawn: the host
 * follows it in dx/dy from the next frame on, this one is drawn with it here) */
static int side_vram_jx, side_vram_jy;
static const uint16_t *kept_left, *kept_right;
static int kept_first, kept_last;
void vdc_set_side_kept(const uint16_t *left, const uint16_t *right, int first, int last)
{
    kept_left = left;
    kept_right = right;
    kept_first = first;
    kept_last = last;
}
void vdc_set_side_vram(const uint16_t *vram, int side, const int16_t *dx, const int16_t *dy)
{
    side_vram = vram;
    side_vram_side = side;
    side_vram_dx = dx;
    side_vram_dy = dy;
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
        int e = d->wide < 0 ? 0 : d->wide > VDC_WIDE_MAX ? VDC_WIDE_MAX : d->wide;
        d->fb_wide = w == 256 ? e : 0;
        fb_shift = wide_shift < -d->fb_wide ? -d->fb_wide : wide_shift > d->fb_wide ? d->fb_wide : wide_shift;
        w += 2 * d->fb_wide;
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
static uint8_t row_bg_blank[PCE_FB_MAX_H], row_bg_edge[PCE_FB_MAX_H];
static const uint8_t *row_clip;
const uint16_t *vdc_row_scroll(void) { return row_scroll; }
const uint8_t *vdc_row_bg_blank(void) { return row_bg_blank; }
const uint8_t *vdc_row_bg_edge(void) { return row_bg_edge; }
void vdc_set_row_clip(const uint8_t *rows) { row_clip = rows; }
static uint64_t sprite_hide;
void vdc_set_sprite_hide(uint64_t mask) { sprite_hide = mask; }
static VdcFade sprite_fade[VDC_FADES];
static int n_sprite_fade;
void vdc_set_sprite_fades(const VdcFade *list, int n)
{
    n_sprite_fade = n < 0 ? 0 : n > VDC_FADES ? VDC_FADES : n;
    if (n_sprite_fade) memcpy(sprite_fade, list, (size_t)n_sprite_fade * sizeof *list);
}
static const uint8_t FADE_DITHER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static const VdcExtraSprite *extra_spr;
#define EXTRA_PX 0x4000   /* (a pixel of an extra sprite: kept beside the 256 pixels with side tiles) */
static int n_extra;
void vdc_set_extra_sprites(const VdcExtraSprite *list, int n) { extra_spr = list; n_extra = list ? n : 0; }
static bool hide_corner;
void vdc_set_hide_corner(bool on) { hide_corner = on; }
static int move_lo, move_hi = -1, move_dx;
void vdc_set_sprite_move(int pattern_lo, int pattern_hi, int dx)
{
    move_lo = pattern_lo;
    move_hi = pattern_hi;
    move_dx = dx;
}

/* x0: screen x of out[0] (negative in widescreen) */
static void render_bg(Vdc *d, int y, int x0, int w, uint16_t *out, int line)
{
    static const int map_w[4] = {32, 64, 128, 128};
    uint16_t mwr = d->reg[R_MWR];
    int mw = map_w[(mwr >> 4) & 3];
    int mh = (mwr & 0x40) ? 64 : 32;
    int py = y & (mh * 8 - 1);
    int row = py & 7;
    int bxr = d->reg[R_BXR] & 0x3FF;
    int sx = bxr + x0;
    const uint16_t *map = &d->vram[(py >> 3) * mw];
    /* side tiles: columns of the map before the first shown in the original 256 pixels
     * (first) and after the last (last) */
    bool sides = side_left && side_right && x0 < 0;
    int first = bxr >> 3, last = (bxr + 255) >> 3;
    const uint16_t *sl = sides ? &side_left[(py >> 3) * VDC_SIDE_TILES] : NULL;
    const uint16_t *sr = sides ? &side_right[(py >> 3) * VDC_SIDE_TILES] : NULL;
    const uint16_t *own_map = map;
    for (int x = 0; x < w; x++) {
        int px = (sx + x) & (mw * 8 - 1);
        /* (side VRAM: beyond the original 256 pixels on its side, map and tiles from it) */
        const uint16_t *v = d->vram;
        map = own_map;
        int prow = row, srow = py >> 3;
        bool on_vram = false;
        if (side_vram && (side_vram_side < 0 ? x0 + x < 0 : x0 + x >= 256)) {
            int spy = (y + side_vram_dy[line] + side_vram_jy) & (mh * 8 - 1);
            on_vram = true;
            srow = spy >> 3;
            v = side_vram;
            map = &side_vram[(spy >> 3) * mw];
            px = (sx + x + side_vram_dx[line] + side_vram_jx) & (mw * 8 - 1);
            prow = spy & 7;
        }
        uint16_t e = map[px >> 3];
        if (map_fill && w > 256 && (map_fill_all || x0 + x < 0 || x0 + x >= 256)) {
            /* beside the original 256 pixels: the map's own entry, or its fill */
            uint16_t f = map_fill[(py >> 3) * VDC_FILL_W + ((px >> 3) & (VDC_FILL_W - 1))];
            if (f != VDC_SIDE_NONE) e = f;
        }
        if (map_patch) {   /* (anywhere in the picture) */
            uint16_t f = map_patch[(py >> 3) * VDC_FILL_W + ((px >> 3) & (VDC_FILL_W - 1))];
            if (f != VDC_SIDE_NONE) e = f;
        }
        bool flip = false;
        if (sides) {
            int col = (sx + x) >> 3;   /* (arithmetic shift: floor) */
            int k = col < first ? first - 1 - col : col > last ? col - last - 1 : -1;
            if (k >= VDC_SIDE_TILES) k = VDC_SIDE_TILES - 1;
            if (side_real && col >= 0 && col < mw) k = -1;   /* the map's own, short of wrapping round */
            if (k >= 0) {
                bool lft = col < first;
                flip = (side_flip & (lft ? VDC_SIDE_LEFT : VDC_SIDE_RIGHT)) != 0;
                if (on_vram && kept_left && kept_right) {
                    /* (the room as it was: its kept side tiles past where its picture began,
                     * its own map's columns short of them) */
                    int oc = px >> 3;
                    int j = lft ? kept_first - 1 - oc : oc - kept_last - 1;
                    j = ((j % mw) + mw) % mw;
                    if (j >= mw / 2) j -= mw;
                    if (j >= 0) {
                        if (j >= VDC_SIDE_TILES) j = VDC_SIDE_TILES - 1;
                        e = (lft ? kept_left : kept_right)[srow * VDC_SIDE_TILES + j];
                    } else {
                        flip = false;   /* (e: that map's own entry) */
                    }
                } else {
                    e = lft ? sl[k] : sr[k];
                    if (e == VDC_SIDE_MAP) {
                        e = map[px >> 3];
                        flip = false;
                    }
                }
                if (e == VDC_SIDE_NONE) {
                    out[x] = 0;
                    continue;
                }
                if (e & VDC_SIDE_OWN) flip = false;   /* (the map's own entry, as it is) */
            }
        }
        uint32_t base = ((uint32_t)(e & 0x7FF) * 16 + prow) & 0x7FFF;
        uint16_t p01 = v[base], p23 = v[(base + 8) & 0x7FFF];
        int bit = flip ? (px & 7) : 7 - (px & 7);   /* (side flip: the tile drawn mirrored) */
        int c = ((p01 >> bit) & 1) | ((p01 >> (8 + bit)) & 1) << 1 |
                ((p23 >> bit) & 1) << 2 | ((p23 >> (8 + bit)) & 1) << 3;
        out[x] = c ? (uint16_t)((e >> 12) << 4 | c) : 0;
    }
}

/* out: 0 = none, else 0x100 | palette<<4 | color, bit 15 = in front of BG.
 * x0: screen x of out[0] (negative in widescreen). */
static void render_sprites(Vdc *d, int y, int x0, int w, uint16_t *out)
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
        if (++count > 16) {
            raise(d, VDC_ST_OR, (d->reg[R_CR] & 0x02) != 0);
            /* the hardware shows 16 sprites per line; widescreen shows more of the level
             * and drops the limit rather than lose sprites at the new edges */
            if (!x0) break;
        }
        int sw = (attr & 0x0100) ? 32 : 16;
        int sx = (s[1] & 0x3FF) - 32;
        /* 10-bit X: in widescreen, values past the right of any real display are
         * sprites to the left of the picture */
        if (x0 && sx >= 1024 - 32 + x0 - 32) sx -= 1024;
        int fade = 16;   /* (vdc_set_sprite_fades: by the sprite's middle) */
        for (int k = 0; k < n_sprite_fade; k++) {
            const VdcFade *f = &sprite_fade[k];
            int mx = sx + sw / 2, my = sy + h / 2;
            if (!(s[3] & 0x80) && mx >= f->x0 && mx < f->x1 && my >= f->y0 && my < f->y1 && f->level < fade)
                fade = f->level;
        }
        sx -= x0;
        int pattern = (s[2] >> 1) & 0x3FF;
        if (pattern >= move_lo && pattern <= move_hi) sx += move_dx;
        if (sw == 32) pattern &= ~1;
        if (h == 32) pattern &= ~2;
        if (h == 64) pattern &= ~6;
        int r = y - sy;
        if (attr & 0x8000) r = h - 1 - r;
        bool xflip = (attr & 0x0800) != 0;
        uint16_t pal = (uint16_t)(0x100 | (attr & 0xF) << 4);
        uint16_t front = (attr & 0x80) ? 0x8000 : 0;
        bool hidden = (sprite_hide >> i) & 1;
        /* (parked above the screen, its last rows peeking in: hide_corner; as the menu's
         * pages slide over, the parked flames move along with them, to the right of x 0) */
        if (hide_corner && sy < 0 && sy + h <= 8) hidden = true;
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
            if (fade < 16 && FADE_DITHER[y & 3][(x + x0) & 3] >= fade) continue;
            if (i == 0) spr0_drawn[x] = true;
            else if (spr0_drawn[x]) raise(d, VDC_ST_CR, (d->reg[R_CR] & 0x01) != 0);
            if (!out[x] && !hidden) out[x] = (uint16_t)(front | pal | col);
        }
    }
    /* the host's extra sprites (vdc_set_extra_sprites): behind the game's own, outside its
     * sprite count and collisions */
    for (int i = 0; i < n_extra; i++) {
        const VdcExtraSprite *e = &extra_spr[i];
        int cgy = (e->attr >> 12) & 3, h = cgy == 0 ? 16 : cgy == 1 ? 32 : 64;
        if (y < e->y || y >= e->y + h) continue;
        int sw = (e->attr & 0x0100) ? 32 : 16, pattern = e->pattern & 0x3FF;
        if (sw == 32) pattern &= ~1;
        if (h == 32) pattern &= ~2;
        if (h == 64) pattern &= ~6;
        int r = y - e->y;
        if (e->attr & 0x8000) r = h - 1 - r;
        bool xflip = (e->attr & 0x0800) != 0;
        uint16_t pal = (uint16_t)(0x100 | (e->attr & 0xF) << 4), front = (e->attr & 0x80) ? 0x8000 : 0;
        for (int c = 0; c < sw; c++) {
            int x = e->x - x0 + c;
            if (x < 0 || x >= w || out[x]) continue;
            int cc = xflip ? sw - 1 - c : c;
            int cell = pattern + (r >> 4) * 2 + (cc >> 4);
            uint32_t a = ((uint32_t)cell * 64 + (r & 15)) & 0x7FFF;
            int bit = 15 - (cc & 15);
            int col = ((d->vram[a] >> bit) & 1) | ((d->vram[(a + 16) & 0x7FFF] >> bit) & 1) << 1 |
                      ((d->vram[(a + 32) & 0x7FFF] >> bit) & 1) << 2 |
                      ((d->vram[(a + 48) & 0x7FFF] >> bit) & 1) << 3;
            if (col) out[x] = (uint16_t)(front | EXTRA_PX | pal | col);
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
        if (fade_on) fade_begin(vce);
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
    int x0 = -d->fb_wide + fb_shift;
    int orig = d->fb_wide - fb_shift;   /* where the original 256 pixels start */
    side_vram_jx = side_vram_jy = 0;
    if (side_vram) {   /* (as the host measures it: a jump past 32 px) */
        int jx = ((d->reg[R_BXR] & 0x3FF) - row_scroll[row] + 512) & 511, jy = (y - row_map_y[row] + 512) & 511;
        if (jx > 256) jx -= 512;
        if (jy > 256) jy -= 512;
        if (jx > 32 || jx < -32 || jy > 32 || jy < -32) side_vram_jx = -jx, side_vram_jy = -jy;
    }
    row_scroll[row] = d->reg[R_BXR] & 0x3FF;
    row_map_y[row] = (uint16_t)y;
    if (cr & 0x80) render_bg(d, y, x0, w, bg, row);
    if (cr & 0x40) render_sprites(d, row, x0, w, sp);
    if (side_left && side_right && d->fb_wide && !side_real) {
        /* side tiles: no sprites beside the original 256 pixels (a game parks the ones it
         * does not use just off its screen) */
        for (int x = 0; x < w; x++)
            if (((x < orig && !(side_sprites & VDC_SIDE_LEFT)) || (x >= orig + 256 && !(side_sprites & VDC_SIDE_RIGHT))) &&
                !(sp[x] & EXTRA_PX))
                sp[x] = 0;
    }
    {
        bool any = false;
        for (int x = orig; x < orig + 256 && x < w && !any; x++) any = bg[x] != 0;
        row_bg_blank[row] = !any;
        row_bg_edge[row] = d->fb_wide && w >= 4 && (bg[0] | bg[1] | bg[2] | bg[3]) != 0;
    }

    uint32_t *dst = &d->fb[row * PCE_FB_MAX_W];
    const uint32_t *pal = fade_on ? fade_argb : vce->argb;
    if ((row & 15) == 0) memcpy(line_pal[row >> 4], pal, sizeof line_pal[0]);   /* (vdc_line_palette) */
    uint8_t *spx = &sprite_px[row * PCE_FB_MAX_W];
    uint32_t *bhx = &behind_px[row * PCE_FB_MAX_W];
    /* With the background and the sprites both off (the screen blanked, as between rooms),
     * the console shows color $100, the border color, not the backdrop $000: Stage 0 sets
     * its next room's red backdrop while blanked and waits 7 frames before turning the
     * picture back on, which would otherwise flash red (#20). Mednafen does the same. */
    uint32_t back = (cr & 0xC0) ? pal[0] : pal[0x100];
    for (int x = 0; x < w; x++) {
        uint16_t s = sp[x];
        spx[x] = s && ((s & 0x8000) || !bg[x]);
        /* (behind: a sprite's pixel the background hides, else the background colour) */
        bhx[x] = s && !spx[x] ? pal[s & 0x1FF] : back;
        if (spx[x]) dst[x] = pal[s & 0x1FF];
        else if (bg[x]) dst[x] = pal[bg[x]];
        else dst[x] = back;
    }
    if (d->fb_wide) {
        /* beyond the room's walls (or the original 256 pixels on a clipped row): black
         * behind the sprites */
        int c0 = d->wide_x0 < 0 ? 0 : d->wide_x0, c1 = d->wide_x1 > w ? w : d->wide_x1;
        if (row_clip && row_clip[row]) {
            if (row_clip[row] == VDC_CLIP_BOTH && c0 < orig) c0 = orig;
            if (c1 > orig + 256) c1 = orig + 256;
        }
        for (int x = 0; x < w; x++) {
            if (x >= c0 && x < c1) continue;
            uint16_t s = sp[x];
            spx[x] = s != 0;
            dst[x] = s ? pal[s & 0x1FF] : 0xFF000000u;
        }
        /* side fade: darker towards the picture's outer edges, black at them */
        if (side_fade) {
            int left = orig, right = w - (orig + 256);
            for (int x = 0; x < w; x++) {
                int k;   /* brightness, 0..256 */
                if (x < orig && (side_fade & VDC_SIDE_LEFT)) k = left > 0 ? (x + 1) * 256 / (left + 1) : 256;
                else if (x >= orig + 256 && (side_fade & VDC_SIDE_RIGHT)) k = right > 0 ? (w - x) * 256 / (right + 1) : 256;
                else continue;
                uint32_t c = dst[x];
                dst[x] = 0xFF000000u | (((c & 0xFF00FF) * (uint32_t)k >> 8) & 0xFF00FF) |
                         (((c & 0xFF00) * (uint32_t)k >> 8) & 0xFF00);
            }
        }
    }
}
