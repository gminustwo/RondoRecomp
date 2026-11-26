/* Torch glow (Video > Graphic enhancements > Torch glow): the wall torches' flames light the wall
 * round them, the light flickering. A pass over the finished picture before it is shown, as
 * Smooth dithering's: only the picture changes (the game, its RAM and save states are the
 * same with it on or off). No SDL here, so the headless tool uses it too. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The torches found in src (w x h pixels, ARGB, pitch pixels a row) lit into dst (the same
 * size; the whole picture is copied). frame: the game's frame count, the flicker's clock;
 * world_x: the camera's X, so each torch keeps its own flicker as the picture scrolls.
 * Returns how many torches were lit (0: dst is a plain copy). */
int torch_glow(const uint32_t *src, uint32_t *dst, int w, int h, int pitch, uint64_t frame, int world_x);

/* Candle glow (Video > Graphic enhancements > Candle glow): the blue-flamed candles (Stage 4's: a
 * candle object, whipped for hearts) get a soft blue halo round their flame, flickering.
 * Worked on img in place (a copy of the picture); returns how many were lit. */
int candle_glow(uint32_t *img, int w, int h, int pitch, uint64_t frame, int world_x);

/* Alcove glow (Video > Graphic enhancements > Alcove glow): wall torches in the castle halls' painted
 * alcoves, whole or half behind the wall, light only the alcove's red bricks round them,
 * flickering. In place; skip_full leaves torches with the whole flame showing (Torch glow's).
 * Run before torch_glow (it finds flames by colours Torch glow changes). */
/* Moon glow (Video > Graphic enhancements > Moon glow): a faint rim of light round the moon
 * (Stage 5's docks), only in the sky round it, breathing very slowly. In place; 1 if the moon
 * was found. */
/* Shield shine (Video > Graphic enhancements > Shield shine): the wall of heraldic shields
 * (Stage 5's docks: only call it there, area $26/$27; elsewhere a castle pillar's base in the
 * rims' colours took a glint): a glint runs round a shield's rim. In place; returns how many
 * shields were found. */
/* Sail sway (Video > Graphic enhancements > Sail sway): the purple sails over Stage 5's docks move in
 * the wind, the tall ones swaying, the short ones' lower halves fluttering. In place; run
 * before the other passes. 1 if sails were found. */
int sail_sway(uint32_t *img, int w, int h, int pitch, uint64_t frame, int world_x);

/* Night sky (Video > Graphic enhancements > Night sky): the stars over Stage 5's docks twinkle
 * faintly, and rarely (about once a minute of starry sky, at least 20 s apart) a small, dim
 * shooting star crosses it. In place; run after sail_sway and before moon_glow. 1 if the
 * starry sky was found. night_sky_streak_at: a shooting star from that frame on (tests). */
/* Sail sway on Stage 5's ship (area $32: only call it there): its grey sails move in the
 * wind, in a style picked at random each time they come into view (rigging_sway_style forces
 * one: 1-5, 7-10, for tests; -1 random). sprite: vdc_sprite_pixels (sprites are left out);
 * behind: vdc_behind_pixels (shown where cloth moves off).
 * In place; returns how many sails moved. */
int rigging_sway(uint32_t *img, int w, int h, int pitch, uint64_t frame, int world_x, int world_y,
                 const uint8_t *sprite, const uint32_t *behind);
void rigging_sway_style(int style);

int night_sky(uint32_t *img, int w, int h, int pitch, uint64_t frame);
void night_sky_streak_at(int frame);

int shield_shine(uint32_t *img, int w, int h, int pitch, uint64_t frame);

int moon_glow(uint32_t *img, int w, int h, int pitch, uint64_t frame);

int alcove_glow(uint32_t *img, int w, int h, int pitch, uint64_t frame, int world_x, bool skip_full);
