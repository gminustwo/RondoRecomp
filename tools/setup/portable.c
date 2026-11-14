/* See portable.h. */
#include "portable.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "../frontend/programs.h"
#include "setup.h"

static bool is_dir(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

bool portable_on(void)
{
    char user[1024];
    rondo_user_dir(user, sizeof user);
    return is_dir(user);
}

static SDL_EnumerationResult SDLCALL any_visit(void *userdata, const char *dir, const char *name)
{
    (void)dir;
    (void)name;
    *(bool *)userdata = true;
    return SDL_ENUM_SUCCESS;
}

static bool has_files(const char *dir)
{
    bool any = false;
    if (is_dir(dir)) SDL_EnumerateDirectory(dir, any_visit, &any);
    return any;
}

bool portable_appdata(char *out, size_t n, bool *files)
{
    if (!rondo_appdata_dir(out, n)) return false;
    if (files) *files = has_files(out);
    return true;
}

/* everything in one folder into another (made), files there replaced */
typedef struct Copy { const char *to; bool ok; } Copy;

static SDL_EnumerationResult SDLCALL copy_visit(void *userdata, const char *dir, const char *name)
{
    Copy *c = userdata;
    char from[1100], to[1100];
    join(from, sizeof from, dir, name);
    join(to, sizeof to, c->to, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(from, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        Copy sub = {to, SDL_CreateDirectory(to)};
        if (sub.ok) SDL_EnumerateDirectory(from, copy_visit, &sub);
        if (!sub.ok) c->ok = false;
    } else if (info.type == SDL_PATHTYPE_FILE && !SDL_CopyFile(from, to)) {
        snprintf(g_err, sizeof g_err, "cannot copy %s to %s: %s", from, to, SDL_GetError());
        c->ok = false;
    }
    return SDL_ENUM_CONTINUE;
}

static bool copy_tree(const char *from, const char *to)
{
    if (!SDL_CreateDirectory(to)) {
        snprintf(g_err, sizeof g_err, "cannot make %s: %s", to, SDL_GetError());
        return false;
    }
    Copy c = {to, true};
    if (!SDL_EnumerateDirectory(from, copy_visit, &c)) {
        snprintf(g_err, sizeof g_err, "cannot read %s: %s", from, SDL_GetError());
        return false;
    }
    return c.ok;
}

bool portable_enable(bool copy)
{
    char user[1024], appdata[1024];
    rondo_user_dir(user, sizeof user);
    if (!is_dir(user) && !SDL_CreateDirectory(user)) {
        snprintf(g_err, sizeof g_err, "cannot make %s: %s", user, SDL_GetError());
        return false;
    }
    bool files = false;
    if (copy && portable_appdata(appdata, sizeof appdata, &files) && files) {
        if (!copy_tree(appdata, user)) return false;
        say("  copied the settings and saves in %s\n", appdata);
    }
    say("  portable: the game keeps its settings and saves in %s\n", user);
    return true;
}

bool portable_disable(void)
{
    char user[1024], appdata[1024];
    rondo_user_dir(user, sizeof user);
    if (!is_dir(user)) return true;
    if (!has_files(user)) {
        if (!SDL_RemovePath(user)) {
            snprintf(g_err, sizeof g_err, "cannot remove %s: %s", user, SDL_GetError());
            return false;
        }
        say("  removed the empty %s\n", user);
        return true;
    }
    if (!portable_appdata(appdata, sizeof appdata, NULL)) {
        snprintf(g_err, sizeof g_err, "no profile folder to copy the settings and saves back to");
        return false;
    }
    if (!copy_tree(user, appdata)) return false;
    char old[1100];
    for (int i = 1; i < 100; i++) {
        if (i == 1) snprintf(old, sizeof old, "%s.old", user);
        else snprintf(old, sizeof old, "%s.old%d", user, i);
        if (!SDL_GetPathInfo(old, NULL)) break;
    }
    if (!SDL_RenamePath(user, old)) {
        snprintf(g_err, sizeof g_err, "copied back to %s, but cannot rename %s: %s", appdata, user, SDL_GetError());
        return false;
    }
    say("  copied the settings and saves back to %s; the folder is kept as %s\n", appdata, old);
    return true;
}
