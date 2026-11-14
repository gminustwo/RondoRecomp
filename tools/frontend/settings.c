/* See settings.h. A plain key = value file; unknown keys are ignored. */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void settings_default(Settings *s)
{
    memset(s, 0, sizeof *s);
    s->aspect = ASPECT_4_3;
    s->scaling = SCALE_FIT;
    s->filter = FILTER_SHARP;
    s->effect = EFFECT_OFF;
    s->effect_strength = STRENGTH_MEDIUM;
    s->sides = SIDES_BLUR;
    s->vsync = true;
    s->skip = SKIP_START;
    s->rewind = true;
    s->volume = 10;
    s->slot = 1;
    s->start_screen = true;
    s->language = LANG_EN;
    s->ra_enabled = true;
    s->check_updates = true;
    bind_defaults(s->bind_pad, s->bind_key);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void settings_load(Settings *s, const char *path)
{
    settings_default(s);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        while (*key == ' ') key++;
        for (char *e = key + strlen(key); e > key && e[-1] == ' '; ) *--e = 0;
        while (*val == ' ') val++;
        int n = atoi(val);
        if (!strcmp(key, "fullscreen")) s->fullscreen = n != 0;
        else if (!strcmp(key, "vsync")) s->vsync = n != 0;
        else if (!strcmp(key, "aspect")) s->aspect = clampi(n, 0, ASPECT_COUNT - 1);
        else if (!strcmp(key, "scaling")) s->scaling = clampi(n, 0, SCALE_COUNT - 1);
        else if (!strcmp(key, "filter")) s->filter = clampi(n, 0, FILTER_COUNT - 1);
        else if (!strcmp(key, "screen_effect")) s->effect = clampi(n, 0, EFFECT_COUNT - 1);
        else if (!strcmp(key, "effect_strength")) s->effect_strength = clampi(n, 0, STRENGTH_COUNT - 1);
        else if (!strcmp(key, "sides")) s->sides = clampi(n, 0, SIDES_COUNT - 1);
        else if (!strcmp(key, "skip_cinematics")) s->skip = clampi(n, 0, SKIP_COUNT - 1);
        else if (!strcmp(key, "rewind")) s->rewind = n != 0;
        else if (!strcmp(key, "volume")) s->volume = clampi(n, 0, 10);
        else if (!strcmp(key, "slot")) s->slot = clampi(n, 1, 9);
        else if (!strcmp(key, "cheat_health")) s->cheat_health = n != 0;
        else if (!strcmp(key, "cheat_hearts")) s->cheat_hearts = n != 0;
        else if (!strcmp(key, "cheat_lives")) s->cheat_lives = n != 0;
        else if (!strcmp(key, "maria_unlocked")) s->maria_unlocked = n != 0;
        else if (!strcmp(key, "play_maria")) s->play_maria = n != 0;
        else if (!strcmp(key, "language")) s->language = clampi(n, 0, LANG_COUNT - 1);
        else if (!strcmp(key, "disc_ja")) snprintf(s->disc_ja, sizeof s->disc_ja, "%s", val);
        else if (!strcmp(key, "start_screen")) s->start_screen = n != 0;
        else if (!strcmp(key, "eight_way")) s->eight_way = n != 0;
        else if (!strcmp(key, "limp_whip")) s->limp_whip = n != 0;
        else if (!strcmp(key, "disc_en")) snprintf(s->disc_en, sizeof s->disc_en, "%s", val);
        else if (!strcmp(key, "retroachievements")) s->ra_enabled = n != 0;
        else if (!strcmp(key, "ra_hardcore")) s->ra_hardcore = n != 0;
        else if (!strcmp(key, "ra_user")) snprintf(s->ra_user, sizeof s->ra_user, "%s", val);
        else if (!strcmp(key, "ra_token")) snprintf(s->ra_token, sizeof s->ra_token, "%s", val);
        else if (!strcmp(key, "check_updates")) s->check_updates = n != 0;
        else if (!strncmp(key, "bind_pad_", 9) || !strncmp(key, "bind_key_", 9)) {
            bool pad = key[5] == 'p';
            for (int a = 0; a < ACT_COUNT; a++)
                if (!strcmp(key + 9, bind_action(a)->key)) {
                    if (pad) s->bind_pad[a] = bind_pad_valid(n) ? n : BIND_NONE;
                    else s->bind_key[a] = n > 0 && n < 512 ? n : 0;
                }
        }
    }
    fclose(f);
}

bool settings_save(const Settings *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f,
            "# Rondo Recomp options (also changed from the in-game menu: F1 / Back)\n"
            "fullscreen = %d\nvsync = %d\naspect = %d\nscaling = %d\nfilter = %d\nscreen_effect = %d\n"
            "effect_strength = %d\nsides = %d\n"
            "skip_cinematics = %d\nrewind = %d\nvolume = %d\nslot = %d\nstart_screen = %d\n"
            "eight_way = %d\nlimp_whip = %d\n"
            "cheat_health = %d\ncheat_hearts = %d\ncheat_lives = %d\n"
            "maria_unlocked = %d\nplay_maria = %d\n"
            "language = %d\ndisc_ja = %s\ndisc_en = %s\n",
            s->fullscreen, s->vsync, s->aspect, s->scaling, s->filter, s->effect, s->effect_strength, s->sides,
            s->skip, s->rewind, s->volume, s->slot, s->start_screen,
            s->eight_way, s->limp_whip,
            s->cheat_health, s->cheat_hearts, s->cheat_lives,
            s->maria_unlocked, s->play_maria,
            s->language, s->disc_ja, s->disc_en);
    fprintf(f, "# RetroAchievements: the account and its login token (not the password; Log out removes it)\n"
               "retroachievements = %d\nra_hardcore = %d\nra_user = %s\nra_token = %s\n",
            s->ra_enabled, s->ra_hardcore, s->ra_user, s->ra_token);
    fprintf(f, "# updates: ask GitHub for a newer release at launch (the start screen's Updates page)\n"
               "check_updates = %d\n",
            s->check_updates);
    fprintf(f, "# controls: controller buttons (SDL gamepad button numbers; 100 LT, 101 RT, -1 none) and keys\n"
               "# (SDL scancodes; 0 none), changed from the Controls page\n");
    for (int a = 0; a < ACT_COUNT; a++)
        fprintf(f, "bind_pad_%s = %d\nbind_key_%s = %d\n", bind_action(a)->key, s->bind_pad[a], bind_action(a)->key,
                s->bind_key[a]);
    return fclose(f) == 0;
}
