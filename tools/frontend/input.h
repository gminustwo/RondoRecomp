/* Controls. The defaults (each can be changed on the Controls page: binds.h):
 *
 *                Controller           Keyboard
 *   Move         d-pad / left stick   arrows   (fixed)
 *   Jump  (I)    A                    X
 *   Attack (II)  X                    Z
 *   Sub-weapon   B                    C        (Up + Attack)
 *   Item Crash   Y                    V        (SELECT)
 *   Backflip     RB                   A        (Jump twice; Maria: double jump)
 *   Run          hold LB              hold S   (with a direction; see game.h)
 *   Pause (RUN)  Start                Enter
 *   Menu         Back / View          F1       (and Esc, fixed)
 *   Rewind       hold LT              hold R
 *   Save state   -                    F5
 *   Load state   -                    F8
 *
 * A controller control can be a combination: hold one button and press another (Back + RB).
 * The second button then does only the combination until it is let go, and a button that
 * starts a combination does its own action when it is let go without one, if that action is
 * a press (Menu, Save / Load state), not when pressed. Held ones (game buttons, run, rewind)
 * start at once as before.
 *
 * Sub-weapon and backflip are short input sequences ("macros") the game itself reads as
 * Up + II and as a second press of I early in a jump (it needs each press held for 3+
 * frames; 4 are used). */
#pragma once

#include <SDL3/SDL.h>
#include <stdint.h>

#include "binds.h"

/* NAV_CLEAR_PAD / NAV_CLEAR_KEY: the controller's X / the Delete key (Controls page: clear the
 * row's controller button / key) */
typedef enum { NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_OK, NAV_BACK, NAV_MENU, NAV_CLEAR_PAD, NAV_CLEAR_KEY } Nav;

typedef struct MacroStep {
    uint8_t set, clear;
    int frames;
} MacroStep;

typedef struct Input {
    SDL_Gamepad *pads[8];
    uint8_t last_out;          /* what the game got last frame */
    bool prev_sub, prev_flip;
    MacroStep q[6];
    int q_len, q_pos, q_left;
    int stick_dir;             /* menu: last stick direction (edge detection) */
    Uint64 stick_next;         /* menu: auto-repeat time */
    bool pad_last;             /* a controller was used last (else the keyboard or mouse) */
    const int *bind_pad, *bind_key;   /* the controls (Settings' arrays, input_use_binds) */
    bool trig_held[2];         /* LT / RT down (any controller), for presses as buttons */
    bool trig_pressed[2];      /* ... and the event being handled pressed it */
    bool trig_released[2];     /* ... or let it go */
    /* combinations (binds.h), by button or trigger (BIND_LT / BIND_RT) */
    bool armed[BIND_CONTROLS];     /* pressed, and still a press of its own if let go now */
    bool swallowed[BIND_CONTROLS]; /* pressed as a combination's second: only that until let go */
    bool released_armed;           /* the event being handled let go of an armed one */
} Input;

void input_init(Input *in);
void input_close(Input *in);
/* Gamepads coming and going, and which kind of device is in use (every event). */
void input_device_event(Input *in, const SDL_Event *ev);
/* A controller is the device in use: one was pressed last, or one was connected (at start
 * or since) and the keyboard or mouse has not been used after it. */
bool input_pad_last(const Input *in);
/* The controls to use: ACT_COUNT controller buttons and keys (kept by the caller). */
void input_use_binds(Input *in, const int *pad, const int *key);
/* Keys held, read in place of the keyboard's (--showcase: showcase.h); NULL: the keyboard. */
void input_use_keys(const bool *keys);
/* An action's control is held / was pressed by this event (key, button or trigger). */
bool input_held(const Input *in, int act);
bool input_pressed(const Input *in, const SDL_Event *ev, int act);
/* The control an event pressed, for binding: a controller button or trigger (*is_pad) or a
 * key. False for anything else (releases, sticks, key repeats). */
bool input_control_of(const Input *in, const SDL_Event *ev, bool *is_pad, int *value);
/* A controller button or trigger the event pressed (*down) or let go. */
bool input_pad_event(const Input *in, const SDL_Event *ev, int *value, bool *down);
/* The PC Engine pad for this frame, macros applied. */
uint8_t input_game(Input *in);
/* Forget held macros, and buttons that would still count as a press when let go (menu
 * opened or closed, state loaded). */
void input_cancel_macros(Input *in);
bool input_rewind_held(const Input *in);
/* Run button held (LB / S): the frontend speeds the walk up (game_apply_run). */
bool input_run_held(const Input *in);
/* Menu navigation from an event, and from the left stick (polled each frame). */
Nav input_nav_event(const SDL_Event *ev);
Nav input_nav_stick(Input *in);
/* Test with a virtual controller: combinations, the buttons they share, menu on release. 0 if
 * all well, else the problems (the first described in err). */
int input_check(char *err, int errlen);
