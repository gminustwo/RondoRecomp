/* RondoRecomp Setup, windowed: choose your zipped disc image, watch it being turned into
 * rondo.exe (rondo on Linux), then play. Runs the same steps as rondo_setup
 * (tools/setup/main.c) in a worker thread; text is drawn with SDL3's built-in debug font, so
 * no font files are needed. It also asks GitHub for a newer version (../frontend/update.h)
 * and offers to install it (rondo_setup --update). */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>   /* execv */
#endif

#include "../frontend/http.h"
#include "../frontend/programs.h"
#include "../frontend/update.h"
#include "vc.h"

int setup_main(int argc, char **argv);
extern void (*setup_line_hook)(const char *text);
extern void (*setup_progress_hook)(const char *what, int pct);

enum { IDLE, RUNNING, DONE, FAILED };
enum { MODE_IMPORT, MODE_REBUILD, MODE_UPDATE };

#define MAX_LINES 300
#define COLS 76
static SDL_Mutex *mtx;
static char lines[MAX_LINES][COLS + 1];
static int n_lines;
static char partial[1024];
static char prog_what[128];
static int prog_pct = -1;
static SDL_AtomicInt state;
static char input_path[1024];
static int run_mode;
static int run_result;     /* setup_main's: 3 when there was nothing to update */
static bool updated;
static char base_dir[1024];

static void push_line(const char *text)
{
    /* wrap long lines */
    size_t len = strlen(text);
    size_t off = 0;
    do {
        if (n_lines == MAX_LINES) {
            memmove(lines[0], lines[1], sizeof lines[0] * (MAX_LINES - 1));
            n_lines--;
        }
        size_t n = len - off > COLS ? COLS : len - off;
        memcpy(lines[n_lines], text + off, n);
        lines[n_lines][n] = 0;
        n_lines++;
        off += n;
    } while (off < len);
}

static void gui_line(const char *text)
{
    SDL_LockMutex(mtx);
    for (const char *c = text; *c; c++) {
        size_t l = strlen(partial);
        if (*c == '\n') { push_line(partial); partial[0] = 0; }
        else if (*c != '\r' && l + 1 < sizeof partial) { partial[l] = *c; partial[l + 1] = 0; }
    }
    SDL_UnlockMutex(mtx);
}

static void gui_progress(const char *what, int pct)
{
    SDL_LockMutex(mtx);
    snprintf(prog_what, sizeof prog_what, "%s", what);
    prog_pct = pct;
    SDL_UnlockMutex(mtx);
}

static int SDLCALL worker(void *arg)
{
    (void)arg;
    char *argv[8];
    int argc = 0;
    argv[argc++] = "RondoRecomp Setup";
    if (run_mode == MODE_REBUILD) argv[argc++] = "--rebuild";
    else if (run_mode == MODE_UPDATE) argv[argc++] = "--update";
    else argv[argc++] = input_path;
    argv[argc++] = "--out";
    argv[argc++] = base_dir;
    argv[argc] = NULL;
    int r = setup_main(argc, argv);
    SDL_LockMutex(mtx);
    if (partial[0]) { push_line(partial); partial[0] = 0; }
    prog_pct = -1;
    SDL_UnlockMutex(mtx);
    run_result = r;
    if (run_mode == MODE_UPDATE && r == 0) updated = true;
    SDL_SetAtomicInt(&state, r == 0 || (run_mode == MODE_UPDATE && r == 3) ? DONE : FAILED);
    return r;
}

static void start(int mode)
{
    run_mode = mode;
    SDL_LockMutex(mtx);
    n_lines = 0;
    partial[0] = 0;
    prog_pct = -1;
    SDL_UnlockMutex(mtx);
    SDL_SetAtomicInt(&state, RUNNING);
    SDL_Thread *t = SDL_CreateThread(worker, "setup", NULL);
    if (t) SDL_DetachThread(t);
    else SDL_SetAtomicInt(&state, FAILED);
}

static SDL_AtomicInt key_asked;   /* a Virtual Console import needed a key: asked for once */

/* a file dialog that could not open (NULL from SDL; an empty list is a cancel): said under
 * the buttons. On a Steam Deck in Game Mode there may be none. */
static char dialog_error[160];

static void dialog_failed(void)
{
    SDL_Log("file dialog: %s", SDL_GetError());
    snprintf(dialog_error, sizeof dialog_error, "No file dialog could open. On a Steam Deck, add discs in Desktop Mode.");
}

static void SDLCALL on_pick(void *userdata, const char *const *files, int filter)
{
    (void)userdata;
    (void)filter;
    if (!files) { dialog_failed(); return; }
    dialog_error[0] = 0;
    if (!files[0]) return;                           /* cancelled */
    snprintf(input_path, sizeof input_path, "%s", files[0]);
    SDL_SetAtomicInt(&key_asked, 0);
    start(MODE_IMPORT);
}

/* the key file the player chose (vc.h): the same import again, with it */
static void SDLCALL on_key(void *userdata, const char *const *files, int filter)
{
    (void)userdata;
    (void)filter;
    static char key_path[1024];
    if (!files) { dialog_failed(); return; }
    if (!files[0]) return;
    snprintf(key_path, sizeof key_path, "%s", files[0]);
    vc_keys_file = key_path;
    start(MODE_IMPORT);
}

static bool exists(const char *name)
{
    char path[1100];
    snprintf(path, sizeof path, "%s%s", base_dir, name);
    return SDL_GetPathInfo(path, NULL);
}

typedef struct Button { float x, y, w, h; const char *label; int id; } Button;
enum { B_CHOOSE = 1, B_FOLDER, B_PLAY, B_REBUILD, B_QUIT, B_UPDATE };

static bool hit(const Button *b, float mx, float my)
{
    return mx >= b->x && mx < b->x + b->w && my >= b->y && my < b->y + b->h;
}

static void draw_button(SDL_Renderer *r, const Button *b, bool hover)
{
    SDL_FRect rc = {b->x, b->y, b->w, b->h};
    SDL_SetRenderDrawColor(r, hover ? 120 : 70, hover ? 40 : 24, hover ? 40 : 30, 255);
    SDL_RenderFillRect(r, &rc);
    SDL_SetRenderDrawColor(r, 200, 170, 120, 255);
    SDL_RenderRect(r, &rc);
    float tx = b->x + (b->w - 8.0f * (float)strlen(b->label)) / 2;
    SDL_RenderDebugText(r, tx, b->y + (b->h - 8) / 2, b->label);
}

/* The game; from the AppImage, the AppImage again, which starts the game it built (and is
 * the new one after an update). On Windows it starts and Setup quits; on Linux Setup becomes
 * the game once its window is closed (play_after, exec): the same process, which Steam keeps
 * track of (it closes what a game leaves running when the game's own process ends). */
#ifndef _WIN32
static char play_after[1100];
#endif
static const char *play_arg;   /* --autotest-play: the game's --autotest */

static bool play(void)
{
    char exe[1100];
    if (rondo_appimage()) snprintf(exe, sizeof exe, "%s", rondo_appimage());
    else snprintf(exe, sizeof exe, "%s" GAME_PROGRAM, base_dir);
#ifdef _WIN32
    const char *args[] = {exe, play_arg, NULL};
    SDL_Process *p = SDL_CreateProcess(args, false);
    if (!p) return false;
    SDL_DestroyProcess(p);
#else
    if (!SDL_GetPathInfo(exe, NULL)) return false;
    snprintf(play_after, sizeof play_after, "%s", exe);
#endif
    return true;
}

int main(int argc, char **argv)
{
    /* --rebuild: start rebuilding at once (the AppImage, when it is newer than the game) */
    bool auto_rebuild = false;
    /* --autotest-play (the game's --autotest-setup): note this process next to the game, then
     * Play, which runs the game's --autotest */
    bool autotest_play = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rebuild")) auto_rebuild = true;
        if (!strcmp(argv[i], "--autotest-play")) { autotest_play = true; play_arg = "--autotest"; }
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    /* where the game is built: this folder, or the AppImage's own (programs.h) */
    snprintf(base_dir, sizeof base_dir, "%s", rondo_home_dir());
    SDL_CreateDirectory(base_dir);
    mtx = SDL_CreateMutex();
    setup_line_hook = gui_line;
    setup_progress_hook = gui_progress;
    update_check_start();   /* a newer version on GitHub? (answered in the loop: http_poll) */

    SDL_Window *win = SDL_CreateWindow("RondoRecomp Setup", 1280, 800, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!ren) return 1;
    SDL_SetRenderLogicalPresentation(ren, 640, 400, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    bool running = true;
    float mx = 0, my = 0;
    Uint64 t0 = SDL_GetTicks();
    if (auto_rebuild && exists("game")) start(MODE_REBUILD);
    while (running) {
        int st = SDL_GetAtomicInt(&state);
        /* the rebuild the AppImage asked for is done: on to the game */
        if (auto_rebuild && st == DONE && run_mode == MODE_REBUILD && play()) break;
        static int frames;
        if (autotest_play && ++frames == 30) {   /* the window has been up for half a second */
            char log_path[1100], line[200];
            snprintf(log_path, sizeof log_path, "%ssetup_autotest.log", base_dir);
            size_t len = 0;
            char *before = SDL_LoadFile(log_path, &len);
#ifdef _WIN32
            snprintf(line, sizeof line, "%sSetup: window up, playing\n", before ? before : "");
#else
            snprintf(line, sizeof line, "%sSetup: process %d, window up, playing\n", before ? before : "", (int)getpid());
#endif
            SDL_free(before);
            SDL_SaveFile(log_path, line, strlen(line));
            if (play()) break;
        }
        /* a Virtual Console release that no key found opens: ask for the key file, once */
        if (st == FAILED && run_mode == MODE_IMPORT && run_result == 4 && SDL_CompareAndSwapAtomicInt(&key_asked, 0, 1)) {
            static const SDL_DialogFileFilter keys[] = {
                {"Key file (keys.txt, otp.bin, keys.bin)", "txt;bin"}, {"All files", "*"}};
            SDL_ShowOpenFileDialog(on_key, NULL, win, keys, 2, NULL, false);
        }
        bool have_game = exists("game"), have_exe = exists(GAME_PROGRAM);
        http_poll();
        const UpdateInfo *news = NULL;
        bool can_update = update_state(&news, NULL) == UPD_AVAILABLE && !updated && have_game;
        static char update_label[40];
        if (can_update) snprintf(update_label, sizeof update_label, "Update to %s", news->version);
        Button buttons[6];
        int nb = 0;
        if (can_update && st != RUNNING) buttons[nb++] = (Button){456, 10, 160, 22, update_label, B_UPDATE};
        if (st == IDLE || st == DONE || st == FAILED) {
            /* the disc: a zip (or a .7z, .cue or .chd file), or the folder it was unzipped to */
            buttons[nb++] = (Button){24, 340, 140, 32, "Disc zip...", B_CHOOSE};
            buttons[nb++] = (Button){172, 340, 140, 32, "Disc folder...", B_FOLDER};
            if (have_exe && st != FAILED) buttons[nb++] = (Button){320, 340, 90, 32, "Play", B_PLAY};
            if (have_game && st != DONE) buttons[nb++] = (Button){418, 340, 100, 32, "Rebuild", B_REBUILD};
            buttons[nb++] = (Button){526, 340, 90, 32, "Quit", B_QUIT};
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            SDL_ConvertEventToRenderCoordinates(ren, &ev);
            if (ev.type == SDL_EVENT_QUIT) {
                if (st != RUNNING) running = false;
            } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                mx = ev.motion.x;
                my = ev.motion.y;
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT) {
                for (int i = 0; i < nb; i++) {
                    if (!hit(&buttons[i], ev.button.x, ev.button.y)) continue;
                    if (buttons[i].id == B_CHOOSE) {
                        static const SDL_DialogFileFilter filters[] = {
                            {"Disc image (zip, 7z, cue or chd), Wii VC (wad)", "zip;7z;cue;chd;wad"}, {"All files", "*"}};
                        SDL_ShowOpenFileDialog(on_pick, NULL, win, filters, 2, NULL, false);
                    } else if (buttons[i].id == B_FOLDER) {
                        SDL_ShowOpenFolderDialog(on_pick, NULL, win, NULL, false);
                    } else if (buttons[i].id == B_REBUILD) {
                        start(MODE_REBUILD);
                    } else if (buttons[i].id == B_UPDATE) {
                        start(MODE_UPDATE);
                    } else if (buttons[i].id == B_PLAY) {
                        if (play()) running = false;
                    } else if (buttons[i].id == B_QUIT) {
                        running = false;
                    }
                }
            }
        }

        SDL_SetRenderDrawColor(ren, 18, 10, 24, 255);
        SDL_RenderClear(ren);
        SDL_SetRenderDrawColor(ren, 230, 200, 140, 255);
        SDL_RenderDebugText(ren, 24, 16, "RondoRecomp Setup");
        SDL_SetRenderDrawColor(ren, 170, 160, 180, 255);
        SDL_RenderDebugText(ren, 24, 30, "Turns your own copy of Castlevania: Rondo of Blood into a native PC game.");
        if (can_update && st == IDLE) {
            char line[120];
            snprintf(line, sizeof line, "RondoRecomp %s is available (this is " RONDO_VERSION "): click Update.", news->version);
            SDL_SetRenderDrawColor(ren, 255, 214, 120, 255);
            SDL_RenderDebugText(ren, 24, 44, line);
            SDL_SetRenderDrawColor(ren, 170, 160, 180, 255);
        }

        SDL_LockMutex(mtx);
        if (st == IDLE) {
            SDL_RenderDebugText(ren, 24, 60, "1. Your disc image: a .cue with its .bin/.img/.iso/.wav tracks, or a .chd.");
            SDL_RenderDebugText(ren, 24, 72, "   Zipped: click \"Disc zip...\" and pick the zip (or the .cue or .chd).");
            SDL_RenderDebugText(ren, 24, 84, "   Unzipped: click \"Disc folder...\" and pick the folder it is in.");
            SDL_RenderDebugText(ren, 24, 96, "   The original Japanese disc or the English translation: either works.");
            SDL_RenderDebugText(ren, 24, 112, "2. Setup checks the disc, extracts it, recompiles the game code");
            if (rondo_appimage()) {
                char where[100];
                snprintf(where, sizeof where, "   in %.70s", base_dir);
                SDL_RenderDebugText(ren, 24, 124, "   and builds the game (about a minute; longer on a Steam Deck)");
                SDL_RenderDebugText(ren, 24, 136, where);
            } else {
                SDL_RenderDebugText(ren, 24, 124, "   and builds " GAME_PROGRAM " in this folder. This takes about a minute.");
            }
            SDL_RenderDebugText(ren, 24, 152, "3. Click Play.");
            SDL_RenderDebugText(ren, 24, 164, "Both languages: choose the other disc afterwards; it is added next");
            SDL_RenderDebugText(ren, 24, 176, "to the first, and the game's menu switches (Language).");
            SDL_RenderDebugText(ren, 24, 200, "Rebuild: recompile again, including code " GAME_PROGRAM " recorded");
            SDL_RenderDebugText(ren, 24, 212, "while you played (misses.txt).");
            SDL_RenderDebugText(ren, 24, 236, "No game files are included with RondoRecomp.");
            SDL_RenderDebugText(ren, 24, 260, "Virtual Console: a Wii .wad (Japanese) or a Wii U title (English: its");
            SDL_RenderDebugText(ren, 24, 272, "folder, a zip or a 7z). Encrypted: Setup needs your console's key file");
            SDL_RenderDebugText(ren, 24, 284, "(keys.txt as Cemu's, otp.bin, keys.bin), or a .wad installed in Dolphin.");
        } else {
            int first = n_lines > 30 ? n_lines - 30 : 0;
            float y = 52;
            SDL_SetRenderDrawColor(ren, 200, 200, 200, 255);
            for (int i = first; i < n_lines; i++, y += 9) SDL_RenderDebugText(ren, 24, y, lines[i]);
            if (st == RUNNING) {
                static const char spin[] = "|/-\\";
                char busy[160];
                Uint64 t = (SDL_GetTicks() - t0) / 150;
                if (prog_pct >= 0 && prog_pct < 100) {
                    snprintf(busy, sizeof busy, "%c %s", spin[t % 4], prog_what);
                    SDL_FRect bar = {24, 322, 592, 8}, fill = {24, 322, 5.92f * (float)prog_pct, 8};
                    SDL_SetRenderDrawColor(ren, 60, 50, 70, 255);
                    SDL_RenderFillRect(ren, &bar);
                    SDL_SetRenderDrawColor(ren, 200, 150, 80, 255);
                    SDL_RenderFillRect(ren, &fill);
                } else {
                    snprintf(busy, sizeof busy, "%c working...", spin[t % 4]);
                }
                SDL_SetRenderDrawColor(ren, 230, 200, 140, 255);
                SDL_RenderDebugText(ren, 24, 344, busy);
            }
        }
        SDL_UnlockMutex(mtx);
        if (st == DONE) {
            SDL_SetRenderDrawColor(ren, 140, 220, 140, 255);
            SDL_RenderDebugText(ren, 24, 324, run_mode != MODE_UPDATE ? "Done. " GAME_PROGRAM " is ready."
                                              : run_result == 3        ? "This is the latest version."
                                                                       : "Updated. Click Play.");
        }
        if (dialog_error[0] && st != RUNNING) {
            SDL_SetRenderDrawColor(ren, 240, 120, 110, 255);
            SDL_RenderDebugText(ren, 24, 312, dialog_error);
        }
        if (st == FAILED) {
            SDL_SetRenderDrawColor(ren, 240, 120, 110, 255);
            SDL_RenderDebugText(ren, 24, 324, run_mode == MODE_IMPORT && run_result == 4
                                                  ? "Setup needs your console's key file: choose it (Setup then goes on)."
                                                  : "Setup did not finish; see the messages above.");
        }
        for (int i = 0; i < nb; i++) draw_button(ren, &buttons[i], hit(&buttons[i], mx, my));
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
    SDL_Quit();
#ifndef _WIN32
    if (play_after[0]) {   /* play(): Setup becomes the game */
        const char *args[] = {play_after, play_arg, NULL};
        execv(play_after, (char *const *)args);
        fprintf(stderr, "cannot start %s\n", play_after);
        return 1;
    }
#endif
    return 0;
}
