/* RetroAchievements (retroachievements.org; this game: Castlevania: Rondo of Blood, game 4855),
 * through RetroAchievements' own client library, rcheevos (third_party/rcheevos).
 *
 * The player logs in once on the menu's RetroAchievements page (user name and password); the
 * server's login token is kept in settings.ini, never the password. The disc is identified
 * by RetroAchievements' PC Engine CD hash (the disc's title and boot program; the English
 * patch and the Japanese original share it), read with the runtime's own disc reader.
 * Achievements and leaderboards are checked after every frame against the console's memory
 * as RetroAchievements maps it (work RAM, CD RAM, Super System Card RAM, backup RAM).
 *
 * Hardcore mode is RetroAchievements' stricter mode: while it is on (and a game's
 * achievements are loaded), loading save states, rewind, the cheats and Richter's run are
 * off; switching it on restarts the game. The server decides whether this client's hardcore
 * unlocks count: until RetroAchievements registers RondoRecomp it records them as softcore
 * (its "Warning: Unknown Emulator" notice), so the achievements run as softcore and say so,
 * while hardcore's rules stay on. Server requests run on threads of their own (http.h), so
 * a slow connection never holds up the game. */
#pragma once

#include <SDL3/SDL.h>
#include <stdbool.h>

#include "pce/pce.h"
#include "settings.h"

/* Once at startup: the console whose memory is read, and the settings (logs in with the
 * kept token when RetroAchievements is on). */
void ach_init(Pce *pce, Settings *set);
void ach_shutdown(void);

/* The settings changed (on/off, hardcore): applies them. */
void ach_configure(void);
/* The menu's Log in: a login with this name and password (the answer comes later). */
void ach_login(const char *user, const char *password);
void ach_logout(void);

/* The machine was started (power-on or reset) with this disc: its achievements are loaded
 * (or, for the disc already loaded, reset to their start). */
void ach_game_started(const char *cue);
/* After each emulated frame. */
void ach_frame(void);
/* Once each pass of the main loop: server answers, and the periodic work while the game is
 * paused (rich presence pings). Returns true when the settings changed (a login's token,
 * a logout), so they are saved. */
bool ach_update(void);

bool ach_hardcore(void);          /* hardcore mode's rules in effect: no loading states, rewind or cheats */
bool ach_take_reset(void);        /* the library asked for a restart (hardcore was switched on) */

/* Save states: the achievements' progress (hit counts) is kept next to the state file. */
void ach_save_progress(const char *state_path);
void ach_load_progress(const char *state_path);

/* The menu page's status lines (account, game, progress). */
void ach_status(char *line0, size_t n0, char *line1, size_t n1);
bool ach_logged_in(void);
/* Unlock pop-ups, leaderboard trackers. */
void ach_draw(SDL_Renderer *ren);

/* Test (--test-achievements): the disc's hash, and the game RetroAchievements knows it as
 * (asked from the server; no login needed). Returns the game id, 0 if unknown, -1 if no
 * answer (err says why). */
int ach_test_identify(const char *cue, char hash[33], char *err, size_t errlen);
/* Test (features): an achievement of the test's own (RetroAchievements' condition syntax),
 * checked with ach_test_triggered after frames against the console's memory as
 * RetroAchievements reads it; true once it has triggered. */
bool ach_test_trigger(const char *memaddr);
bool ach_test_triggered(void);
/* Test: a sample unlock pop-up, shown fully (for a screenshot). */
void ach_test_popup(void);
