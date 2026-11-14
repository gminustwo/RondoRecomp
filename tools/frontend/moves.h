/* Moves in the manner of Super Castlevania IV, made for RondoRecomp (the idea from Pizzano's
 * Super Rondo of Blood hack; none of its code): Richter whips in 8 directions and Maria
 * throws her doves in 8, and Richter's whip goes limp while Attack is held. No SDL here, so
 * rondo_headless can test them (--moves).
 *
 * The game has one whip, straight ahead. Its strike (whip object frames 05-07) gets its
 * hitbox from a table in Richter's code (bank $6E, docs/findings.md, "The whip"): for another
 * direction that table is rewritten for the strike's frames, and the strike's sprites are
 * left out of the picture and drawn turned to that direction around Richter's hand, from
 * their own pixels. A dove flying out has its frame-by-frame movement turned the same way;
 * the game's own code then slows it and brings it home as usual.
 *
 * Up and Attack is the game's own sub-weapon throw: with 8 directions on, Up is not passed
 * to the game while Attack is held (the Sub-weapon control still throws). Down aims only in
 * the air. While the whip hangs the game gets no direction. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pce/pce.h"

typedef struct MoveOptions {
    bool eight_way;            /* Richter's whip and Maria's doves in 8 directions */
    bool limp_whip;            /* Richter's whip hangs while Attack is held */
} MoveOptions;

#define MOVES_WIN_W 208        /* the picture around Richter the whip is redrawn in */
#define MOVES_WIN_H 176

typedef struct Moves {
    uint8_t aim;               /* the directions held this frame (PAD_UP ...) */
    bool attack;               /* Attack held */
    /* the whip */
    int whip;                  /* its object (1-15), -1 none */
    int dir;                   /* the strike's direction: 0 ahead, 1 up and ahead, 2 up,
                                * -1 down and ahead, -2 down */
    int facing;                /* the strike's: 1 right, -1 left */
    struct { uint16_t at; uint8_t orig[4], ours[4]; bool on; } patch[3];
    bool draw;                 /* the strike's sprites were left out: draw them turned */
    bool draw_limp;            /* ... or draw the hanging whip */
    int win_x, win_y;          /* the window's top left, in the picture */
    int pivot_x, pivot_y;      /* Richter's hand, in the picture */
    int hand_f, hand_y;        /* and ahead of / below his X, Y */
    uint8_t win[MOVES_WIN_H][MOVES_WIN_W];   /* the strike's pixels: palette << 4 | colour, 0 none */
    /* the limp whip */
    bool limp;                 /* hanging */
    int limp_frames;
    float link_angle[8];       /* each link's direction (radians, 0 ahead, up positive) */
    /* doves */
    struct { bool on; int dir, facing; } dove[16];
    uint8_t active[16];        /* objects 0-15 as they were after the last frame */
} Moves;

void moves_reset(Moves *m);
/* Before each frame: the pad the game is to get and whether Attack is held; returns the pad
 * the game gets (Up left out while Attack is held, no direction while the whip hangs). */
uint8_t moves_pad(Moves *m, const Pce *p, const MoveOptions *o, uint8_t pad, bool attack_held);
/* Around each frame: `live` is false while rewinding, which shows frames already played as
 * the game drew them (nothing is changed or drawn then). */
void moves_before_frame(Moves *m, Pce *p, const MoveOptions *o, bool live);
void moves_after_frame(Moves *m, Pce *p, const MoveOptions *o, bool live);
