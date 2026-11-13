/* What each control does: the actions the player can rebind (Controls page), each with a
 * controller button and a key. Moving (d-pad, left stick, arrows), the menus' own keys,
 * F6 / F7 (state slot), F11 (fullscreen) and Esc (menu) are fixed; keys of the player's own
 * can move as well (the Move actions: WASD, say). */
#pragma once

#include <stdbool.h>

enum {
    ACT_JUMP, ACT_ATTACK, ACT_SUB, ACT_CRASH, ACT_FLIP, ACT_RUN, ACT_PAUSE, ACT_MENU, ACT_REWIND,
    ACT_SAVE, ACT_LOAD, ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_COUNT
};

/* A controller binding: an SDL_GamepadButton, BIND_LT / BIND_RT for the triggers, a
 * combination of two of those (hold the first, press the second: Back + RB), or BIND_NONE.
 * A key binding: an SDL_Scancode, 0 for none. */
#define BIND_NONE (-1)
#define BIND_LT 100
#define BIND_RT 101
#define BIND_CONTROLS (BIND_RT + 1)   /* single buttons and triggers are below this */
#define BIND_COMBO(hold, press) ((((hold) + 1) << 8) | (press))
#define BIND_PRESS(b) ((b) < 0 ? BIND_NONE : (b) & 0xff)          /* the button pressed */
#define BIND_HOLD(b) ((b) < 0 ? BIND_NONE : ((b) >> 8) - 1)        /* held first, or BIND_NONE */

typedef struct ActionInfo {
    const char *name;      /* as on the Controls page */
    const char *key;       /* in settings.ini: bind_pad_<key>, bind_key_<key> */
    int pad, kbd;          /* the defaults */
    bool clearable;        /* may be left without a control (the game can be played without it) */
    bool keys_only;        /* a key in addition to the arrows (Move); a controller moves with
                            * its d-pad and left stick */
} ActionInfo;

const ActionInfo *bind_action(int act);
void bind_defaults(int pad[ACT_COUNT], int key[ACT_COUNT]);
const char *bind_pad_name(int pad);   /* "A", "LB", "LT", "Back + RB", ... ; "-" for none */
const char *bind_key_name(int key);   /* "Enter", "F5", "X", ... ; "-" for none */
/* Controls kept for something else: the d-pad (moving), the Guide button; the arrows
 * (moving), Esc (menu), F6 / F7 (state slot), F11 (fullscreen). */
bool bind_pad_reserved(int pad);   /* a combination: either of its buttons */
bool bind_key_reserved(int key);
/* A value a controller binding can have (settings.ini is checked with it). */
bool bind_pad_valid(int pad);
/* Give action `act` a controller button (is_pad) or key. The action that had it gets this
 * one's old control (they swap); its own control again leaves it. Returns a line for the menu
 * to show, or NULL. */
const char *bind_assign(int pad[ACT_COUNT], int key[ACT_COUNT], int act, bool is_pad, int value);
/* Leave action `act` without a controller button (is_pad) or key, where the game can do
 * without it. Returns a line for the menu to show. */
const char *bind_clear(int pad[ACT_COUNT], int key[ACT_COUNT], int act, bool is_pad);
/* Test: assignments, swaps, clearing and names behave. 0 if all well, else the problems
 * (the first described in err). */
int bind_check(char *err, int errlen);
