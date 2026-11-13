/* --showcase SCRIPT: a scripted tour of the game, for recording a video of it. The script
 * presses and lets go of keys at set frames of the window (about 59.83 a second), as a
 * player at the keyboard would, so the menus, the game, rewind and the rest all behave as
 * they do for a player; real input is ignored meanwhile, as in the test runs.
 * --showcase-fast SCRIPT runs the same uncapped and silent (to check a script quickly).
 *
 * Commands, one a line ('#' starts a comment):
 *   wait N            the next commands N frames later
 *   press KEY...      hold keys down (SDL key names: Right, X, Return, Escape, F8, ...)
 *   release KEY...    let them go
 *   tap KEY... [N]    press, let go N frames later (default 6); the script waits for it
 *   mark TEXT         note the frame in SCRIPT.log (for captions when editing)
 *   shot FILE         a screenshot of the frame (BMP)
 *   quit
 */
#pragma once

#include <SDL3/SDL.h>
#include <stdbool.h>

bool showcase_load(const char *path, char *err, size_t errlen);
/* the keys held by the script, for SDL_GetKeyboardState's place (input.c) */
const bool *showcase_keys(void);
/* Before the frame's events are read: the frame's key presses and releases as events (with
 * the device id `which`), its marks logged. Returns false once the script has quit. A shot
 * to take after the frame is drawn goes in shot (empty if none). */
bool showcase_tick(SDL_Window *win, Uint32 which, char *shot, size_t shotlen);
