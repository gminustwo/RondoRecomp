/* Portable mode from Setup: the game keeps its profile (settings.ini, saves, save states) in
 * a folder named user next to it when there is one (../frontend/locate.h, programs.h's
 * rondo_user_dir), instead of %APPDATA%\RondoRecomp\RondoRecomp (~/.local/share/RondoRecomp/
 * RondoRecomp on Linux). Setup's checkbox and rondo_setup --portable on|off make or retire
 * that folder. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* the user folder exists: the game is portable */
bool portable_on(void);
/* where the profile is otherwise (and, there, whether there is anything in it) */
bool portable_appdata(char *out, size_t n, bool *has_files);
/* Makes the user folder; `copy` copies the profile the game has had so far into it (it is
 * left where it was). False with g_err set on failure. */
bool portable_enable(bool copy);
/* An empty user folder is removed. One with something in it is copied back to the other
 * profile folder (its files there are replaced) and kept, renamed user.old (or user.old2
 * ...). False with g_err set on failure; nothing is renamed then. */
bool portable_disable(void);
