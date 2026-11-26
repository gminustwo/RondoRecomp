/* The release's program files, which are named differently on each system: Setup builds the
 * game next to itself, and each can start the other. */
#pragma once

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The release's version (Setup, the game's User-Agent for RetroAchievements, the packages) */
#define RONDO_VERSION "1.4"
/* The compiler the release carries (zig/, tools/fetch_zig.sh): an update without it (update.h)
 * is for installations that have this one */
#define RONDO_ZIG_VERSION "0.17.0"

/* (macOS: RondoRecomp.app's programs, see rondo_mac_app; the game is built outside it) */
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
 * RONDO_APPIMAGE, RONDO_APPDIR and RONDO_HOME, each folder ending in a slash) and in the
 * macOS app (rondo_mac_app below, which sets RONDO_APP, RONDO_APPDIR and RONDO_HOME):
 * - the tools: Setup's programs, sdk/ and zig/ (in the AppImage and the app: read-only);
 * - home: where Setup builds the game (game/, build/, the game itself).
 * Elsewhere both are the folder of the running program. */
static inline const char *rondo_env_dir(const char *name)
{
    const char *d = SDL_getenv(name);
    return d && *d ? d : NULL;
}

/* The macOS app, RondoRecomp.app (tools/mac/build.sh): Setup's programs are in
 * Contents/MacOS, and sdk/, zig/ and version.txt in Contents/Resources, the tools folder
 * (SDL_GetBasePath's in an app). Its files are signed, so the game is built in a folder of
 * the player's instead, ~/Library/Application Support/RondoRecomp/install (RONDO_HOME sets
 * another). The app's programs call this first: in the app, it names the three for them and
 * for the game they start (which runs outside the app, from home): RONDO_APP, the app itself
 * (which the updater replaces), RONDO_APPDIR and RONDO_HOME. False outside an app. */
#ifdef __APPLE__
#include <stdlib.h>   /* setenv */
static inline void rondo_setenv(const char *name, const char *value)
{
    setenv(name, value, 1);   /* for execv, and SDL's copy for its processes */
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value, true);
}
static inline bool rondo_mac_app(void)
{
    static const char tail[] = ".app/Contents/Resources/";
    const char *base = SDL_GetBasePath();
    size_t l = base ? strlen(base) : 0, t = sizeof tail - 1;
    if (l <= t || strcmp(base + l - t, tail) != 0) return false;
    char app[1024], home[1024];
    snprintf(app, sizeof app, "%.*s", (int)(l - t + 4), base);   /* .../RondoRecomp.app */
    const char *h = rondo_env_dir("RONDO_HOME"), *user = SDL_getenv("HOME");
    if (h) snprintf(home, sizeof home, "%s%s", h, h[strlen(h) - 1] == '/' ? "" : "/");
    else if (user && *user) snprintf(home, sizeof home, "%s/Library/Application Support/RondoRecomp/install/", user);
    else return false;
    SDL_CreateDirectory(home);
    rondo_setenv("RONDO_APP", app);
    rondo_setenv("RONDO_APPDIR", base);   /* this app's, even when started by another one's */
    rondo_setenv("RONDO_HOME", home);
    return true;
}
#endif
/* the macOS app this runs from or was started by, NULL if none */
static inline const char *rondo_app(void)
{
#ifdef __APPLE__
    return rondo_env_dir("RONDO_APP");
#else
    return NULL;
#endif
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
#if defined(_WIN32) || defined(__APPLE__)
    return NULL;
#else
    return rondo_env_dir("RONDO_APPIMAGE");
#endif
}

/* One of Setup's programs (SETUP_PROGRAM, SETUP_CLI_PROGRAM): in the tools folder, but in the
 * macOS app, where they are in Contents/MacOS next to Contents/Resources. */
static inline void rondo_program(char *out, size_t n, const char *name)
{
    const char *tools = rondo_tools_dir();
    size_t l = strlen(tools);
    static const char res[] = "/Contents/Resources/";
    if (l >= sizeof res - 1 && !strcmp(tools + l - (sizeof res - 1), res))
        snprintf(out, n, "%.*sMacOS/%s", (int)(l - (sizeof "Resources/" - 1)), tools, name);
    else
        snprintf(out, n, "%s%s", tools, name);
}

/* Portable mode (locate.h): the folder named user that the game keeps its profile in when it
 * is there, next to the game (on Linux's AppImage, next to the AppImage file, the game itself
 * being built in a folder of the player's; with the macOS app, in that folder, next to the
 * game); no separator at the end. */
static inline void rondo_user_dir(char *out, size_t n)
{
    const char *image = rondo_appimage(), *base = SDL_GetBasePath();
#ifdef __APPLE__
    base = rondo_home_dir();   /* (the game's folder, also when Setup in the app asks) */
#endif
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
#elif defined(__APPLE__)
    const char *h = SDL_getenv("HOME");
    if (!h || !*h) return false;
    snprintf(out, n, "%s/Library/Application Support/RondoRecomp/RondoRecomp/", h);
#else
    const char *x = SDL_getenv("XDG_DATA_HOME"), *h = SDL_getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/RondoRecomp/RondoRecomp/", x);
    else if (h && *h) snprintf(out, n, "%s/.local/share/RondoRecomp/RondoRecomp/", h);
    else return false;
#endif
    return true;
}
