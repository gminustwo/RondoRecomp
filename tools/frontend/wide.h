/* 16:9 widescreen for Rondo of Blood: the VDC draws extra columns on both sides of the
 * 256-pixel picture (vdc.wide), and small changes to the game make it fill them:
 * background columns and rows are streamed further out, and sprites are built out there
 * too. Objects themselves are left alone (see WIDE_MAX). No SDL here, so the headless tool
 * uses it too.
 *
 * Every change is a byte patch checked against the original bytes first (the game loads
 * code from the disc as it goes). Patches are put in or taken out only at safe points
 * (outside gameplay, or while the game changes rooms), so a room is never half drawn the
 * old way. A room is shown wide when every patch is in place and it is wider than the
 * screen (or its scenery scrolls sideways, like the Stage 0 carriage chase); a room one
 * screen wide (vertical shafts, boss rooms) keeps the original 4:3 picture.
 * See docs/findings.md, "Widescreen". */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pce/pce.h"

/* Largest extension per side (pixels): 16:9 with square pixels. The game itself already
 * prepares 64 px past each side of its 256 (objects spawn and start moving there,
 * background columns are drawn there), so enemies behave exactly as in the original. A
 * 16:9 screen needs 42 with the TV's 4:3 pixels (256 * 4/3 = 341 wide): inside that. With
 * square pixels it needs 70 (224 lines * 16/9 = 398 wide): objects then spawn and start
 * moving 72 px out (wide.c, OBJECT_REACH), so enemies come on out of sight. Wider screens show the 16:9 picture with
 * the sides beside it. (0.6-0.12 went to 112 for 21:9 and moved objects' spawning further
 * out to do it.) */
#define WIDE_MAX 72

typedef struct Wide {
    int want;              /* requested extension per side, pixels (0 = off) */
    bool edges;            /* level edges: the picture moves over at a room's end instead
                            * of showing black past its wall (wide_frame) */
    bool menus;            /* the title and file menus drawn wide too (their backgrounds
                            * carried on past the sides; wide_frame) */
    bool near_spawns;      /* objects made where the 4:3 game makes them (64 px out), not
                            * further out to come into the wider picture from beyond it */
    bool room_sides;       /* rooms kept 4:3 (boss rooms, shafts) shown mirrored beside the
                            * picture, fading to black (room_sides; else black) */
    /* results of the last wide_frame(), for the frame that follows it */
    int active;            /* extension drawn (0 = original 4:3 picture) */
    bool patched;          /* every background change is in place */
    int shift;             /* the picture's shift this frame (vdc_set_wide_shift) */
    int build_cam;         /* the camera as the game last built its sprites (hud_at_build) */
    bool build_seen;
    /* the camera's limits (LIMIT_LEFT page, LIMIT_RIGHT X) while the room last scrolled: a
     * boss locking the camera to one screen keeps the picture wide, with these walls */
    int open_lo, open_hi;
    bool open_seen;
    /* the limits last frame; calm: they changed with no new room (past a door), the picture
     * not shifted until the camera has gone 128 px from calm_cam */
    int lim_lo, lim_hi;
    bool lim_seen, calm;
    int calm_cam;
    int inside_shift;      /* the shift while the camera was last within the room's limits */
    int menu_good;         /* frames in a row a menu's sides could be filled (to MENU_FRAMES) */
    int menu_bad;          /* ... and since then could not */
    int menu_bxr, menu_moved;   /* a menu's sideways scroll last frame, frames it has not moved */
    int gen_r, gen_l;      /* screen-fixed generators moved this far out (0 = where the game
                            * put them, 64 px) */
    /* bookkeeping */
    uint8_t layer_x[8];    /* scroll layers' X, to notice scenery that scrolls */
    uint64_t move_history; /* bit per recent frame: some scenery layer moved */
    int scenery_moving;    /* how many of the last 60 frames it did */
    /* rows of the picture: X scroll last frame, frames it has not changed, how a band
     * holding a parked object is cut off (VDC_CLIP_*; see wide_frame) */
    uint16_t row_x[PCE_FB_MAX_H];
    uint8_t row_still[PCE_FB_MAX_H];
    uint8_t row_clip[PCE_FB_MAX_H];
    /* objects: screen X last frame, frames it has not changed */
    int16_t obj_x[0x58];
    uint8_t obj_still[0x58];
    /* objects: kind last frame, and whether it had been drawn (edge spawns, wide_frame) */
    uint8_t obj_kind[0x58];
    bool obj_shown[0x58];
} Wide;

/* frames a band's scroll must stay put to count as parked (the sky moves every few frames,
 * the carriage band not at all while it waits) */
#define PARKED_FRAMES 20
/* frames in a row a menu's sides must fill before it is drawn wide */
#define MENU_FRAMES 8
/* frames a wide menu stays wide with its last sides while they cannot be filled */
#define MENU_HOLD 60

/* Pixels per side that fill a window of aspect window_aspect (width / height) when the
 * game's 256-pixel picture is shown at picture_aspect (4:3, or 256:224 with square
 * pixels); rounded down to even, capped at WIDE_MAX. */
int wide_extent(double window_aspect, double picture_aspect);

/* Call before every emulated frame. Puts game changes in or takes them out (at safe
 * points), and sets p->vdc.wide and the room mask for the frame about to run. */
void wide_frame(Wide *w, Pce *p);

/* Take every game change out now (e.g. before a state is saved for a build without
 * widescreen). Leaves the next frame at 4:3. */
void wide_off(Wide *w, Pce *p);

/* Diagnostics: one line per game change (bank:offset, in / out / other code). */
int wide_report(Pce *p, char *out, int n);

/* Diagnostics: bit i set = leave patch i out (rondo_headless --wide-skip). */
extern uint32_t wide_debug_skip;
