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

    /* Unused, always 0: held widescreen's host settings until 0.13. Kept so the machine
     * state keeps its size and layout, and save states from 0.12 still load. */
    int reserved[4];
} Vdc;

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
 * - vdc_set_sprite_hide: sprites (a bit per sprite table entry) left out of the picture
 *   from now on; 0 = none. They still count toward each line's 16 and sprite 0's
 *   collision, as if drawn: only their pixels are not put in the picture. */
const uint16_t *vdc_row_scroll(void);
/* Sprite pixels (host-side, like the above): 1 where the last picture shows a sprite's pixel
 * (in front of the background, or over its colour 0), PCE_FB_MAX_W a row (graphic
 * enhancements: telling the background's scenery from sprites drawn over it). */
const uint8_t *vdc_sprite_pixels(void);
/* ... and what is behind the background at each pixel: the colour of a sprite it hides there,
 * else the background colour (what would show if the background's pixel were taken away). */
const uint32_t *vdc_behind_pixels(void);
void vdc_set_sprite_hide(uint64_t mask);
