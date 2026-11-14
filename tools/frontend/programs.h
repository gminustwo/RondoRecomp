/* The release's program files, which are named differently on each system: Setup builds the
 * game next to itself, and each can start the other. */
#pragma once

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The release's version (Setup, the game's User-Agent for RetroAchievements, the packages) */
#define RONDO_VERSION "1.2"
/* The compiler the release carries (zig/, tools/fetch_zig.sh): an update without it (update.h)
 * is for installations that have this one */
#define RONDO_ZIG_VERSION "0.17.0"

#ifdef _WIN32
#define GAME_PROGRAM "rondo.exe"
#define SETUP_PROGRAM "RondoRecomp Setup.exe"
#define SETUP_CLI_PROGRAM "rondo_setup.exe"
#else
#define GAME_PROGRAM "rondo"
#define SETUP_PROGRAM "RondoRecomp Setup"
#define SETUP_CLI_PROGRAM "rondo_setup"
#endif

/* Two folders, the same one but on Linux's AppImage (tools/linux/AppRun, which sets
 * RONDO_APPIMAGE, RONDO_APPDIR and RONDO_HOME, each folder ending in a slash):
 * - the tools: Setup's programs, sdk/ and zig/ (in the AppImage: read-only);
 * - home: where Setup builds the game (game/, build/, the game itself).
 * Elsewhere both are the folder of the running program. */
static inline const char *rondo_env_dir(const char *name)
{
    const char *d = SDL_getenv(name);
    return d && *d ? d : NULL;
}
static inline const char *rondo_tools_dir(void)
{
    const char *d = rondo_env_dir("RONDO_APPDIR");
    const char *base = SDL_GetBasePath();
    return d ? d : base ? base : "./";
}
static inline const char *rondo_home_dir(void)
{
    const char *d = rondo_env_dir("RONDO_HOME");
    const char *base = SDL_GetBasePath();
    return d ? d : base ? base : "./";
}
/* the AppImage file this runs from, NULL if it does not */
static inline const char *rondo_appimage(void)
{
#ifdef _WIN32
    return NULL;
#else
    return rondo_env_dir("RONDO_APPIMAGE");
#endif
}

/* Portable mode (locate.h): the folder named user that the game keeps its profile in when it
 * is there, next to the game (on Linux's AppImage, next to the AppImage file, the game itself
 * being built in a folder of the player's); no separator at the end. */
static inline void rondo_user_dir(char *out, size_t n)
{
    const char *image = rondo_appimage(), *base = SDL_GetBasePath();
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", image ? image : base ? base : "./");
    size_t l = strlen(dir);
    if (image)
        while (l && dir[l - 1] != '/') dir[--l] = 0;   /* the AppImage's folder */
    bool sep = l && (dir[l - 1] == '/' || dir[l - 1] == '\\');
    snprintf(out, n, "%s%suser", dir, sep ? "" : "/");
}

/* The profile folder otherwise: SDL_GetPrefPath("RondoRecomp", "RondoRecomp")'s, worked out
 * here (that call also makes it), ending in a separator. */
static inline bool rondo_appdata_dir(char *out, size_t n)
{
#ifdef _WIN32
    const char *a = SDL_getenv("APPDATA");
    if (!a || !*a) return false;
    snprintf(out, n, "%s\\RondoRecomp\\RondoRecomp\\", a);
#else
    const char *x = SDL_getenv("XDG_DATA_HOME"), *h = SDL_getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/RondoRecomp/RondoRecomp/", x);
    else if (h && *h) snprintf(out, n, "%s/.local/share/RondoRecomp/RondoRecomp/", h);
    else return false;
#endif
    return true;
}
