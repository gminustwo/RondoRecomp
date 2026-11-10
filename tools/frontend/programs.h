/* The release's program files, which are named differently on each system: Setup builds the
 * game next to itself, and each can start the other. */
#pragma once

/* The release's version (Setup, the game's User-Agent for RetroAchievements, the packages) */
#define RONDO_VERSION "1.0"
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
