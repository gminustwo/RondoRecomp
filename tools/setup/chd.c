/* See chd.h. The .chd itself is read by the runtime's reader (runtime/pce/chd.c). */
#include "chd.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pce/chd.h"
#include "pce/disc.h"
#include "setup.h"

DiscKind chd_identify(const char *path, uint64_t *fingerprint)
{
    if (fingerprint) *fingerprint = 0;
    Disc disc;
    char err[256];
    if (!disc_open(&disc, path, err, sizeof err)) {
        snprintf(g_err, sizeof g_err, "%s", err);
        return DISC_INVALID;
    }
    const DiscTrack *t = NULL;
    for (int i = 0; i < disc.n_tracks && !t; i++)
        if (disc.tracks[i].data) t = &disc.tracks[i];
    uint8_t s1[2048], s2[2048];
    DiscKind kind = DISC_INVALID;
    if (!t) {
        snprintf(g_err, sizeof g_err, "the .chd has no data track: it is not a PC Engine CD");
    } else if (!disc_read_data(&disc, t->lba + 1, 1, s1) || !disc_read_data(&disc, t->lba + 2, 1, s2)) {
        snprintf(g_err, sizeof g_err, "the .chd is damaged (its first data track cannot be read)");
    } else {
        kind = discid_identify_sectors(s1, s2, fingerprint);
        if (kind == DISC_INVALID) snprintf(g_err, sizeof g_err, "the .chd is not a PC Engine CD");
    }
    disc_close(&disc);
    return kind;
}

static void checking(int pct, void *ctx)
{
    (void)ctx;
    setup_progress_hook("Checking the .chd", pct);
}

bool chd_install(const char *path, const char *stage, char *out, size_t out_len)
{
    char err[256];
    Chd *c = chd_open(path, err, sizeof err);
    if (!c) { snprintf(g_err, sizeof g_err, "%s", err); return false; }
    int data = 0, audio = 0;
    for (int i = 0; i < chd_track_count(c); i++) {
        if (chd_track(c, i)->audio) audio++;
        else data++;
    }
    bool ok = chd_verify(c, checking, NULL);
    if (!ok) snprintf(g_err, sizeof g_err, "%s", chd_error(c));
    chd_close(c);
    if (!ok) return false;

    const char *name = strrchr(path, '/'), *bs = strrchr(path, '\\');
    if (bs > name) name = bs;
    name = name ? name + 1 : path;
    remove_tree(stage);
    join(out, out_len, stage, name);
    SDL_IOStream *from = SDL_IOFromFile(path, "rb");
    SDL_IOStream *to = SDL_CreateDirectory(stage) ? SDL_IOFromFile(out, "wb") : NULL;
    Sint64 total = from ? SDL_GetIOSize(from) : -1, done = 0;
    enum { CHUNK = 1 << 20 };
    uint8_t *buf = malloc(CHUNK);
    ok = from && to && total > 0 && buf;
    int last = -1;
    while (ok && done < total) {
        size_t n = SDL_ReadIO(from, buf, CHUNK);
        ok = n > 0 && SDL_WriteIO(to, buf, n) == n;
        done += (Sint64)n;
        int pct = (int)(done * 100 / total);
        if (pct != last) setup_progress_hook("Copying the .chd", last = pct);
    }
    free(buf);
    if (from) SDL_CloseIO(from);
    if (to && !SDL_CloseIO(to)) ok = false;
    if (!ok) {
        snprintf(g_err, sizeof g_err, "cannot copy %s to %s", path, out);
        return false;
    }
    say("  %d tracks: %d data, %d audio; kept as a .chd (%lld MB)\n", data + audio, data, audio,
        (long long)(total >> 20));
    return true;
}
