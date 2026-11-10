/* RondoRecomp Setup (command line): turns the player's own disc image into rondo.exe (Windows)
 * or rondo (Linux).
 *
 * usage: rondo_setup <disc.zip | disc.cue | folder> [--out DIR] [--sdk DIR] [--zig ZIG]
 *                    [--allow-unknown] [--no-build]
 *        rondo_setup --rebuild [--out DIR] ...   (reuse DIR/game, add DIR/misses.txt, rebuild)
 *        rondo_setup --update [--check]          (the latest release from GitHub: install.c)
 *
 *   1. import    read the zip (or a .cue, or a folder with the unzipped image: its .cue files,
 *                subfolders too), find a CUE sheet whose files are all present
 *   2. identify  PC Engine CD signature + boot-sector fingerprint (refuses unknown discs
 *                unless --allow-unknown)
 *   3. extract   to DIR/game (staged in DIR/game.tmp, renamed only after the check passes)
 *   4. generate  DIR/build/gen: the game's code recompiled to C (see docs/recompiler.md),
 *                using DIR/misses.txt from earlier play sessions as extra entry points
 *   5. build     zig cc: SDK runtime sources + generated code + SDL3 -> DIR/rondo.exe (DIR/rondo)
 *
 * Defaults: DIR = folder of this program, SDK = DIR/sdk, ZIG = DIR/zig/zig.exe (DIR/zig/zig).
 * Each system's Setup builds the game for its own system.
 */
#include <SDL3/SDL.h>
#ifndef SETUP_GUI
#include <SDL3/SDL_main.h>
#endif
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pce/discid.h"
#include "../frontend/programs.h"
#include "../recomp/gen.h"
#include "setup.h"
#include "zip.h"

#define SETUP_VERSION RONDO_VERSION

#ifdef _WIN32
#define ZIG_PROGRAM "zig/zig.exe"
#define ZIG_TARGET "x86_64-windows-gnu"
#define SDL_LINK "libSDL3.dll.a"   /* import library, in sdk/lib */
#else
#define ZIG_PROGRAM "zig/zig"
/* glibc 2.35 (Ubuntu 22.04, SteamOS 3 and newer): zig links against that version's symbols,
 * whichever the machine building has */
#define ZIG_TARGET "x86_64-linux-gnu.2.35"
#define SDL_LINK "libSDL3.so.0"    /* the library itself, in sdk/lib */
#endif

/* ---- output hooks (stdout for the command line; the GUI installs its own) ------------- */

static void cli_line(const char *line) { fputs(line, stdout); }
static void cli_progress(const char *what, int pct)
{
    printf("\r    %s %3d%%", what, pct);
    if (pct >= 100) printf("\n");
    fflush(stdout);
}
void (*setup_line_hook)(const char *text) = cli_line;
void (*setup_progress_hook)(const char *what, int pct) = cli_progress;

void say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    setup_line_hook(buf);
}

#define MAX_FILES 128

char g_err[512];

static bool ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && SDL_strcasecmp(s + a - b, suffix) == 0;
}

void join(char *out, size_t n, const char *a, const char *b)
{
    size_t l = strlen(a);
    bool sep = l && (a[l - 1] == '/' || a[l - 1] == '\\');
    const char *native = strchr(a, '\\') ? "\\" : "/";   /* match the base path's separators */
    snprintf(out, n, "%s%s%s", a, (l && !sep) ? native : "", b);
}

static void dirname_of(char *out, size_t n, const char *path)
{
    snprintf(out, n, "%s", path);
    char *s = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > s) s = b;
    if (s) s[1] = 0; else out[0] = 0;
}

static const char *basename_of(const char *path)
{
    const char *s = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b > s) s = b;
    return s ? s + 1 : path;
}

/* Absolute path with native separators, so child processes resolve it reliably. */
static void absolute(char *path, size_t n)
{
#ifdef _WIN32
    char full[1024];
    if (_fullpath(full, path, sizeof full)) snprintf(path, n, "%s", full);
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
#else
    (void)path; (void)n;
#endif
}

/* ---- file system helpers ------------------------------------------------------ */

static SDL_EnumerationResult SDLCALL remove_visit(void *ctx, const char *dir, const char *name)
{
    (void)ctx;
    char path[1024];
    join(path, sizeof path, dir, name);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY)
        SDL_EnumerateDirectory(path, remove_visit, NULL);
    SDL_RemovePath(path);
    return SDL_ENUM_CONTINUE;
}

void remove_tree(const char *path)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return;
    if (info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory(path, remove_visit, NULL);
    SDL_RemovePath(path);
}

bool make_dirs_for(const char *file)
{
    char dir[1024];
    dirname_of(dir, sizeof dir, file);
    return !dir[0] || SDL_CreateDirectory(dir);
}

/* ---- CUE sheets ---------------------------------------------------------------- */

/* Collect the file names a CUE sheet references (FILE "name" TYPE). */
static int cue_files(const char *text, char names[][512], int max)
{
    int n = 0;
    const char *p = text;
    while (*p && n < max) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        const char *end = p;
        if (*p) p++;
        while (line < end && isspace((unsigned char)*line)) line++;
        if (end - line < 5 || SDL_strncasecmp(line, "FILE", 4) || !isspace((unsigned char)line[4])) continue;
        line += 4;
        while (line < end && isspace((unsigned char)*line)) line++;
        const char *s = line, *e;
        if (*s == '"') {
            s++;
            e = s;
            while (e < end && *e != '"') e++;
        } else {
            e = s;
            while (e < end && !isspace((unsigned char)*e)) e++;
        }
        size_t len = (size_t)(e - s);
        if (len == 0 || len >= 512) continue;
        memcpy(names[n], s, len);
        names[n][len] = 0;
        for (char *c = names[n]; *c; c++) if (*c == '\\') *c = '/';
        n++;
    }
    return n;
}

/* ---- import --------------------------------------------------------------------- */

static void progress(void *ctx, uint64_t done, uint64_t total)
{
    static int last = -1;
    int pct = total ? (int)(done * 100 / total) : 100;
    if (pct != last && (pct % 10 == 0 || pct == 100)) {
        setup_progress_hook((const char *)ctx, pct);
        last = pct;
    }
    if (done == total) last = -1;
}

/* Extract one candidate CUE (and its files) from the zip into `stage`; returns the staged cue path. */
static bool import_zip_cue(Zip *z, const ZipEntry *cue, const char *stage, char *cue_out, size_t cue_out_len)
{
    char *text = zip_read_text(z, cue, 1 << 16, g_err, sizeof g_err);
    if (!text) return false;
    static char names[MAX_FILES][512];
    int n = cue_files(text, names, MAX_FILES);
    char cdir[512];
    dirname_of(cdir, sizeof cdir, cue->name);
    const ZipEntry *files[MAX_FILES];
    for (int i = 0; i < n; i++) {
        char full[1100];
        snprintf(full, sizeof full, "%s%s", cdir, names[i]);
        files[i] = zip_find(z, full);
        if (!files[i]) {
            snprintf(g_err, sizeof g_err, "%s refers to \"%s\", which is not in the zip", cue->name, names[i]);
            free(text);
            return false;
        }
    }
    if (n == 0) { snprintf(g_err, sizeof g_err, "%s lists no track files", cue->name); free(text); return false; }

    remove_tree(stage);
    if (!SDL_CreateDirectory(stage)) { snprintf(g_err, sizeof g_err, "cannot create %s", stage); free(text); return false; }
    join(cue_out, cue_out_len, stage, basename_of(cue->name));
    FILE *f = fopen(cue_out, "wb");
    if (!f || fwrite(text, 1, strlen(text), f) != strlen(text)) {
        if (f) fclose(f);
        snprintf(g_err, sizeof g_err, "cannot write %s", cue_out);
        free(text);
        return false;
    }
    fclose(f);
    free(text);
    for (int i = 0; i < n; i++) {
        char dest[1100];
        join(dest, sizeof dest, stage, names[i]);
        if (!make_dirs_for(dest)) { snprintf(g_err, sizeof g_err, "cannot create folder for %s", dest); return false; }
        if (!zip_extract(z, files[i], dest, progress, (void *)names[i], g_err, sizeof g_err)) return false;
    }
    return true;
}

static bool import_cue(const char *cue_path, const char *stage, char *cue_out, size_t cue_out_len)
{
    size_t len = 0;
    char *text = SDL_LoadFile(cue_path, &len);
    if (!text) { snprintf(g_err, sizeof g_err, "cannot read %s", cue_path); return false; }
    static char names[MAX_FILES][512];
    int n = cue_files(text, names, MAX_FILES);
    SDL_free(text);
    char cdir[1024];
    dirname_of(cdir, sizeof cdir, cue_path);
    remove_tree(stage);
    if (!SDL_CreateDirectory(stage)) { snprintf(g_err, sizeof g_err, "cannot create %s", stage); return false; }
    join(cue_out, cue_out_len, stage, basename_of(cue_path));
    if (!SDL_CopyFile(cue_path, cue_out)) { snprintf(g_err, sizeof g_err, "cannot copy %s", cue_path); return false; }
    for (int i = 0; i < n; i++) {
        char src[1100], dest[1100];
        join(src, sizeof src, cdir, names[i]);
        join(dest, sizeof dest, stage, names[i]);
        say("    %s\n", names[i]);
        if (!make_dirs_for(dest) || !SDL_CopyFile(src, dest)) {
            snprintf(g_err, sizeof g_err, "cannot copy %s", src);
            return false;
        }
    }
    return true;
}

/* ---- generate + build ------------------------------------------------------------ */

static void gen_log(const char *line) { say("  %s\n", line); }

/* Recompile the game's code from the extracted disc into gen_dir. */
static bool generate(const char *game_cue, const char *gen_dir, const char *misses, uint64_t fingerprint)
{
    remove_tree(gen_dir);
    if (!SDL_CreateDirectory(gen_dir)) { snprintf(g_err, sizeof g_err, "cannot create %s", gen_dir); return false; }
    static char build_id[96];
    snprintf(build_id, sizeof build_id, "setup " SETUP_VERSION ", disc %016llX", (unsigned long long)fingerprint);
    GenOptions go = {0};
    go.cue = game_cue;
    go.out_dir = gen_dir;
    go.build_id = build_id;
    if (misses && SDL_GetPathInfo(misses, NULL)) {
        go.seed_files[0] = misses;
        say("  using entry points recorded while playing (%s)\n", misses);
    }
    GenStats gs;
    return gen_run(&go, &gs, gen_log, g_err, sizeof g_err);
}

/* the compiler: its messages are passed on (and no console window opens on Windows) */
static bool run(const char *const *args) { return setup_run(args); }

#ifdef _WIN32
/* The player's own icon for rondo.exe: icon.ico, else icon.png, next to Setup (RondoRecomp
 * ships none: no game artwork is included). A PNG goes into an .ico as it is (Windows takes
 * PNG images in icons). Writes build/rondo.rc for the compiler; false if there is no icon.
 * Windows only: on Linux the launcher or Steam gives the game its picture. */
static bool icon_resource(const char *out, char *rc_path, size_t n)
{
    char ico[1100], png[1100], made[1100];
    join(ico, sizeof ico, out, "icon.ico");
    join(png, sizeof png, out, "icon.png");
    const char *use = NULL;
    if (SDL_GetPathInfo(ico, NULL)) {
        use = ico;
    } else if (SDL_GetPathInfo(png, NULL)) {
        size_t len = 0;
        uint8_t *data = SDL_LoadFile(png, &len);
        if (!data || len < 24 || memcmp(data, "\x89PNG", 4)) {
            say("  icon.png is not a PNG image; rondo.exe keeps the default icon\n");
            SDL_free(data);
            return false;
        }
        uint32_t w = (uint32_t)data[16] << 24 | data[17] << 16 | data[18] << 8 | data[19];
        uint32_t h = (uint32_t)data[20] << 24 | data[21] << 16 | data[22] << 8 | data[23];
        if (w == 0 || h == 0 || w > 256 || h > 256) {
            say("  icon.png must be at most 256 x 256 pixels; rondo.exe keeps the default icon\n");
            SDL_free(data);
            return false;
        }
        uint8_t head[22] = {0, 0, 1, 0, 1, 0,                         /* ICONDIR: an icon, one image */
                            (uint8_t)(w & 255), (uint8_t)(h & 255), 0, 0, 1, 0, 32, 0,
                            (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24),
                            22, 0, 0, 0};
        join(made, sizeof made, out, "build/icon.ico");
        SDL_IOStream *f = SDL_IOFromFile(made, "wb");
        bool ok = f && SDL_WriteIO(f, head, sizeof head) == sizeof head && SDL_WriteIO(f, data, len) == len;
        if (f) SDL_CloseIO(f);
        SDL_free(data);
        if (!ok) return false;
        use = made;
    } else {
        return false;
    }
    char quoted[1100];
    snprintf(quoted, sizeof quoted, "%s", use);
    for (char *c = quoted; *c; c++) if (*c == '\\') *c = '/';
    join(rc_path, n, out, "build/rondo.rc");
    /* zig's resource compiler cannot open a path that mixes \ and / */
    for (char *c = rc_path; *c; c++) if (*c == '/') *c = '\\';
    char text[1300];
    snprintf(text, sizeof text, "1 ICON \"%s\"\n", quoted);
    if (!SDL_SaveFile(rc_path, text, strlen(text))) return false;
    say("  rondo.exe gets your icon (%s)\n", use == ico ? "icon.ico" : "icon.png");
    return true;
}
#endif

static bool build(const char *sdk, const char *zig, const char *out, const char *gen_dir)
{
    /* the list of runtime sources ships with the SDK */
    char list_path[1024];
    join(list_path, sizeof list_path, sdk, "sources.txt");
    size_t len = 0;
    char *list = SDL_LoadFile(list_path, &len);
    if (!list) { snprintf(g_err, sizeof g_err, "SDK not found (%s)", list_path); return false; }

    static char storage[600][1024];
    const char *args[700];
    int n = 0, s = 0;
    static char inc_rt[1100], inc_sdl[1100], libdir[1100], exe[1100];
    join(inc_rt, sizeof inc_rt, sdk, "src/runtime");
    join(inc_sdl, sizeof inc_sdl, sdk, "include");
    join(libdir, sizeof libdir, sdk, "lib");
    /* linked in build/ first, then put in place (the one there may be running) */
    join(exe, sizeof exe, out, "build/" GAME_PROGRAM);
    SDL_RemovePath(exe);
    static char opt_i1[1200], opt_i2[1200], opt_i3[1200], opt_l[1200];
    snprintf(opt_i1, sizeof opt_i1, "-I%s", inc_rt);
    snprintf(opt_i2, sizeof opt_i2, "-I%s", inc_sdl);
    snprintf(opt_i3, sizeof opt_i3, "-I%s", gen_dir);
    join(opt_l, sizeof opt_l, libdir, SDL_LINK);   /* passed by path */

    args[n++] = zig;
    args[n++] = "cc";
    args[n++] = "-target";
    args[n++] = ZIG_TARGET;
    args[n++] = "-O2";
    args[n++] = "-std=c11";
#ifndef _WIN32
    args[n++] = "-D_DEFAULT_SOURCE";          /* glibc: strdup, strcasecmp (rcheevos) under -std=c11 */
#endif
    args[n++] = "-w";
    args[n++] = "-s";                          /* no debug info / .pdb */
    args[n++] = opt_i1;
    args[n++] = opt_i2;
    args[n++] = opt_i3;
    /* the SDK's sources, and options for them: -I<path relative to the SDK>, -D<define> */
    int sdk_files = 0;
    for (char *line = strtok(list, "\r\n"); line && s < 200; line = strtok(NULL, "\r\n")) {
        if (!*line || *line == '#') continue;
        if (!strncmp(line, "-I", 2)) {
            snprintf(storage[s], sizeof storage[s], "-I");
            join(storage[s] + 2, sizeof storage[s] - 2, sdk, line + 2);
        } else if (!strncmp(line, "-D", 2)) {
            snprintf(storage[s], sizeof storage[s], "%s", line);
        } else {
            join(storage[s], sizeof storage[s], sdk, line);
            sdk_files++;
        }
        args[n++] = storage[s++];
    }
    int gen_count = 0;
    int count = 0;
    char **gen_files = SDL_GlobDirectory(gen_dir, "*.c", 0, &count);
    for (int i = 0; gen_files && i < count && s < 600; i++) {
        join(storage[s], sizeof storage[s], gen_dir, gen_files[i]);
        args[n++] = storage[s++];
        gen_count++;
    }
    SDL_free(gen_files);
#ifdef _WIN32
    /* the icon: compiled to a .res first (zig cc cannot build an .rc together with -s) */
    static char rc[1100], res[1100];
    if (icon_resource(out, rc, sizeof rc)) {
        snprintf(res, sizeof res, "%s", rc);
        snprintf(res + strlen(res) - 3, 5, ".res");
        const char *rc_args[] = {zig, "rc", "-fo", res, rc, NULL};
        if (run(rc_args)) args[n++] = res;
        else say("  the icon could not be built (%s); rondo.exe keeps the default icon\n", g_err);
    }
    args[n++] = opt_l;
    args[n++] = "-lwinhttp";                 /* RetroAchievements' server (tools/frontend/http.c) */
    args[n++] = "-Wl,--subsystem,windows";   /* GUI program: zig cc ignores -mwindows */
#else
    args[n++] = opt_l;
    args[n++] = "-lm";
    args[n++] = "-Wl,-rpath,$ORIGIN";        /* libSDL3.so.0 is found next to rondo */
#endif
    args[n++] = "-o";
    args[n++] = exe;
    args[n] = NULL;
    SDL_free(list);

    /* keep the compiler's cache next to the build instead of in the user profile */
    char cache[1100];
    join(cache, sizeof cache, out, "build/zig-cache");
    SDL_CreateDirectory(cache);
    SDL_Environment *env = SDL_GetEnvironment();
    SDL_SetEnvironmentVariable(env, "ZIG_GLOBAL_CACHE_DIR", cache, true);
    SDL_SetEnvironmentVariable(env, "ZIG_LOCAL_CACHE_DIR", cache, true);
    say("    compiling %d source files (runtime + recompiled game code; the first build also prepares\n"
           "    the C library, which takes longer)\n", sdk_files + gen_count);
    if (!run(args)) return false;
    char final_exe[1100];
    join(final_exe, sizeof final_exe, out, GAME_PROGRAM);
    if (!setup_replace(out, exe, final_exe)) return false;

    /* the libraries the game loads, next to it */
#ifdef _WIN32
    static const char *dlls[] = {"SDL3.dll", "libiconv-2.dll"};
    const char *dll_dir = "bin";
#else
    static const char *dlls[] = {"libSDL3.so.0"};
    const char *dll_dir = "lib";
#endif
    for (size_t i = 0; i < sizeof dlls / sizeof dlls[0]; i++) {
        char lib[1100], src[1200], dst[1100];
        join(lib, sizeof lib, sdk, dll_dir);   /* (not into src itself: glibc's snprintf clears it first) */
        join(src, sizeof src, lib, dlls[i]);
        join(dst, sizeof dst, out, dlls[i]);
        SDL_PathInfo si, di;
        if (!SDL_GetPathInfo(src, &si)) continue;
        /* already there (e.g. Setup itself runs from this folder with the same DLL loaded) */
        if (SDL_GetPathInfo(dst, &di) && di.size == si.size) continue;
        char fresh[1200];
        snprintf(fresh, sizeof fresh, "%s.new", dst);
        if (!SDL_CopyFile(src, fresh)) {
            snprintf(g_err, sizeof g_err, "cannot copy %s", src);
            return false;
        }
        if (!setup_replace(out, fresh, dst)) return false;
    }
    return true;
}

/* ---- languages ------------------------------------------------------------------ */

/* Each disc is kept in game/en or game/ja, by the language of its text, so a player can
 * install both and switch in the game's menu. */
static const char *lang_dir(DiscLanguage l) { return l == DISC_LANG_JA ? "ja" : "en"; }
static const char *lang_name(DiscLanguage l)
{
    return l == DISC_LANG_JA ? "the Japanese original" : "the English patch (fan translation)";
}

/* The .cue of the disc installed in game/<sub>, if there is one. */
static bool installed_cue(const char *game, const char *sub, char *out, size_t n)
{
    char dir[1100];
    join(dir, sizeof dir, game, sub);
    int count = 0;
    char **cues = SDL_GlobDirectory(dir, "*.cue", SDL_GLOB_CASEINSENSITIVE, &count);
    bool ok = false;
    for (int i = 0; cues && i < count && !ok; i++) {
        if (strchr(cues[i], '/') || strchr(cues[i], '\\')) continue;     /* this folder only */
        join(out, n, dir, cues[i]);
        ok = true;
    }
    SDL_free(cues);
    return ok;
}

typedef struct { const char *from, *to; bool ok; } MoveCtx;

static SDL_EnumerationResult SDLCALL move_visit(void *userdata, const char *dir, const char *name)
{
    MoveCtx *m = userdata;
    char src[1100], dst[1100];
    join(src, sizeof src, dir, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(src, &info) || info.type != SDL_PATHTYPE_FILE) return SDL_ENUM_CONTINUE;
    join(dst, sizeof dst, m->to, name);
    if (!SDL_RenamePath(src, dst)) m->ok = false;
    return SDL_ENUM_CONTINUE;
}

/* Setup up to 0.6 extracted its one disc straight into game/: move those files into the
 * disc's language folder, so a second language can be added next to it. */
static bool migrate_single_disc(const char *game)
{
    char cue[1200];
    if (!installed_cue(game, "", cue, sizeof cue)) return true;
    DiscLanguage l = discid_language(cue);
    if (l == DISC_LANG_UNKNOWN) l = DISC_LANG_EN;
    char to[1100];
    join(to, sizeof to, game, lang_dir(l));
    remove_tree(to);
    if (!SDL_CreateDirectory(to)) return false;
    MoveCtx m = {game, to, true};
    SDL_EnumerateDirectory(game, move_visit, &m);
    say("  moved the %s disc installed earlier to %s\n", l == DISC_LANG_JA ? "Japanese" : "English", to);
    return m.ok;
}

/* The disc the game code is recompiled from: the English one if installed (its capture run
 * sees the most code), else the Japanese one. The other runs the same code, checked as it
 * goes (99% native). */
static bool code_disc(const char *game, char *out, size_t n)
{
    return installed_cue(game, "en", out, n) || installed_cue(game, "ja", out, n) || installed_cue(game, "", out, n);
}

/* ---- main --------------------------------------------------------------------- */

static int fail(const char *step)
{
    say("\nSetup failed while %s:\n  %s\n", step, g_err);
    return 1;
}

int setup_main(int argc, char **argv)
{
    const char *input = NULL, *out_arg = NULL, *sdk_arg = NULL, *zig_arg = NULL;
    bool allow_unknown = false, do_build = true, rebuild = false, update = false, check = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out_arg = argv[++i];
        else if (!strcmp(argv[i], "--sdk") && i + 1 < argc) sdk_arg = argv[++i];
        else if (!strcmp(argv[i], "--zig") && i + 1 < argc) zig_arg = argv[++i];
        else if (!strcmp(argv[i], "--allow-unknown")) allow_unknown = true;
        else if (!strcmp(argv[i], "--no-build")) do_build = false;
        else if (!strcmp(argv[i], "--rebuild")) rebuild = true;
        else if (!strcmp(argv[i], "--update")) update = true;
        else if (!strcmp(argv[i], "--check")) check = true;
        else input = argv[i];
    }
    if (update) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", out_arg ? out_arg : rondo_home_dir());
        absolute(dir, sizeof dir);
        return setup_update(dir, check);
    }
    if (!input && !rebuild) {
        fprintf(stderr, "usage: rondo_setup <disc.zip | disc.cue | folder> [--out DIR] [--sdk DIR] [--zig ZIG] "
                        "[--allow-unknown] [--no-build]\n       rondo_setup --rebuild [--out DIR] [--sdk DIR] [--zig ZIG]\n"
                        "       rondo_setup --update [--check] [--out DIR]\n");
        return 2;
    }
    /* the game is built in home; sdk/ and zig/ are next to Setup (programs.h: both are this
     * folder but in the Linux AppImage) */
    char out[1024], sdk[1024], zig[1024], stage[1100], game[1100], gen_dir[1100], cue[1100];
    snprintf(out, sizeof out, "%s", out_arg ? out_arg : rondo_home_dir());
    if (sdk_arg) snprintf(sdk, sizeof sdk, "%s", sdk_arg); else join(sdk, sizeof sdk, rondo_tools_dir(), "sdk");
    if (zig_arg) snprintf(zig, sizeof zig, "%s", zig_arg); else join(zig, sizeof zig, rondo_tools_dir(), ZIG_PROGRAM);
    SDL_CreateDirectory(out);
    absolute(out, sizeof out);
    absolute(sdk, sizeof sdk);
    absolute(zig, sizeof zig);
    join(stage, sizeof stage, out, "game.tmp");
    join(game, sizeof game, out, "game");
    join(gen_dir, sizeof gen_dir, out, "build/gen");

    char game_cue[1200] = "", misses[1100];
    join(misses, sizeof misses, out, "misses.txt");
    DiscKind kind = DISC_INVALID;
    uint64_t fp = 0;

    if (rebuild) {
        say("RondoRecomp Setup " SETUP_VERSION " (rebuild)\n\n[1/4] Using the extracted disc in %s\n", game);
        if (!migrate_single_disc(game)) { snprintf(g_err, sizeof g_err, "cannot reorganise %s", game); return fail("rebuilding"); }
        if (!code_disc(game, game_cue, sizeof game_cue)) { snprintf(g_err, sizeof g_err, "no extracted disc in %s; run Setup with your zip first", game); return fail("rebuilding"); }
        say("\n[2/4] Checking the disc\n");
        kind = discid_identify(game_cue, &fp);
        if (kind == DISC_INVALID) { snprintf(g_err, sizeof g_err, "%s is not a readable PC Engine CD image", game_cue); return fail("checking the disc"); }
        say("  %s (fingerprint %016llX)\n", kind == DISC_RONDO ? "Castlevania: Rondo of Blood" : "unknown release",
               (unsigned long long)fp);
    } else {
        say("RondoRecomp Setup " SETUP_VERSION "\n\n[1/4] Importing %s\n", input);
        SDL_PathInfo info;
        if (ends_with(input, ".zip")) {
            Zip z;
            if (!zip_open(&z, input, g_err, sizeof g_err)) return fail("opening the zip");
            int cues = 0;
            for (int i = 0; i < z.n && kind != DISC_RONDO; i++) {
                if (!ends_with(z.entries[i].name, ".cue")) continue;
                cues++;
                say("  found %s\n", z.entries[i].name);
                if (!import_zip_cue(&z, &z.entries[i], stage, cue, sizeof cue)) { say("  skipped: %s\n", g_err); continue; }
                kind = discid_identify(cue, &fp);
                if (kind == DISC_PCE_UNKNOWN && allow_unknown) break;
                if (kind != DISC_RONDO) say("  skipped: not a recognised Rondo of Blood disc\n");
            }
            zip_close(&z);
            if (!cues) { snprintf(g_err, sizeof g_err, "the zip contains no .cue file"); remove_tree(stage); return fail("importing"); }
        } else if (ends_with(input, ".cue")) {
            if (!import_cue(input, stage, cue, sizeof cue)) { remove_tree(stage); return fail("importing"); }
            kind = discid_identify(cue, &fp);
        } else if (SDL_GetPathInfo(input, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
            /* the unzipped image: its .cue files (subfolders too), each checked where it is
             * before anything is copied */
            int count = 0, n_cues = 0;
            char **files = SDL_GlobDirectory(input, NULL, 0, &count);   /* the whole tree ("*" stops at a folder) */
            char found[1100] = "";
            for (int i = 0; files && i < count && kind != DISC_RONDO; i++) {
                if (!ends_with(files[i], ".cue")) continue;
                n_cues++;
                char path[1100];
                join(path, sizeof path, input, files[i]);
                say("  found %s\n", files[i]);
                DiscKind k = discid_identify(path, &fp);
                if (k == DISC_RONDO || (k == DISC_PCE_UNKNOWN && allow_unknown && !found[0])) {
                    snprintf(found, sizeof found, "%s", path);
                    kind = k;
                } else {
                    say("  skipped: %s\n", k == DISC_INVALID ? "not a readable PC Engine CD image (are its track files next to it?)"
                                                            : "not a recognised Rondo of Blood disc");
                }
            }
            SDL_free(files);
            if (!n_cues) { snprintf(g_err, sizeof g_err, "no .cue file in %s", input); return fail("importing"); }
            if (found[0]) {
                if (!import_cue(found, stage, cue, sizeof cue)) { remove_tree(stage); return fail("importing"); }
                kind = discid_identify(cue, &fp);
            }
        } else {
            snprintf(g_err, sizeof g_err, "expected a .zip file, a .cue file or a folder");
            return fail("importing");
        }

        say("\n[2/4] Checking the disc\n");
        if (kind == DISC_INVALID || (kind == DISC_PCE_UNKNOWN && !allow_unknown)) {
            remove_tree(stage);
            snprintf(g_err, sizeof g_err, kind == DISC_INVALID
                     ? "no readable PC Engine CD image found"
                     : "this PC Engine CD is not a recognised Rondo of Blood release (fingerprint %016llX); "
                       "use --allow-unknown to try it anyway", (unsigned long long)fp);
            return fail("checking the disc");
        }
        DiscLanguage lang = discid_language(cue);
        if (lang == DISC_LANG_UNKNOWN) lang = DISC_LANG_EN;
        say("  %s, %s (fingerprint %016llX)\n", kind == DISC_RONDO ? "Castlevania: Rondo of Blood" : "unknown release",
            lang_name(lang), (unsigned long long)fp);
        SDL_CreateDirectory(game);
        if (!migrate_single_disc(game)) {
            remove_tree(stage);
            snprintf(g_err, sizeof g_err, "cannot reorganise %s", game);
            return fail("extracting");
        }
        char dest[1200];
        join(dest, sizeof dest, game, lang_dir(lang));
        remove_tree(dest);
        if (!SDL_RenamePath(stage, dest)) {
            snprintf(g_err, sizeof g_err, "cannot move %s to %s: %s", stage, dest, SDL_GetError());
            return fail("extracting");
        }
        say("  extracted to %s\n", dest);
        char other[1200];
        DiscLanguage ol = lang == DISC_LANG_JA ? DISC_LANG_EN : DISC_LANG_JA;
        if (installed_cue(game, lang_dir(ol), other, sizeof other))
            say("  the %s disc installed earlier is kept: both languages are available (the game's menu,\n"
                "  Language)\n", ol == DISC_LANG_JA ? "Japanese" : "English");
        else
            say("  to have %s as well, run Setup again with that disc's zip\n", ol == DISC_LANG_JA ? "Japanese" : "English");
        code_disc(game, game_cue, sizeof game_cue);
    }

    say("\n[3/4] Recompiling the game code\n");
    if (!generate(game_cue, gen_dir, misses, fp)) return fail("recompiling");

    if (!do_build) { say("\nDone (--no-build).\n"); return 0; }
    say("\n[4/4] Building " GAME_PROGRAM "\n");
    if (!SDL_GetPathInfo(zig, NULL)) {
        snprintf(g_err, sizeof g_err, "compiler not found at %s", zig);
        return fail("building");
    }
    if (!build(sdk, zig, out, gen_dir)) return fail("building");
    /* which version built it: the AppImage rebuilds the game when it is a newer one */
    char stamp[1100];
    join(stamp, sizeof stamp, out, "build/version.txt");
    SDL_SaveFile(stamp, RONDO_VERSION "\n", strlen(RONDO_VERSION "\n"));
    char done[1100];
    join(done, sizeof done, out, GAME_PROGRAM);
    say("\nDone: %s\n", done);
    return 0;
}

#ifndef SETUP_GUI
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep progress and errors in order */
    return setup_main(argc, argv);
}
#endif
