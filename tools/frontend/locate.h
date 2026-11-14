/* Finding the user's disc image so rondo.exe can start without arguments. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "pce/discid.h"

/* A disc installed by Setup (game/en, then game/ja next to the executable), else the
 * remembered path (if it still identifies as a PC Engine CD), else a search of the folders
 * around the executable for a recognised disc. */
bool locate_disc(char *out, size_t out_len);

/* The disc Setup installed for a language: game/ja (japanese) or game/en next to the
 * executable, or the single disc Setup up to 0.6 put straight into game/, if it is in that
 * language. */
bool locate_installed(bool japanese, char *out, size_t out_len);

void locate_remember(const char *cue_path);

/* The player's profile folder (settings.ini, saves, states, the remembered disc), ending in a
 * separator: the folder in the RONDORECOMP_PROFILE environment variable (a separate profile,
 * e.g. to try the game as a new player; Setup and rondo.exe started from each other inherit
 * it), else a folder named user next to the game if there is one (portable mode; on Linux's
 * AppImage, next to the AppImage), else %APPDATA%\RondoRecomp\RondoRecomp
 * (~/.local/share/RondoRecomp/RondoRecomp on Linux). */
bool locate_profile_dir(char *out, size_t n);
