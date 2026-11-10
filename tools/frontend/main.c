/* Windowed frontend (SDL3): start screen, video, input, audio, menu, save states, rewind,
 * cheats.
 *
 * usage: rondo [game.cue] [--play] [--autotest] [--autotest-features DIR] [--autotest-start]
 *              [--test-achievements] [--autotest-update DIR]
 *   With no disc argument it uses the disc Setup installed, then the remembered disc, then
 *   searches the folders around the executable, then asks once with a file picker and
 *   remembers the choice. It opens on the start screen (Play, language, options; the game
 *   waits until Play) unless --play is given or the option is off.
 *
 * Controls: see input.h (also on the menu's Controls page). F1 / Esc / the controller's
 * Back button open the menu; options are kept in settings.ini in the pref folder
 * (%APPDATA%\RondoRecomp\RondoRecomp on Windows) with bram.bin and states\.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "achievements.h"
#include "capture_script.h"
#include "game.h"
#include "input.h"
#include "locate.h"
#include "menu.h"
#include "pce/pce.h"
#include "pce/state.h"
#include "recomp/recomp.h"
#include "settings.h"
#include "theme.h"
#include "video.h"
#include "prompt.h"
#include "programs.h"
#include "update.h"

#define FRAME_RATE (7159090.0 / (455.0 * 263.0))   /* 59.826 Hz */
#define AUDIO_RATE 44100

/* ---- on-screen messages ------------------------------------------------------------ */

static char osd_text[160];
static Uint64 osd_until;

static void show_osd(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(osd_text, sizeof osd_text, fmt, ap);
    va_end(ap);
    osd_until = SDL_GetTicks() + 2500;
}

/* A one-line notice on a plaque with pointed ends (theme.h): top centre (clear of the HUD
 * on the left), or bottom right. */
static void draw_banner(SDL_Renderer *ren, const char *text, bool bottom_right)
{
    int ww, wh;
    SDL_GetRenderOutputSize(ren, &ww, &wh);
    float u = th_unit(ww, wh), px = 14 * u, track = 1.0f * u, h = 30 * u, m = 14 * u;
    float w = th_measure(ren, TH_FONT_UI, px, text, track) + h + 28 * u;
    float x = bottom_right ? (float)ww - w - m : ((float)ww - w) * 0.5f, y = bottom_right ? (float)wh - h - m : m;
    th_plaque(ren, x, y, x + w, y + h, u, 1.0f);
    th_text(ren, TH_FONT_UI, px, x + w * 0.5f, y + h * 0.5f, TH_TEXT, text, 0.5f, 0.5f, track, TH_NONE);
}

/* ---- choosing the disc image ------------------------------------------------------- */

typedef struct {
    SDL_AtomicInt done;
    char path[1024];
} Pick;

static void SDLCALL on_pick(void *userdata, const char *const *files, int filter)
{
    (void)filter;
    Pick *p = userdata;
    if (files && files[0]) snprintf(p->path, sizeof p->path, "%s", files[0]);
    else p->path[0] = 0;
    SDL_SetAtomicInt(&p->done, 1);
}

static bool pick_file(SDL_Window *win, SDL_Renderer *ren, char *out, size_t n)
{
    static Pick pick;
    static const SDL_DialogFileFilter filters[] = {{"PC Engine CD image (*.cue)", "cue"}};
    SDL_SetAtomicInt(&pick.done, 0);
    pick.path[0] = 0;
    SDL_ShowOpenFileDialog(on_pick, &pick, win, filters, 1, NULL, false);
    while (!SDL_GetAtomicInt(&pick.done)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT) return false;
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
    if (!pick.path[0]) return false;
    snprintf(out, n, "%s", pick.path);
    return true;
}

/* Command line, then the remembered path, then a search near the executable, then ask. */
static bool choose_disc(SDL_Window *win, SDL_Renderer *ren, const char *arg, char *out, size_t n)
{
    if (arg) {
        snprintf(out, n, "%s", arg);
        if (discid_identify(out, NULL) != DISC_INVALID) { locate_remember(out); return true; }
    } else if (locate_disc(out, n)) {
        locate_remember(out);
        return true;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Rondo Recomp",
                             "Select the .cue file of your Castlevania: Rondo of Blood disc image.\n"
                             "It will be remembered for next time.", win);
    for (;;) {
        if (!pick_file(win, ren, out, n)) return false;
        DiscKind kind = discid_identify(out, NULL);
        if (kind == DISC_RONDO) { locate_remember(out); return true; }
        if (kind == DISC_PCE_UNKNOWN) {
            const SDL_MessageBoxButtonData buttons[] = {
                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Use it"},
                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Choose another"},
            };
            const SDL_MessageBoxData box = {
                SDL_MESSAGEBOX_WARNING, win, "Rondo Recomp",
                "This is a PC Engine CD image, but not a release of Rondo of Blood this build "
                "recognises. Use it anyway?", 2, buttons, NULL};
            int id = 0;
            SDL_ShowMessageBox(&box, &id);
            if (id == 1) { locate_remember(out); return true; }
            continue;
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rondo Recomp",
                                 "That file could not be read as a PC Engine CD image.\n"
                                 "Choose the .cue file of the disc image.", win);
    }
}

/* ---- the running game ---------------------------------------------------------------- */

static Pce pce;
static Rewind rw;
static bool have_rewind;
static GameWatch gw;
static char pref_dir[1024];
static bool no_save_files;   /* test runs: fresh backup RAM, nothing written to bram.bin */
static TitlePrompt prompt;   /* the title's PUSH RUN BUTTON! as this PC's button (pad set per frame) */

static void run_frame(void)
{
    prompt_before_frame(&prompt, &pce);
    pce_run_frame(&pce);
    prompt_after_frame(&prompt, &pce);
}

typedef struct Session {
    char disc[1024];
    uint64_t disc_id;
    int language;
    char bram_path[1100];
} Session;

static void flush_bram(Session *ss)
{
    if (pce.bram_dirty && ss->bram_path[0]) SDL_SaveFile(ss->bram_path, pce.bram, 0x800);
    pce.bram_dirty = false;
}

/* LANG_JA for the original Japanese disc, LANG_EN for a translated one. */
static int disc_language(const char *cue) { return discid_language(cue) == DISC_LANG_JA ? LANG_JA : LANG_EN; }

/* The disc for a language: installed by Setup (game/en, game/ja), chosen for it earlier in
 * the menu, or the main disc if it is in that language. */
static bool disc_for(int lang, const Settings *set, const char *main_disc, char *out, size_t n)
{
    if (locate_installed(lang == LANG_JA, out, n)) return true;
    const char *chosen = lang == LANG_JA ? set->disc_ja : set->disc_en;
    if (chosen[0] && discid_identify(chosen, NULL) != DISC_INVALID && disc_language(chosen) == lang) {
        snprintf(out, n, "%s", chosen);
        return true;
    }
    if (main_disc[0] && disc_language(main_disc) == lang) {
        snprintf(out, n, "%s", main_disc);
        return true;
    }
    return false;
}

/* (Re)start the machine with a disc; the session's language is the disc's. The disc is
 * checked first, so a failure leaves the running game alone. */
static bool start_game(Session *ss, const char *disc_arg, char *err, int err_len)
{
    /* a copy: callers pass ss->disc itself (a restart), which is rewritten below; copying a
     * string onto itself is undefined (glibc's snprintf empties it) */
    char disc[1024];
    snprintf(disc, sizeof disc, "%s", disc_arg);
    if (discid_identify(disc, NULL) == DISC_INVALID) {
        snprintf(err, (size_t)err_len, "%s could not be read as a PC Engine CD image", disc);
        return false;
    }
    flush_bram(ss);
    if (!pce_init(&pce, disc, err, err_len) || !pce_boot(&pce, err, err_len)) return false;
    snprintf(ss->disc, sizeof ss->disc, "%s", disc);
    int language = disc_language(disc);
    ss->language = language;
    ss->disc_id = 0;
    discid_identify(disc, &ss->disc_id);
    /* in-game saves (backup RAM), one file per language */
    snprintf(ss->bram_path, sizeof ss->bram_path, "%sbram%s.bin", pref_dir, language == LANG_JA ? "-ja" : "");
    if (no_save_files) ss->bram_path[0] = 0;
    size_t n = 0;
    void *data = ss->bram_path[0] ? SDL_LoadFile(ss->bram_path, &n) : NULL;
    if (data && n >= 0x800) memcpy(pce.bram, data, 0x800);
    SDL_free(data);
    pce.bram_dirty = false;
    if (have_rewind) rewind_reset(&rw, &pce);
    game_watch_reset(&gw, &pce);
    ach_game_started(disc);
    return true;
}

/* The Language option: restart with set->language's disc (each language is its own disc: the
 * original Japanese one, or the English translation). Without one installed or chosen
 * before, `ask` lets the player pick its .cue. The outcome goes to status; set->language
 * ends up as the running disc's. */
static void switch_language(Session *ss, Settings *set, const char *main_disc, bool ask, SDL_Window *win,
                            SDL_Renderer *ren, char *status, size_t status_len)
{
    int want = set->language;
    char disc[1024], err[256];
    bool have = disc_for(want, set, main_disc, disc, sizeof disc), picked = false;
    const char *problem = want == LANG_JA ? "The Japanese original is not installed"
                                          : "The English patch is not installed";
    if (!have && ask) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Rondo Recomp",
            want == LANG_JA
                ? "The Japanese original is its own disc image (Akumajou Dracula X: Chi no Rondo).\n"
                  "Select its .cue file; it will be remembered.\n"
                  "(Running Setup with its zip installs it next to the English patch.)"
                : "The English patch is its own disc image (the fan translation).\nSelect its .cue file; "
                  "it will be remembered.\n(Running Setup with its zip installs it next to the Japanese "
                  "original.)", win);
        if (!pick_file(win, ren, disc, sizeof disc)) problem = "No disc chosen";
        else if (discid_identify(disc, NULL) == DISC_INVALID) problem = "That is not a PC Engine CD image";
        else if (disc_language(disc) != want)
            problem = want == LANG_JA ? "That is the English patch" : "That is the Japanese original";
        else have = picked = true;
    }
    if (have && start_game(ss, disc, err, sizeof err)) {
        if (picked && want == LANG_JA) snprintf(set->disc_ja, sizeof set->disc_ja, "%s", disc);
        if (picked && want == LANG_EN) snprintf(set->disc_en, sizeof set->disc_en, "%s", disc);
        snprintf(status, status_len, "Restarted: the %s", want == LANG_JA ? "Japanese original" : "English patch");
    } else {
        snprintf(status, status_len, "%.100s", have ? err : problem);
    }
    set->language = ss->language;
}

/* The start screen's lines under the title: the disc being played, the other language. */
static void start_screen_info(Menu *m, const Session *ss, const Settings *set, const char *main_disc)
{
    char other[1024];
    int o = ss->language == LANG_JA ? LANG_EN : LANG_JA;
    bool have = disc_for(o, set, main_disc, other, sizeof other);
    snprintf(m->info[0], sizeof m->info[0], "%s  -  %s %s", ss->language == LANG_JA ? "Japanese original" : "English patch",
             o == LANG_JA ? "Japanese original" : "English patch", have ? "also installed" : "not installed");
    snprintf(m->info[1], sizeof m->info[1], "%s", recomp_build_id);
}

/* Start screen: RondoRecomp Setup (next to rondo.exe) adds a disc or rebuilds. */
static bool open_setup(char *status, size_t n)
{
    char exe[1100];
    snprintf(exe, sizeof exe, "%s" SETUP_PROGRAM, rondo_tools_dir());   /* programs.h: the AppImage's */
    if (!SDL_GetPathInfo(exe, NULL)) {
        snprintf(status, n, SETUP_PROGRAM " is not next to " GAME_PROGRAM);
        return false;
    }
    const char *args[] = {exe, NULL};
    SDL_Process *proc = SDL_CreateProcess(args, false);
    if (!proc) {
        snprintf(status, n, "Cannot start Setup: %.80s", SDL_GetError());
        return false;
    }
    SDL_DestroyProcess(proc);      /* it keeps running; rondo.exe quits so it can be rebuilt */
    return true;
}

/* ---- updates (update.h) -------------------------------------------------------------- */

static SDL_EnumerationResult SDLCALL remove_visit(void *ctx, const char *dir, const char *name)
{
    (void)ctx;
    char path[1200];
    snprintf(path, sizeof path, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != '\\' ? "/" : "", name);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(path, remove_visit, NULL);
    SDL_RemovePath(path);
    return SDL_ENUM_CONTINUE;
}

/* What an update left next to the game (the old files that were in use then): removed now,
 * but for any still in use. */
static void update_leftovers(void)
{
    const char *base = SDL_GetBasePath();
    char dir[1100];
    snprintf(dir, sizeof dir, "%supdate", base ? base : "");
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(dir, &info) || info.type != SDL_PATHTYPE_DIRECTORY) return;
    SDL_EnumerateDirectory(dir, remove_visit, NULL);
    SDL_RemovePath(dir);
}

/* The Updates page's lines, the start screen's notice, and an update being installed
 * (Setup): its progress. True once it is installed: the game restarts. */
static bool update_tick(Menu *m)
{
    const UpdateInfo *info;
    const char *why;
    UpdateState st = update_state(&info, &why);
    for (int i = 0; i < 5; i++) m->upd[i][0] = 0;
    m->notice[0] = m->upd_badge[0] = 0;
    if (m->updating) {
        char line[160];
        int pct;
        UpdateRun r = update_run_poll(line, sizeof line, &pct);
        m->upd_pct = pct;
        snprintf(m->upd[0], sizeof m->upd[0], "Updating to Rondo Recomp %s", info ? info->version : "");
        if (pct >= 0) snprintf(m->upd[1], sizeof m->upd[1], "%.120s  %d%%", line, pct);
        else snprintf(m->upd[1], sizeof m->upd[1], "%s", line);
        snprintf(m->upd[3], sizeof m->upd[3], "Setup downloads it, puts it in place and rebuilds the game.");
        snprintf(m->upd[4], sizeof m->upd[4], "The game restarts when it is done (about a minute).");
        if (r == RUN_INSTALLED) {
            snprintf(m->upd[1], sizeof m->upd[1], "Installed: restarting...");
            return true;
        }
        if (r == RUN_LATEST || r == RUN_FAILED) {
            m->updating = false;
            m->upd_pct = -1;
            snprintf(m->status, sizeof m->status, "%s", line);
        }
        return false;
    }
    snprintf(m->upd[0], sizeof m->upd[0], "This is Rondo Recomp %s.", update_current_version());
    switch (st) {
    case UPD_IDLE: snprintf(m->upd[1], sizeof m->upd[1], "Check now asks GitHub for a newer version."); break;
    case UPD_CHECKING: snprintf(m->upd[1], sizeof m->upd[1], "Asking GitHub for the latest version..."); break;
    case UPD_LATEST: snprintf(m->upd[1], sizeof m->upd[1], "This is the latest version."); break;
    case UPD_FAILED: snprintf(m->upd[1], sizeof m->upd[1], "Could not check for updates: %.110s", why); break;
    case UPD_AVAILABLE:
        snprintf(m->upd[1], sizeof m->upd[1], "Version %s is available: %.1f MB to download.", info->version,
                 (double)info->size / 1e6);
        for (int i = 0; i < 3; i++)
            if (info->notes[i][0]) snprintf(m->upd[2 + i], sizeof m->upd[0], "- %.150s", info->notes[i]);
        snprintf(m->notice, sizeof m->notice, "Version %s is available  -  see Updates", info->version);
        snprintf(m->upd_badge, sizeof m->upd_badge, "%s available", info->version);
        break;
    }
    return false;
}

static void update_install(Menu *m)
{
    const UpdateInfo *info;
    UpdateState st = update_state(&info, NULL);
    if (!menu_on_start_screen(m)) {
        snprintf(m->status, sizeof m->status, "Leave the game for the start screen first.");
        return;
    }
    if (st != UPD_AVAILABLE) {
        snprintf(m->status, sizeof m->status, st == UPD_CHECKING ? "Still asking GitHub: a moment."
                                                                 : "There is no newer version to install.");
        return;
    }
    char setup[1100], err[200];
    snprintf(setup, sizeof setup, "%s" SETUP_CLI_PROGRAM, rondo_tools_dir());
    if (!SDL_GetPathInfo(setup, NULL)) {
        snprintf(m->status, sizeof m->status, SETUP_CLI_PROGRAM " is not next to " GAME_PROGRAM ".");
        return;
    }
    if (!update_run_start(setup, err, sizeof err)) {
        snprintf(m->status, sizeof m->status, "Cannot start Setup: %.100s", err);
        return;
    }
    m->updating = true;
    m->upd_pct = -1;
    m->status[0] = 0;
}

/* After an update: the new game in place of this one (args: its arguments, NULL-terminated). */
static void relaunch(const char *const *args)
{
    const char *base = SDL_GetBasePath();
    static char exe[1100];
    /* from the AppImage: the new AppImage, which starts the game it built */
    if (rondo_appimage()) snprintf(exe, sizeof exe, "%s", rondo_appimage());
    else snprintf(exe, sizeof exe, "%s" GAME_PROGRAM, base ? base : "");
    const char *argv[8] = {exe};
    for (int i = 0; args[i] && i < 6; i++) argv[i + 1] = args[i];
#ifdef _WIN32
    SDL_Process *p = SDL_CreateProcess(argv, false);   /* a windowed program: no console */
    if (p) SDL_DestroyProcess(p);
#else
    SDL_Quit();
    execv(exe, (char *const *)argv);                   /* the same process: Steam keeps track of it */
#endif
}

static void state_path(char *out, size_t n, const Session *ss, int slot)
{
    snprintf(out, n, "%sstates/slot%d%s.state", pref_dir, slot, ss->language == LANG_JA ? "-ja" : "");
}

/* Input events a test run sends itself carry this device id; test runs drop all other input
 * events (keyboard, mouse, controller), whatever window has the focus. */
#define TEST_INPUT 0x52524543u

static bool test_input(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        return ev->key.which == TEST_INPUT;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        return ev->button.which == TEST_INPUT;
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_WHEEL: case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return false;
    default:
        return true;   /* window, device and other events */
    }
}

/* VSync: each frame is shown on the screen's refresh, never part-way through one (no
 * tearing). The game's own timing (59.826 Hz, the frame timer in the main loop) still sets
 * the pace, so on a 60 Hz screen one frame in about 350 is shown twice; on a variable
 * refresh screen none. */
static void set_vsync(SDL_Renderer *ren, bool on)
{
    if (!SDL_SetRenderVSync(ren, on ? 1 : SDL_RENDERER_VSYNC_DISABLED) && on)
        SDL_SetRenderVSync(ren, SDL_RENDERER_VSYNC_ADAPTIVE);
}

static void screenshot(SDL_Renderer *ren, const char *path)
{
    SDL_Surface *s = SDL_RenderReadPixels(ren, NULL);
    if (s) {
        SDL_SaveBMP(s, path);
        SDL_DestroySurface(s);
    }
}

/* features test: a Controls page count fed presses. Each step is a controller button (b >= 0)
 * or a key (-scancode), pressed or let go. */
typedef struct CountStep { int b; bool down; } CountStep;

static void count_feed(Menu *m, Settings *s, const Input *in, const CountStep *steps, int n)
{
    for (int i = 0; i < n; i++) {
        SDL_Event e;
        SDL_zero(e);
        if (steps[i].b >= 0) {
            e.type = steps[i].down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
            e.gbutton.button = (Uint8)steps[i].b;
            e.gbutton.down = steps[i].down;
        } else {
            e.type = steps[i].down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.scancode = (SDL_Scancode)-steps[i].b;
            e.key.down = steps[i].down;
        }
        menu_capture(m, s, in, &e);
    }
}

static void count_end(Menu *m, Settings *s)
{
    m->capture_until = 0;                  /* the count is over */
    menu_update(m, s);
}

int main(int argc, char **argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /* --autotest: save/load a state and rewind without user input, then quit.
     * --autotest-features DIR: play the capture script with cheats and automatic cinematic
     * skipping, take screenshots of the video modes and menu into DIR, then quit. */
    bool autotest = false, play_now = false, autotest_start = false, test_ach = false;
    const char *features_dir = NULL, *disc_arg = NULL, *update_dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--autotest")) autotest = true;
        else if (!strcmp(argv[i], "--play")) play_now = true;
        else if (!strcmp(argv[i], "--autotest-features") && i + 1 < argc) features_dir = argv[++i];
        else if (!strcmp(argv[i], "--autotest-start")) autotest_start = true;
        else if (!strcmp(argv[i], "--test-achievements")) test_ach = true;   /* the discs' hashes, asked of the server */
        /* --autotest-update DIR: the start screen's notice, the Updates page, Download and
         * install (with RONDO_UPDATE_URL / RONDO_UPDATE_FROM), then the new game's --autotest */
        else if (!strcmp(argv[i], "--autotest-update") && i + 1 < argc) update_dir = argv[++i];
        else disc_arg = argv[i];
    }
    char autotest_log[2048] = "";
#define TLOG(...) snprintf(autotest_log + strlen(autotest_log), sizeof autotest_log - strlen(autotest_log), __VA_ARGS__)

    locate_profile_dir(pref_dir, sizeof pref_dir);
    char settings_path[1100], states_dir[1100];
    snprintf(settings_path, sizeof settings_path, "%ssettings.ini", pref_dir);
    if (autotest || features_dir || autotest_start || test_ach || update_dir) {
        /* test runs keep their state files apart from the player's */
        no_save_files = true;
        size_t l = strlen(pref_dir);
        snprintf(pref_dir + l, sizeof pref_dir - l, "test/");
        SDL_CreateDirectory(pref_dir);
    }
    snprintf(states_dir, sizeof states_dir, "%sstates", pref_dir);
    SDL_CreateDirectory(states_dir);
    Settings set;
    settings_load(&set, settings_path);
    bool test_mode = autotest || features_dir || autotest_start || test_ach || update_dir;
    if (test_mode) {   /* fixed options; nothing is saved */
        settings_default(&set);
        if (features_dir) {
            set.skip = SKIP_AUTO;
            set.cheat_health = set.cheat_hearts = set.cheat_lives = true;
        }
    }

    char title[256];
    snprintf(title, sizeof title, "Rondo Recomp (%s)", recomp_build_id);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (set.fullscreen && !test_mode) flags |= SDL_WINDOW_FULLSCREEN;
    if (test_mode) flags |= SDL_WINDOW_NOT_FOCUSABLE;   /* never takes the keyboard from the player */
    SDL_Window *win = SDL_CreateWindow(title, features_dir ? 1280 : 960, 720, flags);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    Video video;
    if (!ren || !video_init(&video, ren)) {
        fprintf(stderr, "SDL video setup: %s\n", SDL_GetError());
        return 1;
    }
    set_vsync(ren, set.vsync && !test_mode);   /* test runs go as fast as they can */

    char disc_path[1024];
    if (!choose_disc(win, ren, disc_arg, disc_path, sizeof disc_path)) {
        SDL_Quit();
        return 0;
    }
    ach_init(&pce, &set);
    /* features test: an achievement of its own on the game's state (work RAM $30 = 05 playing,
     * $31 = 01), read as RetroAchievements reads the console's memory */
    if (features_dir) ach_test_trigger("0xH0030=5_0xH0031=1");
    if (test_ach) {
        /* --test-achievements: each installed disc's RetroAchievements hash and the game the
         * server knows it as (no login needed), into rondo.log; exit code 0 if all are game 4855 */
        int bad = 0;
        for (int l = 0; l < LANG_COUNT; l++) {
            char disc[1024], hash[33] = "", why[200] = "";
            if (!disc_for(l, &set, disc_path, disc, sizeof disc)) continue;
            int id = ach_test_identify(disc, hash, why, sizeof why);
            TLOG("achievements: %s disc: hash %s, RetroAchievements game %d%s%s\n", l == LANG_JA ? "Japanese" : "English",
                 hash[0] ? hash : "(none)", id, id < 0 ? " - " : id == 4855 ? " (Castlevania: Rondo of Blood)" : " - NOT 4855",
                 id < 0 ? why : "");
            if (id != 4855) bad++;
        }
        const char *base = SDL_GetBasePath();
        char log_path[1024];
        snprintf(log_path, sizeof log_path, "%srondo.log", base ? base : "");
        FILE *lf = fopen(log_path, "w");
        if (lf) { fputs(autotest_log, lf); fclose(lf); }
        fputs(autotest_log, stdout);
        ach_shutdown();
        SDL_Quit();
        return bad ? 1 : 0;
    }
    have_rewind = rewind_init(&rw, 256u << 20, (int)(FRAME_RATE * 60 * 30));   /* ~1 KB a frame, up to 30 min */
    Session ss = {0};
    char err[256];
    bool started = false;
    {   /* the chosen language's disc if there is one (a disc on the command line wins) */
        char want[1024];
        if (!disc_arg && disc_for(set.language, &set, disc_path, want, sizeof want))
            started = start_game(&ss, want, err, sizeof err);
    }
    if (!started && !start_game(&ss, disc_path, err, sizeof err)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rondo Recomp", err, win);
        SDL_Quit();
        return 1;
    }
    set.language = ss.language;
    /* entry points that run on the interpreter are kept next to rondo.exe; running Setup with
     * --rebuild recompiles them too */
    char misses_path[1024] = "";
    {
        const char *base = SDL_GetBasePath();
        if (base) {
            snprintf(misses_path, sizeof misses_path, "%smisses.txt", base);
            recomp_load_misses(misses_path);
        }
    }

    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, AUDIO_RATE};
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else fprintf(stderr, "no audio: %s\n", SDL_GetError());

    Input in;
    input_init(&in);
    input_use_binds(&in, set.bind_pad, set.bind_key);   /* the Controls page changes them in place */
    Menu menu = {0};
    /* --autotest-start: the start screen for 600 frames, a mouse click on Play (a real event
     * through the queue), 300 frames later the title screen (Play skips the logo and intro);
     * then from power-on again, START (Enter) during the intro's story text skips it too */
    int t_start_loops = 0, t_phase = 0, t_phase_at = 0;
    if ((!test_mode && !play_now && set.start_screen) || autotest_start || update_dir) {
        menu_open_start_screen(&menu);
        start_screen_info(&menu, &ss, &set, disc_path);
    }
    update_leftovers();
    if ((set.check_updates && !test_mode) || update_dir) update_check_start();
    bool restart = false, told_update = false;
    int t_upd_phase = 0, t_upd_loops = 0;
    bool t_upd_shot = false;
    char t_upd_shot_path[1200] = "";   /* taken once the frame is drawn */
    const double frame_ns = 1e9 / FRAME_RATE;
    double audio_frac = 0.0;
    static int16_t samples[2048 * 2];
    Uint64 next = SDL_GetTicksNS();
    bool running = true, cursor_shown = true;
    bool was_rewinding = false, skipping = false, hold_input = false;
    bool skip_intro = false;       /* the scene run fast is the power-on intro (features test) */
    bool skip_to_title = false;    /* Play on the start screen: on from power-on to the title screen */
    bool skip_power_on = false;    /* Start, A or X pressed in the logo or the intro: to the title */
    int rewind_held_frames = 0;
    /* autotest bookkeeping */
    int t_rewound = 0;
    bool t_saved = false, t_loaded = false, t_shots = false;
    long t_cine_start = -1, t_cine_end = -1;
    Uint64 t_cine_ns = 0;
    int t_min_health = 0x100;
    long t_ach_frame = -1;
    bool t_ach_state = false;

    while (running) {
        if (autotest_start) {          /* phases wait for the skips to finish, at any speed */
            t_start_loops++;
            int since = t_start_loops - t_phase_at;
            bool title = pce.wram[RONDO_STATE] == RONDO_OPENING && pce.wram[RONDO_SUB] >= 2;
            static bool t_ran_fast;        /* the game's own skip: nothing is run fast */
            if (skipping) t_ran_fast = true;
            if (t_phase == 0 && t_start_loops == 600) {
                TLOG("autotest-start: start screen up for %d frames, game frame %llu (%s)\n", t_start_loops - 1,
                     (unsigned long long)pce.frame,
                     pce.frame == 0 ? "the game waits for Play" : "THE GAME RAN BEHIND IT");
                float px = 0, py = 0, wx = 0, wy = 0;
                menu_item_center(&menu, ren, 0, &px, &py);             /* Play */
                SDL_RenderCoordinatesToWindow(ren, px, py, &wx, &wy);
                SDL_Event click = {0};
                click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                click.button.windowID = SDL_GetWindowID(win);
                click.button.button = SDL_BUTTON_LEFT;
                click.button.down = true;
                click.button.clicks = 1;
                click.button.which = TEST_INPUT;
                click.button.x = wx;
                click.button.y = wy;
                SDL_PushEvent(&click);
                t_phase = 1;
                t_phase_at = t_start_loops;
            } else if (t_phase == 1 && ((!menu.open && title) || since > 3000)) {
                TLOG("autotest-start: after clicking Play: %s, %d screen frames later game frame %llu, state %02X/%02X (%s%s)\n",
                     menu.open ? "STILL ON THE START SCREEN" : "playing", since, (unsigned long long)pce.frame,
                     pce.wram[RONDO_STATE], pce.wram[RONDO_SUB],
                     title ? "the title screen: logo and intro skipped" : "NOT AT THE TITLE SCREEN",
                     t_ran_fast ? ", BUT RUN FAST" : ", the game's own skip");
                start_game(&ss, ss.disc, err, sizeof err);      /* power-on, no Play */
                skip_to_title = skip_power_on = skipping = false;
                t_phase = 2;
                t_phase_at = t_start_loops;
            } else if (t_phase == 2 && pce.frame >= 1000) {
                TLOG("autotest-start: power-on again, frame %llu: state %02X/%02X (the intro), pressing Jump (X)\n",
                     (unsigned long long)pce.frame, pce.wram[RONDO_STATE], pce.wram[RONDO_SUB]);
                SDL_Event key = {0};
                key.type = SDL_EVENT_KEY_DOWN;
                key.key.windowID = SDL_GetWindowID(win);
                key.key.scancode = (SDL_Scancode)set.bind_key[ACT_JUMP];
                key.key.down = true;
                key.key.which = TEST_INPUT;
                SDL_PushEvent(&key);
                t_phase = 3;
                t_phase_at = t_start_loops;
            } else if (t_phase == 3 && (title || since > 3000)) {
                TLOG("autotest-start: %d screen frames after Jump: game frame %llu, state %02X/%02X (%s%s)\n", since,
                     (unsigned long long)pce.frame, pce.wram[RONDO_STATE], pce.wram[RONDO_SUB],
                     title ? "the title screen: intro skipped" : "INTRO NOT SKIPPED",
                     t_ran_fast ? ", BUT RUN FAST" : ", the game's own skip");
                set.skip = SKIP_AUTO;                           /* power-on with "automatically" */
                start_game(&ss, ss.disc, err, sizeof err);
                skip_to_title = skip_power_on = skipping = false;
                t_phase = 4;
                t_phase_at = t_start_loops;
            } else if (t_phase == 4 && (title || since > 3000)) {
                TLOG("autotest-start: skip automatically: %d screen frames after power-on: game frame %llu, state %02X/%02X (%s%s)\n",
                     since, (unsigned long long)pce.frame, pce.wram[RONDO_STATE], pce.wram[RONDO_SUB],
                     title ? "the title screen: logo and intro skipped" : "NOT AT THE TITLE SCREEN",
                     t_ran_fast ? ", BUT RUN FAST" : ", the game's own skip");
                running = false;
            }
        }
        if (update_dir) {   /* --autotest-update */
            t_upd_loops++;
            const UpdateInfo *ui;
            const char *uwhy;
            UpdateState ust = update_state(&ui, &uwhy);
            /* a click goes in the loop after the screenshot of what it clicks on */
            static const char *pending;
            const char *click_on = pending;
            pending = NULL;
            if (t_upd_phase == 0 && ust == UPD_AVAILABLE && t_upd_loops > 30) {
                TLOG("autotest-update: this is %s; GitHub (%s) has %s: %s, %lld bytes, sha256 %s%s\n",
                     update_current_version(), update_api_url(), ui->version, ui->asset, ui->size,
                     ui->sha256[0] ? ui->sha256 : "(none)", ui->slim ? " (without the compiler)" : "");
                for (int i = 0; i < 4; i++) if (ui->notes[i][0]) TLOG("autotest-update: new: %s\n", ui->notes[i]);
                TLOG("autotest-update: start screen notice: \"%s\", Updates row: \"%s\"\n", menu.notice, menu.upd_badge);
                snprintf(t_upd_shot_path, sizeof t_upd_shot_path, "%s/update_start_screen.bmp", update_dir);
                pending = "Updates";
                t_upd_phase = 1;
            } else if (t_upd_phase == 0 && (ust == UPD_LATEST || ust == UPD_FAILED || t_upd_loops > 3600)) {
                TLOG("autotest-update: NO UPDATE FOUND (%s)\n", ust == UPD_FAILED ? uwhy : ust == UPD_LATEST ? "latest" : "timeout");
                running = false;
            } else if (t_upd_phase == 1 && t_upd_loops > 60) {
                snprintf(t_upd_shot_path, sizeof t_upd_shot_path, "%s/update_page.bmp", update_dir);
                pending = "Download and install";
                t_upd_phase = 2;
                t_upd_loops = 0;
            } else if (t_upd_phase == 2 && menu.updating && !t_upd_shot && menu.upd_pct >= 30 && menu.upd_pct <= 90) {
                snprintf(t_upd_shot_path, sizeof t_upd_shot_path, "%s/update_progress.bmp", update_dir);
                TLOG("autotest-update: installing: \"%s\" (%d%%)\n", menu.upd[1], menu.upd_pct);
                t_upd_shot = true;
            } else if (t_upd_phase == 2 && !menu.updating && t_upd_loops > 20) {
                TLOG("autotest-update: UPDATE DID NOT INSTALL: %s\n", menu.status);
                running = false;
            } else if (t_upd_phase == 2 && t_upd_loops > 60 * 60 * 15) {
                TLOG("autotest-update: UPDATE TIMED OUT\n");
                running = false;
            }
            if (click_on) {
                float px = 0, py = 0, wx = 0, wy = 0;
                menu_item_center(&menu, ren, menu_find(&menu, click_on), &px, &py);
                SDL_RenderCoordinatesToWindow(ren, px, py, &wx, &wy);
                SDL_Event click = {0};
                click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                click.button.windowID = SDL_GetWindowID(win);
                click.button.button = SDL_BUTTON_LEFT;
                click.button.down = true;
                click.button.clicks = 1;
                click.button.which = TEST_INPUT;
                click.button.x = wx;
                click.button.y = wy;
                SDL_PushEvent(&click);
            }
        }
        bool start_pressed = false, skip_pressed = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                if (menu.updating) snprintf(menu.status, sizeof menu.status, "Wait for the update to finish.");
                else running = false;
                continue;
            }
            /* test runs only take the input they send themselves (marked TEST_INPUT), so a key
             * pressed on the desktop while one runs cannot change it */
            if (test_mode && !test_input(&ev)) continue;
            input_device_event(&in, &ev);
            if (menu.open) {
                MenuResult r = MENU_NOTHING;
                if (menu_editing(&menu)) {
                    r = menu_text(&menu, &set, &ev);           /* typing into a text field */
                } else if (menu_capturing(&menu)) {
                    r = menu_capture(&menu, &set, &in, &ev);   /* the next button or key is the control */
                } else if (ev.type == SDL_EVENT_MOUSE_MOTION || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    SDL_Event e = ev;
                    SDL_ConvertEventToRenderCoordinates(ren, &e);
                    bool motion = ev.type == SDL_EVENT_MOUSE_MOTION;
                    int item = menu_item_at(&menu, ren, motion ? e.motion.x : e.button.x, motion ? e.motion.y : e.button.y);
                    if (motion) menu_hover(&menu, item);
                    else if (e.button.button == SDL_BUTTON_LEFT) r = menu_click(&menu, &set, item);
                    else if (e.button.button == SDL_BUTTON_RIGHT) r = menu_nav(&menu, &set, NAV_BACK);
                } else {
                    Nav nav = input_nav_event(&ev);
                    if (nav == NAV_NONE && input_pressed(&in, &ev, ACT_MENU)) nav = NAV_MENU;   /* rebound */
                    if (nav == NAV_NONE) continue;
                    r = menu_nav(&menu, &set, nav);
                }
                switch (r) {
                case MENU_CLOSE:
                    menu.open = false;
                    hold_input = true;     /* the button that closed the menu is not a game press */
                    break;
                case MENU_SAVE_STATE:
                case MENU_LOAD_STATE: {
                    char path[1200];
                    state_path(path, sizeof path, &ss, set.slot);
                    if (r == MENU_SAVE_STATE) {
                        if (pce_state_save(&pce, path, ss.disc_id, err, sizeof err)) {
                            ach_save_progress(path);
                            snprintf(menu.status, sizeof menu.status, "State %d saved", set.slot);
                        } else {
                            snprintf(menu.status, sizeof menu.status, "Save failed: %.60s", err);
                        }
                    } else if (ach_hardcore()) {
                        snprintf(menu.status, sizeof menu.status, "Hardcore mode: loading a state is off.");
                    } else if (pce_state_load(&pce, path, ss.disc_id, err, sizeof err)) {
                        ach_load_progress(path);
                        if (have_rewind) rewind_reset(&rw, &pce);
                        game_watch_reset(&gw, &pce);
                        pce.bram_dirty = false;
                        menu.open = false;
                        hold_input = true;
                        show_osd("State %d loaded", set.slot);
                    } else {
                        snprintf(menu.status, sizeof menu.status, "Slot %d: %.60s", set.slot, err);
                    }
                    break;
                }
                case MENU_RESET:
                    if (start_game(&ss, ss.disc, err, sizeof err)) {
                        menu.open = false;
                        hold_input = true;
                        show_osd("Game reset");
                    } else {
                        snprintf(menu.status, sizeof menu.status, "%.100s", err);
                    }
                    break;
                case MENU_QUIT:
                    running = false;
                    break;
                case MENU_PLAY:                /* start screen: the game from power-on */
                    if (start_game(&ss, ss.disc, err, sizeof err)) {
                        menu.open = false;
                        hold_input = true;
                        skipping = was_rewinding = false;
                        /* Play was the player's start: on to the title screen, unless story
                         * scenes are never skipped */
                        skip_to_title = set.skip != SKIP_OFF;
                    } else {
                        snprintf(menu.status, sizeof menu.status, "%.100s", err);
                    }
                    break;
                case MENU_START_SCREEN:
                    if (start_game(&ss, ss.disc, err, sizeof err)) {
                        menu_open_start_screen(&menu);
                        start_screen_info(&menu, &ss, &set, disc_path);
                        skipping = was_rewinding = skip_to_title = false;
                    } else {
                        snprintf(menu.status, sizeof menu.status, "%.100s", err);
                    }
                    break;
                case MENU_SETUP:
                    if (open_setup(menu.status, sizeof menu.status)) running = false;
                    break;
                case MENU_LANGUAGE:
                    switch_language(&ss, &set, disc_path, true, win, ren, menu.status, sizeof menu.status);
                    if (menu_on_start_screen(&menu)) start_screen_info(&menu, &ss, &set, disc_path);
                    if (!test_mode) settings_save(&set, settings_path);
                    break;
                case MENU_VIDEO_CHANGED:
                    SDL_SetWindowFullscreen(win, set.fullscreen);
                    set_vsync(ren, set.vsync && !test_mode);
                    if (!test_mode) settings_save(&set, settings_path);
                    break;
                case MENU_SETTINGS_CHANGED:
                    if (!test_mode) settings_save(&set, settings_path);
                    break;
                case MENU_ACH_CHANGED:
                    ach_configure();
                    if (!test_mode) settings_save(&set, settings_path);
                    break;
                case MENU_ACH_LOGIN:
                    if (!set.ra_user[0] || !menu.password[0]) {
                        snprintf(menu.status, sizeof menu.status, "Enter your user name and password first.");
                    } else {
                        ach_login(set.ra_user, menu.password);
                        SDL_memset(menu.password, 0, sizeof menu.password);   /* used once, never kept */
                        snprintf(menu.status, sizeof menu.status, "Logging in as %s...", set.ra_user);
                        if (!test_mode) settings_save(&set, settings_path);
                    }
                    break;
                case MENU_ACH_LOGOUT:
                    ach_logout();
                    snprintf(menu.status, sizeof menu.status, "Logged out of RetroAchievements.");
                    if (!test_mode) settings_save(&set, settings_path);
                    break;
                case MENU_TEXT_START:
                    SDL_StartTextInput(win);
                    break;
                case MENU_TEXT_END:
                    SDL_StopTextInput(win);
                    if (!test_mode) settings_save(&set, settings_path);
                    break;
                case MENU_UPDATE_CHECK:
                    update_check_start();
                    menu.status[0] = 0;
                    break;
                case MENU_UPDATE_INSTALL:
                    update_install(&menu);
                    break;
                case MENU_UPDATE_PAGE: {
                    const UpdateInfo *ui;
                    update_state(&ui, NULL);
                    if (!SDL_OpenURL(ui && ui->page[0] ? ui->page : "https://github.com/" UPDATE_REPO "/releases/latest"))
                        snprintf(menu.status, sizeof menu.status, "Cannot open a web browser: %.80s", SDL_GetError());
                    break;
                }
                default:
                    break;
                }
                if (!menu.open) input_cancel_macros(&in);   /* a button pressed in it is not one let go in play */
                continue;
            }
            /* playing */
            bool open_menu = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                SDL_Scancode sc = ev.key.scancode;
                if (sc == SDL_SCANCODE_ESCAPE) open_menu = true;      /* Esc always; the Menu control below */
                else if (sc == SDL_SCANCODE_F11) {
                    set.fullscreen = !set.fullscreen;
                    SDL_SetWindowFullscreen(win, set.fullscreen);
                    if (!test_mode) settings_save(&set, settings_path);
                } else if (sc == SDL_SCANCODE_F6 || sc == SDL_SCANCODE_F7) {
                    set.slot = (set.slot + (sc == SDL_SCANCODE_F7 ? 1 : 8) - 1) % 9 + 1;
                    show_osd("State slot %d", set.slot);
                    if (!test_mode) settings_save(&set, settings_path);
                }
            }
            {   /* save and load state: their own controls (F5 / F8 unless changed) */
                bool save = input_pressed(&in, &ev, ACT_SAVE), load = input_pressed(&in, &ev, ACT_LOAD);
                if (save || load) {
                    char path[1200];
                    state_path(path, sizeof path, &ss, set.slot);
                    if (save) {
                        if (pce_state_save(&pce, path, ss.disc_id, err, sizeof err)) {
                            ach_save_progress(path);
                            show_osd("State %d saved", set.slot);
                        } else {
                            show_osd("Save failed: %s", err);
                        }
                    } else if (ach_hardcore()) {
                        show_osd("Hardcore mode: loading a state is off");
                    } else if (pce_state_load(&pce, path, ss.disc_id, err, sizeof err)) {
                        ach_load_progress(path);
                        show_osd("State %d loaded", set.slot);
                        if (have_rewind) rewind_reset(&rw, &pce);
                        game_watch_reset(&gw, &pce);
                        input_cancel_macros(&in);
                        pce.bram_dirty = false;
                    } else {
                        show_osd("Slot %d: %s", set.slot, err);
                    }
                }
            }
            if (input_pressed(&in, &ev, ACT_MENU)) open_menu = true;
            if (input_pressed(&in, &ev, ACT_PAUSE)) start_pressed = true;
            if (start_pressed || input_pressed(&in, &ev, ACT_JUMP) || input_pressed(&in, &ev, ACT_ATTACK))
                skip_pressed = true;
            if (open_menu && !test_mode) {
                menu_open(&menu);
                input_cancel_macros(&in);
            }
        }
        if (menu.open) {
            if (menu_update(&menu, &set) == MENU_SETTINGS_CHANGED && !test_mode)   /* a control's count ended */
                settings_save(&set, settings_path);
            Nav nav = test_mode || menu_capturing(&menu) || menu_editing(&menu) ? NAV_NONE : input_nav_stick(&in);
            if (nav != NAV_NONE) {
                MenuResult r = menu_nav(&menu, &set, nav);    /* the stick only steps and changes values */
                if (r == MENU_LANGUAGE) {
                    switch_language(&ss, &set, disc_path, true, win, ren, menu.status, sizeof menu.status);
                    if (menu_on_start_screen(&menu)) start_screen_info(&menu, &ss, &set, disc_path);
                }
                if (r == MENU_ACH_CHANGED) ach_configure();
                if (r == MENU_SETTINGS_CHANGED || r == MENU_VIDEO_CHANGED || r == MENU_LANGUAGE || r == MENU_ACH_CHANGED) {
                    if (r == MENU_VIDEO_CHANGED) {
                        SDL_SetWindowFullscreen(win, set.fullscreen);
                        set_vsync(ren, set.vsync && !test_mode);
                    }
                    if (!test_mode) settings_save(&set, settings_path);
                }
            }
        }

        if (ach_update() && !test_mode) settings_save(&set, settings_path);   /* a login's token */
        if (ach_take_reset()) {               /* hardcore switched on: from power-on */
            if (start_game(&ss, ss.disc, err, sizeof err) && !menu_on_start_screen(&menu)) show_osd("Hardcore mode: the game restarted");
            skipping = was_rewinding = skip_to_title = false;
        }
        if (menu.open) ach_status(menu.ach[0], sizeof menu.ach[0], menu.ach[1], sizeof menu.ach[1]);
        if (update_tick(&menu)) {             /* installed: the new version takes over */
            restart = true;
            running = false;
        }
        if (!told_update && menu.notice[0]) {  /* a newer version: said once (the start screen says it too) */
            told_update = true;
            if (!menu_on_start_screen(&menu)) show_osd("%s (start screen)", menu.notice);
        }

        prompt.pad = input_pad_last(&in);
        prompt.line = prompt.pad ? (set.bind_pad[ACT_PAUSE] == SDL_GAMEPAD_BUTTON_START ? "PUSH  START  BUTTON!"
                                                                                       : "PUSH  PAUSE  BUTTON!")
                                 : (set.bind_key[ACT_PAUSE] == SDL_SCANCODE_RETURN ? "PUSH  ENTER  KEY!" : "PUSH  PAUSE  KEY!");
        GameCheats cheats = {set.cheat_health, set.cheat_hearts, set.cheat_lives, set.maria_unlocked, set.play_maria};
        bool hardcore = ach_hardcore();
        if (hardcore) memset(&cheats, 0, sizeof cheats);   /* RetroAchievements' hardcore mode: none */
        bool rewinding = false, ran_frame = false;
        if (menu_on_start_screen(&menu)) {
            /* the start screen: the game waits for Play (nothing runs, nothing sounds) */
        } else if (!menu.open) {
            /* The logo and the intro: the game's own skip to the title screen (game.h), unless
             * Skip cinematics is Never. Start, A (Jump) or X (Attack) asks for it; Play on the
             * start screen and "automatically" ask at once. The frontend then presses START
             * for the game where it looks for it. */
            bool power_on = game_in_power_on(&pce);
            bool own_skip = power_on && set.skip != SKIP_OFF && !features_dir;
            if (!power_on) skip_to_title = skip_power_on = false;
            if (own_skip) {
                game_allow_power_on_skip(&pce);
                if (skip_pressed) skip_power_on = true;
            }
            bool press_start = own_skip && (skip_power_on || skip_to_title || set.skip == SKIP_AUTO);
            /* Story scenes have no skip of their own: they run fast instead (so does the intro in
             * the features test, whose recorded input counts on every frame of it). */
            if (!skipping && ((features_dir && game_in_intro(&pce) && set.skip == SKIP_AUTO) ||
                              (game_in_cinematic(&pce) &&
                               (set.skip == SKIP_AUTO || (set.skip == SKIP_START && start_pressed))))) {
                skipping = true;
                skip_intro = !game_in_cinematic(&pce);
                if (features_dir) { t_cine_start = (long)pce.frame; t_cine_ns = SDL_GetTicksNS(); }
            }
            rewinding = set.rewind && have_rewind && !skipping && !test_mode && !hardcore && input_rewind_held(&in);
            if (autotest) {
                uint64_t f = pce.frame;
                char path[1200];
                state_path(path, sizeof path, &ss, 9);
                if (f == 600 && !t_saved) {
                    t_saved = true;
                    bool ok = pce_state_save(&pce, path, ss.disc_id, err, sizeof err);
                    TLOG("autotest: save at frame 600: %s\n", ok ? "ok" : err);
                } else if (f == 700 && !t_loaded) {
                    t_loaded = true;
                    bool ok = pce_state_load(&pce, path, ss.disc_id, err, sizeof err);
                    if (ok) rewind_reset(&rw, &pce);
                    TLOG("autotest: load at frame 700: %s, now at frame %llu\n", ok ? "ok" : err, (unsigned long long)pce.frame);
                }
                if (t_loaded && t_rewound < 200 && (t_rewound > 0 || f >= 800)) { rewinding = true; t_rewound++; }
                else if (t_rewound == 200 && was_rewinding) {
                    uint64_t back;
                    memcpy(&back, rw.cur + offsetof(Pce, frame), sizeof back);
                    TLOG("autotest: rewound for 200 frames, resuming at frame %llu\n", (unsigned long long)back);
                }
                if (t_rewound == 200 && f >= 1100 && !rewinding) running = false;
            }

            if (skipping) {
                /* run the scene as fast as possible, silently, ~12 ms per displayed frame (input:
                 * in a story scene an occasional I press, in case it waits for a button; none in
                 * the intro, which ignores buttons, so the title screen is not passed by) */
                Uint64 t0 = SDL_GetTicksNS();
                while ((skip_intro ? game_in_intro(&pce) : game_in_cinematic(&pce)) && SDL_GetTicksNS() - t0 < 12000000) {
                    pce.pad = !skip_intro && (pce.frame & 15) < 4 ? PAD_I : 0;
                    run_frame();
                    audio_frac += AUDIO_RATE / FRAME_RATE;   /* a frame's worth, not heard */
                    int skip_n = (int)audio_frac;
                    audio_frac -= skip_n;
                    pce_audio_render(&pce, samples, skip_n);
                    game_apply_cheats(&pce, &cheats, &gw);
                    ach_frame();
                    flush_bram(&ss);
                }
                ran_frame = true;
                bool still = skip_intro ? game_in_intro(&pce) : game_in_cinematic(&pce);
                if (!still) {
                    skipping = false;
                    hold_input = true;
                    if (have_rewind) rewind_record(&rw, &pce, 0);   /* one step back over the whole scene */
                    if (features_dir) {
                        t_cine_end = (long)pce.frame;
                        TLOG("features: %s frames %ld-%ld skipped in %.2f s (sound left waiting: %u samples%s)\n",
                             skip_intro ? "intro" : "cinematic", t_cine_start, t_cine_end,
                             (double)(SDL_GetTicksNS() - t_cine_ns) / 1e9, pce.snd.count,
                             pce.snd.count > 64 ? ", LAGGING" : "");
                    }
                }
            } else if (rewinding) {
                /* step back (faster after holding for 3 s), then replay the frame to show it */
                int steps = rewind_held_frames > 180 ? 2 : 1;
                uint8_t pad = 0;
                bool moved = false;
                for (int i = 0; i < steps; i++) moved |= rewind_step(&rw, &pad);
                if (moved) {
                    pce_state_restore(&pce, rw.cur);
                    pce.pad = pad;
                    run_frame();
                    ran_frame = true;
                }
                pce.bram_dirty = false;            /* rewinding never overwrites the save file */
                rewind_held_frames++;
                was_rewinding = true;
            } else {
                if (was_rewinding) {               /* continue from the exact rewound state */
                    pce_state_restore(&pce, rw.cur);
                    pce.bram_dirty = false;
                    game_watch_reset(&gw, &pce);
                    was_rewinding = false;
                    rewind_held_frames = 0;
                    input_cancel_macros(&in);
                }
                uint8_t pad = features_dir ? capture_pad((int)pce.frame) : test_mode ? 0 : input_game(&in);
                if (hold_input && !features_dir) { /* until everything is let go */
                    if (pad) pad = 0;
                    else hold_input = false;
                }
                if (press_start)                   /* the game's own skip: a new START every 8 frames */
                    pad = game_power_on_skippable(&pce) && (pce.frame & 7) < 4 ? PAD_RUN : 0;
                pce.pad = pad;
                run_frame();
                game_apply_cheats(&pce, &cheats, &gw);
                game_apply_run(&pce, !test_mode && !hardcore && input_run_held(&in), &gw);
                ach_frame();
                ran_frame = true;
                if (have_rewind && set.rewind) rewind_record(&rw, &pce, pce.pad);
                flush_bram(&ss);
                if (features_dir && game_playing(&pce) && pce.frame > 16200 && pce.wram[RONDO_HEALTH] < t_min_health)
                    t_min_health = pce.wram[RONDO_HEALTH];
            }
        }

        if (features_dir && ran_frame && t_ach_frame < 0 && ach_test_triggered()) {
            t_ach_frame = (long)pce.frame;
            t_ach_state = pce.wram[RONDO_STATE] == 0x05 && pce.wram[RONDO_SUB] == 0x01;
        }

        /* audio: one frame's worth, kept within ~100 ms of queued data (silent while skipping,
         * rewinding or in the menu) */
        if (ran_frame) {
            audio_frac += AUDIO_RATE / FRAME_RATE;
            int n = (int)audio_frac;
            audio_frac -= n;
            if (!skipping) {
                pce_audio_render(&pce, samples, n);
                if (rewinding) memset(samples, 0, (size_t)n * 4);
                else if (set.volume < 10)
                    for (int i = 0; i < n * 2; i++) samples[i] = (int16_t)(samples[i] * set.volume / 10);
                if (audio && SDL_GetAudioStreamQueued(audio) < AUDIO_RATE * 4 / 10)
                    SDL_PutAudioStreamData(audio, samples, n * 4);
            }
        }

        /* video */
        video_upload(&video, pce.vdc.fb, pce.vdc.fb_w, pce.vdc.fb_h, PCE_FB_MAX_W);
        if (features_dir && !t_shots && pce.frame >= 17880) {
            /* screenshots: 16:9 window with each picture mode and screen effect, then the menu
             * pages */
            t_shots = true;
            TLOG("features: frame %llu state %02X/%02X, picture %dx%d\n", (unsigned long long)pce.frame,
                 pce.wram[0x30], pce.wram[0x31], pce.vdc.fb_w, pce.vdc.fb_h);
            static const struct { int aspect, integer, filter, sides, effect; const char *name; } modes[] = {
                {ASPECT_4_3, 0, FILTER_SHARP, SIDES_BLUR, EFFECT_OFF, "video_4x3_blur"},
                {ASPECT_STRETCH, 0, FILTER_SHARP, SIDES_BLUR, EFFECT_OFF, "video_stretch"},
                {ASPECT_SQUARE, 1, FILTER_NEAREST, SIDES_BLACK, EFFECT_OFF, "video_square_integer"},
                {ASPECT_4_3, 0, FILTER_SHARP, SIDES_BLACK, EFFECT_SCANLINES, "effect_scanlines"},
                {ASPECT_4_3, 0, FILTER_SHARP, SIDES_BLACK, EFFECT_CRT, "effect_crt"},
                {ASPECT_4_3, 0, FILTER_SHARP, SIDES_BLUR, EFFECT_CURVED, "effect_curved_crt"},
                {ASPECT_4_3, 0, FILTER_SHARP, SIDES_BLACK, EFFECT_COMPOSITE, "effect_composite"},
            };
            Settings keep = set;
            char path[1200];
            for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
                set.aspect = modes[i].aspect;
                set.scaling = modes[i].integer ? SCALE_WHOLE : SCALE_FIT;
                set.filter = modes[i].filter;
                set.sides = modes[i].sides;
                set.effect = modes[i].effect;
                set.effect_strength = STRENGTH_MEDIUM;
                video_draw(&video, &set);
                snprintf(path, sizeof path, "%s/%s.bmp", features_dir, modes[i].name);
                screenshot(ren, path);
            }
            set = keep;
            Menu m = {0};
            menu_open(&m);
            m.opened_ms = 0;                   /* shown fully open */
            const char *names[] = {"menu_main", "menu_video", "menu_controls"};
            for (int i = 0; i < 3; i++) {
                if (i == 1) { for (int d = 0; d < 4; d++) menu_nav(&m, &set, NAV_DOWN); menu_nav(&m, &set, NAV_OK); }
                if (i == 2) { menu_nav(&m, &set, NAV_BACK); for (int d = 0; d < 3; d++) menu_nav(&m, &set, NAV_DOWN); menu_nav(&m, &set, NAV_OK); }
                video_draw(&video, &set);
                menu_draw(&m, &set, ren);
                snprintf(path, sizeof path, "%s/%s.bmp", features_dir, names[i]);
                screenshot(ren, path);
            }
            /* the notices (top left; START: skip, bottom right) */
            video_draw(&video, &set);
            draw_banner(ren, "State 1 saved", false);
            draw_banner(ren, "START: skip", true);
            snprintf(path, sizeof path, "%s/notices.bmp", features_dir);
            screenshot(ren, path);
            {   /* the RetroAchievements page (not logged in), then an unlock pop-up */
                Menu am = {0};
                menu_open(&am);
                am.opened_ms = 0;
                for (int d = 0; d < 8; d++) menu_nav(&am, &set, NAV_DOWN);
                menu_nav(&am, &set, NAV_OK);
                ach_status(am.ach[0], sizeof am.ach[0], am.ach[1], sizeof am.ach[1]);
                video_draw(&video, &set);
                menu_draw(&am, &set, ren);
                snprintf(path, sizeof path, "%s/menu_achievements.bmp", features_dir);
                screenshot(ren, path);
                video_draw(&video, &set);
                ach_test_popup();
                ach_draw(ren);
                snprintf(path, sizeof path, "%s/achievement_popup.bmp", features_dir);
                screenshot(ren, path);
            }
            {   /* the start screen: a screenshot; START plays, Back stays, an option page returns
                 * to it, and the mouse finds each item in order */
                Menu sm = {0};
                menu_open_start_screen(&sm);
                sm.opened_ms = 0;
                start_screen_info(&sm, &ss, &set, disc_path);
                video_draw(&video, &set);
                menu_draw(&sm, &set, ren);
                snprintf(path, sizeof path, "%s/start_screen.bmp", features_dir);
                screenshot(ren, path);
                MenuResult on_start = menu_nav(&sm, &set, NAV_MENU), on_back = menu_nav(&sm, &set, NAV_BACK);
                int ww, wh, found = 0, last = -1;
                SDL_GetRenderOutputSize(ren, &ww, &wh);
                bool in_order = true;
                float col_x = 0, col_y = 0;
                menu_item_center(&sm, ren, 0, &col_x, &col_y);        /* down the panel's middle */
                for (int y = 0; y < wh; y++) {
                    int it = menu_item_at(&sm, ren, col_x, (float)y);
                    if (it < 0 || it == last) continue;
                    if (it != last + 1) in_order = false;
                    last = it;
                    found++;
                }
                menu_click(&sm, &set, 4);                        /* Video */
                menu_nav(&sm, &set, NAV_BACK);
                MenuResult after = menu_nav(&sm, &set, NAV_MENU);
                TLOG("features: start screen: START %s, Back %s, %d items under the mouse%s, Video then Back %s\n",
                     on_start == MENU_PLAY ? "plays" : "DOES NOT PLAY", on_back == MENU_NOTHING ? "stays" : "LEAVES",
                     found, in_order ? " in order" : " OUT OF ORDER",
                     after == MENU_PLAY ? "returns to it" : "DOES NOT RETURN");
            }
            char merr[200];
            int bbad = bind_check(merr, sizeof merr);
            TLOG("features: controls: %s%s\n", bbad ? "PROBLEM: " : "assigning, swapping, clearing and names ok",
                 bbad ? merr : "");
            int ibad = input_check(merr, sizeof merr);
            TLOG("features: controller combinations: %s%s\n",
                 ibad ? "PROBLEM: " : "Back + RB / LB save and load, Back alone is the menu, RB / LB keep their own",
                 ibad ? merr : "");
            {   /* the Controls page with Save state waiting for its button */
                Menu cm = {0};
                menu_open(&cm);
                cm.opened_ms = 0;
                for (int d = 0; d < 7; d++) menu_nav(&cm, &set, NAV_DOWN);
                menu_nav(&cm, &set, NAV_OK);
                for (int d = 0; d < 9; d++) menu_nav(&cm, &set, NAV_DOWN);
                menu_nav(&cm, &set, NAV_OK);
                video_draw(&video, &set);
                menu_draw(&cm, &set, ren);
                char cpath[1200];
                snprintf(cpath, sizeof cpath, "%s/menu_controls_waiting.bmp", features_dir);
                screenshot(ren, cpath);
                TLOG("features: controls page: Save state %s\n", menu_capturing(&cm) ? "counts down for its control" : "DOES NOT COUNT");
                /* Save state's count: the A that chose the row let go, Back and RB held at the end */
                const int BACK = SDL_GAMEPAD_BUTTON_BACK, RB = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
                          LB = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B = SDL_GAMEPAD_BUTTON_EAST;
                Settings cs = set;
                const CountStep held[] = {{SDL_GAMEPAD_BUTTON_SOUTH, false}, {BACK, true}, {RB, true}};
                count_feed(&cm, &cs, &in, held, 3);
                bool counting = menu_capturing(&cm) && cs.bind_pad[ACT_SAVE] == set.bind_pad[ACT_SAVE];
                video_draw(&video, &cs);
                menu_draw(&cm, &cs, ren);
                snprintf(cpath, sizeof cpath, "%s/menu_controls_count.bmp", features_dir);
                screenshot(ren, cpath);
                count_end(&cm, &cs);
                bool held_ok = counting && cs.bind_pad[ACT_SAVE] == BIND_COMBO(BACK, RB) && !menu_capturing(&cm);
                /* Load state: B pressed and let go during the count; then a key (F9) likewise */
                menu_nav(&cm, &cs, NAV_DOWN);
                menu_nav(&cm, &cs, NAV_OK);
                const CountStep tap[] = {{B, true}, {B, false}};
                count_feed(&cm, &cs, &in, tap, 2);
                count_end(&cm, &cs);
                bool tap_ok = cs.bind_pad[ACT_LOAD] == B;
                menu_nav(&cm, &cs, NAV_OK);
                const CountStep key[] = {{-SDL_SCANCODE_F9, true}, {-SDL_SCANCODE_F9, false}};
                count_feed(&cm, &cs, &in, key, 2);
                count_end(&cm, &cs);
                bool key_ok = cs.bind_key[ACT_LOAD] == SDL_SCANCODE_F9 && cs.bind_pad[ACT_LOAD] == B;
                /* X on the row clears its controller button (the key stays) */
                menu_nav(&cm, &cs, NAV_CLEAR_PAD);
                bool clear_ok = cs.bind_pad[ACT_LOAD] == BIND_NONE && cs.bind_key[ACT_LOAD] == SDL_SCANCODE_F9;
                /* Save state: three buttons held, then nothing at all: both leave it */
                menu_nav(&cm, &cs, NAV_UP);
                menu_nav(&cm, &cs, NAV_OK);
                const CountStep three[] = {{BACK, true}, {LB, true}, {RB, true}};
                count_feed(&cm, &cs, &in, three, 3);
                count_end(&cm, &cs);
                bool three_ok = cs.bind_pad[ACT_SAVE] == BIND_COMBO(BACK, RB);
                menu_nav(&cm, &cs, NAV_OK);
                count_end(&cm, &cs);
                bool none_ok = cs.bind_pad[ACT_SAVE] == BIND_COMBO(BACK, RB) && cs.bind_key[ACT_SAVE] == set.bind_key[ACT_SAVE];
                /* the same two again: kept, not cleared */
                menu_nav(&cm, &cs, NAV_OK);
                const CountStep again[] = {{BACK, true}, {RB, true}};
                count_feed(&cm, &cs, &in, again, 2);
                count_end(&cm, &cs);
                bool again_ok = cs.bind_pad[ACT_SAVE] == BIND_COMBO(BACK, RB);
                bool ok = held_ok && tap_ok && key_ok && clear_ok && three_ok && none_ok && again_ok;
                TLOG("features: controls page count: %s (held %s, tapped %s, key %s, X clears %s, three %s, nothing %s, "
                     "same again %s; save state: %s, load state: %s / %s)\n",
                     ok ? "Back + RB held at the end records Back + RB" : "DOES NOT RECORD AS IT SHOULD", held_ok ? "ok" : "WRONG",
                     tap_ok ? "ok" : "WRONG", key_ok ? "ok" : "WRONG", clear_ok ? "ok" : "WRONG", three_ok ? "ok" : "WRONG",
                     none_ok ? "ok" : "WRONG", again_ok ? "ok" : "WRONG", bind_pad_name(cs.bind_pad[ACT_SAVE]),
                     bind_pad_name(cs.bind_pad[ACT_LOAD]), bind_key_name(cs.bind_key[ACT_LOAD]));
                video_draw(&video, &cs);
                menu_draw(&cm, &cs, ren);
                snprintf(cpath, sizeof cpath, "%s/menu_controls_combos.bmp", features_dir);
                screenshot(ren, cpath);
            }
            int mbad = menu_check(&set, ren, merr, sizeof merr);
            TLOG("features: menu check: %s%s\n", mbad ? "PROBLEM at " : "every value of every option ok",
                 mbad ? merr : "");
        }
        static bool t_stage1_shot;
        if (features_dir && !t_stage1_shot && pce.frame >= 20060) {
            /* Stage 1 */
            t_stage1_shot = true;
            char path[1200];
            video_draw(&video, &set);
            snprintf(path, sizeof path, "%s/stage1.bmp", features_dir);
            screenshot(ren, path);
            TLOG("features: Stage 1 at frame %llu (state %02X/%02X)\n", (unsigned long long)pce.frame,
                 pce.wram[RONDO_STATE], pce.wram[RONDO_SUB]);
            TLOG("features: title screen's PUSH RUN BUTTON! %s (%d frames, as the %s)\n",
                 prompt.frames ? "replaced" : "NOT REPLACED", prompt.frames, prompt.pad ? "controller's" : "keyboard's");
        }
        if (menu.open != cursor_shown) {           /* the mouse pointer only over menus */
            cursor_shown = menu.open;
            if (cursor_shown) SDL_ShowCursor();
            else SDL_HideCursor();
        }
        if (!menu_on_start_screen(&menu)) video_draw(&video, &set);   /* the start screen draws it all */
        if (skipping) {
            draw_banner(ren, skip_intro ? "Skipping the intro..." : "Skipping cinematic...", false);
        } else if (rewinding) {
            char line[64];
            float secs = (float)rewind_frames(&rw) / (float)FRAME_RATE;
            snprintf(line, sizeof line, "<< REWIND  %d:%02d", (int)secs / 60, (int)secs % 60);
            draw_banner(ren, line, false);
        } else if (SDL_GetTicks() < osd_until) {
            draw_banner(ren, osd_text, false);
        }
        if (!menu.open && !skipping && set.skip == SKIP_START &&
            (game_in_cinematic(&pce) || game_power_on_skippable(&pce))) {
            char skip_line[64];
            snprintf(skip_line, sizeof skip_line, "%s: skip",
                     input_pad_last(&in) ? bind_pad_name(set.bind_pad[ACT_PAUSE]) : bind_key_name(set.bind_key[ACT_PAUSE]));
            draw_banner(ren, skip_line, true);
        }
        if (menu.open) menu_draw(&menu, &set, ren);
        ach_draw(ren);                        /* unlocks, leaderboards: over everything */
        if (t_upd_shot_path[0]) {             /* --autotest-update: this frame */
            screenshot(ren, t_upd_shot_path);
            t_upd_shot_path[0] = 0;
        }
        SDL_RenderPresent(ren);

        if (features_dir && pce.frame >= 20100) {
            TLOG("features: lowest health while playing (cheat on): %d of 92; hearts %02X; lives %02X\n",
                 t_min_health, pce.wram[RONDO_HEARTS], pce.wram[RONDO_LIVES]);
            if (t_ach_frame < 0) TLOG("features: RetroAchievements: the test achievement ($30 = 05, $31 = 01) NEVER TRIGGERED\n");
            else TLOG("features: RetroAchievements: the test achievement ($30 = 05, $31 = 01) triggered at frame %ld, %s\n",
                      t_ach_frame, t_ach_state ? "the game's state as it says" : "BUT THE STATE IS NOT THAT");
            /* the Language option, without asking: the other language's disc if installed */
            int from = ss.language;
            set.language = from == LANG_JA ? LANG_EN : LANG_JA;
            char status[160];
            switch_language(&ss, &set, disc_path, false, win, ren, status, sizeof status);
            if (ss.language != from) {
                for (int i = 0; i < 900; i++) {
                    pce.pad = 0;
                    run_frame();
                }
            }
            TLOG("features: played the %s disc; Language -> %s: %s%s\n", from == LANG_JA ? "Japanese" : "English",
                 from == LANG_JA ? "English" : "Japanese", status,
                 ss.language != from ? (pce.wram[RONDO_STATE] == 0x02 ? " (attract mode after 900 frames)" : " (not running?)") : "");
            running = false;
        }
        if (features_dir) continue;   /* run uncapped */
        next += (Uint64)frame_ns;
        Uint64 now = SDL_GetTicksNS();
        if (next > now) SDL_DelayNS(next - now);
        else if (now - next > (Uint64)(frame_ns * 5)) next = now;   /* fell behind: resync */
    }

    flush_bram(&ss);
    if (!test_mode) settings_save(&set, settings_path);
    if (misses_path[0] && recomp_stats()->miss_entries) recomp_write_misses(misses_path);
    /* rondo.log next to the executable: what ran natively this session */
    {
        const char *base = SDL_GetBasePath();
        char log_path[1024];
        if (base) {
            /* the update test's own log: the new game's --autotest writes rondo.log */
            snprintf(log_path, sizeof log_path, "%s%s", base, update_dir ? "rondo_update.log" : "rondo.log");
            FILE *lf = fopen(log_path, "w");
            if (lf) {
                const RecompStats *rs = recomp_stats();
                uint64_t tot = rs->native_cycles + rs->interp_cycles;
                fputs(autotest_log, lf);
                int vsync = 0;
                SDL_GetRenderVSync(ren, &vsync);
                fprintf(lf, "video: %s, vsync %s\n", SDL_GetRendererName(ren),
                        vsync == 1 ? "on" : vsync == SDL_RENDERER_VSYNC_ADAPTIVE ? "adaptive" : "off");
                fprintf(lf, "build: %s\nframes: %llu\nrecompiled chunks: %d, entry points: %d\n"
                            "CPU cycles native: %.2f%%\nchunk calls: %llu\nentry points without native code: %u\n",
                        recomp_build_id, (unsigned long long)pce.frame, recomp_chunk_count, recomp_entry_count,
                        tot ? 100.0 * (double)rs->native_cycles / (double)tot : 0.0,
                        (unsigned long long)rs->calls, rs->miss_entries);
                fclose(lf);
            }
        }
    }
    ach_shutdown();
    input_close(&in);
    video_free(&video);
    if (audio) SDL_DestroyAudioStream(audio);
    if (restart) {
        const char *none[] = {NULL}, *autotest_args[] = {"--autotest", NULL};
        relaunch(update_dir ? autotest_args : none);   /* returns on Windows (and if it fails) */
    }
    SDL_Quit();
    return 0;
}
