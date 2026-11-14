/* Player options, kept in settings.ini in the user's pref folder. */
#pragma once

#include <stdbool.h>

#include "binds.h"

enum { ASPECT_4_3, ASPECT_SQUARE, ASPECT_STRETCH, ASPECT_COUNT };
enum { FILTER_SHARP, FILTER_NEAREST, FILTER_SMOOTH, FILTER_COUNT };
enum { SIDES_BLUR, SIDES_BLACK, SIDES_COUNT };
enum { SCALE_FIT, SCALE_WHOLE, SCALE_COUNT };
enum { EFFECT_OFF, EFFECT_SCANLINES, EFFECT_CRT, EFFECT_CURVED, EFFECT_COMPOSITE, EFFECT_COUNT };
enum { STRENGTH_LIGHT, STRENGTH_MEDIUM, STRENGTH_STRONG, STRENGTH_COUNT };
enum { SKIP_START, SKIP_AUTO, SKIP_OFF, SKIP_COUNT };
enum { LANG_EN, LANG_JA, LANG_COUNT };

typedef struct Settings {
    /* video */
    bool fullscreen;
    bool vsync;            /* show each frame on the screen's refresh (no tearing) */
    int aspect;            /* ASPECT_* */
    int scaling;           /* SCALE_*: fit the screen, or whole multiples only */
    int filter;            /* FILTER_* */
    int effect;            /* EFFECT_*: scanlines, a CRT (flat or curved), composite */
    int effect_strength;   /* STRENGTH_* */
    int sides;             /* SIDES_*: what fills a wide screen beside the picture */
    /* gameplay */
    int skip;              /* SKIP_*: story cinematics */
    bool rewind;
    int volume;            /* 0-10 */
    bool start_screen;     /* show the start screen before the game (rondo.exe --play skips it once) */
    bool eight_way;        /* Richter's whip and Maria's doves in 8 directions (moves.h) */
    bool limp_whip;        /* Richter's whip hangs while Attack is held (moves.h) */
    int slot;              /* save-state slot 1-9 */
    /* cheats */
    bool cheat_health, cheat_hearts, cheat_lives;
    bool maria_unlocked;   /* the game offers Maria (its own player change) */
    bool play_maria;       /* every stage, the Prologue too, played as Maria */
    /* language: which disc image to run */
    int language;          /* LANG_* */
    char disc_ja[1024];    /* the Japanese disc image (.cue or .chd) the player chose, if not installed by Setup */
    char disc_en[1024];    /* the same for the English (translated) disc */
    /* RetroAchievements (achievements.h): the account's name and login token (never the
     * password), whether achievements are on, and hardcore mode */
    bool ra_enabled;
    bool ra_hardcore;
    char ra_user[64];
    char ra_token[64];
    /* updates (update.h): ask GitHub for a newer release at launch */
    bool check_updates;
    /* controls (binds.h): a controller button and a key for each action */
    int bind_pad[ACT_COUNT];
    int bind_key[ACT_COUNT];
} Settings;

void settings_default(Settings *s);
void settings_load(Settings *s, const char *path);
bool settings_save(const Settings *s, const char *path);
