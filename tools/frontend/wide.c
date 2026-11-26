/* See wide.h. */
#include "wide.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "game.h"

/* A change to the game: `len` bytes at physical bank:offset, applied only when the bytes
 * there are `orig` (so other code loaded into the same RAM is left alone). An `orig` of
 * {0} stands for our bank $00's RTS filler ($60s): no real original starts with BRK. */
typedef struct Patch {
    const char *what;
    uint8_t bank;
    uint16_t off;          /* offset within the 8 KiB bank */
    uint8_t len;
    uint8_t orig[64];
    uint8_t wide[96];
    /* a byte of `wide` set while the game runs (VAR_*, at var_at), 0 = none: compared as
     * anything, written with the frame's value (var_value) */
    uint8_t var, var_at;
} Patch;

/* Values that follow the picture each frame (see wide_frame): how far past the screen
 * sprites are built, how far out objects spawn and start moving, and where a room's first
 * drawing starts. */
enum { VAR_NONE, VAR_SPRITE_RIGHT, VAR_SPRITE_LEFT, VAR_ECHO_RIGHT, VAR_SPAWN_RIGHT, VAR_SPAWN_LEFT,
       VAR_MOVE_RIGHT, VAR_MOVE_LEFT, VAR_ROOM_START, VAR_SWEEP_RIGHT, VAR_SWEEP_LEFT, VAR_COUNT };
static uint8_t var_value[VAR_COUNT];   /* 0 = the template's byte */

static const uint8_t RTS_FILLER[96] = {
    0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60,
    0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60,
    0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60,
    0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60,
    0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60,
    0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60};

#define FILL60 /* 52 bytes of our bank $00's RTS filler */ \
    {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, \
     0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, \
     0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, \
     0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60}

#define E WIDE_MAX         /* every patch is made for the widest supported view */
#define WIDE_STREAMED 64   /* the widest a side shows (wide_extent: background drawn there) */
#ifndef STREAM
#define STREAM 0x50        /* columns streamed this far past the edge (see patches[0]) */
#endif

/* (patches without a varying byte leave var and var_at out: 0) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const Patch patch_tab[] = {
    /* Background streaming (bank $6F, the scroll-layer code; every layer and both axes
     * use it): when the camera crosses a 32-pixel column, the game draws the column 64
     * pixels past the edge it is moving towards. The table holds that distance (low
     * byte; the page byte after it stays +1/-1). STREAM = 80 covers WIDE_MAX (a column
     * comes in at least that far past the edge). No further: a scenery band can show map
     * columns other layers stream into (Stage 2-1: the far trees, layer 1, is not streamed
     * itself; the playfield and middle layers' columns, blank at the top, fill its rows),
     * and the game keeps those writes out of its view only near its own 64. At 96 or 112
     * (0.6-0.12) they landed in view: a black strip drifting across the sky. */
    {"background columns streamed further out", 0x6F, 0x0780, 2, {0x40, 0xC0}, {STREAM, 0x100 - STREAM}},
    /* Some layers (the scenery of the Stage 0 carriage chase) are first drawn by stage
     * code out to the old 64 px only, which would leave a 48 px gap the new distance
     * never fills. So each streamed column (6F:A7A5) is drawn twice: at the old distance,
     * then at the new one, by code in free space in our own bank $00 (mapped at $E000
     * throughout) that switches the table above between the two. */
    {"streamed columns drawn at the old and the new distance", 0x6F, 0x07A5, 3, {0xBD, 0x66, 0x3E}, {0x4C, 0x10, 0xFE}},
    {"column drawing at both distances", 0x00, 0x1E10, 52, FILL60,
     {0x5A,                     /* PHY                              */
      0xDA,                     /* PHX                              */
      0xA9, 0x40,               /* LDA #$40      the old distances  */
      0x8D, 0x80, 0xA7,         /* STA $A780                        */
      0xA9, 0xC0,               /* LDA #$C0                         */
      0x8D, 0x81, 0xA7,         /* STA $A781                        */
      0x20, 0x2E, 0xFE,         /* JSR draw                         */
      0xFA,                     /* PLX                              */
      0x7A,                     /* PLY                              */
      0xA9, STREAM,             /* LDA #STREAM   the new distances  */
      0x8D, 0x80, 0xA7,         /* STA $A780                        */
      0xA9, 0x100 - STREAM,     /* LDA #-STREAM                     */
      0x8D, 0x81, 0xA7,         /* STA $A781                        */
      0x4C, 0x2E, 0xFE,         /* JMP draw (returns to the caller) */
      0xBD, 0x66, 0x3E,         /* draw: LDA $3E66,X  (as at A7A5)  */
      0x4C, 0xA8, 0xA7,         /* JMP $A7A8                        */
      0x40, 0xC0,               /* $FE34: the old distances, kept for vertical use */
      0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60}},
    /* The table is also read for vertical distances: where full-height columns start
     * (64 px above the camera, 6F:A7C4) and how far rows are streamed above and below
     * (6F:A8A4). Those stay as they were, read from the copy at $FE34; scenery layers
     * shorter than the map would otherwise be drawn into each other's rows. */
    {"columns start 64 px above the camera, as before", 0x6F, 0x07C5, 2, {0x80, 0xA7}, {0x34, 0xFE}},
    {"rows streamed 64 px above and below, as before", 0x6F, 0x08A5, 2, {0x80, 0xA7}, {0x34, 0xFE}},
    /* Room start (6F:A94C): the game draws 13 columns from 64 px left of the camera by
     * moving a temporary camera 384 px left and stepping it right 32 px at a time. With
     * the table above each column lands STREAM - 64 px further right, so start 480 px left
     * (camera + 0x20 - 0x200) and draw all 16 columns: the whole 512-pixel map, from
     * camera - 224 + STREAM. */
    {"room start draws 16 columns", 0x6F, 0x0963, 7,
     {0xA9, 0x0C, 0x85, 0x16, 0xA9, 0x80, 0x44}, {0xA9, 0x0F, 0x85, 0x16, 0xA9, 0x20, 0x44}, VAR_ROOM_START, 5},
    /* Rows streamed while the camera moves up or down (6F:A8D5) span the original 256
     * pixels from the camera; draw 16 blocks from 112 px left instead (the whole map). */
    {"rows drawn 512 px wide", 0x6F, 0x08D5, 7,
     {0xAD, 0x66, 0x3E, 0x29, 0x7F, 0x85, 0x08}, {0xA9, 0x10, 0x85, 0x08, 0xEA, 0xEA, 0xEA}},
    {"rows start 112 px left", 0x6F, 0x08EC, 2, {0x10, 0x15}, {0xEA, 0xEA}},

    /* Objects. The game spawns them 64 px out (camera + $140 / - $40, bank $69), lets them
     * move from there (70:A0B0 / A0BD) and removes them 128 px out: enough for 16:9 with the
     * TV's pixels (42 a side), where they are left exactly as the game has them. Square
     * pixels show 70: there (and only there, see OBJECT_REACH) objects spawn and start moving
     * 72 px out instead, and the screen-fixed generators at 320 / -64 move out to match
     * (move_generators). Put in or out as a room starts, with the background. (0.6-0.12,
     * going to 112 px for 21:9, moved all of this out to 128 px.) */
    {"objects spawn 72 px right of the screen", 0x69, 0x1E68, 2, {0x69, 0x40}, {0x69, 0x48}, VAR_SPAWN_RIGHT, 1},
    {"objects spawn 72 px left of the screen", 0x69, 0x1E7F, 2, {0x69, 0xC0}, {0x69, 0xB8}, VAR_SPAWN_LEFT, 1},
    {"objects move from 72 px left of the screen", 0x70, 0x00B0, 1, {0xC0}, {0xB8}, VAR_MOVE_LEFT, 0},
    {"objects move from 72 px right of the screen", 0x70, 0x00BD, 1, {0x40}, {0x48}, VAR_MOVE_RIGHT, 0},
    /* As a room (or a part of one entered through a door) starts, the game makes the
     * objects already within reach by sweeping its spawn line from the camera out to
     * camera + $140 (69:5D3E: $0141 steps); after that, objects are made only as the line
     * reaches them. With the line further out, those between $140 and it were never made
     * (Stage 3's skeleton room left the hero behind the background: the object that puts
     * him back in front was one of them). The sweep goes out to the spawn line. */
    {"room start makes objects out to the spawn line", 0x69, 0x1D61, 2, {0xA9, 0x41}, {0xA9, 0x49}, VAR_SWEEP_RIGHT, 1},
    /* ... and to the left, which the game sweeps 64 px ($41 steps): objects between that and
     * the spawn line to the left were never made either (Stage 3's middle chain, which comes
     * back with its ball, did not after the hero climbed back up to it). */
    {"room start makes objects out to the left spawn line", 0x69, 0x1D86, 2, {0xA9, 0x41}, {0xA9, 0x49}, VAR_SWEEP_LEFT, 1},
    /* The spawn list holds events as well as objects (type 6: a routine run as the camera
     * comes to it). The sweep above made events between $140 and the spawn line happen as
     * the room started, which the game does only when the camera gets there: one, in
     * Stage 2's behemoth chase, set the flag ($92) that has the behemoth remove itself as it
     * appears, so it never broke through the wall. While sweeping (the game's own $2744 is
     * 1), events past $140 ahead or 64 px behind are left out, as the game's sweep has them. */
    {"events past $140 left out of the sweep", 0x69, 0x1FDA, 4, {0xA5, 0x08, 0x0A, 0xA8}, {0x20, 0xA0, 0xFF, 0xEA}},
    {"room start sweep: events as the game has them", 0x00, 0x1F90, 75, {0},
     {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60,
      0xAD, 0x44, 0x27,         /* FFA0 entry: LDA $2744  1 while sweeping        */
      0xF0, 0x2D,               /* FFA3 BEQ go       not sweeping                */
      0xA5, 0x08,               /* FFA5 LDA <$08     the entry's type            */
      0xC9, 0x06,               /* FFA7 CMP #6                                   */
      0xD0, 0x27,               /* FFA9 BNE go       not an event                */
      0x38,                     /* FFAB SEC                                      */
      0xA5, 0x12,               /* FFAC LDA <$12     its X - the camera's        */
      0xED, 0x08, 0x3E,         /* FFAE SBC $3E08                                */
      0x48,                     /* FFB1 PHA                                      */
      0xA5, 0x13,               /* FFB2 LDA <$13                                 */
      0xED, 0x96, 0x3E,         /* FFB4 SBC $3E96                                */
      0xF0, 0x18,               /* FFB7 BEQ goP      0..255 ahead                */
      0xC9, 0x01,               /* FFB9 CMP #1                                   */
      0xF0, 0x06,               /* FFBB BEQ one                                  */
      0xC9, 0xFF,               /* FFBD CMP #$FF                                 */
      0xF0, 0x09,               /* FFBF BEQ minus                                */
      0x80, 0x14,               /* FFC1 BRA skipP    further either way          */
      0x68,                     /* FFC3 one: PLA                                 */
      0xC9, 0x41,               /* FFC4 CMP #$41                                 */
      0x90, 0x0A,               /* FFC6 BCC go       up to $140 ahead            */
      0x80, 0x0E,               /* FFC8 BRA skip                                 */
      0x68,                     /* FFCA minus: PLA                               */
      0xC9, 0xC0,               /* FFCB CMP #$C0                                 */
      0xB0, 0x03,               /* FFCD BCS go       up to 64 behind             */
      0x80, 0x07,               /* FFCF BRA skip                                 */
      0x68,                     /* FFD1 goP: PLA                                 */
      0xA5, 0x08,               /* FFD2 go: LDA <$08 (as at 69:5FDA)             */
      0x0A,                     /* FFD4 ASL                                      */
      0xA8,                     /* FFD5 TAY                                      */
      0x60,                     /* FFD6 RTS                                      */
      0x68,                     /* FFD7 skipP: PLA                               */
      0x68,                     /* FFD8 skip: PLA    (our return address:        */
      0x68,                     /* FFD9 PLA           the entry is left)         */
      0x60}},                   /* FFDA RTS                                      */

    /* ---- drawing: follows each frame's choice of wide or 4:3 (sprites past the 256
     * pixels would only take up the 64 hardware sprites in a 4:3 picture) ---- */

    /* Sprite building (bank $72, 72:8000): each sprite piece is dropped unless its X
     * (screen + 32) is 0..$11F; let pieces through from -E to $11F + E. */
    {"sprites drawn up to WIDE_MAX px right of the screen", 0x72, 0x0133, 1, {0x20}, {0x20 + E}, VAR_SPRITE_RIGHT, 0},
    {"sprites with X 0..15 drawn (unless parked: see $FE62)", 0x72, 0x013A, 3, {0xDD, 0xA1, 0x81}, {0x4C, 0x62, 0xFE}},
    /* The piece's X high byte decides (72:8123: 0 -> 8138, positive -> 812A, negative ->
     * dropped): through $FC40, which does the same, negatives going to the check below,
     * except for the copies of echoed objects (see $FC00). */
    {"piece culling through $FC40 (echoes)", 0x72, 0x0123, 7, {0xF0, 0x13, 0x10, 0x03, 0x4C, 0x8C, 0x81},
     {0x4C, 0x40, 0xFC, 0xEA, 0xEA, 0xEA, 0xEA}},
    /* Each object is drawn (72:803F: STX <$D9, BSR $8066) through $FC00, which draws echoed
     * objects twice more. */
    {"objects drawn through $FC00 (echoes)", 0x72, 0x003F, 6, {0x86, 0xD9, 0x44, 0x23, 0x30, 0x20},
     {0x20, 0x00, 0xFC, 0x30, 0x21, 0xEA}},
    /* Echoes: an object the ECHO_TABLE marks (written before every frame: screen-wide weather
     * such as Stage 2's rain, two objects fixed to the screen whose drops spread about 128 px
     * either side of them, so they only ever cover the 256 pixels) is drawn again 256 px to
     * the right and 256 px to the left, and the rain tiles across the wider picture. Only
     * while 16 sprite entries are left for the objects after it. Generated by a small
     * assembler; the culling below tells the copies apart by $8066's return address. */
    {"echoed objects drawn 256 px either side", 0x00, 0x1C00, 53, {0},
     {0x86, 0xD9,               /* FC00 STX <$D9                                */
      0x20, 0x66, 0x80,         /* FC02 JSR $8066     the object as the game has it */
      0x30, 0x2D,               /* FC05 BMI ret       sprite table full         */
      0xA6, 0xD9,               /* FC07 LDX <$D9                                */
      0xBD, 0xC0, 0xFC,         /* FC09 LDA $FCC0,X   ECHO_TABLE                */
      0xF0, 0x24,               /* FC0C BEQ ok                                  */
      0xA5, 0x17,               /* FC0E LDA <$17      sprite entries left       */
      0xC9, 0x10,               /* FC10 CMP #$10                                */
      0x90, 0x1E,               /* FC12 BCC ok                                  */
      0xFE, 0x70, 0x2B,         /* FC14 INC $2B70,X   X + 256                   */
      0x20, 0x66, 0x80,         /* FC17 JSR $8066     (returns to FC1A)         */
      0x08,                     /* FC1A PHP                                     */
      0xA6, 0xD9,               /* FC1B LDX <$D9                                */
      0xDE, 0x70, 0x2B,         /* FC1D DEC $2B70,X                             */
      0x28,                     /* FC20 PLP                                     */
      0x30, 0x11,               /* FC21 BMI ret                                 */
      0xDE, 0x70, 0x2B,         /* FC23 DEC $2B70,X   X - 256                   */
      0x20, 0x66, 0x80,         /* FC26 JSR $8066     (returns to FC29)         */
      0x08,                     /* FC29 PHP                                     */
      0xA6, 0xD9,               /* FC2A LDX <$D9                                */
      0xFE, 0x70, 0x2B,         /* FC2C INC $2B70,X                             */
      0x28,                     /* FC2F PLP                                     */
      0x30, 0x02,               /* FC30 BMI ret                                 */
      0xA9, 0x00,               /* FC32 ok: LDA #0    N clear: the next object  */
      0x60}},                   /* FC34 ret: RTS                                */
    /* A = the piece's X high byte (flags from it), X = its size index. The copies keep only
     * pieces past the 256 pixels on their own side, so the original picture is untouched:
     * the copy 256 right keeps screen X 256..255+E, the copy 256 left X < 0 (from -E, as
     * the negative check has it). */
    {"echo copies culled to the sides", 0x00, 0x1C40, 78, {0},
     {0x48,                     /* FC40 PHA                                     */
      0xDA,                     /* FC41 PHX                                     */
      0xBA,                     /* FC42 TSX                                     */
      0xBD, 0x04, 0x21,         /* FC43 LDA $2104,X   $8066's return address hi */
      0xC9, 0xFC,               /* FC46 CMP #$FC                                */
      0xD0, 0x0B,               /* FC48 BNE normal                              */
      0xBD, 0x03, 0x21,         /* FC4A LDA $2103,X   ... and lo                */
      0xC9, 0x19,               /* FC4D CMP #$19      the copy 256 right        */
      0xF0, 0x13,               /* FC4F BEQ plus                                */
      0xC9, 0x28,               /* FC51 CMP #$28      the copy 256 left         */
      0xF0, 0x22,               /* FC53 BEQ minus                               */
      0xFA,                     /* FC55 normal: PLX                             */
      0x68,                     /* FC56 PLA                                     */
      0xF0, 0x05,               /* FC57 BEQ n0                                  */
      0x10, 0x06,               /* FC59 BPL npos                                */
      0x4C, 0x00, 0xFE,         /* FC5B JMP $FE00     negative X check          */
      0x4C, 0x38, 0x81,         /* FC5E n0: JMP $8138                           */
      0x4C, 0x2A, 0x81,         /* FC61 npos: JMP $812A                         */
      0xFA,                     /* FC64 plus: PLX                               */
      0x68,                     /* FC65 PLA                                     */
      0xC9, 0x01,               /* FC66 CMP #1                                  */
      0xD0, 0x21,               /* FC68 BNE drop                                */
      0xA5, 0x05,               /* FC6A LDA <$05                                */
      0xC9, 0x20,               /* FC6C CMP #$20      at or past screen X 256   */
      0x90, 0x1B,               /* FC6E BCC drop                                */
      0xC9, 0x20 + E,           /* FC70 CMP #$20+E                              */
      0xB0, 0x17,               /* FC72 BCS drop                                */
      0x4C, 0x3F, 0x81,         /* FC74 JMP $813F     keep                      */
      0xFA,                     /* FC77 minus: PLX                              */
      0x68,                     /* FC78 PLA                                     */
      0xF0, 0x07,               /* FC79 BEQ m0                                  */
      0xC9, 0xFF,               /* FC7B CMP #$FF                                */
      0xD0, 0x0C,               /* FC7D BNE drop                                */
      0x4C, 0x00, 0xFE,         /* FC7F JMP $FE00     negative X check          */
      0xA5, 0x05,               /* FC82 m0: LDA <$05                            */
      0xC9, 0x20,               /* FC84 CMP #$20      left of screen X 0        */
      0xB0, 0x03,               /* FC86 BCS drop                                */
      0x4C, 0x38, 0x81,         /* FC88 JMP $8138                               */
      0x4C, 0x8C, 0x81},        /* FC8B drop: JMP $818C                         */
     VAR_ECHO_RIGHT, 0x31},
    /* ...in free space in our own bank $00 (RTS filler; mapped at $E000 throughout):
     * A = X high byte (negative). Keep the piece if it is $FF and the low byte >= -E. */
    {"negative sprite X check", 0x00, 0x1E00, 16,
     {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60},
     {0xC9, 0xFF,               /* CMP #$FF          */
      0xD0, 0x09,               /* BNE drop          */
      0xA5, 0x05,               /* LDA <$05          */
      0xC9, 0x100 - E,          /* CMP #-E           */
      0x90, 0x03,               /* BCC drop          */
      0x4C, 0x50, 0xFE,         /* JMP $FE50         */
      0x4C, 0x8C, 0x81},        /* drop: JMP $818C   */
     VAR_SPRITE_LEFT, 7},
    /* ...then the piece's object (slot in zp $D9 while the builder draws it) is dropped if
     * the parked-object table (PARKED_TABLE, written before every frame) marks it: something
     * the game parks just off the left of its 256 pixels, such as the Prologue's waiting
     * horses, stays out of sight in the wider picture too. */
    {"objects parked off the left not drawn", 0x00, 0x1E50, 17,
     {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60},
     {0xDA,                     /* PHX               */
      0xA6, 0xD9,               /* LDX <$D9  (slot)  */
      0xBD, 0x80, 0xFE,         /* LDA $FE80,X       */
      0xFA,                     /* PLX               */
      0xC9, 0x00,               /* CMP #0            */
      0xD0, 0x03,               /* BNE drop          */
      0x4C, 0x3F, 0x81,         /* JMP $813F (keep)  */
      0x4C, 0x8C, 0x81}},       /* drop: JMP $818C   */
    /* Pieces up to 32 px left of the screen (X + 32 = 0..31, A = that, X = the size index):
     * kept, except for a parked object, which gets the game's own lower bound again. */
    {"pieces just left of the screen: parked objects culled as before", 0x00, 0x1E62, 27,
     {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60,
      0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60},
     {0x48,                     /* PHA               */
      0xDA,                     /* PHX               */
      0xA6, 0xD9,               /* LDX <$D9  (slot)  */
      0xBD, 0x80, 0xFE,         /* LDA $FE80,X       */
      0xFA,                     /* PLX               */
      0xC9, 0x00,               /* CMP #0            */
      0xF0, 0x09,               /* BEQ keep          */
      0x68,                     /* PLA               */
      0xDD, 0xA1, 0x81,         /* CMP $81A1,X       */
      0x90, 0x06,               /* BCC drop          */
      0x4C, 0x3F, 0x81,         /* go: JMP $813F     */
      0x68,                     /* keep: PLA         */
      0x80, 0xFA,               /* BRA go            */
      0x4C, 0x8C, 0x81}},       /* drop: JMP $818C   */

    /* Hits: the game tests its weapons (and the hero) against the other objects in 8-bit
     * screen X (in Stage 1, 6B:9413 builds the weapons' boxes, 6B:9466 each object's),
     * leaving out anything whose box is not within the 256 pixels: a skeleton in the wider
     * picture could not be hit. Both are worked out through this code of ours, which first
     * takes HIT_BIAS (written before every frame, wide_frame) off X: the 256 pixels tested
     * are moved to where the hero faces in the wider picture; the spot a hit spark is shown
     * at gets it back. The game's code is in each stage's own bank $6B, at a different place
     * in each: found by its bytes and sent here by hit_patches, not by this table. */
    {"hit boxes moved by HIT_BIAS", 0x00, 0x1F40, 79, {0},
     {0x18,                     /* FF40 weapon: CLC                              */
      0x7D, 0xC8, 0x2B,         /* FF41 ADC $2BC8,X   X low (as at 6B:9434)      */
      0x9D, 0x10, 0x27,         /* FF44 STA $2710,X                              */
      0xBD, 0x70, 0x2B,         /* FF47 LDA $2B70,X                              */
      0x65, 0x00,               /* FF4A ADC <$00                                 */
      0x9D, 0x30, 0x27,         /* FF4C STA $2730,X   X high                     */
      0x38,                     /* FF4F SEC                                      */
      0xBD, 0x10, 0x27,         /* FF50 LDA $2710,X                              */
      0xED, 0xD8, 0xFE,         /* FF53 SBC $FED8     HIT_BIAS low               */
      0x9D, 0x10, 0x27,         /* FF56 STA $2710,X                              */
      0xBD, 0x30, 0x27,         /* FF59 LDA $2730,X                              */
      0xED, 0xD9, 0xFE,         /* FF5C SBC $FED9     ... high                   */
      0x9D, 0x30, 0x27,         /* FF5F STA $2730,X                              */
      0x60,                     /* FF62 RTS                                      */
      0x18,                     /* FF63 object: CLC   (as at 6B:9495)            */
      0x7D, 0xC8, 0x2B,         /* FF64 ADC $2BC8,X                              */
      0x8D, 0xFF, 0x26,         /* FF67 STA $26FF                                */
      0xBD, 0x70, 0x2B,         /* FF6A LDA $2B70,X                              */
      0x65, 0x00,               /* FF6D ADC <$00                                 */
      0x48,                     /* FF6F PHA                                      */
      0x38,                     /* FF70 SEC                                      */
      0xAD, 0xFF, 0x26,         /* FF71 LDA $26FF                                */
      0xED, 0xD8, 0xFE,         /* FF74 SBC $FED8                                */
      0x8D, 0xFF, 0x26,         /* FF77 STA $26FF                                */
      0x68,                     /* FF7A PLA                                      */
      0xED, 0xD9, 0xFE,         /* FF7B SBC $FED9     Z: within the 256 (BNE)    */
      0x60,                     /* FF7E RTS                                      */
      0xAD, 0xFF, 0x26,         /* FF7F spark: LDA $26FF                         */
      0x18,                     /* FF82 CLC                                      */
      0x6D, 0xD8, 0xFE,         /* FF83 ADC $FED8     back on the screen         */
      0x60,                     /* FF86 RTS                                      */
      0xAD, 0x13, 0x27,         /* FF87 spark: LDA $2713                         */
      0x18,                     /* FF8A CLC                                      */
      0x6D, 0xD8, 0xFE,         /* FF8B ADC $FED8                                */
      0x60}},                   /* FF8E RTS                                      */
};
#pragma GCC diagnostic pop
#define PARKED_TABLE 0x1E80   /* bank $00 offset ($FE80): a byte per object slot, 1 = parked */
#define HIT_BIAS 0x1ED8       /* bank $00 offset ($FED8): 16-bit screen X the hit test's 256 pixels start at */
#define ECHO_TABLE 0x1CC0     /* bank $00 offset ($FCC0): a byte per object slot, 1 = echoed */
#define N_PATCHES (int)(sizeof patch_tab / sizeof patch_tab[0])
/* The patches as they apply to the code loaded now (patch_tab moved to the layout of bank
 * $6F the stage has: see layout_6f) */
static Patch patches[N_PATCHES];

/* Bank $6F, the scroll-layer code, comes in two layouts: most stages' (its room start at
 * $A94C), and Stage 5's ship's (area $32, Death), where the same code stands $390 bytes
 * further on ($ACDC), the streaming table at $AB10 instead of $A780. The patches to it, and
 * the code of ours in bank $00 that reads and writes that table and jumps back into it, are
 * moved to whichever layout is loaded: shift_6f bytes on. */
static int shift_6f = -1;   /* -1: not worked out yet */
static bool layout_ok;      /* the layout loaded is one the patches fit (layout_6f) */
static const uint8_t ROOM_START_A[] = {0x82, 0xA9, 0x90, 0x9D, 0x66, 0x3E};   /* A94C (+ shift_6f) */
/* A9B1 (+ shift_6f): LDY $45; LDA $A994,Y (the table's address moves with the code) */
static uint8_t ROOM_START_B[] = {0xA4, 0x45, 0xB9, 0x94, 0xA9, 0x82};
static void patches_moved(int shift)
{
    memcpy(patches, patch_tab, sizeof patches);
    for (int i = 0; i < N_PATCHES; i++) {
        Patch *q = &patches[i];
        if (q->bank == 0x6F) q->off = (uint16_t)(q->off + shift);
        else if (!(q->bank == 0x00 && q->off == 0x1E10)) continue;   /* ("column drawing at both distances") */
        /* addresses in the table's code: $A780, $A781 (the table) and $A7A8 (back into the
         * column drawing) */
        for (int k = 0; k + 1 < q->len; k++)
            for (int which = 0; which < 2; which++) {
                uint8_t *b = which ? q->wide : q->orig;
                if (which == 0 && !q->orig[0]) continue;   /* (RTS filler) */
                if (b[k + 1] == 0xA7 && (b[k] == 0x80 || b[k] == 0x81 || b[k] == 0xA8)) {
                    int a = (0xA700 | b[k]) + shift;
                    b[k] = (uint8_t)a;
                    b[k + 1] = (uint8_t)(a >> 8);
                }
            }
    }
}
static void layout_6f(Pce *p);
#define N_WORLD 8          /* patches[0..7] change how the room's background is streamed into
                            * the map (put in or out as a room starts) */
#define WORLD_PATCHES ((1u << N_WORLD) - 1)
#define OBJECT_PATCHES (((1u << 8) - 1) << N_WORLD)   /* patches[8..15]: objects further out */
#define DRAW_PATCHES (((1u << N_PATCHES) - 1) & ~WORLD_PATCHES & ~OBJECT_PATCHES)   /* the rest */
/* of those, the ones that build sprites out past the game's screen (16-24: the piece culling
 * goes through the echoes' code, which leads to the negative X check): in a room kept 4:3 with
 * sides too, so a thing thrown or rolling past the old edge goes on being drawn (the barrel in
 * Stage 1's house). Not the hit test's (25): hits as in 4:3 there. */
#define SPRITE_PATCHES (((1u << 25) - 1) & ~((1u << 16) - 1))
/* objects reach further out only for a picture wider than the 64 px they already reach */
#define OBJECT_REACH 64
/* How far past the 4:3 screen objects are made and start moving in a widened room: the
 * picture's extension and a margin for wide sprites drawn around their X (the broken
 * bridge's sections and statues, 64 px and more), at most 120 (the game removes objects 128
 * px out). With the game's 64 (or the 72 used before), they appeared in the picture's edges
 * as they were made. */
#define OBJECT_MARGIN 50
static int object_reach(int ext)
{
    int d = ext + OBJECT_MARGIN;
    return d < 0x40 ? 0x40 : d > 0x78 ? 0x78 : d;
}
/* enemies sent in from just past the screen's edges start at most this far out (wide_frame) */
#define EDGE_SPAWN_MAX 80
/* frames after a room starts within which it may still open (wide_frame) */
#define LATE_OPEN 8

static uint8_t *at(Pce *p, const Patch *q) { return p->page[q->bank] ? p->page[q->bank] + q->off : NULL; }
static const uint8_t *orig_of(const Patch *q) { return q->orig[0] ? q->orig : RTS_FILLER; }

/* 0.6-0.12 and the first 16:9 build sent negative pieces to $FE00 with a patch at 72:8127
 * alone, now part of the one at 72:8123: a save state from then holds it. Put the game's
 * bytes back, so the patch here finds them. */
static void undo_old_patches(Pce *p)
{
    static const uint8_t old[7] = {0xF0, 0x13, 0x10, 0x03, 0x4C, 0x00, 0xFE};
    uint8_t *m = p->page[0x72] ? p->page[0x72] + 0x0123 : NULL;
    if (m && !memcmp(m, old, sizeof old)) {
        m[5] = 0x8C;
        m[6] = 0x81;
        p->cpu.page_gen[0x72]++;
    }
    /* interim builds changed the column drawing at $FE10 (52 or 64 bytes): a save state
     * holds that code, in use (6F:A7A5 jumps to it), so it is put back in place */
    const Patch *t = &patches[2];   /* "column drawing at both distances" */
    uint8_t *c = at(p, t);
    if (c && c[0] == 0xE0 && c[1] == 0x00 && c[0x24] == 0x40 && c[0x25] == 0xC0) {
        memcpy(c, t->wide, t->len);
        for (int i = t->len; i < 64; i++) c[i] = 0x60;
        p->cpu.page_gen[t->bank]++;
    }
    /* ... and the room start sweep's event check at $FFA0 (earlier: 52 or 68 bytes, the
     * right side only), in use while the spawner's call to it is in: replaced in place */
    for (int i = 0; i < N_PATCHES; i++) {
        const Patch *q = &patches[i];
        if (q->bank != 0x00 || q->off != 0x1F90) continue;
        uint8_t *e = at(p, q);
        if (e && memcmp(e, q->wide, q->len) &&
            ((e[0x10] == 0xAD && e[0x11] == 0x44 && e[0x12] == 0x27 && e[0x14] == 0x26) ||
             (e[0] == 0xA9 && e[2] == 0x8D && e[3] == 0xDA && e[4] == 0xFE))) {
            memcpy(e, q->wide, q->len);
            p->cpu.page_gen[q->bank]++;
        }
    }
}

/* Diagnostics: patches to leave out (bit i = patches[i]), counted as in place. */
uint32_t wide_debug_skip;

/* patches[0], the streaming distance, and patches[1], the column drawn at both distances:
 * while a column is drawn at the old distance ($FE10) the table holds the old one, and a
 * frame can end right then (the game running long). */
#define PATCH_DISTANCE 0
#define PATCH_TWICE 1

/* The object patches are little more than their varying byte, so their own bytes cannot
 * tell whether they are in: they are counted in while this is set (set_patches). */
static bool objects_on;

/* The patch's bytes are at m (its varying byte, if any, holding whatever value). */
static bool is_in(const uint8_t *m, const Patch *q)
{
    if (!q->var) return !memcmp(m, q->wide, q->len);
    return !memcmp(m, q->wide, q->var_at) &&
           !memcmp(m + q->var_at + 1, q->wide + q->var_at + 1, (size_t)(q->len - q->var_at - 1));
}

/* Which patches are in place. */
static uint32_t patches_in(Pce *p)
{
    uint32_t mask = wide_debug_skip;
    for (int i = 0; i < N_PATCHES; i++) {
        const uint8_t *m = at(p, &patches[i]);
        if (m && is_in(m, &patches[i]) && (objects_on || !(OBJECT_PATCHES & 1u << i))) mask |= 1u << i;
    }
    /* mid-column: the old distance, put there for a moment by the column drawing */
    const Patch *d = &patches[PATCH_DISTANCE];
    const uint8_t *m = at(p, d);
    if ((mask & 1u << PATCH_TWICE) && m && !memcmp(m, d->orig, d->len)) mask |= 1u << PATCH_DISTANCE;
    return mask;
}

/* Put the patches in `mask` in (or take them out) where the original (or patched) bytes
 * are found. */
static void set_patches(Pce *p, uint32_t mask, bool on)
{
    if (on && !layout_ok) mask &= ~WORLD_PATCHES;   /* (a layout of bank $6F the patches do not fit) */
    if (mask & OBJECT_PATCHES) objects_on = on;
    for (int i = 0; i < N_PATCHES; i++) {
        if (!(mask & 1u << i) || (wide_debug_skip & 1u << i)) continue;
        const Patch *q = &patches[i];
        uint8_t *m = at(p, q);
        if (!m) continue;
        if (on ? !memcmp(m, orig_of(q), q->len) : is_in(m, q)) {
            memcpy(m, on ? q->wide : orig_of(q), q->len);
            if (on && q->var && var_value[q->var]) m[q->var_at] = var_value[q->var];
            p->cpu.page_gen[q->bank]++;   /* recompiled code in this bank is re-checked */
        }
    }
}

/* Which layout of bank $6F is loaded (by its room start's bytes); on a change, what of ours
 * the old layout put in (the code in bank $00) is taken out, and the patches moved. */
/* Is the scroll code at `sh` bytes on? Its room start, and the streaming table and the
 * column drawing where the patches expect them (as the game has them, or patched). */
static bool layout_fits(const uint8_t *b, int sh)
{
    if (sh < 0 || sh > 0x1000) return false;
    const uint8_t *t = b + 0x0780 + sh, *c = b + 0x07A5 + sh;
    return !memcmp(b + 0x094C + sh, ROOM_START_A, sizeof ROOM_START_A) &&
           ((t[0] == 0x40 && t[1] == 0xC0) || (t[0] == STREAM && t[1] == 0x100 - STREAM)) &&
           ((c[0] == 0xBD && c[1] == 0x66 && c[2] == 0x3E) || (c[0] == 0x4C && c[1] == 0x10 && c[2] == 0xFE));
}

/* Which layout of bank $6F is loaded: the room start found anywhere in the bank, the table
 * and column drawing checked where they would then stand (most stages: 0; Stage 5's ship:
 * $390; area $1C's castle: $389). None that fits: the bank is left alone (layout_ok false,
 * set_patches puts none of the background patches in). On a change, what of ours the old
 * layout put in is taken out first, and the patches moved. */
static void layout_6f(Pce *p)
{
    const uint8_t *b = p->page[0x6F];
    int sh = shift_6f;
    bool ok = false;
    if (b && sh >= 0 && layout_fits(b, sh)) ok = true;
    else if (b) {
        for (int i = 0x094C; i + (int)sizeof ROOM_START_A <= 0x2000 && !ok; i++)
            if (b[i] == ROOM_START_A[0] && layout_fits(b, i - 0x094C)) {
                sh = i - 0x094C;
                ok = true;
            }
    }
    if (!ok && sh < 0) sh = 0;
    /* (our column drawing in bank $00, made for another layout and left there, as a save
     * state from another stage holds it, writes into the middle of this layout's code: put
     * back to the filler, unless this layout's patch jumps to it) */
    uint8_t *fe = p->page[0x00] ? p->page[0x00] + 0x1E10 : NULL;
    if (fe && fe[0] == 0x5A && fe[1] == 0xDA && fe[2] == 0xA9 && fe[3] == 0x40 && fe[4] == 0x8D) {
        Patch cur = patch_tab[2];
        if (sh != shift_6f || shift_6f < 0) {
            patches_moved(sh);
            cur = patches[2];
            if (shift_6f >= 0) patches_moved(shift_6f);
        } else {
            cur = patches[2];
        }
        const uint8_t *c = b ? b + 0x07A5 + sh : NULL;
        bool jumps = ok && c && c[0] == 0x4C && c[1] == 0x10 && c[2] == 0xFE;
        if (!ok || (memcmp(fe, cur.wide, cur.len) && !jumps)) {
            memset(fe, 0x60, cur.len);
            p->cpu.page_gen[0x00]++;
        }
    }
    if (sh == shift_6f && ok == layout_ok) return;
    if (shift_6f >= 0 && layout_ok) set_patches(p, WORLD_PATCHES, false);
    shift_6f = sh;
    layout_ok = ok;
    patches_moved(sh);
    int a = 0xA994 + sh;
    ROOM_START_B[3] = (uint8_t)a;
    ROOM_START_B[4] = (uint8_t)(a >> 8);
}

/* The hit test's four places in bank $6B (see "hit boxes moved by HIT_BIAS"): each stage
 * has its own copy of the bank, the code at a different offset, so they are found by their
 * bytes (each unique in the bank). Put in (on) or taken out, while the code they call is in. */
static const struct { uint8_t len; uint8_t orig[15], wide[15]; } HIT_SITES[4] = {
    {15, {0x18, 0x7D, 0xC8, 0x2B, 0x9D, 0x10, 0x27, 0xBD, 0x70, 0x2B, 0x65, 0x00, 0x9D, 0x30, 0x27},
     {0x20, 0x40, 0xFF, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA}},
    /* (the object's: up to its BNE, which stays) */
    {12, {0x18, 0x7D, 0xC8, 0x2B, 0x8D, 0xFF, 0x26, 0xBD, 0x70, 0x2B, 0x65, 0x00},
     {0x20, 0x63, 0xFF, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA}},
    {6, {0xAD, 0xFF, 0x26, 0x8D, 0xEB, 0x3B}, {0x20, 0x7F, 0xFF, 0x8D, 0xEB, 0x3B}},
    {6, {0xAD, 0x13, 0x27, 0x8D, 0xED, 0x3B}, {0x20, 0x87, 0xFF, 0x8D, 0xED, 0x3B}},
};
static int hit_at[4] = {-1, -1, -1, -1};   /* where each was last found */

static int hit_find(const uint8_t *page, const uint8_t *bytes, int len, int hint)
{
    if (hint >= 0 && hint + len <= 0x2000 && !memcmp(page + hint, bytes, len)) return hint;
    for (int i = 0; i + len <= 0x2000; i++)
        if (page[i] == bytes[0] && !memcmp(page + i, bytes, len)) return i;
    return -1;
}

/* Stage objects that act at a distance of their own from the 4:3 screen, inside the wider
 * picture (found by their code's bytes in the stage's banks, like the hit test's):
 * - Stage 4's swinging spiked balls on chains (bank $81; kinds $C4 chain, $C3 ball): each chain
 *   piece removes itself at screen X $160 or -$60, 96 px past the 4:3 screen, and the ball goes
 *   with it; with the picture reaching 128 px out (Level ends) and the ball swinging some 60 px
 *   from its anchor, it vanished in sight. Moved out to +-192 ($1C0 / -$C0).
 * - Stage 2's ship (bank $71, kind $BB, 71:D08C): the object that paints a girder's end into
 *   the background map does so once it is within 64 px of the screen's left (X >= -$40), which
 *   is the wide picture's left edge: the girder appeared in sight. From -$70 instead (from -$80
 *   it also painted where the 512 px map holds the room further on: girder tiles left under
 *   the next part's spiked chairs; -$70 keeps it clear and shows most of the girder early) (it runs at
 *   any distance: its flags leave out the game's distance gate, 70:A09D). keep_tiles keeps the
 *   tiles if a column drawn afterwards would put the level's blank cells back.
 * Both while the picture is wide. */
static const struct { uint8_t bank, len, at_r, at_l; uint8_t orig[21]; uint8_t wide_r, wide_l; } FAR_SITES[] = {
    {0x81, 21, 4, 15, {0xBD, 0xC8, 0x2B, 0xC9, 0x60, 0x90, 0x03, 0x4C, 0x54, 0x40, 0x60,
                       0xBD, 0xC8, 0x2B, 0xC9, 0xA0, 0xB0, 0x03, 0x4C, 0x54, 0x40}, 0xC0, 0x40},
    {0x71, 16, 13, 13, {0xBD, 0x70, 0x2B, 0x10, 0x0B, 0xC9, 0xFF, 0xD0, 0xF3,
                        0xBD, 0xC8, 0x2B, 0xC9, 0xC0, 0x90, 0xEC}, 0x90, 0x90},
};
#define N_FAR (int)(sizeof FAR_SITES / sizeof FAR_SITES[0])
static int far_at[N_FAR] = {-1, -1};

static void far_patches(Pce *p, bool on)
{
    for (int k = 0; k < N_FAR; k++) {
        uint8_t *page = p->page[FAR_SITES[k].bank];
        if (!page) continue;
        uint8_t want[21];
        memcpy(want, FAR_SITES[k].orig, FAR_SITES[k].len);
        want[FAR_SITES[k].at_r] = FAR_SITES[k].wide_r;
        want[FAR_SITES[k].at_l] = FAR_SITES[k].wide_l;
        const uint8_t *from = on ? FAR_SITES[k].orig : want, *to = on ? want : FAR_SITES[k].orig;
        int len = FAR_SITES[k].len;
        int at_ = hit_find(page, to, len, far_at[k]);
        if (at_ >= 0) { far_at[k] = at_; continue; }   /* already as wanted */
        at_ = hit_find(page, from, len, far_at[k]);
        if (at_ < 0) continue;
        memcpy(page + at_, to, len);
        far_at[k] = at_;
        p->cpu.page_gen[FAR_SITES[k].bank]++;
    }
}

/* The game removes objects more than 128 px past its screen (70:A06C: X < -$80, 70:A079:
 * X >= $180). At a level's end the picture reaches that far (64 px a side, shifted 64 more:
 * Level ends), so things there vanished in sight (Stage 4's block switch). While the picture
 * is wide the line is moved 32 px further out, -$A0 / $1A0. (The bytes round each, checked.) */
static const uint8_t REMOVE_CODE[] = {0xBD, 0x70, 0x2B, 0xF0, 0x16, 0x10, 0x0D, 0xBD, 0xC8, 0x2B, 0xC9, 0x80, 0xB0, 0x0D,
                                      0x20, 0x54, 0x40, 0x4C, 0x15, 0xA1, 0xBD, 0xC8, 0x2B, 0xC9, 0x80, 0xB0, 0xF3};
#define REMOVE_AT 0x0065            /* bank $70 offset (70:A065) */
#define REMOVE_LEFT 11              /* ... its CMP's value: X's low byte below it (X < 0) */
#define REMOVE_RIGHT 24             /* ... and above it (X >= $100) */

static void remove_patch(Pce *p, bool on)
{
    uint8_t *m = p->page[0x70];
    if (!m) return;
    m += REMOVE_AT;
    uint8_t l = on ? 0x60 : 0x80, r = on ? 0xA0 : 0x80;
    for (int i = 0; i < (int)sizeof REMOVE_CODE; i++)
        if (i != REMOVE_LEFT && i != REMOVE_RIGHT && m[i] != REMOVE_CODE[i]) return;   /* (not this code) */
    if ((m[REMOVE_LEFT] != 0x80 && m[REMOVE_LEFT] != 0x60) || (m[REMOVE_RIGHT] != 0x80 && m[REMOVE_RIGHT] != 0xA0)) return;
    if (m[REMOVE_LEFT] == l && m[REMOVE_RIGHT] == r) return;
    m[REMOVE_LEFT] = l;
    m[REMOVE_RIGHT] = r;
    p->cpu.page_gen[0x70]++;
}

static void hit_patches(Pce *p, bool on)
{
    uint8_t *page = p->page[0x6B];
    if (!page) return;
    const Patch *code = NULL;
    for (int i = 0; i < N_PATCHES; i++)
        if (patches[i].bank == 0x00 && patches[i].off == 0x1F40) code = &patches[i];
    const uint8_t *c = code && p->page[0x00] ? p->page[0x00] + code->off : NULL;
    if (on && (!c || !is_in(c, code))) on = false;   /* (nothing to call) */
    bool changed = false;
    for (int k = 0; k < 4; k++) {
        const uint8_t *from = on ? HIT_SITES[k].orig : HIT_SITES[k].wide;
        const uint8_t *to = on ? HIT_SITES[k].wide : HIT_SITES[k].orig;
        int len = HIT_SITES[k].len;
        if (hit_find(page, to, len, hit_at[k]) >= 0) {   /* already as wanted */
            hit_at[k] = hit_find(page, to, len, hit_at[k]);
            continue;
        }
        int at_ = hit_find(page, from, len, hit_at[k]);
        if (at_ < 0) continue;
        memcpy(page + at_, to, len);
        hit_at[k] = at_;
        changed = true;
    }
    if (changed) p->cpu.page_gen[0x6B]++;
}

/* Patches in place get this frame's values (var_value). */
static void update_vars(Pce *p)
{
    for (int i = 0; i < N_PATCHES; i++) {
        const Patch *q = &patches[i];
        if (!q->var || (wide_debug_skip & 1u << i) || (!objects_on && (OBJECT_PATCHES & 1u << i))) continue;
        uint8_t *m = at(p, q);
        uint8_t v = var_value[q->var] ? var_value[q->var] : q->wide[q->var_at];
        if (m && is_in(m, q) && m[q->var_at] != v) {
            m[q->var_at] = v;
            p->cpu.page_gen[q->bank]++;
        }
    }
}

int wide_extent(double window_aspect, double picture_aspect)
{
    double w = 256.0 * window_aspect / picture_aspect;
    int e = (int)floor((w - 256.0) / 2.0);
    if (e < 0) e = 0;
    if (e > WIDE_MAX) e = WIDE_MAX;
    /* no further than the game has its background drawn: columns 32 px wide streamed STREAM
     * px past the edge when the camera crosses one, so (STREAM / 32) * 32 = 64 px are always
     * there; at 70 the last 6 px showed what the map held before every 32 px walked */
    if (e > WIDE_STREAMED) e = WIDE_STREAMED;
    return e & ~1;
}

/* ---- objects ------------------------------------------------------------------------ */

/* Object arrays (88 slots, work RAM offsets): kind, flags, screen X low/high, type. */
#define OBJ_SLOTS 0x58
#define OBJ_KIND 0x0750      /* 0 = free slot */
#define OBJ_FLAGS 0x10F0     /* bit 0: fixed to the screen (does not scroll) */
#define ECHO_STILL_FRAMES 8  /* an echoed object has moved within this many frames */
#define FADE_IN_FRAMES 30    /* a scenery object made past the 4:3 spawn line fades in over this */
static uint8_t fade_t[0x58];
#define OBJ_XL 0x0BC8
#define OBJ_XH 0x0B70
#define OBJ_FACING 0x0AC0    /* 0 = facing right (the hit test turns boxes round by it) */
#define OBJ_TYPE 0x0A10
#define OBJ_LAYER 0x0F38     /* & $7F: the scroll layer the object moves with (0 = the camera) */
#define HUD_SLOT 0x57        /* the HUD (health, hearts, score): type 5, screen X 8 */
#define SUBWEAPON_SLOT 0x56  /* the sub-weapon icon hanging from the HUD: type 3, screen X 28 */
#define OBJ_FRAME 0x0A68     /* sprite frame drawn; 0 = none */

/* Some objects are generators fixed to the screen at the spawn edge (e.g. Stage 1's
 * skeleton generator sits at screen X 320 and drops skeletons there). While objects spawn
 * further out (right / left: pixels past that side of the screen; the game's is 64), move
 * the ones at the game's edge or at the last one we used (gen_r / gen_l) to the new one. */
static void move_generators(Wide *w, Pce *p, int right, int left)
{
    uint8_t *r = p->wram;
    int was_r = w->gen_r ? w->gen_r : 0x40, was_l = w->gen_l ? w->gen_l : 0x40;
    for (int i = 0; i < OBJ_SLOTS; i++) {
        if (!r[OBJ_KIND + i] || !(r[OBJ_FLAGS + i] & 1)) continue;
        int x = r[OBJ_XL + i] | r[OBJ_XH + i] << 8;
        int to = -1;
        if (x == 0x0140 || x == 0x0100 + was_r) to = 0x0100 + right;
        else if (x == 0x10000 - 0x40 || x == 0x10000 - was_l) to = 0x10000 - left;
        if (to >= 0 && to != x) {
            r[OBJ_XL + i] = (uint8_t)to;
            r[OBJ_XH + i] = (uint8_t)(to >> 8);
        }
    }
    w->gen_r = right;
    w->gen_l = left;
}

/* Move one HUD object `by` px from its original screen X (home): only if it is the HUD's
 * (its type) and anywhere from home out to as far as widescreen ever puts it on that side
 * (where the game puts it, or this or an earlier session did: a save state holds it). */
static void place_hud_object(Pce *p, int slot, uint8_t type, int home, int by)
{
    uint8_t *r = p->wram;
    if (r[OBJ_TYPE + slot] != type) return;
    int x = (int16_t)(r[OBJ_XL + slot] | r[OBJ_XH + slot] << 8);
    int reach = 2 * WIDE_MAX;
    if (by <= 0 ? (x > home || x < home - reach) : (x < home || x > home + reach)) return;
    int to = home + by;
    r[OBJ_XL + slot] = (uint8_t)to;
    r[OBJ_XH + slot] = (uint8_t)(to >> 8);
}

/* Keep the HUD at the edges of the picture: at the left the frame (health, hearts, score)
 * and the sub-weapon icon that hangs from it, both set up by each stage as it starts; at the
 * right a boss's health bar (slot $55, type 5, settled at X 216 after sliding in). The bar
 * cannot be moved as an object (past X 253 its own code slides it in again), so its sprites
 * (patterns $1F8-$1FF, the bar's own) are drawn further right instead. The stage's title
 * letters ($54-$55, the one in $55 at X 88) are not the HUD's and stay centred. left / right:
 * how far the picture reaches past the original 256 pixels on each side. */
#define BOSS_BAR_SLOT 0x55
static void place_hud_left(Pce *p, int left)
{
    place_hud_object(p, HUD_SLOT, 0x05, 8, -left);
    place_hud_object(p, SUBWEAPON_SLOT, 0x03, 28, -left);
}

static void place_hud(Pce *p, int left, int right)
{
    place_hud_left(p, left);
    const uint8_t *r = p->wram;
    int bx = r[OBJ_XL + BOSS_BAR_SLOT] | r[OBJ_XH + BOSS_BAR_SLOT] << 8;
    bool bar = r[OBJ_TYPE + BOSS_BAR_SLOT] == 0x05 && r[OBJ_FRAME + BOSS_BAR_SLOT] && bx >= 216 && bx <= 254;
    vdc_set_sprite_move(0x1F8, bar ? 0x1FF : 0, right);
}

/* ---- the room ----------------------------------------------------------------------- */

/* Scroll layers (work RAM): layer 0 is the playfield camera. */
#define CAM_XL 0x1E08        /* X low byte, layers 0-7 */
#define CAM_PAGE 0x1E96      /* X high byte mod 16, layers 0-7 */
#define CAM_Y 0x1E20         /* Y low byte, layers 0-7 */
#define CAM_Y_PAGE 0x1E8E    /* Y high byte, layers 0-7 */
#define LIMIT_LEFT 0x1EBE    /* camera's leftmost page */
#define LIMIT_RIGHT 0x1EC6   /* camera's rightmost X: page, low byte */

/* signed distance a - b in the camera's 4096-pixel space */
static int wrap(int d) { d &= 0xFFF; return d >= 0x800 ? d - 0x1000 : d; }

/* The room's walls relative to camera X cam: left, the left wall's screen X (<= 0 inside the
 * room), right, the right wall's (just past its last pixel). Measured directly while the
 * camera is within the room's limits (a room can be wider than 2048 px: the boat ride's
 * is 3584, where the wrapped distance put the left wall on the screen's right and blacked
 * the whole background out); else the nearest way round. */
static void walls_of(int lo, int hi, int cam, int *left, int *right)
{
    if (cam >= lo && cam <= hi) {
        *left = lo - cam;
        *right = hi - cam + 256;
    } else {
        *left = wrap(lo - cam);
        *right = wrap(hi - cam) + 256;
    }
}

/* an open arena made walkable (arena_walk): its left limit, which the game cannot hold (its
 * left limit is a page), or -1 */
static int walk_lo = -1;
static int limit_lo(const uint8_t *r) { return walk_lo >= 0 ? walk_lo : r[LIMIT_LEFT] * 256; }
static int limit_hi(const uint8_t *r) { return r[LIMIT_RIGHT] * 256 + r[LIMIT_RIGHT + 1]; }

static void room_walls(const uint8_t *r, int cam, int *left, int *right)
{
    walls_of(limit_lo(r), limit_hi(r), cam, left, right);
}

/* Boss rooms one screen wide (the camera locked to one screen for the whole room) that have
 * no wall at their sides: the level carries on beside the arena, and the game draws it there
 * as the room starts (64 px left of the screen, 96 right; 6F:A94C), as it was when the
 * camera came in. With Video > Open arenas these are shown wide, as the game draws them
 * (no other change to the room), instead of kept 4:3. Matched by area ($A2, Richter's and
 * Maria's alike), room ($83) and the camera's locked X. */
#define AREA 0xA2
#define ROOM 0x83
/* quiet: kinds of object removed while the arena is walkable (arena_walk), things of the
 * level beside it that the 4:3 fight never brings in because its camera never moves */
/* left: opened to the left only, by twice the side (the level's floor ends at the arena's
 * right edge); flat: no parallax while walkable (flat_layers) */
static const struct { uint8_t area, room; uint16_t cam; uint8_t quiet[4]; bool left, flat; } ARENAS[] = {
    /* Stage 3: the giant three-eyed skull, before the bridge. A bat generator ($35, 250 px
     * left of the arena, with its launchers $36) sends its bats ($34) in at the game's
     * screen edge once the camera has gone left. */
    {0x14, 0x00, 0x0300, {0x34, 0x35, 0x36}, false, false},
    /* Stage 1: the wyvern, at the castle gate. Its floor ends at the 4:3 picture's right edge
     * (opened both ways, Richter walked off it and fell, at X 777), and walking in, Level
     * ends already shows the level up to there: opened to the left only, the picture stays
     * as it stood as the camera locks (opened both ways, it jumped 71 px). */
    {0x0E, 0x00, 0x0200, {0}, true, true},
    /* an arena of the host's own (secret.c), its camera held at page 3 while it is fought */
    {0x10, 0x01, 0x0300, {0}, false, false},
};

static bool open_arena(const uint8_t *r)
{
    int lo = limit_lo(r), hi = limit_hi(r);
    if (lo != hi) return false;
    for (size_t i = 0; i < sizeof ARENAS / sizeof ARENAS[0]; i++)
        if ((r[AREA] & 0xFE) == ARENAS[i].area && r[ROOM] == ARENAS[i].room && lo == ARENAS[i].cam) return true;
    return false;
}

/* ---- walkable arenas ------------------------------------------------------------------
 * An open arena is made as wide to play in as the picture (Video > Open arenas): its camera
 * limits are widened by the extension each way, and Level ends' shift moves the picture
 * back as the camera moves, so the arena stands still on the screen while the game's own
 * 256 pixels follow Richter (the ground under him, hits and the rest work as ever). The
 * right limit is a page and X ($1EC6/$1EC7) and the game holds it as it is; the left is a
 * page alone ($1EBE: the camera stops at its X 0). So the left check in Richter's walk
 * (6A:6971, the camera at the limit or crossing it while he walks left) is sent to code of
 * ours in bank $00 that does the same against a left limit with an X (WALK_LEFT). */
#define WALK_CODE 0x1A00   /* bank $00 offset ($FA00): our left check */
#define WALK_LEFT 0x1EDA   /* bank $00 offset ($FEDA): the left limit's X, then page */
static const uint8_t WALK_SITE[18] = {   /* 6A:6971: LDA $3E96 / CMP $3EBE / BNE / LDA $3E00 / ORA $3E08 / BNE / BRA */
    0xAD, 0x96, 0x3E, 0xCD, 0xBE, 0x3E, 0xD0, 0x0A, 0xAD, 0x00, 0x3E, 0x0D, 0x08, 0x3E, 0xD0, 0x09, 0x80, 0xCC};
static const uint8_t WALK_JUMP[18] = {   /* JMP $FA00 */
    0x4C, 0x00, 0xFA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA};
/* (the site's neighbours it jumps to, checked by their first bytes: offset from the site) */
static const struct { int at; uint8_t len, bytes[6]; } WALK_NEAR[4] = {
    {-0x22, 5, {0xA5, 0x04, 0x8D, 0x20, 0x2C}},         /* 694F: Richter moves, the camera not */
    {-0xA8, 5, {0xAD, 0xC8, 0x2B, 0xC9, 0x80}},         /* 68C9: carry = Richter right of centre */
    {+0x75, 6, {0xAD, 0xD8, 0x2D, 0x8D, 0x7E, 0x3E}},   /* 69E6: the camera moves at his speed */
    {+0x82, 5, {0xA9, 0x80, 0x8D, 0xC8, 0x2B}},         /* 69F3: he is put at centre, the camera moves */
};
#define WALK_CODE_LEN 0x8D
static int walk_site = -1;   /* where WALK_SITE was last found in bank $6A */

/* our left check at $FA00, jumping back into the walk at the site's neighbours (a = 694F,
 * b = 68C9, c = 69E6, d = 69F3 as the dump has them) */
static void walk_code(uint8_t *o, int site)
{
    int base = 0x6000 + site;
    int a = base - 0x22, b = base - 0xA8, c = base + 0x75, d = base + 0x82;
    const uint8_t t[WALK_CODE_LEN] = {
        0xAD, 0x00, 0x3E,         /* FA00 LDA $3E00     camera at the limit (sub 0, X, page)? */
        0xD0, 0x13,               /* FA03 BNE notat                                         */
        0xAD, 0x08, 0x3E,         /* FA05 LDA $3E08                                         */
        0xCD, 0xDA, 0xFE,         /* FA08 CMP $FEDA                                         */
        0xD0, 0x0B,               /* FA0B BNE notat                                         */
        0xAD, 0x96, 0x3E,         /* FA0D LDA $3E96                                         */
        0xCD, 0xDB, 0xFE,         /* FA10 CMP $FEDB                                         */
        0xD0, 0x03,               /* FA13 BNE notat                                         */
        0x4C, (uint8_t)a, (uint8_t)(a >> 8),   /* FA15 JMP a  Richter moves              */
        0x20, (uint8_t)b, (uint8_t)(b >> 8),   /* FA18 notat: JSR b  right of centre?    */
        0xB0, 0x1E,               /* FA1B BCS off                                           */
        0xAD, 0xD8, 0x2D,         /* FA1D LDA $2DD8     new camera = camera + his speed      */
        0x18,                     /* FA20 CLC                                               */
        0x6D, 0x00, 0x3E,         /* FA21 ADC $3E00                                         */
        0x85, 0x02,               /* FA24 STA $02                                           */
        0xAD, 0x80, 0x2D,         /* FA26 LDA $2D80                                         */
        0x6D, 0x08, 0x3E,         /* FA29 ADC $3E08                                         */
        0x85, 0x03,               /* FA2C STA $03                                           */
        0xA9, 0xFF,               /* FA2E LDA #$FF                                          */
        0x6D, 0x96, 0x3E,         /* FA30 ADC $3E96     (its page)                          */
        0x20, 0x82, 0xFA,         /* FA33 JSR cmp                                           */
        0x90, 0x1F,               /* FA36 BCC cross                                         */
        0x4C, (uint8_t)c, (uint8_t)(c >> 8),   /* FA38 JMP c                              */
        0xA5, 0x00,               /* FA3B off: LDA $00  new camera = camera + his offset    */
        0x18,                     /* FA3D CLC                                               */
        0x6D, 0x00, 0x3E,         /* FA3E ADC $3E00                                         */
        0x85, 0x02,               /* FA41 STA $02                                           */
        0xA5, 0x01,               /* FA43 LDA $01                                           */
        0x6D, 0x08, 0x3E,         /* FA45 ADC $3E08                                         */
        0x85, 0x03,               /* FA48 STA $03                                           */
        0xA9, 0xFF,               /* FA4A LDA #$FF                                          */
        0x6D, 0x96, 0x3E,         /* FA4C ADC $3E96                                         */
        0x20, 0x82, 0xFA,         /* FA4F JSR cmp                                           */
        0x90, 0x03,               /* FA52 BCC cross                                         */
        0x4C, (uint8_t)d, (uint8_t)(d >> 8),   /* FA54 JMP d                              */
        0x62,                     /* FA57 cross: CLA   camera speed = limit - camera        */
        0x38,                     /* FA58 SEC                                               */
        0xED, 0x00, 0x3E,         /* FA59 SBC $3E00                                         */
        0x8D, 0x7E, 0x3E,         /* FA5C STA $3E7E                                         */
        0xAD, 0xDA, 0xFE,         /* FA5F LDA $FEDA                                         */
        0xED, 0x08, 0x3E,         /* FA62 SBC $3E08                                         */
        0x8D, 0x86, 0x3E,         /* FA65 STA $3E86                                         */
        0xA5, 0x03,               /* FA68 LDA $03      Richter moves the rest: new - limit  */
        0x38,                     /* FA6A SEC                                               */
        0xED, 0xDA, 0xFE,         /* FA6B SBC $FEDA                                         */
        0x85, 0x03,               /* FA6E STA $03                                           */
        0xAD, 0x20, 0x2C,         /* FA70 LDA $2C20                                         */
        0x18,                     /* FA73 CLC                                               */
        0x65, 0x02,               /* FA74 ADC $02                                           */
        0x8D, 0x20, 0x2C,         /* FA76 STA $2C20                                         */
        0xAD, 0xC8, 0x2B,         /* FA79 LDA $2BC8                                         */
        0x65, 0x03,               /* FA7C ADC $03                                           */
        0x8D, 0xC8, 0x2B,         /* FA7E STA $2BC8                                         */
        0x60,                     /* FA81 RTS                                               */
        0xCD, 0xDB, 0xFE,         /* FA82 cmp: CMP $FEDB  carry = new camera >= the limit   */
        0xD0, 0x05,               /* FA85 BNE done                                          */
        0xA5, 0x03,               /* FA87 LDA $03                                           */
        0xCD, 0xDA, 0xFE,         /* FA89 CMP $FEDA                                         */
        0x60,                     /* FA8C done: RTS                                         */
    };
    memcpy(o, t, sizeof t);
}

/* Put the walk's left check in (on) or take it out; false when it is not in place after. */
static bool walk_patch(Pce *p, bool on)
{
    uint8_t *g = p->page[0x6A], *z = p->page[0x00];
    if (!g || !z) return false;
    int s = hit_find(g, on ? WALK_SITE : WALK_JUMP, 18, walk_site);
    if (s < 0) {   /* already as wanted, or not this code */
        s = hit_find(g, on ? WALK_JUMP : WALK_SITE, 18, walk_site);
        if (s >= 0) walk_site = s;
        return on && s >= 0;
    }
    walk_site = s;
    if (!on) {
        memcpy(g + s, WALK_SITE, 18);
        p->cpu.page_gen[0x6A]++;
        return false;
    }
    for (int k = 0; k < 4; k++) {
        int at_ = s + WALK_NEAR[k].at;
        if (at_ < 0 || at_ + WALK_NEAR[k].len > 0x2000 || memcmp(g + at_, WALK_NEAR[k].bytes, WALK_NEAR[k].len)) return false;
    }
    uint8_t code[WALK_CODE_LEN];
    walk_code(code, s);
    uint8_t *c = z + WALK_CODE;
    if (memcmp(c, code, sizeof code)) {
        for (int i = 0; i < WALK_CODE_LEN; i++)
            if (c[i] != 0x60) return false;   /* (not our free space) */
        memcpy(c, code, sizeof code);
        p->cpu.page_gen[0x00]++;
    }
    memcpy(g + s, WALK_JUMP, 18);
    p->cpu.page_gen[0x6A]++;
    return true;
}

static int walk_cam0;   /* the arena's own camera X (its limits as the game has them) */
static bool walk_flat;  /* its parallax turned off (ARENAS flat) */
/* In a walkable arena the scenery layers (1-3: the camera / 2, / 4, / 8; the wyvern's gate
 * wall, railing and castle) moving at their own speeds put the backdrop's sprite pieces out
 * of line with it as Richter walked, where the 4:3 fight never moves at all: the parallax is
 * turned off instead (the user's choice), the whole scene moving as one. With Level ends the
 * layers go with the camera from where they stood at the arena's own camera; without, they
 * are held there. The backdrop's own objects (OBJ_LAYER 2, 3: the castle's pieces) are placed
 * by the game from these layer X's, so they go with them. Drifting layers (the clouds) drift.
 * (Richter once fell through the floor while this was being tried: that was the floor's end,
 * see the arena's `right`, not the layers.) */
#define LAYER_SUB 0x1E00     /* each layer's X fraction */
#define LAYER_VLO 0x1E7E     /* each layer's X speed: fraction, whole (added each frame, 6F:A6F4) */
#define LAYER_VHI 0x1E86
/* Each frame, partway down it, the game works out a layer's speed as half the next layer
 * in's (6F:A12F: the whole byte, then the fraction, each shifted right) and adds the speeds
 * in. Set from outside the frame, the layers lagged the camera a frame and shook. So, while
 * an arena is walkable, the halving is taken out (the two RORs made NOPs): the layers get
 * the camera's own speed and the game moves them with it, in step; their places are lined
 * up with the camera's once a frame (flat_layers). */
static const uint8_t HALVE_SITE[] = {0xBD, 0x86, 0x3E, 0x6A, 0x99, 0x86, 0x3E, 0xBD, 0x7E, 0x3E, 0x6A, 0x99, 0x7E, 0x3E, 0x60};
static const uint8_t HALVE_FLAT[] = {0xBD, 0x86, 0x3E, 0xEA, 0x99, 0x86, 0x3E, 0xBD, 0x7E, 0x3E, 0xEA, 0x99, 0x7E, 0x3E, 0x60};
static int halve_site = -1;
static void halve_patch(Pce *p, bool on)
{
    uint8_t *g = p->page[0x6F];
    if (!g) return;
    int at_ = hit_find(g, on ? HALVE_SITE : HALVE_FLAT, sizeof HALVE_SITE, halve_site);
    if (at_ < 0) return;   /* (already as wanted, or not this code) */
    halve_site = at_;
    memcpy(g + at_, on ? HALVE_FLAT : HALVE_SITE, sizeof HALVE_SITE);
    p->cpu.page_gen[0x6F]++;
}
#define LAYER_PAGE 0x1E10    /* each layer's X high byte (and the nibble at CAM_PAGE) */
static int flat_shift = 1 << 30;   /* the picture's shift last frame (1 << 30: none yet) */
static void layer_set(uint8_t *r, int k, int x)
{
    r[CAM_XL + k] = (uint8_t)x;
    r[LAYER_PAGE + k] = (uint8_t)(x >> 8);
    r[CAM_PAGE + k] = (uint8_t)((r[CAM_PAGE + k] & 0xF0) | ((x >> 8) & 0x0F));
}
static void flat_layers(uint8_t *r, int cam0, int shift)
{
    int cam = (r[CAM_PAGE] & 0x0F) << 8 | r[CAM_XL];
    /* (each from where it stands at the arena's own camera: as the camera locks, the castle
     * where it naturally is) */
    for (int k = 1; k <= 3; k++) {
        layer_set(r, k, (cam0 >> k) + (cam - cam0));
        r[LAYER_SUB + k] = r[LAYER_SUB];
    }
    /* The clouds (layers 4-6) drift by themselves, but the picture's shift that holds the
     * arena still carried them along as Richter walked: the shift's change is taken back out
     * of them, so they only drift. */
    if (flat_shift != 1 << 30 && shift != flat_shift)
        for (int k = 4; k <= 6; k++) {
            int x = (r[LAYER_PAGE + k] << 8 | r[CAM_XL + k]) - (shift - flat_shift);
            layer_set(r, k, x);
        }
    flat_shift = shift;
}

/* Each frame: an open arena (ARENAS) in play is made walkable, its right limit widened by
 * the extension and the left one (WALK_LEFT) likewise; true while it is. Once widened it
 * stays so until the game puts the room's own limits back (a new room, a life lost), even
 * with the option turned off meanwhile (the camera may stand away from the arena's X). */
static bool arena_walk(Wide *w, Pce *p, uint8_t s)
{
    uint8_t *r = p->wram;
    int cam0 = -1;
    const uint8_t *quiet = NULL;
    bool left = false;
    for (size_t i = 0; i < sizeof ARENAS / sizeof ARENAS[0]; i++)
        if ((r[AREA] & 0xFE) == ARENAS[i].area && r[ROOM] == ARENAS[i].room && r[LIMIT_LEFT] == ARENAS[i].cam >> 8) {
            cam0 = ARENAS[i].cam;
            quiet = ARENAS[i].quiet;
            left = ARENAS[i].left;
            walk_flat = ARENAS[i].flat;
        }
    int hi = r[LIMIT_RIGHT] * 256 + r[LIMIT_RIGHT + 1];
    bool own = cam0 >= 0 && hi == cam0 && (cam0 & 0xFF) == 0;   /* (the limits as the game has them) */
    /* (widened by us: also from a save state made while it was, when walk_lo is not set) */
    bool ours = cam0 >= 0 && hi > cam0 && hi <= cam0 + WIDE_MAX;
    bool on = s == RONDO_PLAYING && (ours || (own && w->want > 0 && w->open_arenas));
    uint8_t *z = p->page[0x00];
    if (on && z && walk_patch(p, true)) {
        /* (as wide as the picture; from a save state made while it was widened otherwise,
         * by an older build or another width, widened again to this one) */
        int e = w->want > 0 ? w->want : left ? 1 : hi - cam0;
        /* (the room as wide as the picture either way, so Level ends holds it still; one
         * pixel past the arena's own camera on the right at least: with the limits equal the
         * room is locked again, 4:3) */
        int er = left ? 1 : e, el = left ? 2 * e - 1 : e;
        if (cam0 + er != hi) {
            r[LIMIT_RIGHT] = (uint8_t)((cam0 + er) >> 8);
            r[LIMIT_RIGHT + 1] = (uint8_t)(cam0 + er);
        }
        walk_lo = cam0 - el;
        walk_cam0 = cam0;
        z[WALK_LEFT] = (uint8_t)walk_lo;
        z[WALK_LEFT + 1] = (uint8_t)(walk_lo >> 8);
        /* the arena's quiet kinds removed (no sprite left behind: kind 0 with a frame is
         * drawn, as the HUD is) */
        for (int i = 0x10; i < OBJ_SLOTS; i++)
            for (int k = 0; k < 4 && quiet[k]; k++)
                if (r[OBJ_KIND + i] == quiet[k]) {
                    r[OBJ_KIND + i] = 0;
                    r[OBJ_FRAME + i] = 0;
                }
        return true;
    }
    walk_patch(p, false);
    if (ours) {   /* (could not stay walkable: the arena's own limits back) */
        r[LIMIT_RIGHT] = (uint8_t)(cam0 >> 8);
        r[LIMIT_RIGHT + 1] = (uint8_t)cam0;
    }
    walk_lo = -1;
    return false;
}

/* the camera can move sideways in this room */
static bool room_scrolls(const uint8_t *r)
{
    return r[LIMIT_RIGHT] * 256 + r[LIMIT_RIGHT + 1] > r[LIMIT_LEFT] * 256;
}

/* The two routines that draw a room when it starts (6F:A94C, 6F:A9B1; checked by their
 * first bytes). The game has just loaded the room's header, camera limits included. */

/* Called at every dispatch point (Pce.on_entry): when a room starts, put the background
 * changes in for rooms the camera scrolls sideways through, and take them out for every
 * other room, whose background map may be shared out between layers in ways the wider
 * drawing would overwrite (the Stage 0 carriage chase is one). */
/* kinds of object moved round from one side of the screen to the other (wide_frame): seen
 * jumping, or three or more of a kind standing exactly 128 px apart (the bridge pillars, so
 * their first time round is covered too). Forgotten as a room starts. */
static bool recycled[256];
static bool jumped[256];          /* a kind seen moved round (recycled for the room) */
static uint32_t row_sig[256];     /* find_rows: each kind's slots, ... */
static int row_travel[256];       /* ... and the camera's net move since they changed */
static int rows_cam;              /* the camera last frame (find_rows) */
static bool scenery_rows[64];   /* map rows seen scrolled apart from the playfield (this room) */
/* ...counted: frames (the camera moving) each row was seen apart, and seen with the playfield */
static uint16_t rows_apart_n[64], rows_pf_n[64];

/* Objects made further out than the background is drawn (see OBJECT_MARGIN) can write their
 * tiles into the map before the game draws that column (the Stage 3 cave's floor blocks,
 * kind $A4); the column then wiped them. Map cells changed a few at a time in a column are
 * taken as objects' tiles and kept, with where in the level they stand; a column drawn over
 * them there (many cells at once) has them put back. Forgotten as a room starts. */
#define KEEP_W 128
static uint16_t keep_prev[64 * KEEP_W];   /* the map last frame */
static uint16_t keep_val[64 * KEEP_W];
static int16_t keep_x[64 * KEEP_W], keep_y[64 * KEEP_W];   /* level column, row; -1 none */
static int16_t keep_cx[64 * KEEP_W], keep_cy[64 * KEEP_W]; /* the camera then (kept only near it) */
static int keep_mw, keep_mh;
static bool keep_on;

static void keep_reset(void) { keep_on = false; }

/* the level position (8 px units) a map cell stands for: the 512 px from 192 before the
 * camera while it goes left (objects made and columns drawn on that side), from 112 before
 * while it goes right (`before`, as rows are drawn, 6F:A8EC; with 64, the girder painted
 * further left on Stage 2's ship, far_patches, was taken for the place 512 px right, and put
 * over the chairs there as that column was drawn); rows from 128 px above */
static int keep_dir;   /* -1 left, 1 right */
static int keep_pos(int cell, int cam, int map_px, int before)
{
    int lo = cam - before;
    int x = ((cell * 8 - lo) % map_px + map_px) % map_px + lo;
    return x >> 3;
}

static void keep_tiles(Pce *p, bool on)
{
    static const int map_w[4] = {32, 64, 128, 128};
    Vdc *d = &p->vdc;
    int mw = map_w[(d->reg[9] >> 4) & 3], mh = (d->reg[9] & 0x40) ? 64 : 32;
    const uint8_t *r = p->wram;
    int cx = (r[CAM_PAGE] & 0x0F) << 8 | r[CAM_XL];
    int cy = (r[CAM_Y_PAGE] & 0x0F) << 8 | r[CAM_Y];
    static int last_cx;
    if (keep_on && cx != last_cx) keep_dir = wrap(cx - last_cx) < 0 ? -1 : 1;
    last_cx = cx;
    if (!on || mw != keep_mw || mh != keep_mh) keep_on = false;
    if (!keep_on) {
        for (int i = 0; i < mw * mh; i++) {
            keep_prev[i] = d->vram[i];
            keep_x[i] = -1;
        }
        keep_mw = mw;
        keep_mh = mh;
        keep_on = on;
        return;
    }
    /* cells written in each column and row: a column or a row drawn writes many at once,
     * changed or not (counted by what changed, a row drawn again over the Stage 2 ship's
     * girder end, a few cells, looked like an object's tiles: the girder's tiles the object
     * had put there were lost and the blank ones kept, a black gap at the picture's left) */
    const uint8_t *wr = vdc_bat_written();
    int col_n[KEEP_W] = {0}, row_n[64] = {0};
    for (int row = 0; row < mh; row++)
        for (int c = 0; c < mw; c++) {
            int i = row * mw + c;
            if ((i < VDC_BAT_WATCH && wr[i]) || d->vram[i] != keep_prev[i]) {
                col_n[c]++;
                row_n[row]++;
            }
        }
    /* most of the map at once (a room drawn again, a save state loaded): start over */
    int total = 0;
    for (int c = 0; c < mw; c++) total += col_n[c];
    if (total > mw * mh / 4) {
        keep_on = false;
        keep_tiles(p, on);
        return;
    }
    for (int c = 0; c < mw; c++) {
        if (!col_n[c]) continue;
        int lx = keep_pos(c, cx, mw * 8, keep_dir < 0 ? 192 : 112);
        /* (the game draws columns and rows in as they come, off its screen; what it draws
         * again on it, as a drawbridge laid down, is its own) */
        bool off_x = lx * 8 + 8 <= cx - 48 || lx * 8 >= cx + 256 + 48;
        for (int row = 0; row < mh; row++) {
            int i = row * mw + c;
            int ly = keep_pos(row, cy, mh * 8, 128);
            bool off = off_x || ly * 8 + 8 <= cy - 48 || ly * 8 >= cy + 224 + 48;
            if (col_n[c] >= 12 || row_n[row] >= 12) {
                /* a column (row) drawn: objects' tiles here put back */
                /* (only while the camera is still near where it was: a map 512 px tall or
                 * wide holds the same cell for places that far apart) */
                bool near = wrap(cx - keep_cx[i]) >= -192 && wrap(cx - keep_cx[i]) <= 192 &&
                            wrap(cy - keep_cy[i]) >= -192 && wrap(cy - keep_cy[i]) <= 192;
                if (off && near && keep_x[i] == lx && keep_y[i] == ly && d->vram[i] != keep_val[i])
                    d->vram[i] = keep_val[i];
            } else if (d->vram[i] != keep_prev[i]) {
                keep_val[i] = d->vram[i];
                keep_x[i] = (int16_t)lx;
                keep_y[i] = (int16_t)ly;
                keep_cx[i] = (int16_t)cx;
                keep_cy[i] = (int16_t)cy;
            }
            keep_prev[i] = d->vram[i];
        }
    }
}
#define CAM_HISTORY 4
static int cams[CAM_HISTORY] = {-1, -1, -1, -1};   /* the camera over the last frames (scenery_fill) */

static int edge_shift(int left, int right, int ext);

/* Level ends: the picture's shift for camera X cam (walls left / right as room_walls has
 * them). Past a door the game scrolls its camera on beyond the room's limits into the next
 * part: the shift it had at the limit (all the extra width behind, at the room's end) eases
 * back to 0 as the camera goes on (a pixel for a pixel), so it is 0 by the time the next
 * part's limits come in (with none, the picture jumped by the whole extra width then).
 * keep: the shift is this frame's (kept as the one at the limit). */
/* Leaving through a door (or off a room's edge, $31 = 03): as it starts, the game loads the
 * next part's graphics into VRAM over tiles the room left behind uses, and then draws the
 * next part into the map. The picture beyond the original 256 pixels on the side away from
 * the door shows the room left behind, which 4:3 never shows by then: with those tiles
 * replaced it broke up (the chapel's left door: the upper right went dark). So VRAM is
 * copied every frame while playing, the copy kept as the leaving starts, and that side is
 * drawn from it (vdc_set_side_vram): the room as it was, still. The side toward the door
 * shows the part coming in. The side is the one Richter stands on as the leaving starts. */
static int leave_side;          /* -1 left, 1 right, 0 not leaving */
static uint16_t leave_vram[0x8000];

/* how far the camera is past a room's limits: > 0 past the right one, < 0 past the left, 0
 * within them (measured directly within them: a room 2048 px wide put its right end, by the
 * wrapped distance, 2048 px left of its left one, and the picture jumped there) */
static int past_limits(int cam, int lo, int hi)
{
    if (cam >= lo && cam <= hi) return 0;
    int right = (cam - hi) & 0xFFF, left = (lo - cam) & 0xFFF;
    return right <= left ? right : -left;
}

/* Leaving a room kept 4:3 with sides beside it (walls: room_sides) through a door: the sides
 * worked out every frame from the picture's edge columns turned to junk as the camera went
 * on into the next part (the stair room's door to the courtyard). So the sides as they were
 * when the leaving starts are kept and go off with the camera, as the room's own picture
 * does; between them and the picture, on the side away from the door, the room as it was
 * (leave_vram, which that side is drawn from); toward the door, the map's own (the next part
 * as the game draws it), drawn unflipped (VDC_SIDE_OWN). */
static uint16_t side_l[64 * VDC_SIDE_TILES], side_r[64 * VDC_SIDE_TILES];   /* drawn */
static uint16_t keep_l[64 * VDC_SIDE_TILES], keep_r[64 * VDC_SIDE_TILES];
static int keep_cam, keep_first, keep_last;
static int keep_mirrored;   /* (the sides mirrored then: VDC_SIDE_LEFT | VDC_SIDE_RIGHT) */   /* the camera and the picture's map columns then */
/* How far the game has drawn the next part while leaving (in level columns of 8 px): the room
 * start drew 44 columns from the camera; as the camera goes, each column written is taken as
 * the one of its place in the 64-column map nearest the camera (written 64-96 px ahead). The
 * door side shows nothing past it: the map's columns there are left over from before (the
 * courtyard's first screen is all it draws through the door; past it, a strip of the stair
 * room's map). */
static int leave_lo = -1, leave_hi = -1;
static void leave_drawn(Pce *p, bool leaving, int cam)
{
    static const int map_w[4] = {32, 64, 128, 128};
    int mw = map_w[(p->vdc.reg[9] >> 4) & 3], cc = cam >> 3;
    if (!leaving) {
        leave_lo = leave_hi = -1;
        return;
    }
    if (leave_hi < 0) leave_lo = cc - 8, leave_hi = cc + 44;
    int delta = cc - (vdc_row_scroll()[0] >> 3);   /* (level column = map column + delta, mod mw) */
    const uint8_t *wr = vdc_bat_written();
    for (int i = 0; i < VDC_BAT_WATCH && i < mw * 64; i++) {
        if (!wr[i]) continue;
        int c = i % mw, lc = cc - 16 + (((c + delta) - (cc - 16)) % mw + mw) % mw;
        if (lc > leave_hi) leave_hi = lc;
        if (lc < leave_lo) leave_lo = lc;
    }
}

static void sides_kept(const Vdc *d, bool leaving, int cam, int away)
{
    int mh = (d->reg[9] & 0x40) ? 64 : 32;
    int bx = vdc_row_scroll()[0], first = bx >> 3, last = (bx + 255) >> 3;
    if (!leaving) {
        memcpy(keep_l, side_l, sizeof keep_l);
        memcpy(keep_r, side_r, sizeof keep_r);
        keep_cam = cam, keep_first = first, keep_last = last;
        return;
    }
    enum { T = VDC_SIDE_TILES };
    /* (until the camera moves the door side is black: the next part's colours come in a few
     * frames after its tiles, and 4:3 shows none of it until then) */
    bool moved = cam != keep_cam;
    int cc = cam >> 3;   /* (the picture's first column in the level, near enough) */
    for (int r = 0; r < 64; r++) {
        uint16_t *l = &side_l[r * T], *rt = &side_r[r * T];
        const uint16_t *kl = &keep_l[r * T], *kr = &keep_r[r * T];
        for (int k = 0; k < T; k++) {
            if (r >= mh) {
                l[k] = rt[k] = VDC_SIDE_NONE;
                continue;
            }
            /* (the side away from the door is drawn by the renderer from the kept sides and
             * the room as it was, line by line: vdc_set_side_kept; the door side is the map's
             * own, as far as the next part is drawn) */
            if (away < 0)
                l[k] = kl[k];
            else
                l[k] = moved && cc - 1 - k >= leave_lo ? VDC_SIDE_MAP : VDC_SIDE_NONE;
            if (away > 0)
                rt[k] = kr[k];
            else
                rt[k] = moved && cc + 32 + k <= leave_hi ? VDC_SIDE_MAP : VDC_SIDE_NONE;
        }
    }
}

/* After leaving through a door the next part's picture can stand 70 px over from where the
 * leaving left it (the courtyard starts at its left end: Level ends puts its wall at the
 * picture's edge), a jump 4:3 does not make: the shift goes over SHIFT_EASE px a frame then,
 * for as long as it takes. (Both calls in a frame, the HUD's as it is built and the picture's,
 * ease from the last picture's shift, w->shift: the same answer.) */
#define SHIFT_EASE 3
static int shift_ease;   /* frames of easing left (set while leaving) */
static int ease_side;    /* the side away from the door, still drawn from the room left behind
                          * (leave_vram) as the picture eases (-1 left, 1 right, 0 none) */
static int hud_last;     /* the HUD's last offset (place_hud), eased with the picture */
static int hud_eased(int to)
{
    if (shift_ease <= 0) return to;
    return to > hud_last + SHIFT_EASE ? hud_last + SHIFT_EASE : to < hud_last - SHIFT_EASE ? hud_last - SHIFT_EASE : to;
}
static int picture_shift_to(Wide *w, const uint8_t *r, int cam, int left, int right, int ext, bool keep);
static int picture_shift(Wide *w, const uint8_t *r, int cam, int left, int right, int ext, bool keep)
{
    int to = picture_shift_to(w, r, cam, left, right, ext, keep);
    if (shift_ease <= 0) return to;
    return to > w->shift + SHIFT_EASE ? w->shift + SHIFT_EASE : to < w->shift - SHIFT_EASE ? w->shift - SHIFT_EASE : to;
}

static int picture_shift_to(Wide *w, const uint8_t *r, int cam, int left, int right, int ext, bool keep)
{
    if (!ext || !w->edges) return 0;
    if (!room_scrolls(r)) return edge_shift(left, right, ext);   /* (a boss's lock: walls kept) */
    int lo = limit_lo(r), hi = limit_hi(r), beyond = 0;
    int past = past_limits(cam, lo, hi);
    beyond = past > 0 ? past : -past;
    if (beyond > 0) {
        int b = w->inside_shift;
        return b < 0 ? (b + beyond < 0 ? b + beyond : 0) : (b - beyond > 0 ? b - beyond : 0);
    }
    int shift = w->calm ? 0 : edge_shift(left, right, ext);
    if (keep) w->inside_shift = shift;
    return shift;
}

/* The HUD's sprites are built during a frame (72:8000) and shown with the next one, whose
 * picture is shifted (Level ends) for the camera as it stands once this frame has moved it:
 * so the HUD is put again as the building starts, the camera moved by then (put before the
 * frame it would be a step behind, jittering by a pixel as the camera steps 1 and 2 px). */
static void hud_at_build(Wide *w, Pce *p)
{
    const uint8_t *r = p->wram;
    int ext = p->vdc.wide;
    if (!ext || game_state(p) != RONDO_PLAYING || !w->edges || !room_scrolls(r)) return;
    int cam = (r[CAM_PAGE] & 0x0F) << 8 | r[CAM_XL];
    int left, right;
    room_walls(r, cam, &left, &right);
    place_hud_left(p, hud_eased(ext - picture_shift(w, r, cam, left, right, ext, false)));
    w->build_cam = cam;   /* (the picture follows it too: wide_frame) */
    w->build_seen = true;
}

/* Edge spawns (see wide_frame): where an object the game makes at x, 16 / 24 / 32 px past an
 * edge of its screen, is put instead (x itself if anywhere else). */
static int edge_spawn_to(int x, int ext, int shift)
{
    int to = x;
    if (x == -16 || x == -24 || x == -32) {
        int d = -x + ext - shift;
        to = -(d < EDGE_SPAWN_MAX ? d : EDGE_SPAWN_MAX);
        if (to > x) to = x;
    } else if (x == 256 + 16 || x == 256 + 24 || x == 256 + 32) {
        int d = x - 256 + ext + shift;
        to = 256 + (d < EDGE_SPAWN_MAX ? d : EDGE_SPAWN_MAX);
        if (to < x) to = x;
    }
    return to;
}
static bool edge_batch[OBJ_SLOTS];   /* made with two or more others in one frame: left alone */

/* ---- the door behind Richter -----------------------------------------------------------
 * Past a door (kind $99) the camera scrolls a screen on with the door still drawn, then the
 * next room starts and the game clears the objects: the door is gone. 4:3 shows a few
 * pixels of it at the edge by then; the wider picture shows all of it, gone at once. So its
 * last look is kept as a picture: the door's object copied into a free slot as kind 0 with
 * its sprite frame, which the game leaves alone and draws (as it does the HUD), moved with
 * the camera here and let go once off the picture (or the slot is taken, or the room
 * changes again). Not a door: kind 0 runs no code. */
#define DOOR_KIND 0x99
#define DOOR_TYPE 0x05   /* a door's (and so its copy's) type */
#define OBJ_ARRAYS 55                 /* the object arrays, $2750-$3A37, a byte per slot each */
static uint8_t door_copy[OBJ_ARRAYS]; /* the door's slot as last seen while leaving */
static int door_cam = -1;             /* the camera then (-1: none) */
static int door_lo, door_hi;          /* the room's limits then */
static uint8_t door_room;             /* ... and the room */
static int ghost = -1;                /* the slot holding the copy */
static uint8_t ghost_frame;
static int ghost_world;               /* its X in the level: the camera + its screen X */
static int ghost_world_y;             /* ... and its Y (up a stair the camera moves up) */
#define OBJ_YL 0x0CD0
#define OBJ_YH 0x0C78
static int cam_y_of(const uint8_t *r) { return (r[CAM_Y_PAGE] & 0x0F) << 8 | r[CAM_Y]; }

/* The door's look: the sprite patterns its pieces use (the sprite table entries round the
 * door's place as the game last drew it), summed. A next room with graphics of its own loads
 * its patterns over them: the copy would be junk (the chapel's door past the stairs room's
 * start), so it is let go once they no longer sum the same. */
static uint16_t door_pats[32];
static int n_door_pats;
static uint32_t door_look;

static void door_patterns(const Pce *p, int x, int y)
{
    n_door_pats = 0;
    for (int k = 0; k < 64 && n_door_pats < 32; k++) {
        const uint16_t *e = &p->vdc.satb[k * 4];
        int sy = (e[0] & 0x3FF) - 64, sx = (e[1] & 0x3FF) - 32;
        if (sx < x - 48 || sx > x + 48 || sy < y - 96 || sy > y + 48) continue;
        int w = (e[3] & 0x100) ? 2 : 1, h = ((e[3] >> 12) & 3) == 0 ? 1 : ((e[3] >> 12) & 3) == 1 ? 2 : 4;
        int pat = (e[2] >> 1) & 0x3FF;
        if (w == 2) pat &= ~1;
        if (h == 2) pat &= ~2;
        if (h == 4) pat &= ~6;
        for (int dy = 0; dy < h && n_door_pats < 32; dy++)
            for (int dx = 0; dx < w && n_door_pats < 32; dx++) door_pats[n_door_pats++] = (uint16_t)(pat + dy * 2 + dx);
    }
}

static uint32_t door_patterns_sum(const Pce *p)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n_door_pats; i++)
        for (int k = 0; k < 64; k++) h = (h ^ p->vdc.vram[(door_pats[i] * 64 + k) & 0x7FFF]) * 16777619u;
    return h;
}

/* the copy put where it stands for the camera now (wide_frame, and as the sprites are built,
 * when the game has moved its camera for the frame) */
static void ghost_place(Pce *p)
{
    uint8_t *r = p->wram;
    if (ghost < 0 || r[OBJ_KIND + ghost] || r[OBJ_FRAME + ghost] != ghost_frame) return;
    int cam = (r[CAM_PAGE] & 0x0F) << 8 | r[CAM_XL];
    int x = wrap(ghost_world - cam), y = wrap(ghost_world_y - cam_y_of(r));
    r[OBJ_XL + ghost] = (uint8_t)x;
    r[OBJ_XH + ghost] = (uint8_t)(x >> 8);
    r[OBJ_YL + ghost] = (uint8_t)y;
    r[OBJ_YH + ghost] = (uint8_t)(y >> 8);
}

static void door_ghost(Pce *p, int ext)
{
    uint8_t *r = p->wram;
    int cam = (r[CAM_PAGE] & 0x0F) << 8 | r[CAM_XL];
    bool playing = game_state(p) == RONDO_PLAYING;
    /* a copy a save state holds (made before it was saved: our record of it, its place and
     * its look, is not in the state): removed, or it would stay put on the screen, maybe
     * drawn with another room's patterns. Only the slots copies go into; kind 0 drawn there
     * with a door's type is only ever ours. */
    if (ghost < 0 && playing)
        for (int i = 0x53; i >= 0x10; i--)
            if (!r[OBJ_KIND + i] && r[OBJ_FRAME + i] && r[OBJ_TYPE + i] == DOOR_TYPE) r[OBJ_FRAME + i] = 0;
    /* the copy kept: moved with the camera, let go off the picture or when taken */
    if (ghost >= 0) {
        bool ours = !r[OBJ_KIND + ghost] && r[OBJ_FRAME + ghost] == ghost_frame;
        ghost_place(p);
        int x = wrap(ghost_world - cam);
        bool off = x < -ext - 64 || x > 256 + ext + 64;
        /* (kept while leaving too: made as the old room's objects go, before $31 is 01) */
        bool gone = off || !playing || (r[0x31] != 0x01 && r[0x31] != 0x03) || r[ROOM] != door_room || !ext ||
                    (n_door_pats && door_patterns_sum(p) != door_look);
        /* (a room with a door of its own there, as the boss room past Stage 2's tower door
         * has: the copy steps aside, not drawn twice) */
        for (int i = 0x10; i < 0x54 && !gone; i++) {
            int xi = (int16_t)(r[OBJ_XL + i] | r[OBJ_XH + i] << 8);
            if (i != ghost && r[OBJ_KIND + i] == DOOR_KIND && r[OBJ_FRAME + i] && xi - x < 24 && x - xi < 24) gone = true;
        }
        if (ours && gone) r[OBJ_FRAME + ghost] = 0;
        if (!ours || gone) ghost = -1;
    }
    if (!playing || !ext) {
        door_cam = -1;
        return;
    }
    /* leaving through a door ($31 = 03): its look and the camera, every frame */
    bool door_there = false;
    for (int i = 0x10; i < 0x54; i++) door_there |= r[OBJ_KIND + i] == DOOR_KIND && r[OBJ_FRAME + i];
    if (r[0x31] == 0x03 && (door_cam < 0 || r[ROOM] == door_room)) {
        /* (the room number changes a frame or two before the leaving ends: the door as seen
         * in the room it belongs to is kept) */
        for (int i = 0x10; i < 0x54; i++)
            if (r[OBJ_KIND + i] == DOOR_KIND && r[OBJ_FRAME + i]) {
                for (int k = 0; k < OBJ_ARRAYS; k++) door_copy[k] = r[OBJ_KIND + k * OBJ_SLOTS + i];
                door_cam = cam;
                door_patterns(p, (int16_t)(r[OBJ_XL + i] | r[OBJ_XH + i] << 8), (int16_t)(r[OBJ_YL + i] | r[OBJ_YH + i] << 8));
                door_look = door_patterns_sum(p);
                door_lo = limit_lo(r);
                door_hi = limit_hi(r);
                door_room = r[ROOM];
            }
        return;
    }
    /* the next room started (its number in, the old room's objects gone, $31 still 03 for a
     * frame: the copy made then, or the door was missing for that frame): the copy put where
     * the door stands now. The camera had gone on to a screen past the limit it left by
     * (hi + 256 or lo - 256): that screen is the new room's. */
    if (door_cam >= 0 && ghost < 0 && r[ROOM] != door_room && (r[0x31] == 0x01 || (r[0x31] == 0x03 && !door_there))) {
        int target = past_limits(door_cam, door_lo, door_hi) > 0 ? door_hi + 256 : door_lo - 256;
        int x = (int16_t)(door_copy[(OBJ_XL - OBJ_KIND) / OBJ_SLOTS] |
                          door_copy[(OBJ_XH - OBJ_KIND) / OBJ_SLOTS] << 8) - wrap(target - door_cam);
        int slot = -1;
        for (int i = 0x53; i >= 0x10 && slot < 0; i--)
            if (!r[OBJ_KIND + i] && !r[OBJ_FRAME + i]) slot = i;
        if (slot >= 0 && x > -ext - 64 && x < 256 + ext + 64) {
            for (int k = 0; k < OBJ_ARRAYS; k++) r[OBJ_KIND + k * OBJ_SLOTS + slot] = door_copy[k];
            r[OBJ_KIND + slot] = 0;
            r[OBJ_XL + slot] = (uint8_t)x;
            r[OBJ_XH + slot] = (uint8_t)(x >> 8);
            ghost = slot;
            ghost_frame = r[OBJ_FRAME + slot];
            ghost_world = cam + x;
            ghost_world_y = cam_y_of(r) + (int16_t)(r[OBJ_YL + slot] | r[OBJ_YH + slot] << 8);
            door_room = r[ROOM];
        }
        door_cam = -1;
    }
    if (r[0x31] == 0x01) door_cam = -1;
}

/* As the game starts building its sprites (72:8000): objects made this frame at an edge
 * spawn's place, with a sprite already, moved out before they are drawn (wide_frame would
 * move them only before the next frame, after one frame shown inside the picture: the castle
 * hall's kind-31 creatures popped in at 272). Same rules as wide_frame's. */
static void edge_spawns_at_build(Wide *w, Pce *p)
{
    uint8_t *r = p->wram;
    if (!w->edge_live || game_state(p) != RONDO_PLAYING) return;
    int born = 0;
    for (int i = 0; i < OBJ_SLOTS; i++) born += r[OBJ_KIND + i] && !w->obj_kind[i];
    for (int i = 0; i < OBJ_SLOTS; i++) {
        uint8_t kind = r[OBJ_KIND + i];
        /* (first drawn, or just made: made at the edge before it has a sprite, it moves a
         * pixel before it is first drawn, as Stage 4's Medusa heads, kind $27, at -16) */
        bool first = kind && ((r[OBJ_FRAME + i] && !(kind == w->obj_kind[i] && w->obj_shown[i])) || kind != w->obj_kind[i]);
        bool batch = w->obj_kind[i] ? edge_batch[i] : born >= 3;
        if (!first || batch || (r[OBJ_FLAGS + i] & 1) || i == HUD_SLOT || i == SUBWEAPON_SLOT) continue;
        int x = (int16_t)(r[OBJ_XL + i] | r[OBJ_XH + i] << 8), to = edge_spawn_to(x, w->active, w->shift);
        if (to != x) {
            r[OBJ_XL + i] = (uint8_t)to;
            r[OBJ_XH + i] = (uint8_t)(to >> 8);
        }
    }
}

/* ---- past a door into a new room ---------------------------------------------------------
 * Some doors start the next room with no room start (no new map: the camera has scrolled a
 * screen on, and the room number and limits change). Past the new room's walls the picture
 * then shows what the map held there: the part just left (calm: no black). With the same
 * graphics that is the room behind the door, as it was. A new room with graphics of its own
 * (the castle hall into the brick cellar) draws those map cells with its tiles and palettes:
 * junk. So the strips beside the screen the camera is scrolling to (the new room's, ext a
 * side; the one past the new room's wall) are summed as they are while leaving (map cells, their tiles and palettes), and
 * when the new room's limits come, a strip that no longer sums the same is black after all
 * (calm_black). */
static uint32_t leave_sum;
static int leave_to = -1;   /* the screen the camera scrolls to while leaving (-1 none) */
static uint8_t leave_room;

static uint32_t strip_sum(Pce *p, int from, int n)
{
    static const int map_w[4] = {32, 64, 128, 128};
    const Vdc *d = &p->vdc;
    int mw = map_w[(d->reg[9] >> 4) & 3], mh = (d->reg[9] & 0x40) ? 64 : 32;
    uint32_t h = 2166136261u;
    for (int c = from >> 3; c < (from + n + 7) >> 3; c++)
        for (int row = 0; row < mh; row++) {
            uint16_t e = d->vram[row * mw + (c & (mw - 1))];
            h = (h ^ e) * 16777619u;
            const uint16_t *t = &d->vram[((e & 0x7FF) * 16) & 0x7FFF];
            for (int i = 0; i < 16; i++) h = (h ^ t[i]) * 16777619u;
            for (int i = 0; i < 16; i++) h = (h ^ p->vce.pal[((e >> 12) * 16 + i) & 511]) * 16777619u;
        }
    return h;
}

static bool leave_right;    /* leaving by the right (the strip left of the screen scrolled to) */

static uint32_t strips_sum(Pce *p, int to, int ext)
{
    return leave_right ? strip_sum(p, to - ext, ext) : strip_sum(p, to + 256, ext);
}

static void beyond_door(Wide *w, Pce *p, uint8_t s, int cam, int ext)
{
    const uint8_t *r = p->wram;
    if (s != RONDO_PLAYING || !ext) {
        leave_to = -1;
        return;
    }
    if (r[0x31] == 0x03 && (leave_to < 0 || r[ROOM] == leave_room)) {
        int lo = limit_lo(r), hi = limit_hi(r), to = -1;
        int past = past_limits(cam, lo, hi);
        if (past > 0) to = (hi + 256) & 0xFFF;
        else if (past < 0) to = (lo - 256) & 0xFFF;
        if (to >= 0) {
            leave_right = past > 0;
            leave_to = to;
            leave_room = r[ROOM];
            leave_sum = strips_sum(p, to, ext);
        }
        return;
    }
    /* in the new room, while calm: black once the strips are not what they were */
    if (w->calm && !w->calm_black && leave_to >= 0 && r[ROOM] != leave_room &&
        strips_sum(p, leave_to, ext) != leave_sum)
        w->calm_black = true;
    if (!w->calm && r[0x31] == 0x01) leave_to = -1;
}

static void on_entry(Pce *p, void *ctx)
{
    uint16_t pc = p->cpu.pc;
    if (shift_6f < 0) layout_6f(p);   /* (the patches set up before anything uses them) */
    if (pc == 0x8000 && p->cpu.mpr[4] == 0x72 && p->page[0x72]) {
        edge_spawns_at_build(ctx, p);
        door_ghost(p, ((Wide *)ctx)->want);   /* (the door's copy made in the frame it goes) */
        ghost_place(p);
        hud_at_build(ctx, p);
        return;
    }
    if (p->cpu.mpr[5] != 0x6F || !p->page[0x6F] || pc < 0xA000 || pc >= 0xC000) return;
    /* (the layout as wide_frame last worked it out: worked out here, it searched the bank at
     * every dispatch point while none fitted) */
    if (!layout_ok) return;
    int sh = shift_6f < 0 ? 0 : shift_6f;
    if (pc != 0xA94C + sh && pc != 0xA9B1 + sh) return;
    const uint8_t *code = p->page[0x6F] + (pc & 0x1FFF);
    if (memcmp(code, pc == 0xA94C + sh ? ROOM_START_A : ROOM_START_B, 6)) return;
    Wide *w = ctx;
    const uint8_t *r = p->wram;
    bool scrolls = room_scrolls(r);
    /* The room is first drawn as the whole 512-pixel map from 144 px left of the camera. A
     * room starting within the extension of its left end draws from the camera instead,
     * and one at its right end from 176 px left, so the side the picture is shifted to
     * (wide_frame) is all there. */
    uint8_t start = 0x20;
    if (w->edges && w->want > 0 && scrolls) {
        int cam = (r[CAM_PAGE] & 0x0F) << 8 | r[CAM_XL];
        int wl, wr;
        room_walls(r, cam, &wl, &wr);
        int from_left = -wl, to_right = wr - 256;
        if (from_left < w->want) start = 0xB0;
        else if (to_right < w->want) start = 0x00;
    }
    var_value[VAR_ROOM_START] = start;
    memset(recycled, 0, sizeof recycled);
    memset(jumped, 0, sizeof jumped);
    memset(row_sig, 0, sizeof row_sig);
    memset(row_travel, 0, sizeof row_travel);
    memset(scenery_rows, 0, sizeof scenery_rows);
    memset(rows_apart_n, 0, sizeof rows_apart_n);
    memset(rows_pf_n, 0, sizeof rows_pf_n);
    cams[0] = -1;
    keep_reset();
    w->open_seen = false;
    w->lim_seen = w->calm = false;
    w->room_age = 0;
    /* (not in the title state, $03: the save menu's Tactics demos start their rooms there, and
     * wide_frame takes the changes out again outside gameplay, mid-column: the game came back
     * from its column drawing to our code gone, and ran off; the demos stay 4:3) */
    bool play = game_state(p) != RONDO_TITLE;
    set_patches(p, WORLD_PATCHES, w->want > 0 && scrolls && play);
    set_patches(p, OBJECT_PATCHES, w->want > 0 && scrolls && play);
    update_vars(p);
}

/* ---- menus ------------------------------------------------------------------------ */

/* The menus (title, file select) are drawn on a map that holds nothing of theirs past the
 * 256 pixels, but their backgrounds are rows of a few tiles that join seamlessly (the file
 * select's waves: 144-147 and 148-149 in any order). Each row is carried on past both
 * edges with the tile that most often follows (precedes) its edge tile across the picture,
 * seen at least twice. Art that does not repeat (the title's clouds, story scenes) has no
 * such tile and stays 4:3. */
static uint16_t try_l[64 * VDC_SIDE_TILES], try_r[64 * VDC_SIDE_TILES];     /* worked out */

#define PAIRS 4096
static uint32_t pair_key[PAIRS];
static uint16_t pair_n[PAIRS];
static uint16_t pair_used[PAIRS], n_used;   /* the slots in use */

static void pair_add(uint16_t a, uint16_t b)
{
    uint32_t key = (uint32_t)a << 16 | b, i = (key * 2654435761u) >> 20;
    while (pair_n[i] && pair_key[i] != key) i = (i + 1) & (PAIRS - 1);
    if (!pair_n[i]) {
        if (n_used >= PAIRS / 2) return;   /* (a map has far fewer) */
        pair_used[n_used++] = (uint16_t)i;
    }
    pair_key[i] = key;
    if (pair_n[i] < 0xFFFF) pair_n[i]++;
}

/* the tile seen most often after a (before it: before = true), at least twice (at least
 * once: once = true); or none */
static uint16_t pair_next_n(uint16_t a, bool before, bool once)
{
    uint16_t best = VDC_SIDE_NONE;
    int n = once ? 0 : 1;
    for (int u = 0; u < n_used; u++) {
        int i = pair_used[u];
        uint16_t x = (uint16_t)(pair_key[i] >> 16), y = (uint16_t)pair_key[i];
        if ((before ? y : x) == a && pair_n[i] > n) {
            n = pair_n[i];
            best = before ? x : y;
        }
    }
    return best;
}
static uint16_t pair_next(uint16_t a, bool before) { return pair_next_n(a, before, false); }

/* Works out the side tiles for the last frame's picture (ext a side); true when nearly all
 * of them could be, and then they are the ones drawn. */
static bool menu_sides(Pce *p, int ext)
{
    static const int map_w[4] = {32, 64, 128, 128};
    const Vdc *d = &p->vdc;
    int mw = map_w[(d->reg[9] >> 4) & 3], mh = (d->reg[9] & 0x40) ? 64 : 32;
    int rows = d->fb_h < PCE_FB_MAX_H ? d->fb_h : PCE_FB_MAX_H;
    if (rows <= 0 || d->fb_w - 2 * d->fb_wide != 256) return false;
    /* one sideways scroll for the whole picture */
    const uint16_t *rs = vdc_row_scroll(), *my = vdc_row_map_y();
    for (int y = 1; y < rows; y++)
        if (rs[y] != rs[0]) return false;
    int first = rs[0] >> 3, last = (rs[0] + 255) >> 3;
    bool shown[64] = {false};
    for (int y = 0; y < rows; y++) shown[(my[y] & (mh * 8 - 1)) >> 3] = true;
#define TILE(r, c) d->vram[(r) * mw + ((c) & (mw - 1))]
    for (int u = 0; u < n_used; u++) pair_n[pair_used[u]] = 0;
    n_used = 0;
    for (int r = 0; r < mh; r++)
        if (shown[r])
            for (int c = first; c < last; c++) pair_add(TILE(r, c), TILE(r, c + 1));
    int need = (ext + 7) / 8 + 1, filled = 0, total = 0;
    if (need > VDC_SIDE_TILES) need = VDC_SIDE_TILES;
    for (int r = 0; r < 64; r++) {
        uint16_t *l = &try_l[r * VDC_SIDE_TILES], *rt = &try_r[r * VDC_SIDE_TILES];
        uint16_t a = r < mh ? TILE(r, first) : VDC_SIDE_NONE, b = r < mh ? TILE(r, last) : VDC_SIDE_NONE;
        for (int k = 0; k < VDC_SIDE_TILES; k++) {
            a = a == VDC_SIDE_NONE ? a : pair_next(a, true);
            b = b == VDC_SIDE_NONE ? b : pair_next(b, false);
            l[k] = a;
            rt[k] = b;
            if (r < mh && shown[r] && k < need) {
                total += 2;
                filled += (a != VDC_SIDE_NONE) + (b != VDC_SIDE_NONE);
            }
        }
    }
#undef TILE
    if (!total || filled * 100 < total * 97) return false;
    memcpy(side_l, try_l, sizeof side_l);
    memcpy(side_r, try_r, sizeof side_r);
    return true;
}

/* Rooms kept 4:3 (one screen wide: boss rooms, vertical shafts) have nothing of theirs past
 * the 256 pixels: each side shows the room mirrored at that edge, fading to black towards
 * the picture's outer edge (vdc_set_side_fade), so it reads as the room going on into the
 * dark rather than more of it to play in; each tile is drawn flipped (vdc_set_side_flip),
 * so the sides are the room's exact mirror image. Only for a picture with one sideways scroll;
 * sprites stay in the 256 pixels (side tiles). */
/* Rooms whose sky is not mirrored (SKY_ROOMS): only what runs right across the room, the
 * floor and railing and the mountains' skyline, is mirrored beside it; the sky shows plain
 * beside the rest, so yards, sails, spars and ropes crossing it inside the room are not
 * repeated at its sides (see room_sides). Stage 5's ship below Death's mast (area $32, room
 * 0: one screen wide, kept 4:3, while Richter climbs). */
static const struct { uint8_t area, room; uint16_t sky; } SKY_ROOMS[] = {
    {0x32, 0x00, 0x110},   /* (its sky: tile $110; guessed as the commonest tile shown, it was the
                            * clouds' with the mountains in view, and everything was mirrored) */
};
static int sky_room(const uint8_t *r)
{
    for (size_t i = 0; i < sizeof SKY_ROOMS / sizeof SKY_ROOMS[0]; i++)
        if (r[AREA] == SKY_ROOMS[i].area && r[ROOM] == SKY_ROOMS[i].room) return SKY_ROOMS[i].sky;
    return 0;
}

/* Carried sides: beside a room kept 4:3 whose edge is a pattern repeating every 1, 2 or 4
 * columns (a stone pillar, a wall, a floor, water), that pattern carried on outward at full
 * brightness instead of the room mirrored, so the room reads as going on (Stage 1's town: the
 * pillar at its right edge becomes a wall of stone blocks, the black above it stays black, the
 * water below it water). A row repeats when the columns from the edge match those a period in
 * for two columns past it; a row that does not carries on its last two columns as a block (a
 * pillar one block wide). A side is carried when its last two columns hold few different tiles
 * over the rows on screen (CARRY_KINDS: a pillar or a plain wall); else it is mirrored
 * as before. */
static int edge_repeat(const Vdc *d, int mw, int row, int edge, int dir)
{
#define T(c) d->vram[row * mw + ((c) & (mw - 1))]
    static const int ps[3] = {1, 2, 4};
    for (int j = 0; j < 3; j++) {
        int per = ps[j];
        bool ok = true;
        for (int i = 0; i < per + 2 && ok; i++) ok = T(edge + dir * i) == T(edge + dir * (i + per));
        if (ok) return per;
    }
    return 0;
#undef T
}

/* the sides (VDC_SIDE_LEFT / RIGHT) filled with the room's edge carried on */
#define CARRY_KINDS 12   /* the most different tiles the edge's two columns may hold */
static int carried_sides(const Vdc *d, int mw, int mh, int first, int last, int rows)
{
    int per[2][64], sides = 0;
    bool on[64] = {false};
    const uint16_t *my = vdc_row_map_y();
    for (int y = 0; y < rows; y++) on[(my[y] & (mh * 8 - 1)) >> 3] = true;
    /* the edge's last two columns, over the rows on screen (the rest of the map is other
     * rooms'): a pillar or a wall standing there is a few tiles, used again and again */
    for (int s = 0; s < 2; s++) {
        uint16_t seen[CARRY_KINDS];
        int ns = 0;
        bool few = true;
        for (int r = 0; r < mh && few; r++) {
            if (!on[r]) continue;
            for (int k = 0; k < 2 && few; k++) {
                /* (by tile: the pillar is drawn in the water's palette where it stands in it) */
                uint16_t e = d->vram[r * mw + ((s ? last - k : first + k) & (mw - 1))] & 0x7FF;
                int i = 0;
                while (i < ns && seen[i] != e) i++;
                if (i == ns) {
                    if (ns == CARRY_KINDS) few = false;
                    else seen[ns++] = e;
                }
            }
        }
        if (few && ns) sides |= s ? VDC_SIDE_RIGHT : VDC_SIDE_LEFT;
    }
    for (int r = 0; r < mh; r++) {
        per[0][r] = edge_repeat(d, mw, r, first, 1);
        per[1][r] = edge_repeat(d, mw, r, last, -1);
        if (!per[0][r]) per[0][r] = 2;   /* (else the edge's last two columns, as a block) */
        if (!per[1][r]) per[1][r] = 2;
    }
    for (int r = 0; r < mh; r++) {
        uint16_t *l = &side_l[r * VDC_SIDE_TILES], *rt = &side_r[r * VDC_SIDE_TILES];
        int pl = per[0][r], pr = per[1][r];
        for (int k = 0; k < VDC_SIDE_TILES; k++) {
            /* column first-1-k is the edge column a whole number of periods along, likewise
             * last+1+k */
            int cl = first - 1 - k, cr = last + 1 + k;
            if (sides & VDC_SIDE_LEFT)
                l[k] = d->vram[r * mw + ((first + (((cl - first) % pl) + pl) % pl) & (mw - 1))];
            if (sides & VDC_SIDE_RIGHT)
                rt[k] = d->vram[r * mw + ((last - (((last - cr) % pr) + pr) % pr) & (mw - 1))];
        }
    }
    return sides;
}

/* Side recipes: rooms whose sides are laid out by hand. Beside the room on `side`: the room's
 * wall (its commonest 2 x 2 block of map entries, in step with the map) down to row `floor`,
 * the edge carried on below it (carried_sides' rows), and a lantern of the game's
 * hung on the wall: a 32 x 32 sprite (sprite patterns `pat_a` / `pat_b`, flickering between
 * them, with attributes `attr`, as the game draws its wall lanterns) `dx` px out from the room's
 * edge, its top at row `row` (0: none). The sprite's graphics and palette are the game's,
 * as loaded for the room. */
/* (`ledge`: map rows ledge..floor-1 carry on the nearest repeat past the edge's pillar,
 * ledge_entry, as a ledge running along the wall's foot; ledge = floor for none. `copy`: up to
 * two blocks of the room's own map entries copied into the wall, columns col..col+w-1, rows
 * row0..row1, placed from side column k (0: next to the room); w 0 for none. A lantern: row
 * 0 for none. `brick`: map cells of the room itself, columns col..col+w-1, rows row0..row1,
 * drawn as the wall instead (vdc_set_map_patch: the map is left as it is); w 0 for none.
 * `worn`: the wall's map entries worn_a / worn_b drawn instead of its second row's two
 * (T[2], T[3]) in rows worn_r0..worn_r1, about one in three, by map position (the room's own
 * worn bricks, breaking up the repeat); worn_a 0 for none.) */
typedef struct { uint8_t col, w, row0, row1, k; } SideCopy;
typedef struct { uint8_t row, col; uint16_t entry; } SideMark;
/* (rows are counted from the room's top: the map row shown at the top of the picture. The
 * room is known by `mark`: map entries it has at those places, as rooms are numbered by the
 * way in: Stage 1's house is room 4 from a save state and room 1 walking in from the start.) */
static const struct { uint8_t area, side, ledge, floor, dx, row; uint16_t pat_a, pat_b, attr; SideCopy copy[2], brick;
                      uint16_t worn_a, worn_b; uint8_t worn_r0, worn_r1; SideMark mark[3]; uint8_t map, blk_r0, blk_r1; } SIDE_RECIPES[] = {
    /* Stage 1's town, the house with the stairs: brick beside the pillar, with a drainpipe (and
     * the black gap at its top) and one of the room's tall barred windows on it, the ledge
     * along its foot, worn bricks low on it, the pillar's stone into the water below; the
     * room's own short pipe at its edge, above the pillar, bricked over */
    {0x10, VDC_SIDE_RIGHT, 22, 24, 0, 0, 0, 0, 0, {{5, 1, 0, 21, 0}, {8, 4, 4, 14, 2}}, {30, 2, 0, 7, 0},
     0x8234, 0x8235, 12, 21, {{1, 5, 0x621F}, {4, 8, 0x8224}, {8, 30, 0x6124}}, 0, 0, 0},
    /* Stage 1, the stair room by the breakable stone blocks (area $10 room 5): the hidden room
     * behind the blocks, already in the map left of the room (stone, dark brick, a pillar),
     * shown as it is, but for the blocks' own rows (map rows 42-49), where the stone of the
     * blocks at the room's edge carries on across the side: the room behind stays hidden until
     * they break and the camera can go there (then the picture goes wide by itself) */
    {0x10, VDC_SIDE_LEFT, 0, 0, 0, 0, 0, 0, 0, {{0}}, {0}, 0, 0, 0, 0,
     {{38, 34, 0x629B}, {39, 35, 0x62A0}, {42, 34, 0x312A}}, 8, 42, 49},
};
static uint16_t room_patch[64 * VDC_FILL_W];
static bool room_patched;
static VdcExtraSprite side_lantern[1];
static int n_side_lantern;

/* a worn brick at this map row and brick (two columns): about one in three, by place */
static bool worn_at(int row, int brick)
{
    uint32_t n = (uint32_t)(row * 131 + brick * 7919) * 2654435761u;
    n ^= n >> 15;
    n *= 0x27d4eb2du;
    n ^= n >> 13;
    return (n & 0xFFFF) < 0x5700;
}

/* a ledge row's entry for map column `col` beside the room: from the edge (`dir` 1 going right
 * from `edge`, -1 left) inward, the nearest columns that repeat every 1, 2 or 4 (checked for
 * two columns past the repeat: past a pillar at the edge), in step with that repeat */
static uint16_t ledge_entry(const Vdc *d, int mw, int row, int edge, int dir, int col)
{
#define T(c) d->vram[row * mw + ((c) & (mw - 1))]
    static const int ps[3] = {1, 2, 4};
    for (int off = 0; off <= 12; off++) {
        int s = edge + dir * off;
        for (int j = 0; j < 3; j++) {
            int per = ps[j];
            bool ok = true;
            for (int i = 0; i < 2 && ok; i++) ok = T(s + dir * i) == T(s + dir * (i + per));
            if (ok) return T(dir > 0 ? s + ((((col - s) % per) + per) % per) : s - ((((s - col) % per) + per) % per));
        }
    }
    return T(edge);
#undef T
}

/* the room's wall: its commonest 2 x 2 block of map entries (not all one), at least 12 times */
static bool wall_texture(const Vdc *d, int mw, int mh, int first, int last, uint16_t T[4], int *pr, int *pc)
{
    static uint16_t key[2048][4];
    static int cnt[2048], par[2048];
    int nk = 0, best = -1;
    for (int r = 0; r + 1 < mh; r++)
        for (int c = first; c < last; c++) {
            uint16_t b[4] = {d->vram[r * mw + (c & (mw - 1))], d->vram[r * mw + ((c + 1) & (mw - 1))],
                             d->vram[(r + 1) * mw + (c & (mw - 1))], d->vram[(r + 1) * mw + ((c + 1) & (mw - 1))]};
            if (b[0] == b[1] && b[0] == b[2] && b[0] == b[3]) continue;
            int p = (r & 1) << 1 | (c & 1), i = 0;
            while (i < nk && (par[i] != p || memcmp(key[i], b, sizeof b))) i++;
            if (i == nk) {
                if (nk == 2048) continue;
                memcpy(key[nk], b, sizeof b);
                par[nk] = p;
                cnt[nk++] = 0;
            }
            if (++cnt[i] > (best < 0 ? 0 : cnt[best])) best = i;
        }
    if (best < 0 || cnt[best] < 12) return false;
    memcpy(T, key[best], 4 * sizeof T[0]);
    *pr = par[best] >> 1;
    *pc = par[best] & 1;
    return true;
}

/* a side recipe for this room laid over the sides (after carried_sides); the sides it lays */
static int side_recipe(const uint8_t *wr, uint64_t wr_frame, const Vdc *d, int mw, int mh, int first, int last)
{
    int sides = 0;
    /* (the room's top: the map row shown at the top of the picture; recipe rows count from it) */
    const uint16_t *my = vdc_row_map_y(), *rs = vdc_row_scroll();
    int top = (my[0] >> 3) & (mh - 1);
#define ROW(rel) ((top + (rel)) & (mh - 1))
    for (size_t i = 0; i < sizeof SIDE_RECIPES / sizeof SIDE_RECIPES[0]; i++) {
        if (wr[AREA] != SIDE_RECIPES[i].area) continue;
        bool same = true;
        for (int m = 0; m < 3 && same; m++) {
            const SideMark *mk = &SIDE_RECIPES[i].mark[m];
            /* (a `map` recipe's marks are at map rows as they are: the camera can scroll) */
            int mr = SIDE_RECIPES[i].map ? mk->row & (mh - 1) : ROW(mk->row);
            same = d->vram[mr * mw + (mk->col & (mw - 1))] == mk->entry;
        }
        if (!same) continue;
        if (SIDE_RECIPES[i].map) {
            /* the map's own columns past the edge, `map` of them; past those, their outer
             * columns carried on (each row's repeat, as carried_sides) */
            bool right = SIDE_RECIPES[i].side == VDC_SIDE_RIGHT;
            int n = SIDE_RECIPES[i].map, dir = right ? 1 : -1;
            int e = right ? last + n : first - n;   /* (the outermost map column used) */
            for (int r = 0; r < mh; r++) {
                uint16_t *s = right ? &side_r[r * VDC_SIDE_TILES] : &side_l[r * VDC_SIDE_TILES];
                int per = edge_repeat(d, mw, r, e, -dir);
                if (!per) per = 2;
                /* (`blk_r0`..`blk_r1`: the room's edge carried on, from the room's side) */
                bool blk = SIDE_RECIPES[i].blk_r1 && r >= SIDE_RECIPES[i].blk_r0 && r <= SIDE_RECIPES[i].blk_r1;
                int ed = right ? last : first, pb = 0;
                if (blk && !(pb = edge_repeat(d, mw, r, ed, -dir))) pb = 2;
                for (int k = 0; k < VDC_SIDE_TILES; k++) {
                    int c = right ? last + 1 + k : first - 1 - k;
                    if (blk) c = ed - dir * ((((c - ed) * -dir) % pb + pb) % pb);
                    else if (k >= n) c = e - dir * ((((c - e) * -dir) % per + per) % per);
                    s[k] = d->vram[r * mw + (c & (mw - 1))];
                }
            }
            sides |= SIDE_RECIPES[i].side;
            continue;
        }
        uint16_t T[4];
        int pr, pc;
        if (!wall_texture(d, mw, mh, first, last, T, &pr, &pc)) continue;
        bool right = SIDE_RECIPES[i].side == VDC_SIDE_RIGHT;
        /* the wall from the room's top down to `floor`, the ledge rows from `ledge` (below
         * `floor`, the edge as carried_sides carried it) */
        for (int rel = 0; rel < SIDE_RECIPES[i].floor; rel++) {
            int r = ROW(rel);
            uint16_t *s = right ? &side_r[r * VDC_SIDE_TILES] : &side_l[r * VDC_SIDE_TILES];
            bool ledge = rel >= SIDE_RECIPES[i].ledge;
            for (int k = 0; k < VDC_SIDE_TILES; k++) {
                int c = right ? last + 1 + k : first - 1 - k;
                int ti = ((r - pr) & 1) << 1 | ((c - pc) & 1);
                s[k] = ledge ? ledge_entry(d, mw, r, right ? last : first, right ? -1 : 1, c) : T[ti];
                /* (a worn brick here and there: both halves of a brick together, by its place) */
                if (!ledge && ti >= 2 && SIDE_RECIPES[i].worn_a && rel >= SIDE_RECIPES[i].worn_r0 &&
                    rel <= SIDE_RECIPES[i].worn_r1 && worn_at(rel, (c - pc) >> 1))
                    s[k] = ti == 2 ? SIDE_RECIPES[i].worn_a : SIDE_RECIPES[i].worn_b;
            }
        }
        for (int j = 0; j < 2; j++) {
            const SideCopy *cp = &SIDE_RECIPES[i].copy[j];
            for (int rel = cp->row0; cp->w && rel <= cp->row1; rel++) {
                int r = ROW(rel);
                uint16_t *s = right ? &side_r[r * VDC_SIDE_TILES] : &side_l[r * VDC_SIDE_TILES];
                for (int q = 0; q < cp->w; q++) {
                    int k = cp->k + (right ? q : cp->w - 1 - q);
                    if (k < VDC_SIDE_TILES) s[k] = d->vram[r * mw + ((cp->col + q) & (mw - 1))];
                }
            }
        }
        const SideCopy *bk = &SIDE_RECIPES[i].brick;
        if (bk->w) {
            if (!room_patched)
                for (int q = 0; q < 64 * VDC_FILL_W; q++) room_patch[q] = VDC_SIDE_NONE;
            for (int rel = bk->row0; rel <= bk->row1; rel++) {
                int r = ROW(rel);
                for (int c = bk->col; c < bk->col + bk->w; c++)
                    room_patch[r * VDC_FILL_W + (c & (VDC_FILL_W - 1))] = T[((r - pr) & 1) << 1 | ((c - pc) & 1)];
            }
            room_patched = true;
        }
        if (SIDE_RECIPES[i].row && n_side_lantern < 1) {
            /* (where its row is on screen this frame, and the edge's screen X) */
            int dy = (ROW(SIDE_RECIPES[i].row) * 8 - my[0]) & (mh * 8 - 1);
            if (dy >= mh * 8 - 64) dy -= mh * 8;
            int ex = right ? (last + 1) * 8 - (rs[0] & 0x3FF) + SIDE_RECIPES[i].dx
                           : first * 8 - (rs[0] & 0x3FF) - SIDE_RECIPES[i].dx - 32;
            VdcExtraSprite *e = &side_lantern[n_side_lantern++];
            e->x = (int16_t)ex;
            e->y = (int16_t)dy;
            e->pattern = (wr_frame / 4) & 1 ? SIDE_RECIPES[i].pat_b : SIDE_RECIPES[i].pat_a;
            e->attr = SIDE_RECIPES[i].attr;
        }
        sides |= SIDE_RECIPES[i].side;
    }
#undef ROW
    return sides;
}

static int room_walled;   /* the sides room_sides carried on or laid out (not mirrored) */

static bool room_sides(Pce *p, int sky)
{
    static const int map_w[4] = {32, 64, 128, 128};
    const Vdc *d = &p->vdc;
    int mw = map_w[(d->reg[9] >> 4) & 3], mh = (d->reg[9] & 0x40) ? 64 : 32;
    int rows = d->fb_h < PCE_FB_MAX_H ? d->fb_h : PCE_FB_MAX_H;
    if (rows <= 0 || d->fb_w - 2 * d->fb_wide != 256) return false;
    const uint16_t *rs = vdc_row_scroll();
    for (int y = 1; y < rows; y++)
        if (rs[y] != rs[0]) return false;
    int first = rs[0] >> 3, last = (rs[0] + 255) >> 3;
    uint16_t sky_e = (uint16_t)sky;   /* (SKY_ROOMS) the room's plain sky tile */
    /* (SKY_ROOMS) which rows are mirrored: bands of rows with something besides sky in the
     * two columns at the room's edges; a band is mirrored (both sides) only if it reaches
     * both of the room's edges, as the floor and railing and the mountains' skyline do (a row
     * whose edge falls between two peaks is mirrored with the rest, so the skyline is whole).
     * Everything else, a yard or sail crossing one edge or none, the sky and the stars, shows
     * the room's sky beside it (each row's own sky entry: rows give the tile palettes of
     * their own). */
    bool mirror_row[64];
    for (int r = 0; r < 64; r++) mirror_row[r] = !sky;
    if (sky) {
        bool busy[64] = {false}, at_l[64] = {false}, at_r[64] = {false};
        for (int r = 0; r < mh; r++)
            for (int k = 0; k < 2; k++) {   /* (the two columns at each edge: a rope passing
                                              * near an edge does not join bands up) */
                bool nl = (d->vram[r * mw + ((first + k) & (mw - 1))] & 0x7FF) != sky_e;
                bool nr = (d->vram[r * mw + ((last - k) & (mw - 1))] & 0x7FF) != sky_e;
                busy[r] |= nl || nr;
                if (k == 0) at_l[r] = nl, at_r[r] = nr;
            }
        /* (round the map: it loops as the camera climbs, and a band can cross its seam;
         * started at a row of sky, so no band is cut in two) */
        int s0 = 0;
        while (s0 < mh && busy[s0]) s0++;
        if (s0 == mh) {   /* (no row of sky alone: all mirrored) */
            for (int r = 0; r < mh; r++) mirror_row[r] = true;
        } else {
            /* (a band's rows keep the decision they first had on screen, remembered by their
             * place in the level: off-screen rows of the map are redrawn as the game scrolls,
             * which turned a band's decision over and over, the mirror popping in and out as
             * Richter climbed) */
            static int8_t memo[1024];
            static int memo_key = -1, memo_cam = -100000;
            const uint8_t *wr = p->wram;
            int cam_y = wr[0x1E8E] << 8 | wr[0x1E20];
            int key = wr[0xA2] << 8 | wr[0x83];
            if (key != memo_key || cam_y - memo_cam > 64 || memo_cam - cam_y > 64) memset(memo, -1, sizeof memo);
            memo_key = key;
            memo_cam = cam_y;
            const uint16_t *my = vdc_row_map_y();
            bool on[64] = {false};
            int wrow[64];
            for (int y = 0; y < rows; y++) on[(my[y] & (mh * 8 - 1)) >> 3] = true;
            /* (each map row stands for the level's rows a whole map height apart: the one
             * from 4 rows above the camera on, so a row keeps its number as the camera moves) */
            int start = (cam_y >> 3) - 4;
            for (int r = 0; r < mh; r++) wrow[r] = (start + (((r - start) % mh) + mh) % mh) & 1023;
            for (int i = 0; i < mh;) {
                if (!busy[(s0 + i) % mh]) {
                    i++;
                    continue;
                }
                int n = 0;
                bool el = false, er = false;
                int known = -1;
                while (i + n < mh && busy[(s0 + i + n) % mh]) {
                    int r = (s0 + i + n) % mh;
                    el |= at_l[r];
                    er |= at_r[r];
                    if (on[r] && memo[wrow[r]] >= 0) known = memo[wrow[r]];
                    n++;
                }
                bool m = known >= 0 ? known != 0 : el && er;
                for (int k = 0; k < n; k++) {
                    int r = (s0 + i + k) % mh;
                    mirror_row[r] = m;
                    if (on[r] && memo[wrow[r]] < 0) memo[wrow[r]] = (int8_t)m;
                }
                i += n;
            }
        }
    }
    uint16_t sky_row_e = sky_e;   /* (the last row's own sky entry) */
    for (int r = 0; r < 64; r++) {
        uint16_t *l = &side_l[r * VDC_SIDE_TILES], *rt = &side_r[r * VDC_SIDE_TILES];
        if (r < mh && !mirror_row[r]) {
            for (int c = first; c <= last; c++) {
                uint16_t e = d->vram[r * mw + (c & (mw - 1))];
                if ((e & 0x7FF) == sky_e) {
                    sky_row_e = e;
                    break;
                }
            }
            for (int k = 0; k < VDC_SIDE_TILES; k++) l[k] = rt[k] = sky_row_e;
            continue;
        }
        for (int k = 0; k < VDC_SIDE_TILES; k++) {
            /* the room mirrored at its edges: column first-1-k is column first+k, last+1+k
             * is last-k (each tile drawn flipped by vdc_set_side_flip: an exact mirror) */
            l[k] = r < mh ? d->vram[r * mw + ((first + k) & (mw - 1))] : VDC_SIDE_NONE;
            rt[k] = r < mh ? d->vram[r * mw + ((last - k) & (mw - 1))] : VDC_SIDE_NONE;
        }
    }
    room_walled = sky ? 0 : carried_sides(d, mw, mh, first, last, rows);
    n_side_lantern = 0;
    room_patched = false;
    if (!sky) room_walled |= side_recipe(p->wram, p->frame, d, mw, mh, first, last);
    return true;
}


/* a row: every object of a kind (not fixed to the screen), three or four of them, standing
 * exactly 128 px apart (the bridge pillars; not, say, a splash's many particles, three of
 * which can happen to be 128 px apart) */
static void find_rows(const uint8_t *r, int cam_step)
{
    /* each kind's objects (which slots hold it) and how far the camera has gone since they
     * last changed: a row the game moves round keeps its objects; pillars it makes and
     * removes as the camera goes (the broken bridge's, kind $A0) change them within 128 px */
    uint32_t sig[256] = {0};
    for (int i = 0; i < OBJ_SLOTS; i++)
        if (r[OBJ_KIND + i]) sig[r[OBJ_KIND + i]] = sig[r[OBJ_KIND + i]] * 31u + (uint32_t)i + 1u;
    /* (row_travel: the camera's net move since then, so going back and forth does not add
     * up: pillars made 128 px apart cannot get 160 px away without a new one) */
    for (int k = 1; k < 256; k++) {
        if (sig[k] != row_sig[k]) {
            row_sig[k] = sig[k];
            row_travel[k] = 0;
            if (!jumped[k]) recycled[k] = false;   /* (a kind seen made or removed: not a row) */
        } else if (row_travel[k] > -10000 && row_travel[k] < 10000) {
            row_travel[k] += cam_step;
        }
    }
    for (int i = 0; i < OBJ_SLOTS; i++) {
        uint8_t k = r[OBJ_KIND + i];
        if (!k || recycled[k] || (row_travel[k] < 160 && row_travel[k] > -160) || (r[OBJ_FLAGS + i] & 1)) continue;
        int xs[5], n = 0;
        bool fixed = false;
        for (int j = 0; j < OBJ_SLOTS && n <= 4; j++) {
            if (r[OBJ_KIND + j] != k) continue;
            fixed |= (r[OBJ_FLAGS + j] & 1) != 0;
            xs[n++] = (int16_t)(r[OBJ_XL + j] | r[OBJ_XH + j] << 8);
        }
        if (fixed || n < 3 || n > 4) continue;
        for (int a = 1; a < n; a++)   /* (sorted) */
            for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) {
                int t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t;
            }
        bool row = true;
        for (int a = 1; a < n; a++) row &= xs[a] - xs[a - 1] <= 130 && xs[a] - xs[a - 1] >= 126;
        if (row) recycled[k] = true;
    }
}

/* Level ends: how far the picture moves over (positive: right) for a room whose walls are at
 * screen X left and right (right: just past its last pixel), ext a side */
static int edge_shift(int left, int right, int ext)
{
    int bl = left + ext > 0 ? left + ext : 0;              /* black at the left */
    int br = 256 + ext - right > 0 ? 256 + ext - right : 0; /* ... at the right */
    int shift = bl && br ? (bl - br) / 2 : bl - br;
    if (shift > ext) shift = ext;
    if (shift < -ext) shift = -ext;
    return shift;
}

/* The game moves a pillar round by 383 px, not 384, so after it the row stands 127 / 129 px
 * apart, and the copies 256 px either side land a pixel off the pillars they meet at the
 * picture's edges (a pillar seen twice, a pixel apart). Each row is kept exactly 128 px
 * apart: a pillar a pixel or two out of step with the rest is put back in step (scenery: a
 * pixel unseen in the 4:3 picture, where the moving round happens). */
static void align_rows(uint8_t *r)
{
    for (int k = 1; k < 256; k++) {
        if (!recycled[k]) continue;
        int slot[5], phase[5], n = 0;
        for (int i = 0; i < OBJ_SLOTS && n < 5; i++) {
            if (r[OBJ_KIND + i] != k || (r[OBJ_FLAGS + i] & 1)) continue;
            slot[n] = i;
            phase[n++] = (int16_t)(r[OBJ_XL + i] | r[OBJ_XH + i] << 8) & 127;
        }
        if (n < 3 || n > 4) continue;
        /* the phase most of the row has */
        int best = phase[0], most = 0;
        for (int a = 0; a < n; a++) {
            int c = 0;
            for (int b = 0; b < n; b++) c += phase[b] == phase[a];
            if (c > most) { most = c; best = phase[a]; }
        }
        if (most < 2) continue;
        for (int a = 0; a < n; a++) {
            int d = ((best - phase[a] + 64) & 127) - 64;   /* (-64..63) */
            if (d == 0 || d < -2 || d > 2) continue;
            int i = slot[a], x = (int16_t)(r[OBJ_XL + i] | r[OBJ_XH + i] << 8) + d;
            r[OBJ_XL + i] = (uint8_t)x;
            r[OBJ_XH + i] = (uint8_t)(x >> 8);
        }
    }
}

/* ---- scenery ------------------------------------------------------------------------ */

/* Scenery bands (rows scrolled apart from the playfield: Stage 2's distant hills, castle and
 * trees under the bridge) are sometimes drawn only as far across their map as the 4:3
 * picture ever shows of them; the rest of the map is blank tiles, which the wider picture
 * would show as bare sky. Each such map row's longest blank stretch is filled from both
 * ends with the tile most often after (before) the one at its end, seen at least twice in
 * the band, meeting in the middle: fixed to the map, so it scrolls with the band, and drawn
 * only beside the original 256 pixels (vdc_set_map_fill). Shorter gaps, the playfield's
 * rows and rows any playfield line shows are left as they are. */
#define SCENERY_MIN_GAP 12   /* tiles: the shortest blank stretch filled (96 px) */
static uint16_t scenery[64 * VDC_FILL_W];

static bool tile_blank(const Vdc *d, uint16_t e)
{
    const uint16_t *t = &d->vram[((e & 0x7FF) * 16) & 0x7FFF];
    for (int i = 0; i < 16; i++)
        if (t[i]) return false;
    return true;
}

/* Leftovers: in a room with a sky of its own tile (SKY_ROOMS), small pieces of map standing
 * alone in the sky beside the 4:3 picture are drawn as sky. At Death's mast (Stage 5's ship,
 * area $32 room 0) the deck breaks away as the fight starts, and the game clears only what
 * its camera, locked there, can show: one section of the old deck's end (4 x 3 cells, its
 * rail and hull) was left floating in the sea at the wider picture's left. Pieces of up to
 * 16 cells, none of them in the 4:3 picture's columns, touching nothing but sky. Drawn over
 * (the map fill), the game's map is left as it is. */
#define ORPHAN_MAX 16
static int sky_room(const uint8_t *r);
static bool orphans_fill(Pce *p, int cam, bool have)
{
    int sky = sky_room(p->wram);
    const Vdc *d = &p->vdc;
    static const int map_w[4] = {32, 64, 128, 128};
    int mw = map_w[(d->reg[9] >> 4) & 3], mh = (d->reg[9] & 0x40) ? 64 : 32;
    if (!sky || mw < 64 || mw > VDC_FILL_W) return false;
    static uint8_t seen[64][VDC_FILL_W];
    static int16_t cells[64 * VDC_FILL_W][2];
    static const int8_t dr[4] = {-1, 1, 0, 0}, dc[4] = {0, 0, -1, 1};
    memset(seen, 0, sizeof seen);
    bool any = false;
#define OTILE(r, c) d->vram[(r) * mw + ((c) & (mw - 1))]
#define IS_SKY(r, c) ((OTILE(r, c) & 0x7FF) == sky)
    for (int r0 = 0; r0 < mh; r0++)
        for (int c0 = 0; c0 < mw; c0++) {
            if (seen[r0][c0] || IS_SKY(r0, c0)) continue;
            /* the piece: every cell joined to this one */
            int n = 0;
            bool in_view = false;
            cells[n][0] = (int16_t)r0, cells[n][1] = (int16_t)c0, n++;
            seen[r0][c0] = 1;
            for (int h = 0; h < n; h++) {
                int r = cells[h][0], c = cells[h][1];
                /* (in the 4:3 picture: past its first 8 px, mostly left blank by the game) */
                int px = ((c * 8 - cam) & (mw * 8 - 1));
                if (px >= mw * 4) px -= mw * 8;
                if (px + 7 >= 8 && px < 256) in_view = true;
                for (int k = 0; k < 4; k++) {
                    int rr = r + dr[k], cc = (c + dc[k]) & (mw - 1);
                    if (rr < 0 || rr >= mh || seen[rr][cc] || IS_SKY(rr, cc)) continue;
                    seen[rr][cc] = 1;
                    cells[n][0] = (int16_t)rr, cells[n][1] = (int16_t)cc, n++;
                }
            }
            if (n > ORPHAN_MAX || in_view) continue;
            if (!have && !any)
                for (int i = 0; i < 64 * VDC_FILL_W; i++) scenery[i] = VDC_SIDE_NONE;
            for (int i = 0; i < n; i++) {
                int r = cells[i][0], c = cells[i][1];
                /* (the sky beside it in its row, its palette with it) */
                uint16_t e = (uint16_t)sky;
                for (int k = 1; k < mw; k++) {
                    if (IS_SKY(r, c - k)) { e = OTILE(r, c - k); break; }
                    if (IS_SKY(r, c + k)) { e = OTILE(r, c + k); break; }
                }
                scenery[r * VDC_FILL_W + c] = e;
                any = true;
            }
        }
#undef OTILE
#undef IS_SKY
    return any;
}

/* (in an arena made walkable, rows only a quarter drawn are filled too: the columns past the
 * art there (a chimney's top) come into the 4:3 pixels, where nothing else covers them) */
static bool scenery_loose;
static bool scenery_fill(Pce *p, int cam)
{
    static const int map_w[4] = {32, 64, 128, 128};
    const Vdc *d = &p->vdc;
    int mw = map_w[(d->reg[9] >> 4) & 3], mh = (d->reg[9] & 0x40) ? 64 : 32;
    if (mw < 64) return false;
    int rows = d->fb_h < PCE_FB_MAX_H ? d->fb_h : PCE_FB_MAX_H;
    const uint16_t *rs = vdc_row_scroll(), *my = vdc_row_map_y();
    /* The rows were drawn with the camera of a frame or two ago (the game moves it while it
     * runs a frame, and a slow frame shows it later): a line scrolled within the camera's
     * travel over the last few frames is the playfield's. (Compared with the camera now
     * alone, every row of a walk looked scrolled apart, and the playfield's own gaps were
     * filled with scenery: stray blocks and noise beside Stage 2's boss.) A camera that
     * jumps (a save state loaded, rewinding, a new screen) forgets the rows seen. */
    if (cams[0] < 0 || wrap(cam - cams[0]) < -32 || wrap(cam - cams[0]) > 32) {
        /* (the rows last drawn are of the picture before the jump: nothing to learn) */
        memset(scenery_rows, 0, sizeof scenery_rows);
        memset(rows_apart_n, 0, sizeof rows_apart_n);
        memset(rows_pf_n, 0, sizeof rows_pf_n);
        for (int k = 0; k < CAM_HISTORY; k++) cams[k] = cam;
        return false;
    }
    for (int k = CAM_HISTORY - 1; k > 0; k--) cams[k] = cams[k - 1];
    cams[0] = cam;
    /* (the camera can move twice in a frame: anywhere it has passed through counts) */
    int span_lo = 0, span_hi = 0;
    for (int k = 1; k < CAM_HISTORY; k++) {
        int d = wrap(cams[k] - cam);
        if (d < span_lo) span_lo = d;
        if (d > span_hi) span_hi = d;
    }
    /* a map row is scenery once it has been seen scrolled apart from the playfield in this
     * room (at a room's end every layer can stand at the same scroll for a moment); one a
     * playfield line shows in the same frame is not */
    uint8_t band[64] = {0};   /* 1 scenery, 2 shown by a playfield line */
    bool pf_line[64] = {false}, apart[64] = {false};
    const uint8_t *blank_line = vdc_row_bg_blank();
    /* (the playfield's scroll is the camera's in most of the game; in some places it stands a
     * few pixels off it (Stage 1's castle approach: 14 px; the wyvern's arena: 256), and every line looked scrolled
     * apart: nothing was learned and the railing band's undrawn columns showed bare at the
     * area's start. So the playfield's offset is the scroll of the line the hero stands on) */
    {
        const uint8_t *r = p->wram;
        int hy = (int16_t)(r[OBJ_YL] | r[OBJ_YH] << 8) - 16;
        if (hy >= 0 && hy < rows && !blank_line[hy]) {
            int off = (((rs[hy] & 0x1FF) - cam + 256) & 0x1FF) - 256;
            span_lo += off, span_hi += off;
        }
    }
    for (int y = 0; y < rows; y++) {
        /* (a line that showed no background, as while a new room is put up, tells nothing) */
        if (blank_line[y]) continue;
        int r = (my[y] & (mh * 8 - 1)) >> 3, x = rs[y] & 0x1FF;
        int dx = ((x - cam + 256) & 0x1FF) - 256;
        if (dx >= span_lo && dx <= span_hi) pf_line[r] = true;
        else apart[r] = true;
    }
    /* (a frame with no line at the camera tells nothing either: the game is scrolling the
     * picture itself, as through a door, the camera left where it was) */
    bool any_pf = false;
    for (int r = 0; r < 64; r++) any_pf |= pf_line[r];
    /* (counted while the camera moves, and only once apart more often than not: a playfield
     * row seen apart for a frame, the game late with its scroll, was taken for scenery for
     * the rest of the room, and its gaps were filled with its own tiles: the pier's plank
     * carried on over the water at Stage 5's start, after walking left and back) */
    bool moving = span_lo != span_hi;
    for (int r = 0; r < 64; r++) {
        if (moving && any_pf) {
            if (apart[r] && !pf_line[r] && rows_apart_n[r] < 60000) rows_apart_n[r]++;
            else if (pf_line[r] && !apart[r] && rows_pf_n[r] < 60000) rows_pf_n[r]++;
        }
        scenery_rows[r] = rows_apart_n[r] >= 4 && rows_apart_n[r] > rows_pf_n[r];
        band[r] = !scenery_rows[r] ? 0 : (apart[r] && pf_line[r]) ? 2 : 1;
    }
#define TILE(r, c) d->vram[(r) * mw + ((c) & (mw - 1))]
    for (int u = 0; u < n_used; u++) pair_n[pair_used[u]] = 0;
    n_used = 0;
    static bool blank[64][VDC_FILL_W];
    bool any_band = false;
    for (int r = 0; r < mh; r++) {
        if (band[r] != 1) continue;
        any_band = true;
        for (int c = 0; c < mw; c++) blank[r][c] = tile_blank(d, TILE(r, c));
        for (int c = 0; c < mw; c++)
            if (!blank[r][c] && !blank[r][(c + 1) & (mw - 1)]) pair_add(TILE(r, c), TILE(r, c + 1));
    }
    if (!any_band) return false;
    for (int i = 0; i < 64 * VDC_FILL_W; i++) scenery[i] = VDC_SIDE_NONE;
    bool any = false;
    for (int r = 0; r < mh; r++) {
        if (band[r] != 1) continue;
        /* the longest blank stretch, going round the map */
        int best_s = 0, best_l = 0;
        for (int c = 0; c < mw; c++) {
            if (!blank[r][c] || blank[r][(c - 1) & (mw - 1)]) continue;
            int l = 0;
            while (l < mw && blank[r][(c + l) & (mw - 1)]) l++;
            if (l > best_l) { best_l = l; best_s = c; }
        }
        /* (a stretch the art leaves out is long; gaps between clouds and hills are short; and
         * a band is mostly drawn: not a row that is empty but for a tower, say) */
        int drawn = 0;
        for (int c = 0; c < mw; c++) drawn += !blank[r][c];
        if (best_l < SCENERY_MIN_GAP || best_l >= mw || drawn * (scenery_loose ? 4 : 2) < mw) continue;
        int half = (best_l + 1) / 2;
        uint16_t e = TILE(r, best_s - 1);
        for (int k = 0; k < half && (e = pair_next_n(e, false, true)) != VDC_SIDE_NONE; k++) {
            scenery[r * VDC_FILL_W + ((best_s + k) & (mw - 1))] = e;
            any = true;
        }
        e = TILE(r, best_s + best_l);
        for (int k = 0; k < best_l - half && (e = pair_next_n(e, true, true)) != VDC_SIDE_NONE; k++) {
            scenery[r * VDC_FILL_W + ((best_s + best_l - 1 - k) & (mw - 1))] = e;
            any = true;
        }
        /* what the pairs left (a chain that breaks off: the wyvern's arena, its castle band
         * and railing) carried on by the row's own repeat, when it has one (the smallest
         * step its drawn part repeats by) */
        int period = 0;
        for (int pp = 1; pp <= 16 && !period; pp++) {
            int same = 0, seen = 0;
            for (int c = 0; c < mw; c++) {
                if (blank[r][c] || blank[r][(c + pp) & (mw - 1)]) continue;
                seen++;
                same += TILE(r, c) == TILE(r, c + pp);
            }
            if (seen >= 16 && same * 10 >= seen * 9) period = pp;
        }
        if (period) {
            for (int k = 0; k < best_l; k++) {
                uint16_t *cell = &scenery[r * VDC_FILL_W + ((best_s + k) & (mw - 1))];
                if (*cell != VDC_SIDE_NONE) continue;
                int back = (k / period + 1) * period;   /* (back past the stretch's start) */
                *cell = TILE(r, best_s + k - back);
                any = true;
            }
        }
        /* (and with no repeat of its own, the run the pairs did fill carried on over the
         * rest, round and round: the arena's castle band filled only from one side) */
        {
            int a0 = -1, a1 = -1;
            for (int k = 0; k < best_l; k++)
                if (scenery[r * VDC_FILL_W + ((best_s + k) & (mw - 1))] != VDC_SIDE_NONE) {
                    if (a0 < 0) a0 = k;
                    a1 = k;
                }
            int n = a1 - a0 + 1;
            if (a0 >= 0 && n >= 2) {
                for (int k = a0; k <= a1; k++)   /* (a run with a hole in it is not carried) */
                    if (scenery[r * VDC_FILL_W + ((best_s + k) & (mw - 1))] == VDC_SIDE_NONE) n = 0;
                for (int k = 0; n && k < best_l; k++) {
                    uint16_t *cell = &scenery[r * VDC_FILL_W + ((best_s + k) & (mw - 1))];
                    if (*cell != VDC_SIDE_NONE) continue;
                    int j = a0 + (((k - a0) % n) + n) % n;
                    *cell = scenery[r * VDC_FILL_W + ((best_s + j) & (mw - 1))];
                    any = true;
                }
            }
        }
    }
#undef TILE
    return any;
}

/* A frame wide_frame leaves alone (below): only the picture's shift (the host's own) set
 * for the camera the game built its sprites with, as the HUD was placed for (hud_at_build).
 * Left as it was, the HUD (placed for the new camera) stood a camera step off for that
 * frame: in a walkable arena it twitched by a pixel or two as Richter jumped. */
static void shift_only(Wide *w, Pce *p)
{
    const uint8_t *r = p->wram;
    int ext = w->want;
    if (!ext || !w->active || !w->build_seen || game_state(p) != RONDO_PLAYING || !room_scrolls(r)) return;
    int left, right;
    room_walls(r, w->build_cam, &left, &right);
    int shift = picture_shift(w, r, w->build_cam, left, right, ext, false);
    vdc_set_wide_shift(shift);
    w->shift = shift;
}

void wide_frame(Wide *w, Pce *p)
{
    /* A frame can end with the CPU still in code of ours (a slow frame: the sprite builder,
     * 72:8000, running on through our pieces in bank $00 at $FA00-$FFFF, mapped at $E000).
     * Taking the changes out then (as a stage ends: leaving play) put the RTS filler back
     * under it and the game ran off into the I/O page (after area $1A's orb). Nothing is
     * changed until it is out of our code; the picture stays as it was for that frame. */
    if (p->cpu.mpr[7] == 0x00 && p->cpu.pc >= 0xFA00) {
        shift_only(w, p);
        return;
    }
    /* ... nor while the game is inside a column drawing called from our code at $FE1C (its
     * return to $FE1F on the stack: the frame can end before it comes back) */
    for (int sp = p->cpu.s + 1; sp < 0xFF; sp++)
        if (p->wram[0x100 + sp] == 0x1E && p->wram[0x100 + sp + 1] == 0xFE) {
            shift_only(w, p);
            return;
        }
    /* Demos (state $03: the title's attract mode and the save menu's Tactics runs) are shown
     * 4:3, widescreen off while they play; it comes back by itself after (they played wide
     * as levels do, and went wrong: their rooms start in this state, which is not play) */
    if (game_state(p) == RONDO_TITLE) {
        if (p->vdc.wide || w->patched || w->active) wide_off(w, p);
        return;
    }
    layout_6f(p);
    uint8_t *r = p->wram;
    uint8_t s = game_state(p);
    /* GAME OVER shown over the frozen level ($31 01-03): drawn as the level was, wide */
    if (s == RONDO_GAME_OVER && r[RONDO_SUB] >= 0x01 && r[RONDO_SUB] <= 0x03) s = RONDO_PLAYING;
    bool want = w->want > 0;

    if (!p->on_entry) {
        p->on_entry = on_entry;
        p->on_entry_ctx = w;
    }
    /* outside gameplay the game is as it was; rooms put the changes back as they start */
    undo_old_patches(p);
    if (s != RONDO_PLAYING) set_patches(p, WORLD_PATCHES | OBJECT_PATCHES, false);
    /* a save state made in a widened room holds the object changes (their varying bytes not
     * the game's own: theirs are 0x40/0xC0): taken as in, so they follow this frame's values */
    if (!objects_on && s == RONDO_PLAYING) {
        /* (one made by an earlier build may lack some: they are put in with the rest) */
        bool all = true, moved = false;
        for (int i = 0; i < N_PATCHES; i++) {
            if (!(OBJECT_PATCHES & 1u << i)) continue;
            const Patch *q = &patches[i];
            const uint8_t *m = at(p, q);
            bool in = m && is_in(m, q);
            all &= in || (m && !memcmp(m, orig_of(q), q->len));
            if (q->var) moved |= in && m[q->var_at] != orig_of(q)[q->var_at];
        }
        if (all && moved) set_patches(p, OBJECT_PATCHES, true);
    }
    /* A room that starts as one screen and opens a frame or two later (the boss room past
     * area $1A's door: limits $200-$200 as it starts, $200-$280 after) was taken as one screen
     * at its start (on_entry) and drawn 4:3 with Room sides: the changes are put in now, in
     * its first frames, while it is still being put up */
    /* ... and one kept to a screen for a while that opens later on (Stage 5's ship: the
     * deck below Death's mast is one screen, $400-$400, until Richter jumps up to the
     * crow's nest, when it opens to $100-$600 for the fight, with no new room): the changes
     * go in as it opens. (The game streams columns 64 px past the screen's edges as ever,
     * so the sides already hold the room when it goes wide.) */
    static bool locked_before;
    bool opens_late = locked_before && room_scrolls(r);
    locked_before = s == RONDO_PLAYING && !room_scrolls(r);
    if (w->room_age < 255) w->room_age++;
    if (want && s == RONDO_PLAYING && (w->room_age <= LATE_OPEN || opens_late) && room_scrolls(r) &&
        (patches_in(p) & WORLD_PATCHES) != WORLD_PATCHES)
        set_patches(p, WORLD_PATCHES | OBJECT_PATCHES, true);
    uint32_t in = patches_in(p);
    w->patched = (in & WORLD_PATCHES) == WORLD_PATCHES;
    bool objects_out = (in & OBJECT_PATCHES) == OBJECT_PATCHES;

    /* scenery that keeps scrolling sideways (the carriage chase): a layer moved on at
     * least 45 of the last 60 frames; changing rooms starts the count again */
    bool moved = false;
    for (int i = 1; i < 8; i++) {
        if (r[CAM_XL + i] != w->layer_x[i]) moved = true;
        w->layer_x[i] = r[CAM_XL + i];
    }
    if (s != RONDO_PLAYING || r[0x31] == 0x03 || r[0x31] == 0x00) w->move_history = 0;
    else w->move_history = (w->move_history << 1 | moved) & ((1ull << 60) - 1);
    int moving = 0;
    for (uint64_t h = w->move_history; h; h &= h - 1) moving++;
    w->scenery_moving = moving;

    /* this frame shows the camera as it is now (it moves during the frame and is shown
     * from the next one) */
    int ext = w->want;
    int cam = (r[CAM_PAGE] & 0x0F) << 8 | r[CAM_XL];
    /* The picture is shifted for the camera the game built its sprites with (hud_at_build):
     * it draws the playfield with the same one, which can be the camera before or after this
     * point, depending on how long its frames run (a slow frame moves the camera while the
     * next picture is drawn). Worked out from the camera now, the picture shook by the
     * camera's step at a room's end. (A jump or a new room: the camera now.) */
    /* an open arena made walkable (arena_walk): its limits widened, so it scrolls */
    bool walk = arena_walk(w, p, s);
    int drawn = cam;
    if (w->build_seen && wrap(cam - w->build_cam) >= -32 && wrap(cam - w->build_cam) <= 32) drawn = w->build_cam;
    w->build_seen = false;
    int left, right;
    room_walls(r, drawn, &left, &right);
    bool scrolls = room_scrolls(r);
    /* objects' tiles kept from columns drawn over them (keep_tiles) */
    keep_tiles(p, want && s == RONDO_PLAYING && scrolls && w->patched && objects_on);
    leave_drawn(p, s == RONDO_PLAYING && r[0x31] == 0x03, drawn);
    vdc_bat_written_clear();   /* (the next frame's writes, for keep_tiles) */
    /* A boss locks the camera to one screen of a room it scrolled through (Stage 2's: the
     * limits close in on the arena, with no new room): the picture stays wide, the map
     * beside the arena (streamed as the camera came in, and still there) shown, and the
     * room's walls are those from before the lock (black beyond them as ever). */
    if (scrolls) {
        w->open_lo = limit_lo(r);
        w->open_hi = limit_hi(r);
        w->open_seen = true;
    } else if (w->open_seen && w->patched && s == RONDO_PLAYING &&
               drawn >= w->open_lo && drawn <= w->open_hi) {
        walls_of(w->open_lo, w->open_hi, drawn, &left, &right);
        scrolls = true;
    }
    int mw = (p->vdc.reg[9] >> 4) & 3;         /* background map width: 64+ tiles needed */
    /* a boss room with no wall at its sides (open_arena; Video > Open arenas) */
    bool arena = want && w->open_arenas && s == RONDO_PLAYING && !scrolls && open_arena(r);
    /* wide: rooms that scroll sideways, drawn with the changes; rooms one screen wide whose
     * scenery scrolls past (their layers loop round the whole map), drawn as the game draws
     * them; and open arenas, likewise. Others (vertical shafts, boss rooms, rooms entered
     * before widescreen was turned on) keep the 4:3 picture. */
    bool show = want && s == RONDO_PLAYING && mw >= 1 &&
                (scrolls ? w->patched || walk : arena || (!w->patched && w->scenery_moving >= 45));
    /* sprites past the 256 pixels are built only while the picture is wide */
    if (!show) hit_patches(p, false);   /* (out before the code they call) */
    set_patches(p, DRAW_PATCHES, show);
    if (show && (patches_in(p) & DRAW_PATCHES) != DRAW_PATCHES) {
        hit_patches(p, false);
        set_patches(p, DRAW_PATCHES, false);
        show = false;
    }
    /* (a room kept 4:3 with sides: its sprites built out past the old edges as well) */
    bool sprites_out = !show && want && s == RONDO_PLAYING && mw >= 1 && !scrolls && w->room_sides;
    if (sprites_out) set_patches(p, SPRITE_PATCHES, true);
    /* the hit test's places, found in this stage's bank $6B (none found: hits as in 4:3) */
    hit_patches(p, show);
    far_patches(p, show);
    remove_patch(p, show);
    /* menus (title, file select): wide once the sides have filled for a few frames in a
     * row; then kept, with the last sides that filled, through a while they do not (a page
     * of the menu sliding over: its frames reach the edges), so it does not flick back */
    bool menu = false;
    if (!show && want && w->menus && (s == RONDO_TITLE || s == RONDO_SAVE_MENU || s == RONDO_GAME_OVER)) {
        bool good = menu_sides(p, ext);
        if (good) {
            w->menu_good = w->menu_good < MENU_FRAMES ? w->menu_good + 1 : MENU_FRAMES;
            /* (the continue screen comes straight from the wide level: wide at once, not
             * after MENU_FRAMES of 4:3) */
            if (s == RONDO_GAME_OVER) w->menu_good = MENU_FRAMES;
            w->menu_bad = 0;
        } else if (w->menu_good >= MENU_FRAMES && ++w->menu_bad > MENU_HOLD) {
            w->menu_good = 0;
        } else if (w->menu_good < MENU_FRAMES) {
            w->menu_good = 0;
        }
        menu = w->menu_good >= MENU_FRAMES;
    } else {
        w->menu_good = w->menu_bad = 0;
    }
    /* a room kept 4:3 in play: mirrored beside it, fading out (room_sides; Video > Room
     * sides) */
    bool walls = !show && !menu && want && w->room_sides && s == RONDO_PLAYING && mw >= 1 && room_sides(p, sky_room(r));
    vdc_set_side_tiles(menu || walls ? side_l : NULL, menu || walls ? side_r : NULL);
    /* (the save menu's parked torch flames: all through its screens with widescreen on, the
     * frames between pages too, which are not widened and showed them a moment) */
    vdc_set_hide_corner(menu || (want && (s == RONDO_SAVE_MENU || s == RONDO_GAME_OVER)));
    /* (mirrored sides fade out and are drawn flipped, an exact mirror; carried ones neither) */
    int mirrored = walls ? (VDC_SIDE_LEFT | VDC_SIDE_RIGHT) & ~room_walled : 0;
    vdc_set_side_fade(mirrored);
    vdc_set_side_flip(mirrored);
    vdc_set_extra_sprites(walls && n_side_lantern ? side_lantern : NULL, walls ? n_side_lantern : 0);
    vdc_set_map_patch(walls && room_patched ? room_patch : NULL);
    /* a menu sliding over to another page (its scroll moved in the last few frames): the
     * pages themselves across the whole picture (vdc_set_side_real) */
    {
        int bxr = vdc_row_scroll()[0];
        if (bxr != w->menu_bxr) w->menu_moved = 0;
        else if (w->menu_moved < 255) w->menu_moved++;
        w->menu_bxr = bxr;
    }
    vdc_set_side_real(menu && w->menu_moved < 4);
    if (!show && !menu && !walls) ext = 0;

    /* In a scene drawn wide over its looping scenery, a band of rows that stands still with
     * nothing in the original 256 pixels holds something parked in the part of its 512-pixel
     * map those pixels do not show: the Prologue's carriage (background) waits there just
     * off both edges, its horses (sprite objects) just off the left, while the forest
     * scrolls past. Such a band shows only the original 256 pixels, and objects standing
     * still off the left are not drawn (the HUD excepted). When the band moves again, the
     * parked thing is coming in from the left: the left side is shown, the right (where the
     * map wraps round to its rear) is not, until it has come in clear of the picture's left
     * edge (on the widest screens the picture shows 480 of the map's 512 pixels, so what is
     * still off the left wraps round into the right side). Decided per band (rows drawn with
     * the same scroll) from the last frame's rows. */
    const uint16_t *rs = vdc_row_scroll();
    const uint8_t *blank = vdc_row_bg_blank();
    const uint8_t *edge = vdc_row_bg_edge();
    int rows = p->vdc.fb_h < PCE_FB_MAX_H ? p->vdc.fb_h : PCE_FB_MAX_H;
    bool clip_parked = ext && !scrolls && !menu && !walls && !arena, any_parked = false;
    for (int y = 0; y < PCE_FB_MAX_H; y++) {
        bool same = y < rows && rs[y] == w->row_x[y];
        w->row_still[y] = same ? (uint8_t)(w->row_still[y] < 255 ? w->row_still[y] + 1 : 255) : 0;
    }
    for (int y0 = 0; y0 < PCE_FB_MAX_H;) {
        int y1 = y0 + 1;
        /* same scroll now and last frame: two bands whose scrolls just happen to meet (the
         * trees passing the parked carriage's) stay apart */
        while (y0 < rows && y1 < rows && rs[y1] == rs[y0] && w->row_x[y1] == w->row_x[y0]) y1++;
        bool all_blank = y0 < rows, all_still = true, at_edge = false;
        uint8_t was = 0;
        for (int y = y0; y < y1; y++) {
            all_blank &= y < rows && blank[y];
            all_still &= w->row_still[y] >= PARKED_FRAMES;
            at_edge |= y < rows && edge[y];
            if (w->row_clip[y] > was) was = w->row_clip[y];
        }
        uint8_t mode = 0;
        if (clip_parked && all_blank && all_still) mode = VDC_CLIP_BOTH;
        else if (clip_parked && was && (all_blank || at_edge)) mode = VDC_CLIP_RIGHT;
        for (int y = y0; y < y1; y++) w->row_clip[y] = mode;
        any_parked |= mode == VDC_CLIP_BOTH;
        y0 = y1;
    }
    for (int y = 0; y < rows; y++) w->row_x[y] = rs[y];
    vdc_set_row_clip(w->row_clip);

    /* objects standing still off the left while a band is parked; and screen-wide weather to
     * echo 256 px either side: objects fixed to the screen (not the HUD) that stay on it but
     * keep moving, such as Stage 2's rain (two objects sweeping down and left across it,
     * their drops about 128 px either side). A banner or a generator does not move. */
    uint8_t *table = p->page[0x00] ? p->page[0x00] + PARKED_TABLE : NULL;
    uint8_t *echo = p->page[0x00] ? p->page[0x00] + ECHO_TABLE : NULL;

    if (ext && s == RONDO_PLAYING) {
        int cam_now = (r[CAM_PAGE] & 0x0F) << 8 | r[CAM_XL], st = wrap(cam_now - rows_cam);
        rows_cam = cam_now;
        find_rows(r, st < -32 || st > 32 ? 0 : st);
        align_rows(r);
    }
    /* Objects that move with a scenery layer (OBJ_LAYER not 0: Stage 2's boss's figure on its
     * tower, kind $A7, in the half-speed layer) made further out than the game makes them
     * (Spawn distance: Far) moved with their layer from then on, at its speed and not the
     * camera's, and came into view off where the game puts them (the figure 25 px off its
     * tower: made 50 px of camera earlier, half of that). While beyond the game's own spawn
     * lines (camera + $140 / - $40), where the 4:3 game has not made them yet, they keep
     * their place in the level (move with the camera); from there on, with their layer. */
    {
        static int last_lx[8];
        static bool lx_seen;
        int lx[8];
        for (int k = 0; k < 8; k++) lx[k] = (r[CAM_PAGE + k] & 0x0F) << 8 | r[CAM_XL + k];
        int cd = wrap(lx[0] - last_lx[0]);
        if (lx_seen && s == RONDO_PLAYING && objects_on && cd >= -32 && cd <= 32)
            for (int i = 0x10; i < 0x54; i++) {
                int layer = r[OBJ_LAYER + i] & 0x7F, prev = w->obj_x[i];
                if (!r[OBJ_KIND + i] || r[OBJ_KIND + i] != w->obj_kind[i] || (r[OBJ_FLAGS + i] & 1) || !layer ||
                    layer > 7 || (prev <= 0x140 && prev >= -0x40))
                    continue;
                int ld = wrap(lx[layer] - last_lx[layer]);
                if (ld < -32 || ld > 32 || ld == cd) continue;
                int x = (int16_t)(r[OBJ_XL + i] | r[OBJ_XH + i] << 8) - (cd - ld);
                r[OBJ_XL + i] = (uint8_t)x;
                r[OBJ_XH + i] = (uint8_t)(x >> 8);
            }
        memcpy(last_lx, lx, sizeof lx);
        lx_seen = true;
    }
    VdcFade fades[VDC_FADES];
    int n_fades = 0;
    for (int i = 0; i < OBJ_SLOTS; i++) {
        int16_t x = (int16_t)(r[OBJ_XL + i] | r[OBJ_XH + i] << 8);
        w->obj_still[i] = x == w->obj_x[i] ? (uint8_t)(w->obj_still[i] < 255 ? w->obj_still[i] + 1 : 255) : 0;
        /* scenery the game keeps as a few objects it moves round from one side of its screen
         * to the other (Stage 2's bridge pillars: three, 128 px apart, kind $44, jumping 384):
         * noticed by such a jump, and drawn 256 px either side too (the echoes below), which
         * puts the copies where the next ones round would be */
        int jump = x - w->obj_x[i];
        if (r[OBJ_KIND + i] && r[OBJ_KIND + i] == w->obj_kind[i] && ((jump >= 376 && jump <= 392) || (jump <= -376 && jump >= -392)))
            recycled[r[OBJ_KIND + i]] = jumped[r[OBJ_KIND + i]] = true;
        w->obj_x[i] = x;
        bool hud = i == HUD_SLOT || i == SUBWEAPON_SLOT;
        bool parked = any_parked && !hud && r[OBJ_FRAME + i] && x < 0 && w->obj_still[i] >= PARKED_FRAMES;
        /* (in a walkable arena, a free slot too: what is made in it this frame just off the
         * left is culled as in 4:3. The skull's controller, $33, makes a bat at -32 now and
         * then that removes itself the next frame, never seen in 4:3, a flash in the wider
         * picture; arena_walk removes any that stays) */
        /* (free: no kind and no sprite; the HUD, a boss's bar and titles are kind 0 drawn) */
        if (table) table[i] = parked || (walk && !hud && !r[OBJ_KIND + i] && !r[OBJ_FRAME + i]);
        /* (the rain only, kind $C2 type 7: not the boat ride's boat and ferryman, fixed to the
         * screen and bobbing, which were drawn again 256 px over at the picture's other edge) */
        bool weather = ext && !hud && r[OBJ_KIND + i] == 0xC2 && (r[OBJ_FLAGS + i] & 1) && r[OBJ_TYPE + i] == 7 &&
                       x >= 0 && x < 256 && w->obj_still[i] < ECHO_STILL_FRAMES;
        bool round = ext && !hud && r[OBJ_KIND + i] && !(r[OBJ_FLAGS + i] & 1) && recycled[r[OBJ_KIND + i]];
        /* ...but not onto one of the row the game is hiding (frame 0: a pillar it leaves out
         * where the bridge ends): the copy 256 px on would stand in for it there. (The other
         * copy is 512 px from it, further than the picture is wide: never wanted at once.) */
        for (int j = 0; round && j < OBJ_SLOTS; j++) {
            if (j == i || r[OBJ_KIND + j] != r[OBJ_KIND + i] || r[OBJ_FRAME + j]) continue;
            int xj = (int16_t)(r[OBJ_XL + j] | r[OBJ_XH + j] << 8), d = xj - x;
            if ((d >= 254 && d <= 258) || (d <= -254 && d >= -258)) round = false;
        }
        /* ...nor near a room's end walls (the bridge's end piers stand where the next pillar
         * would: the game only ever moves its pillars round out of the 4:3 picture's sight) */
        if (round && scrolls && ((x + 256 > right - 96 && x + 256 < 256 + 2 * WIDE_MAX + 32) ||
                                 (x - 256 < left + 96 && x - 256 > -2 * WIDE_MAX - 32)))
            round = false;   /* (only a copy that would be seen: the other one may be wanted) */
        if (echo) echo[i] = weather || round;
        /* Scenery objects on a layer of their own (Stage 5's pier: the wrecked ships, kind $A6,
         * on the slow layers) are made by the game as the camera comes near, at the spawn
         * line, which the wider picture can show of a ship 96 px across (more so at a level's
         * end, the picture shifted: 133 px): the ship popped in, and went again at the line
         * where the game removes them. Made further out than the 4:3 game makes anything
         * they fade in, and they fade out over the last 32 px before that line. */
        {
            int layer = r[OBJ_LAYER + i] & 0x7F;
            bool scen = ext && s == RONDO_PLAYING && !hud && r[OBJ_KIND + i] && !(r[OBJ_FLAGS + i] & 1) && layer &&
                        layer <= 7;
            if (!scen) {
                fade_t[i] = FADE_IN_FRAMES;
            } else {
                if (r[OBJ_KIND + i] != w->obj_kind[i]) fade_t[i] = x < -0x40 || x > 0x140 ? 0 : FADE_IN_FRAMES;
                else if (fade_t[i] < FADE_IN_FRAMES) fade_t[i]++;
                int lv = fade_t[i] * 16 / FADE_IN_FRAMES;
                int pl = x < -0x80 ? (x + 0xA0) * 16 / 0x20 : x > 0x180 ? (0x1A0 - x) * 16 / 0x20 : 16;
                if (pl < lv) lv = pl;
                if (lv < 0) lv = 0;
                if (lv < 16 && n_fades < VDC_FADES) {
                    int y = (int16_t)(r[OBJ_YL + i] | r[OBJ_YH + i] << 8);
                    fades[n_fades++] = (VdcFade){(int16_t)(x - 96), (int16_t)(x + 96), (int16_t)(y - 112),
                                                 (int16_t)(y + 112), (uint8_t)lv};
                }
            }
        }
    }
    vdc_set_sprite_fades(fades, n_fades);

    /* Level edges (w->edges): within the extension of a room's end the picture moves over,
     * all the extra width going to the side the room goes on, so nothing beyond its walls
     * shows; it stays put there until the camera has come that far from the end. (A room
     * narrower than the picture is centred between its walls.) */
    /* Past some doors the game changes the room's limits with no new room (the next part is
     * put up as the hero walks through black): the map is drawn only as far past the 4:3
     * picture as ever, not across the 512 pixels a room start draws, so a shift there would
     * show the part left behind. Kept at 0 until the camera has gone 128 px from where the
     * limits changed (by then the map round it has been drawn). */
    if (room_scrolls(r)) {
        int lo = limit_lo(r), hi = limit_hi(r);
        /* (limits changed while the picture is all black, as when the hero comes back after
         * losing a life, come with the whole map drawn again) */
        const uint8_t *bl = vdc_row_bg_blank();
        bool all_black = true;
        for (int y = 0; y < p->vdc.fb_h && y < PCE_FB_MAX_H && all_black; y++) all_black = bl[y];
        if (w->lim_seen && (lo != w->lim_lo || hi != w->lim_hi) && !all_black && !walk) {
            w->calm = true;
            w->calm_cam = cam;
            w->calm_black = false;
        }
        w->lim_lo = lo;
        w->lim_hi = hi;
        w->lim_seen = true;
    }
    if (w->calm && (wrap(cam - w->calm_cam) >= 128 || wrap(cam - w->calm_cam) <= -128)) w->calm = false;
    beyond_door(w, p, s, cam, ext);
    if (leave_side) shift_ease = 120, ease_side = -leave_side;   /* (leaving, and a while after: see picture_shift) */
    int shift = scrolls ? picture_shift(w, r, drawn, left, right, ext, true) : 0;
    /* leaving through a door: the side away from it drawn from VRAM as it was (leave_vram) */
    {
        /* (the scroll of the last picture drawn; as the leaving starts the game moves the
         * screen to another place in the map, copying only its 256 pixels: a jump in the
         * scroll is followed in the copy, the camera's steps are not) */
        static uint16_t last_x[PCE_FB_MAX_H], last_y[PCE_FB_MAX_H];
        static int16_t leave_dx[PCE_FB_MAX_H], leave_dy[PCE_FB_MAX_H];
        const uint16_t *bx = vdc_row_scroll(), *by = vdc_row_map_y();
        /* (the scroll's jumps followed while leaving and while easing after) */
        if ((s == RONDO_PLAYING && r[0x31] == 0x03 && ext && (scrolls || walls) && leave_side) || ease_side)
            for (int y = 0; y < PCE_FB_MAX_H; y++) {
                int jx = (bx[y] - last_x[y] + 512) & 511, jy = (by[y] - last_y[y] + 512) & 511;
                if (jx > 256) jx -= 512;
                if (jy > 256) jy -= 512;
                if (jx > 32 || jx < -32 || jy > 32 || jy < -32) {
                    leave_dx[y] = (int16_t)(leave_dx[y] - jx);
                    leave_dy[y] = (int16_t)(leave_dy[y] - jy);
                }
            }
        if (s == RONDO_PLAYING && r[0x31] == 0x03 && ext && (scrolls || walls)) {
            if (!leave_side) {
                leave_side = (int16_t)(r[OBJ_XL] | r[OBJ_XH] << 8) < 128 ? -1 : 1;
                memset(leave_dx, 0, sizeof leave_dx);
                memset(leave_dy, 0, sizeof leave_dy);
            }
            vdc_set_side_vram(leave_vram, -leave_side, leave_dx, leave_dy);
            if (walls) sides_kept(&p->vdc, true, drawn, -leave_side);
            vdc_set_side_kept(walls ? keep_l : NULL, walls ? keep_r : NULL, keep_first, keep_last);
            /* (and the door side's sprites: the next part's, the courtyard's trees) */
            vdc_set_side_sprites(walls ? (leave_side < 0 ? VDC_SIDE_LEFT : VDC_SIDE_RIGHT) : 0);
            /* (the kept sides flipped and faded as they were: worked out now, with the map
             * changing under the room's side recipe, a side came and went as mirrored, flipped
             * and faded to black every few frames; the door side never faded, the next part
             * fully lit as 4:3 shows it) */
            if (walls) {
                vdc_set_side_flip(keep_mirrored);
                vdc_set_side_fade(keep_mirrored & (leave_side < 0 ? VDC_SIDE_RIGHT : VDC_SIDE_LEFT));
            }
        } else {
            if (leave_side) shift_ease = 120;   /* (a leaving just ended: see picture_shift) */
            else if (shift_ease > 0 && ext && w->shift == picture_shift_to(w, r, drawn, left, right, ext, false)) shift_ease = 0;
            else if (shift_ease > 0) shift_ease--;
            if (shift_ease <= 0) ease_side = 0;
            leave_side = 0;
            if (ease_side) {
                /* (easing: the room left behind still beside the new one, going off as the
                 * picture moves over; its copy kept) */
                vdc_set_side_vram(leave_vram, ease_side, leave_dx, leave_dy);
            } else {
                vdc_set_side_vram(NULL, 0, NULL, NULL);
                if (s == RONDO_PLAYING && ext && (scrolls || walls)) memcpy(leave_vram, p->vdc.vram, sizeof leave_vram);
            }
            if (walls) sides_kept(&p->vdc, false, drawn, 0), keep_mirrored = mirrored;
            vdc_set_side_kept(NULL, NULL, 0, 0);
            /* (sprites beside the old picture in a room kept 4:3 with sides: things going past
             * its edges, as in a wide room) */
            vdc_set_side_sprites(walls && s == RONDO_PLAYING ? VDC_SIDE_LEFT | VDC_SIDE_RIGHT : 0);
        }
        memcpy(last_x, bx, sizeof last_x);
        memcpy(last_y, by, sizeof last_y);
    }
    if (!scrolls) w->inside_shift = 0;
    vdc_set_wide_shift(shift);
    w->shift = shift;
    /* (no parallax in a walkable arena: flat_layers) */
    halve_patch(p, walk && walk_lo >= 0 && walk_flat);
    if (walk && walk_lo >= 0 && walk_flat) flat_layers(p->wram, walk_cam0, shift);
    else flat_shift = 1 << 30;

    /* the hit test's 256 pixels (HIT_BIAS): the game's own screen, moved towards the side
     * the hero faces only as far as objects there need (4:3: never) */
    {
        uint8_t *hb = p->page[0x00] ? p->page[0x00] + HIT_BIAS : NULL;
        int bias = 0;
        if (show && ext && s == RONDO_PLAYING) {
            int lo = shift - ext, hi = ext + shift;   /* the picture: screen X lo .. hi + 256 */
            int hx = (int16_t)(r[OBJ_XL] | r[OBJ_XH] << 8);
            bool left = r[OBJ_FACING] != 0;
            /* the furthest object past the 256 pixels on the side faced (weapons included;
             * not those fixed to the screen), with 16 px for its box */
            int need = 0;
            for (int i = 0; i < OBJ_SLOTS; i++) {
                if (!r[OBJ_KIND + i] || (r[OBJ_FLAGS + i] & 1)) continue;
                int x = (int16_t)(r[OBJ_XL + i] | r[OBJ_XH + i] << 8);
                if (!left && x > 239 && x < hi + 256 + 16 && x + 16 - 255 > need) need = x + 16 - 255;
                if (left && x < 16 && x > lo - 16 && x - 16 < need) need = x - 16;
            }
            /* ... as far as that needs, keeping 64 px behind the hero (an enemy's touch) */
            if (!left) bias = need < hx - 64 ? need : hx - 64;
            else bias = need > hx + 64 - 256 ? need : hx + 64 - 256;
            if (!left && bias < 0) bias = 0;
            if (left && bias > 0) bias = 0;
            if (bias < lo) bias = lo;
            if (bias > hi) bias = hi;
        }
        if (hb) {
            hb[0] = (uint8_t)bias;
            hb[1] = (uint8_t)(bias >> 8);
        }
    }

    /* Enemies some rooms send in from just past the screen's edges, whatever the camera
     * does (the castle hall's ghouls at X -24 / 280, its kind-31 creatures at 272): put at
     * those places they would appear inside the wider picture, so on the frame one first
     * shows it moves out by the picture's extra width on that side, keeping its distance
     * from the edge, but no more than EDGE_SPAWN_MAX past the screen's edge (the creatures
     * remove themselves further out: kind 31 from 86 px). Only at those exact places (16, 24
     * or 32 px past the edge; things thrown from off screen come out where the thrower is),
     * and not objects fixed to the screen. */
    /* (objects a room's start makes all at once, from its list, are where the level has
     * them: one may stand at -16 by chance, as Stage 3's chains do, and was moved onto the
     * next. Made three or more in a frame: not moved.) */
    /* (also done as the game starts building its sprites, edge_spawns_at_build: one made
     * with its sprite during a frame was drawn where the game put it for that frame, a
     * pop-in inside the picture's edge, before being moved here) */
    w->edge_live = ext && s == RONDO_PLAYING && scrolls && !walk;
    {
        int born = 0;
        for (int i = 0; i < OBJ_SLOTS; i++) born += r[OBJ_KIND + i] && !w->obj_kind[i];
        for (int i = 0; i < OBJ_SLOTS; i++)
            if (r[OBJ_KIND + i] && !w->obj_kind[i]) edge_batch[i] = born >= 3;
            else if (!r[OBJ_KIND + i]) edge_batch[i] = false;
    }
    for (int i = 0; i < OBJ_SLOTS; i++) {
        uint8_t kind = r[OBJ_KIND + i];
        /* (first drawn, or just made: made at the edge before it has a sprite, it moves a
         * pixel before it is first drawn, as Stage 4's Medusa heads, kind $27, at -16) */
        bool first = kind && ((r[OBJ_FRAME + i] && !(kind == w->obj_kind[i] && w->obj_shown[i])) || kind != w->obj_kind[i]);
        w->obj_kind[i] = kind;
        w->obj_shown[i] = kind && r[OBJ_FRAME + i];
        if (!first || edge_batch[i] || !w->edge_live || (r[OBJ_FLAGS + i] & 1) || i == HUD_SLOT || i == SUBWEAPON_SLOT)
            continue;
        int x = (int16_t)(r[OBJ_XL + i] | r[OBJ_XH + i] << 8), to = edge_spawn_to(x, ext, shift);
        if (to != x) {
            r[OBJ_XL + i] = (uint8_t)to;
            r[OBJ_XH + i] = (uint8_t)(to >> 8);
        }
    }

    /* sprites are built out to the picture's edges; objects spawn and start moving past the
     * side the picture reaches further on (no further than 120 px: the game removes them
     * 128 px out) */
    var_value[VAR_SPRITE_RIGHT] = var_value[VAR_ECHO_RIGHT] = (uint8_t)(0x20 + E + (shift > 0 ? shift : 0));
    var_value[VAR_SPRITE_LEFT] = (uint8_t)(0x100 - E - (shift < 0 ? -shift : 0));
    int base = w->near_spawns ? OBJECT_REACH : object_reach(w->want);   /* (Video > Spawn distance) */
    int reach_r = shift > 0 ? ext + shift + 8 : 0, reach_l = shift < 0 ? ext - shift + 8 : 0;
    int dr = reach_r > base ? (reach_r < 0x78 ? reach_r : 0x78) : base;
    int dl = reach_l > base ? (reach_l < 0x78 ? reach_l : 0x78) : base;
    if (w->near_spawns) dr = dl = OBJECT_REACH;
    /* A walkable arena (arena_walk): objects are made only as far out as from the arena's
     * own camera (64 px past its 256 pixels), the spawn line kept still while the camera
     * moves: past the arena's right the level goes on with a bat generator ($35 at $4A0 in
     * Stage 3's), which sent bats in once the camera had gone right. (At least 1: 0 is
     * var_value's "the patch's own byte".) */
    if (walk) {
        int off = wrap(cam - walk_cam0);
        dr = 0x40 - (off > 0 ? off : 0);
        dl = 0x40 - (off < 0 ? -off : 0);
        if (dr < 1) dr = 1;
        if (dl < 1) dl = 1;
        if (!objects_on) set_patches(p, OBJECT_PATCHES, true);
    }
    var_value[VAR_SPAWN_RIGHT] = (uint8_t)dr;
    var_value[VAR_SPAWN_LEFT] = (uint8_t)(0x100 - dl);
    /* ... but start moving where the game's do, 64 px out: the ground objects stand and
     * walk on is looked up (6B:96E3) in 8 columns of 32 px, the 256 pixels round its
     * screen, so further out an object took another column's ground (Stage 3's skeleton,
     * made on a ledge past the screen, fell into the rock). Past 64 px they wait, drawn. */
    var_value[VAR_MOVE_RIGHT] = 0x40;
    var_value[VAR_MOVE_LEFT] = 0xC0;
    var_value[VAR_SWEEP_RIGHT] = (uint8_t)(dr + 1);
    var_value[VAR_SWEEP_LEFT] = (uint8_t)(dl + 1);   /* (steps: camera + $100 + dr included) */
    update_vars(p);
    if (s == RONDO_PLAYING && (objects_out || w->gen_r || w->gen_l)) {
        /* (new generators come at the game's edge) */
        if (objects_out && !walk) move_generators(w, p, dr, dl);
        else if (objects_out) move_generators(w, p, 0x40, 0x40);   /* (generators where the game has them) */
        else {
            move_generators(w, p, 0x40, 0x40);
            w->gen_r = w->gen_l = 0;
        }
    }

    /* scenery bands carried on beside the picture (scenery_fill) */
    scenery_loose = walk;
    bool filled = ext && show && scrolls && s == RONDO_PLAYING && scenery_fill(p, cam);
    bool orphans = ext && show && s == RONDO_PLAYING && orphans_fill(p, cam, filled);
    vdc_set_map_fill(filled || orphans ? scenery : NULL);
    /* (a leftover's last column can stand in the 4:3 picture's first pixels: with no scenery
     * filled, the fill is drawn there too) */
    vdc_set_map_fill_all((filled && walk) || (orphans && !filled));

    /* the door just come through kept in the wider picture (door_ghost) */
    door_ghost(p, w->want);   /* (the wanted width: a room's first frames are drawn 4:3) */

    p->vdc.wide = ext;
    if (menu) {
        p->vdc.wide_x0 = 0;
        p->vdc.wide_x1 = 256 + 2 * ext;
    } else if (scrolls) {
        p->vdc.wide_x0 = left + ext - shift;
        p->vdc.wide_x1 = right + ext - shift;
        /* Black only beside the original 256 pixels, never in them: past a door the game
         * scrolls its camera beyond the room's limits into the next part, which it shows
         * there (the black covered it, and the behemoth breaking in after the hero). While
         * the camera is beyond the limits, nothing is black. */
        int orig = ext - shift;
        if (p->vdc.wide_x0 > orig) p->vdc.wide_x0 = orig;
        if (p->vdc.wide_x1 < orig + 256) p->vdc.wide_x1 = orig + 256;
        /* (nor while settling past a door: beyond the next part's wall is the part just
         * left, drawn as the game had it) */
        if ((w->calm && !w->calm_black) || (room_scrolls(r) && past_limits(drawn, limit_lo(r), limit_hi(r)))) {
            p->vdc.wide_x0 = 0;
            p->vdc.wide_x1 = 256 + 2 * ext;
        }
        /* (nor on the side the room left behind is drawn while the picture eases over) */
        if (ease_side < 0) p->vdc.wide_x0 = 0;
        if (ease_side > 0) p->vdc.wide_x1 = 256 + 2 * ext;
        /* (nor in an arena made walkable: past where its camera may go the level carries on,
         * drawn; the wyvern's stops short of its floor's end, so its right side is passed) */
        if (walk) {
            p->vdc.wide_x0 = 0;
            p->vdc.wide_x1 = 256 + 2 * ext;
        }
    } else {
        p->vdc.wide_x0 = 0;
        p->vdc.wide_x1 = 256 + 2 * ext;
        /* (leaving a room kept 4:3 through a door the game draws the next part only 64 px
         * past its screen, as it comes: the few pixels beyond flicked between nothing, the
         * next column, and what was left there; black there, steadily, instead) */
        if (walls && leave_side > 0) p->vdc.wide_x1 = ext - shift + 256 + 64;
        if (walls && leave_side < 0) p->vdc.wide_x0 = ext - shift - 64;
    }
    /* Leaving through a door (or off a room's edge, $31 = 03): the game draws the part beyond
     * it into the map before and while it scrolls there, and the 512-pixel map holds that
     * part's columns where the picture shows the other side, beyond the original 256 pixels
     * away from the door (the chapel's left door: the upper right went black, the next
     * part's dark top). That side is black while leaving; the side toward the door shows the
     * part coming in. The side is the one Richter stands on as the leaving starts. */
    /* (the HUD is put again as its sprites are built: hud_at_build) */
    /* (beside a room's repeated walls no sprites are drawn: the HUD stays in the 256) */
    if (s == RONDO_PLAYING) {
        hud_last = hud_eased(walls ? 0 : ext - shift);
        place_hud(p, hud_last, walls ? 0 : ext + shift);
    }
    else vdc_set_sprite_move(0, -1, 0);
    w->active = ext;
}

void wide_off(Wide *w, Pce *p)
{
    if (shift_6f < 0) layout_6f(p);
    far_patches(p, false);
    remove_patch(p, false);
    vdc_set_sprite_fades(NULL, 0);
    if (walk_lo >= 0) {   /* (an arena made walkable: its own limits back) */
        walk_patch(p, false);
        p->wram[LIMIT_RIGHT] = (uint8_t)(walk_cam0 >> 8);
        p->wram[LIMIT_RIGHT + 1] = (uint8_t)walk_cam0;
        walk_lo = -1;
    }
    hit_patches(p, false);
    set_patches(p, WORLD_PATCHES | OBJECT_PATCHES | DRAW_PATCHES, false);
    move_generators(w, p, 0x40, 0x40);
    w->gen_r = w->gen_l = 0;
    vdc_set_wide_shift(0);
    vdc_set_side_tiles(NULL, NULL);
    vdc_set_side_real(false);
    vdc_set_side_fade(0);
    vdc_set_side_flip(0);
    vdc_set_extra_sprites(NULL, 0);
    vdc_set_map_patch(NULL);
    vdc_set_hide_corner(false);
    vdc_set_side_vram(NULL, 0, NULL, NULL);
    vdc_set_side_kept(NULL, NULL, 0, 0);
    halve_patch(p, false);
    vdc_set_map_fill(NULL);
    vdc_set_map_fill_all(false);
    vdc_set_side_sprites(0);
    vdc_set_sprite_move(0, -1, 0);
    w->shift = 0;
    w->menu_good = 0;
    place_hud(p, 0, 0);
    w->patched = false;
    w->active = 0;
    p->vdc.wide = 0;
}

int wide_report(Pce *p, char *out, int n)
{
    if (shift_6f < 0) layout_6f(p);
    int used = 0;
    for (int i = 0; i < N_PATCHES && used < n; i++) {
        const Patch *q = &patches[i];
        const uint8_t *m = at(p, q);
        const char *st = !m ? "unmapped" : !memcmp(m, q->wide, q->len) ? "in" : !memcmp(m, orig_of(q), q->len) ? "out" : "other code";
        used += snprintf(out + used, (size_t)(n - used), "  %02X:%04X %-10s %s\n", q->bank, q->off, st, q->what);
    }
    return used;
}
