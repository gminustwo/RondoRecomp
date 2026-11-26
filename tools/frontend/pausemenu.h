/* A CONTINUE / SAVE & END / PLAYER CHANGE menu (the last with Maria unlocked) on the
 * game's own pause screen (RUN pauses), drawn into the
 * game's picture with the game's own letters and heart cursor (the GAME OVER continue
 * screen's, read from the player's disc: none of it is in this program). SAVE & END saves
 * as that screen's does (game_save_progress) and leaves to the start screen. No SDL here,
 * so the headless tool runs it too. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pce/pce.h"

typedef struct PauseMenu {
    int paused_frames;     /* frames the game has been paused */
    bool active;           /* the menu is up */
    int sel;               /* the chosen line (0 CONTINUE, the default) */
    uint8_t prev;          /* the pad last frame (presses are new buttons) */
    bool want_exit;        /* SAVE & END chosen: the caller saves and leaves */
    int change;            /* PLAYER CHANGE under way: frames since the game was given it */
} PauseMenu;

/* Before every frame: the player's pad in, the game's out (while the menu is up the menu
 * takes the d-pad and buttons; CONTINUE gives the game RUN, which ends its pause). live:
 * false for test runs, which only send the game what they mean to. */
uint8_t pause_menu_pad(PauseMenu *m, Pce *p, uint8_t pad, bool live);

/* After every frame: the menu drawn into the picture (p->vdc.fb). false when it is up but
 * the game's letters could not be read from the disc (the caller says it another way). */
bool pause_menu_draw(PauseMenu *m, Pce *p);

/* The game is on its pause screen. */
bool pause_menu_game_paused(const Pce *p);
