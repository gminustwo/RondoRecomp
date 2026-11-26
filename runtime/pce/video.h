/* PC Engine video: HuC6260 VCE (palette, dot clock) and HuC6270 VDC
 * (VRAM, background, sprites, DMA, raster/vblank interrupts). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PCE_FB_MAX_W 512
#define PCE_FB_MAX_H 256
#define PCE_LINES_PER_FRAME 263

typedef struct Vce {
    uint16_t pal[512];      /* 9-bit GRB entries */
    uint32_t argb[512];     /* cached 0xAARRGGBB */
    uint16_t addr;
    uint8_t ctrl;           /* bits 0-1 dot clock, bit 7 grayscale */
} Vce;

void vce_reset(Vce *v);
/* Smooth fades (host setting, outside the machine state; one picture is drawn at a time per
 * process): the game's fades, which step each colour channel through 8 levels, are drawn
 * blended through 256. Only the picture changes: the palette the game reads and writes, save
 * states and the game's behaviour are the same either way. Off by default. */
void vce_set_smooth_fades(bool on);
/* The 512 colours the picture is drawn with now (Smooth fades' blend while it is on). */
const uint32_t *vce_shown_argb(const Vce *v);
/* The 512 colours the line `row` of the last picture was drawn with (taken every 16 lines: a
 * game can change colours while the picture is drawn, as the title's lightning does). */
const uint32_t *vdc_line_palette(int row);
void vce_write(Vce *v, uint16_t off, uint8_t val);
uint8_t vce_read(Vce *v, uint16_t off);

enum {
    VDC_ST_CR = 0x01,   /* sprite 0 collision */
    VDC_ST_OR = 0x02,   /* sprite overflow */
    VDC_ST_RR = 0x04,   /* raster compare */
    VDC_ST_DS = 0x08,   /* VRAM -> SATB DMA done */
    VDC_ST_DV = 0x10,   /* VRAM -> VRAM DMA done */
    VDC_ST_VD = 0x20,   /* vertical blank */
};

typedef struct Vdc {
    uint16_t vram[0x8000];
    uint16_t satb[256];
    uint16_t reg[32];
    uint8_t ar;
    uint8_t status;
    uint16_t read_latch;
    bool irq;               /* IRQ1 line */
    bool satb_pending;      /* DVSSR written: transfer at next vblank */

    /* per-frame state, latched at frame start */
    int display_start, display_end;
    int bg_y;               /* y scroll of the previously rendered line */
    bool first_line;

    /* framebuffer of the active display */
    uint32_t fb[PCE_FB_MAX_W * PCE_FB_MAX_H];
    int fb_w, fb_h;

    /* Widescreen (host setting, not machine state; cleared in save states): draw this many
     * extra pixels on each side of a 256-pixel picture, from the background map and the
     * sprites the game places there. 0 = off, at most VDC_WIDE_MAX. fb_wide is the amount
     * used for the current frame (latched at its start); the original picture starts at
     * fb[fb_wide]. Background columns of the framebuffer outside [wide_x0, wide_x1) are
     * black (beyond the room's walls); sprites are still drawn there. (These four ints were
     * `reserved[4]` in 0.13-1.2: the machine state keeps its size and layout.) */
    int wide;
    int fb_wide;
    int wide_x0, wide_x1;
} Vdc;

#define VDC_WIDE_MAX 128

void vdc_reset(Vdc *d);
void vdc_write(Vdc *d, uint16_t off, uint8_t val);
uint8_t vdc_read(Vdc *d, uint16_t off);

/* Called at the start of each of the 263 lines (before the CPU runs that line): the raster
 * compare. */
void vdc_line_start(Vdc *d, int line);
/* The end of the picture: the sprite table DMA and the vertical blank interrupt. Called
 * part-way into line display_end, after that line's raster compare has been taken. */
void vdc_vblank(Vdc *d);
/* Called after the CPU has run the line: renders it if it is an active display line. */
void vdc_line_end(Vdc *d, const Vce *vce, int line);

/* Host-side, kept outside the machine state (save states are unchanged; one picture is
 * drawn at a time per process):
 * - vdc_row_scroll: the background X scroll (BXR) each displayed row of the last frame was
 *   drawn with; raster splits give bands of rows their own scroll (rondo_headless
 *   --row-scroll).
 * - vdc_row_bg_blank: rows of the last frame whose background showed nothing (colour 0)
 *   across the original 256 pixels; vdc_row_bg_edge: rows of a widened picture whose
 *   background showed something at its left edge (first 4 pixels).
 * - vdc_set_row_clip: per displayed row, where its background is cut off beyond the
 *   original 256 pixels, as beyond a room's walls (sprites or black there): VDC_CLIP_BOTH
 *   both sides, VDC_CLIP_RIGHT the right side only, 0 neither. NULL for none; the array
 *   (PCE_FB_MAX_H bytes) must stay valid while frames are drawn.
 * - vdc_set_sprite_hide: sprites (a bit per sprite table entry) left out of the picture
 *   from now on; 0 = none. They still count toward each line's 16 and sprite 0's
 *   collision, as if drawn: only their pixels are not put in the picture. */
enum { VDC_CLIP_BOTH = 1, VDC_CLIP_RIGHT = 2 };
const uint16_t *vdc_row_scroll(void);
const uint8_t *vdc_row_bg_blank(void);
const uint8_t *vdc_row_bg_edge(void);
void vdc_set_row_clip(const uint8_t *rows);
void vdc_set_sprite_hide(uint64_t mask);
/* - vdc_set_sprite_fades: boxes (screen X as the sprite table has it, 0 = the 4:3 picture's
 *   left; Y) in which a sprite behind the background (scenery: not a hero, an enemy or the
 *   HUD, all drawn in front) is drawn, by its middle, only in part, a 4 x 4 ordered dither,
 *   `level` 0 (none of it) to 16 (all): scenery objects the wider picture would show come and
 *   go fade in and out (wide.c). n 0: none. */
typedef struct { int16_t x0, x1, y0, y1; uint8_t level; } VdcFade;
#define VDC_FADES 8
void vdc_set_sprite_fades(const VdcFade *list, int n);
/* - vdc_set_hide_corner: sprites parked above the screen with at most their last 8 rows in it
 *   (a game parks unused ones there: the save menu's torch flames, peeking into the picture's
 *   top left) left out of the picture, counted as drawn as above. Widescreen's menus: a sliver
 *   in the 4:3 picture's corner stood mid-picture there. */
void vdc_set_hide_corner(bool on);
/* - vdc_set_extra_sprites: sprites of the host's own, drawn from the game's sprite graphics in
 *   VRAM with its palettes, behind the game's sprites and outside its sprite count and
 *   collisions (widescreen: a lantern of the game's hung beside a room). x, y: screen
 *   position as a sprite's (the original 256 pixels from x 0); pattern, attr: as in the
 *   sprite table. NULL for none; the list must stay valid while frames are drawn. */
typedef struct VdcExtraSprite { int16_t x, y; uint16_t pattern, attr; } VdcExtraSprite;
void vdc_set_extra_sprites(const VdcExtraSprite *list, int n);
/* - vdc_set_sprite_move: sprites whose pattern is pattern_lo..pattern_hi drawn dx px further
 *   right (widescreen: a boss's health bar at the picture's right edge); hi < lo for none. */
void vdc_set_sprite_move(int pattern_lo, int pattern_hi, int dx);
/* Widescreen's shift (host-side, like the above): the widened picture is drawn this many
 * pixels further right (negative: left) of the game's screen, at most fb_wide either way,
 * so at a level's edge all the extra width goes to the side the level goes on. The
 * original 256 pixels then start at fb[fb_wide - shift]; wide_x0 / wide_x1 are framebuffer
 * columns as before. Latched at the start of each frame, like fb_wide. */
void vdc_set_wide_shift(int shift);
int vdc_fb_shift(void);
/* Side tiles (host-side, like the above), for screens whose map holds nothing of theirs
 * past the 256 pixels (Rondo's menus): background map entries (BAT words) drawn there
 * instead, VDC_SIDE_TILES a map row (64 rows): left[row * VDC_SIDE_TILES + k] for the k-th
 * column before the first shown in the original 256 pixels, right[...] for the k-th after the
 * last (the furthest repeated beyond); VDC_SIDE_NONE draws the background colour. No
 * sprites are drawn beside the 256 pixels meanwhile. NULL for none; the arrays must stay
 * valid while frames are drawn. vdc_row_map_y: the background map's Y (pixels) each
 * displayed line of the last frame was drawn from. */
#define VDC_SIDE_TILES 12
#define VDC_SIDE_NONE 0xFFFF
#define VDC_SIDE_OWN 0x0800   /* an entry with this bit (not a tile bit here) is never flipped */
#define VDC_SIDE_MAP 0xFFFE   /* the map's own entry there, as it is (not flipped) */
void vdc_set_side_tiles(const uint16_t *left, const uint16_t *right);
/* While on (a menu's pages sliding over), the side tiles give way to the map's own entries
 * wherever the picture has not wrapped round the map (columns 0 to the map's width, counted
 * from the scroll without wrapping), and sprites are drawn beside the 256 pixels too: the
 * page coming in and the one going out show across the whole picture. */
void vdc_set_side_real(bool on);
/* Side fade (host-side, like the above): the picture beside the original 256 pixels darkens
 * towards its outer edges, to black at them (widescreen's walls repeated beside a room kept
 * 4:3, so they recede rather than stand as part of the room). */
enum { VDC_SIDE_LEFT = 1, VDC_SIDE_RIGHT = 2 };   /* (the sides, for the two below) */
/* side tiles: the sides (VDC_SIDE_LEFT | VDC_SIDE_RIGHT) where sprites are drawn after all */
void vdc_set_side_sprites(int sides);
void vdc_set_side_fade(int sides);
/* Side flip (host-side, like the above): side tiles drawn mirrored left to right, which the
 * hardware cannot do; with the side tiles holding the room's columns in mirrored order, the
 * sides are the room's exact mirror image (widescreen's Room sides). */
void vdc_set_side_flip(int sides);
/* Background map writes (host-side, like the above): a flag per word of VRAM below
 * VDC_BAT_WATCH (the map, up to 64 x 64), set when the game writes it (by the data port or
 * DMA), changed or not, until cleared (widescreen: a column or row drawn again shows as one
 * even where it writes what was there). */
#define VDC_BAT_WATCH 0x1000
const uint8_t *vdc_bat_written(void);
void vdc_bat_written_clear(void);
/* Side VRAM (host-side, like the above): beyond the original 256 pixels on one side (-1 left,
 * 1 right), the background drawn from this copy of VRAM (map and tiles) instead, its map read
 * dx[line], dy[line] pixels on from where the scroll points (a line of the picture each);
 * NULL = off
 * (widescreen: the room left behind through a door, as it was before the next part's
 * graphics were loaded). */
/* With side VRAM (below) on a side of side tiles: the side drawn as the room it holds was,
 * line by line from the scroll each line is drawn with: past first (left) / last (right),
 * columns of that VRAM's map, its own side tiles kept (left / right, VDC_SIDE_TILES a row,
 * as for vdc_set_side_tiles); short of them, that map's own columns. NULL: off. */
void vdc_set_side_kept(const uint16_t *left, const uint16_t *right, int first, int last);
void vdc_set_side_vram(const uint16_t *vram, int side, const int16_t *dx, const int16_t *dy);
const uint16_t *vdc_row_map_y(void);
/* Sprite pixels (host-side, like the above): 1 where the last picture shows a sprite's pixel
 * (in front of the background, or over its colour 0), PCE_FB_MAX_W a row (graphic
 * enhancements: telling the background's scenery from sprites drawn over it). */
const uint8_t *vdc_sprite_pixels(void);
/* ... and what is behind the background at each pixel: the colour of a sprite it hides there,
 * else the background colour (what would show if the background's pixel were taken away). */
const uint32_t *vdc_behind_pixels(void);
/* Map fill (host-side, like the above): background map entries drawn instead of the map's
 * own beside the original 256 pixels, by map position: fill[row * VDC_FILL_W + column]
 * (64 rows), VDC_SIDE_NONE for the map's own. For scenery the game drew only as far as its
 * 4:3 picture ever shows, carried on in its own tiles (wide.c). NULL for none. */
#define VDC_FILL_W 128
void vdc_set_map_fill(const uint16_t *fill);
/* the fill over the original 256 pixels too (an arena made walkable: its camera goes where the
 * 4:3 fight never takes it, and the scenery's undrawn columns came into those pixels) */
void vdc_set_map_fill_all(bool all);
/* Map patch (host-side, like the above): like the map fill, background map entries drawn
 * instead of the map's own by map position, but anywhere in the picture, the original 256
 * pixels too (widescreen's side recipes: a drainpipe at a room's edge, which the wall laid
 * beside it would stand against, bricked over). The map in VRAM is left as it is. NULL for
 * none. */
void vdc_set_map_patch(const uint16_t *patch);
