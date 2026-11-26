/* Title art (Video > Title art, with widescreen): the player's own art beside still
 * screens such as the title and the stage map. The game's map holds nothing of theirs past
 * the 256 pixels (wide.c's menus cannot carry them on: their art does not repeat), so art
 * the player made is drawn there instead: any *_sides.png in the profile folder.
 *
 * The file holds no picture of the game's. Its pixels are palette entries, not colours:
 * each side pixel names one of the 512 colours the console draws with, and is drawn with
 * what that entry holds at the time, so the art follows the screen's fades and its lightning
 * as the game's own pixels do. The screen is recognised by its tile layout (which map entry
 * stands where), not by its picture, so the art is there from the screen's first frame.
 * Format: 396 x 228 RGBA. Rows 0-224: columns 0-69 and 326-395 the art, each pixel an entry
 * (R = low 8 bits, G bit 0 = bit 8; alpha 0 = none); columns 70-325 unused (transparent).
 * Rows 225-227: pixel 0 'R' 'S' '1' (the format), then the screen's 28 x 32 map entries,
 * row by row, one a pixel (R = low byte, G = high byte), as titleart_layout() reads them.
 * Made by the converter from generated art (docs/runtime.md, "Title art"). No SDL here
 * (the game loads the PNG: main.c), so the headless tool uses it too. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pce/pce.h"

#define TITLEART_W 396
#define TITLEART_H 225
#define TITLEART_FILE_H 228
#define TITLEART_SIDE 70   /* columns of art each side */
#define TITLEART_ROWS 28   /* the layout: tile rows x columns of the 256 x 224 picture */
#define TITLEART_COLS 32
#define TITLEART_NONE 0xFFFF

typedef struct TitleArt {
    bool loaded;
    uint16_t side[TITLEART_H][2 * TITLEART_SIDE];        /* entry a pixel: left 0-69, right 70-139 */
    uint16_t layout[TITLEART_ROWS][TITLEART_COLS];       /* the screen's map entries */
    bool shown;                                          /* the last frame was the screen */
} TitleArt;

/* From the file's pixels (0xAARRGGBB, w x h). False if it is not the format. */
bool titleart_parse(TitleArt *t, const uint32_t *px, int w, int h);

/* The last frame's map entries as titleart.h describes them: for each tile row, the map
 * row and first column the middle line of that row was drawn from. */
void titleart_layout(const Vdc *d, uint16_t out[TITLEART_ROWS][TITLEART_COLS]);

/* The last frame's picture as palette entries (rondo_headless --dump-layout, for the
 * converter): each pixel of the 256 x 225 background, 0 where it shows colour 0. */
void titleart_entries(const Vdc *d, uint16_t *out);

/* If the last frame is t's screen, writes fb (the picture shown, 256 x h, `pitch` pixels a
 * row) with `ext` columns of art each side (ext <= TITLEART_SIDE) to out (256 + 2 ext wide,
 * the same pitch) and returns true. */
bool titleart_compose(TitleArt *t, const Pce *p, const uint32_t *fb, int h, int pitch, int ext, uint32_t *out);
