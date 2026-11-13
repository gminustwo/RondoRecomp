/* The release's program files, which are named differently on each system: Setup builds the
 * game next to itself, and each can start the other. */
#pragma once

#include <SDL3/SDL.h>

/* The release's version (Setup, the game's User-Agent for RetroAchievements, the packages) */
#define RONDO_VERSION "1.1"
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
