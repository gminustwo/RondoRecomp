/* See showcase.h. */
#include "showcase.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OP_DOWN, OP_UP, OP_MARK, OP_SHOT, OP_QUIT };

typedef struct Step {
    long frame;
    int op;
    SDL_Scancode sc;
    char *text;            /* mark, shot */
} Step;

static Step *steps;
static int nsteps, cap, pos;
static long frame;
static bool keys[SDL_SCANCODE_COUNT];
static FILE *logf;
static Uint64 t0;

const bool *showcase_keys(void) { return keys; }

static bool add(long at, int op, SDL_Scancode sc, const char *text)
{
    if (nsteps == cap) {
        cap = cap ? cap * 2 : 256;
        Step *n = realloc(steps, (size_t)cap * sizeof *n);
        if (!n) return false;
        steps = n;
    }
    steps[nsteps++] = (Step){at, op, sc, text ? SDL_strdup(text) : NULL};
    return true;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* the steps in time order (a tap's release can fall after later presses) */
static int by_frame(const void *a, const void *b)
{
    const Step *x = a, *y = b;
    if (x->frame != y->frame) return x->frame < y->frame ? -1 : 1;
    return x < y ? -1 : x > y;
}

bool showcase_load(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return false; }
    char line[512];
    long t = 0;
    int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *s = trim(line);
        if (!*s) continue;
        char cmd[32] = "";
        int used = 0;
        sscanf(s, "%31s %n", cmd, &used);
        char *rest = trim(s + used);
        if (!strcmp(cmd, "wait")) {
            t += atol(rest);
        } else if (!strcmp(cmd, "mark") || !strcmp(cmd, "shot")) {
            add(t, cmd[0] == 'm' ? OP_MARK : OP_SHOT, SDL_SCANCODE_UNKNOWN, rest);
        } else if (!strcmp(cmd, "quit")) {
            add(t, OP_QUIT, SDL_SCANCODE_UNKNOWN, NULL);
        } else if (!strcmp(cmd, "press") || !strcmp(cmd, "release") || !strcmp(cmd, "tap")) {
            char *tok[9];
            int nt = 0;
            for (char *p = strtok(rest, " \t"); p && nt < 9; p = strtok(NULL, " \t")) tok[nt++] = p;
            long len = 6;   /* tap: a number last is how long */
            if (cmd[0] == 't' && nt > 1 && strspn(tok[nt - 1], "0123456789") == strlen(tok[nt - 1])) len = atol(tok[--nt]);
            SDL_Scancode sc[9];
            int n = 0;
            for (int i = 0; i < nt; i++) {
                SDL_Scancode c = SDL_GetScancodeFromName(tok[i]);
                if (c == SDL_SCANCODE_UNKNOWN) {
                    snprintf(err, errlen, "%s:%d: unknown key %s", path, ln, tok[i]);
                    fclose(f);
                    return false;
                }
                sc[n++] = c;
            }
            for (int i = 0; i < n; i++) {
                if (cmd[0] != 'r') add(t, OP_DOWN, sc[i], NULL);
                if (cmd[0] != 'p') add(cmd[0] == 't' ? t + len : t, OP_UP, sc[i], NULL);
            }
            if (cmd[0] == 't') t += len;
        } else {
            snprintf(err, errlen, "%s:%d: unknown command %s", path, ln, cmd);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    qsort(steps, (size_t)nsteps, sizeof *steps, by_frame);
    char logpath[1100];
    snprintf(logpath, sizeof logpath, "%s.log", path);
    logf = fopen(logpath, "w");
    return true;
}

bool showcase_tick(SDL_Window *win, Uint32 which, char *shot, size_t shotlen)
{
    if (!t0) {   /* the wall clock at frame 0, to line a recording up with the marks */
        t0 = SDL_GetTicksNS();
        SDL_Time now = 0;
        SDL_GetCurrentTime(&now);
        if (logf) fprintf(logf, "#start\t%lld\n", (long long)now);
    }
    shot[0] = 0;
    bool more = true;
    for (; pos < nsteps && steps[pos].frame <= frame; pos++) {
        const Step *s = &steps[pos];
        switch (s->op) {
        case OP_DOWN:
        case OP_UP: {
            bool down = s->op == OP_DOWN;
            if (keys[s->sc] == down) break;
            keys[s->sc] = down;
            SDL_Event e;
            SDL_zero(e);
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = SDL_GetWindowID(win);
            e.key.which = which;
            e.key.scancode = s->sc;
            e.key.key = SDL_GetKeyFromScancode(s->sc, SDL_KMOD_NONE, false);
            e.key.down = down;
            SDL_PushEvent(&e);
            break;
        }
        case OP_MARK:
            if (logf) {
                fprintf(logf, "%ld\t%.3f\t%s\n", frame, (double)(SDL_GetTicksNS() - t0) / 1e9, s->text);
                fflush(logf);
            }
            break;
        case OP_SHOT:
            snprintf(shot, shotlen, "%s", s->text);
            break;
        case OP_QUIT:
            more = false;
            break;
        }
    }
    frame++;
    if (!more && logf) {
        fprintf(logf, "%ld\t%.3f\tquit\n", frame, (double)(SDL_GetTicksNS() - t0) / 1e9);
        fclose(logf);
        logf = NULL;
    }
    return more;
}
