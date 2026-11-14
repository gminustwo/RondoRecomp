/* See locate.h. */
#include "locate.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pce/disc.h"
#include "programs.h"

static void join(char *out, size_t n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    bool sep = l && (dir[l - 1] == '/' || dir[l - 1] == '\\');
    snprintf(out, n, "%s%s%s", dir, sep ? "" : "/", name);
}

/* the folder holding `dir` (a file, or a folder with or without a separator at the end),
 * ending in a separator */
static void parent(char *dir)
{
    size_t l = strlen(dir);
    while (l && (dir[l - 1] == '/' || dir[l - 1] == '\\')) dir[--l] = 0;
    while (l && dir[l - 1] != '/' && dir[l - 1] != '\\') dir[--l] = 0;
}

/* ---- profile and remembered path ------------------------------------------- */

/* Portable mode: a folder named user next to the game (programs.h) */
static bool portable_dir(char *out, size_t n)
{
    char user[1024];
    rondo_user_dir(user, sizeof user);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(user, &info) || info.type != SDL_PATHTYPE_DIRECTORY) return false;
    snprintf(out, n, "%s/", user);
    return true;
}

bool locate_profile_dir(char *out, size_t n)
{
    const char *env = SDL_getenv("RONDORECOMP_PROFILE");
    if (env && *env) {
        size_t l = strlen(env);
        bool sep = env[l - 1] == '/' || env[l - 1] == '\\';
        snprintf(out, n, "%s%s", env, sep ? "" : "/");
        SDL_CreateDirectory(out);
        return true;
    }
    if (portable_dir(out, n)) return true;
    char *dir = SDL_GetPrefPath("RondoRecomp", "RondoRecomp");
    if (!dir) return false;
    snprintf(out, n, "%s", dir);
    SDL_free(dir);
    return true;
}

static bool pref_file(char *out, size_t n)
{
    char dir[1024];
    if (!locate_profile_dir(dir, sizeof dir)) return false;
    snprintf(out, n, "%sdisc.txt", dir);
    return true;
}

void locate_remember(const char *cue_path)
{
    char file[1024];
    if (!pref_file(file, sizeof file)) return;
    FILE *f = fopen(file, "w");
    if (!f) return;
    fprintf(f, "%s\n", cue_path);
    fclose(f);
}

static bool remembered(char *out, size_t n)
{
    char file[1024];
    if (!pref_file(file, sizeof file)) return false;
    FILE *f = fopen(file, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (!ok) return false;
    out[strcspn(out, "\r\n")] = 0;
    return out[0] && discid_identify(out, NULL) != DISC_INVALID;
}

/* ---- search near the executable -------------------------------------------- */

typedef struct {
    char *out;
    size_t out_len;
    bool found;
    int depth;
} Search;

static SDL_EnumerationResult SDLCALL visit(void *userdata, const char *dirname, const char *fname)
{
    Search *s = userdata;
    char path[1024];
    join(path, sizeof path, dirname, fname);
    if (disc_is_image(fname)) {
        if (discid_identify(path, NULL) == DISC_RONDO) {
            snprintf(s->out, s->out_len, "%s", path);
            s->found = true;
            return SDL_ENUM_SUCCESS;
        }
        return SDL_ENUM_CONTINUE;
    }
    if (s->depth == 0 && fname[0] != '.') {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
            s->depth = 1;
            SDL_EnumerateDirectory(path, visit, s);
            s->depth = 0;
            if (s->found) return SDL_ENUM_SUCCESS;
        }
    }
    return SDL_ENUM_CONTINUE;
}

typedef struct {
    char *out;
    size_t out_len;
    bool japanese, found;
} LangSearch;

static SDL_EnumerationResult SDLCALL visit_lang(void *userdata, const char *dirname, const char *fname)
{
    LangSearch *s = userdata;
    char path[1024];
    join(path, sizeof path, dirname, fname);
    if (!disc_is_image(fname) || discid_identify(path, NULL) != DISC_RONDO ||
        (discid_language(path) == DISC_LANG_JA) != s->japanese)
        return SDL_ENUM_CONTINUE;
    snprintf(s->out, s->out_len, "%s", path);
    s->found = true;
    return SDL_ENUM_SUCCESS;
}

bool locate_installed(bool japanese, char *out, size_t out_len)
{
    const char *base = SDL_GetBasePath();
    if (!base) return false;
    char game[1024], dir[1024];
    join(game, sizeof game, base, "game");
    join(dir, sizeof dir, game, japanese ? "ja" : "en");
    LangSearch s = {out, out_len, japanese, false};
    SDL_EnumerateDirectory(dir, visit_lang, &s);
    if (!s.found) SDL_EnumerateDirectory(game, visit_lang, &s);   /* Setup up to 0.6 */
    return s.found;
}

bool locate_disc(char *out, size_t out_len)
{
    const char *base = SDL_GetBasePath();
    if (locate_installed(false, out, out_len) || locate_installed(true, out, out_len)) return true;
    if (remembered(out, out_len)) return true;
    if (!base) return false;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", base);
    /* the executable's folder, its parent and grandparent, each with one level of subfolders */
    for (int level = 0; level < 3 && dir[0]; level++) {
        Search s = {out, out_len, false, 0};
        SDL_EnumerateDirectory(dir, visit, &s);
        if (s.found) return true;
        parent(dir);
    }
    return false;
}
