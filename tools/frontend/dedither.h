/* Smooth dithering (Video > Smooth dithering): the game's ordered dithering, two colours a
 * shade apart in a checkerboard or a pattern of dots that read as one colour between them (the
 * sky behind the burning town), drawn as that colour. Only the picture shown changes. */
#pragma once

#include <stdint.h>

/* src: w x h pixels (0xAARRGGBB), `pitch` pixels a row. Writes the same size to dst (the same
 * pitch; not src). A pixel is blended with its neighbours only where, within 2 pixels of it:
 * the colours near its own (each channel within 40 of 255) are at least two; all but the most
 * common of them are single dots (no two side by side or one above another); and the pattern
 * repeats every 4 pixels along the rows (ordered dithering, not drawn texture such as stone
 * speckle, grain or outlines). The blend weights rows and columns 1 2 2 2 1, which evens out
 * patterns 2 and 4 pixels wide exactly. Colours far from the pixel's (outlines, silhouettes)
 * are never mixed in. */
void dedither(const uint32_t *src, uint32_t *dst, int w, int h, int pitch);
