/* Updates: the latest release on GitHub (UPDATE_REPO). The game checks for one at launch
 * (Settings.check_updates) and the start screen says when there is one; installing it runs
 * Setup (rondo_setup --update), which downloads it, puts its files in place of the old ones,
 * rebuilds the game from the new SDK and hands back to the game, which restarts.
 *
 * A release has, for each system, the full package (RondoRecomp-<v>-windows-x64.zip,
 * RondoRecomp-<v>-linux-x86_64.tar.gz) and, to update, the same without the compiler
 * (...-update-zig<zig version>.zip / .tar.gz), a few MB instead of about 100. An installation
 * takes the smaller one when its compiler is the one named (RONDO_ZIG_VERSION), else the full
 * package. Tests point the check elsewhere (RONDO_UPDATE_URL: the release's JSON, as GitHub's
 * API gives it) and pretend to be an older version (RONDO_UPDATE_FROM). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define UPDATE_REPO "gminustwo/RondoRecomp"

#ifdef _WIN32
#define UPDATE_SYSTEM "windows-x64"
#define UPDATE_EXT ".zip"
#else
#define UPDATE_SYSTEM "linux-x86_64"
#define UPDATE_EXT ".tar.gz"
#endif

typedef struct UpdateInfo {
    char version[32];        /* "1.2" (the release's tag without its v) */
    char page[256];          /* the release's web page */
    char asset[160];         /* the package for this system: its name, address, size, SHA-256 */
    char url[512];
    long long size;
    char sha256[65];         /* "" when GitHub gives none */
    bool slim;               /* without the compiler (this installation's is kept) */
    char notes[4][160];      /* what is new: the release notes' first points, as plain text */
} UpdateInfo;

const char *update_api_url(void);
const char *update_current_version(void);
const char *update_user_agent(void);
/* < 0, 0, > 0 as version a is older, the same, newer than b ("1.10" is newer than "1.9") */
int update_compare(const char *a, const char *b);
/* GitHub's JSON for a release. False if it is not one (err says why); a release without a
 * package for this system leaves info->url empty. */
bool update_parse(const char *json, size_t len, UpdateInfo *info, char *err, size_t errlen);
/* Setup's progress lines ("    Downloading  42%"): true and the parts if seg is one. */
bool update_progress_line(const char *seg, char *what, size_t n, int *pct);

/* ---- the check, in the background (http.h; http_poll delivers it) ---- */
typedef enum { UPD_IDLE, UPD_CHECKING, UPD_LATEST, UPD_AVAILABLE, UPD_FAILED } UpdateState;
void update_check_start(void);
/* info: the newer release (UPD_AVAILABLE); why: what went wrong (UPD_FAILED) */
UpdateState update_state(const UpdateInfo **info, const char **why);

/* ---- installing (the game): Setup's update, followed as it goes ---- */
typedef enum { RUN_NONE, RUN_RUNNING, RUN_INSTALLED, RUN_LATEST, RUN_FAILED } UpdateRun;
/* setup: rondo_setup next to the game. False if it cannot start (err says why). */
bool update_run_start(const char *setup, char *err, size_t errlen);
/* line: its latest message; *pct: the step's progress, -1 when it has none */
UpdateRun update_run_poll(char *line, size_t n, int *pct);
