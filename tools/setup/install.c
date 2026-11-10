/* Updates (rondo_setup --update, the Setup window's Update, the game's Updates page):
 *
 *   1. check     the latest release on GitHub (../frontend/update.h); newer than this one?
 *   2. download  its package for this system into DIR/update/, the one without the compiler
 *                when ours is the one it needs; size and SHA-256 checked against GitHub's
 *   3. unpack    into DIR/update/new/ (zip; tar.gz on Linux, file modes kept)
 *   4. install   each of its files and folders takes the place of ours, which go to
 *                DIR/update/old/ (renamed, so even running programs can be); all put back
 *                if anything fails
 *   5. rebuild   the new Setup (now in DIR) rebuilds the game from the new SDK; if that
 *                fails, the old files are put back
 *
 * The game, the disc images, saves and settings are not in the package and stay as they are.
 * DIR/update/ is removed at the end, but for files still in use (the game and Setup running
 * now): the game removes what is left when it next starts.
 */
#include <SDL3/SDL.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../frontend/child.h"
#include "../frontend/http.h"
#include "../frontend/programs.h"
#include "../frontend/update.h"
#include "setup.h"
#include "zip.h"

/* ---- running a program, its output passed on --------------------------------------- */

static char seg_buf[1024];
static char last_line[512];

static void pass_segment(void)
{
    char what[128];
    int pct;
    if (update_progress_line(seg_buf, what, sizeof what, &pct)) setup_progress_hook(what, pct);
    else if (seg_buf[0]) {
        say("%s\n", seg_buf);
        const char *s = seg_buf;
        while (*s == ' ') s++;
        if (*s) snprintf(last_line, sizeof last_line, "%s", s);
    }
    seg_buf[0] = 0;
}

static void pass_output(const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        size_t l = strlen(seg_buf);
        if (buf[i] == '\n' || buf[i] == '\r') pass_segment();
        else if (l + 1 < sizeof seg_buf) { seg_buf[l] = buf[i]; seg_buf[l + 1] = 0; }
    }
}

bool setup_run(const char *const *args)
{
    char err[256];
    Child *c = child_start(args, err, sizeof err);
    if (!c) { snprintf(g_err, sizeof g_err, "%s", err); return false; }
    seg_buf[0] = last_line[0] = 0;
    char buf[4096];
    int code = -1, got;
    for (;;) {
        while ((got = child_read(c, buf, sizeof buf)) > 0) pass_output(buf, got);
        if (child_wait(c, false, &code)) break;
        SDL_Delay(got < 0 ? 20 : 5);
    }
    while ((got = child_read(c, buf, sizeof buf)) > 0) pass_output(buf, got);
    pass_segment();
    child_free(c);
    if (code != 0) {
        const char *name = strrchr(args[0], '/'), *b = strrchr(args[0], '\\');
        if (b > name) name = b;
        snprintf(g_err, sizeof g_err, "%s failed (exit code %d)%s%.300s", name ? name + 1 : args[0], code,
                 last_line[0] ? ": " : "", last_line);
        return false;
    }
    return true;
}

/* ---- putting files in place ------------------------------------------------------------ */

static bool exists(const char *path) { return SDL_GetPathInfo(path, NULL); }

/* A virus scanner may hold a file it has just seen for a moment: renames are tried again. */
static bool rename_retry(const char *from, const char *to)
{
    for (int i = 0; i < 20; i++) {
        if (SDL_RenamePath(from, to)) return true;
        SDL_Delay(250);
    }
    return false;
}

/* DIR/update/old/<name>, numbered if one is already there (from an earlier update, or put
 * there by this one: it may be needed to put things back) */
static void aside_path(const char *dir, const char *name, char *out, size_t n)
{
    char old[1100];
    join(old, sizeof old, dir, "update");
    SDL_CreateDirectory(old);
    join(old, sizeof old, dir, "update/old");
    SDL_CreateDirectory(old);
    join(out, n, old, name);
    for (int i = 1; exists(out) && i < 1000; i++) {
        char numbered[300];
        snprintf(numbered, sizeof numbered, "%s.%d", name, i);
        join(out, n, old, numbered);
    }
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/'), *b = strrchr(path, '\\');
    if (b > s) s = b;
    return s ? s + 1 : path;
}

bool setup_replace(const char *dir, const char *fresh, const char *dest)
{
    char aside[1200] = "";
    if (exists(dest)) {
        aside_path(dir, base_name(dest), aside, sizeof aside);
        if (!rename_retry(dest, aside)) {
            snprintf(g_err, sizeof g_err, "cannot move %s out of the way: %s", dest, SDL_GetError());
            return false;
        }
    }
    if (!rename_retry(fresh, dest)) {
        snprintf(g_err, sizeof g_err, "cannot put %s in place: %s", dest, SDL_GetError());
        if (aside[0]) rename_retry(aside, dest);
        return false;
    }
    if (aside[0]) {   /* gone now unless it is running (the game removes it when it next starts) */
        char d[1100];
        SDL_RemovePath(aside);
        join(d, sizeof d, dir, "update/old");
        SDL_RemovePath(d);   /* only if empty (an update's own files are there while it runs) */
        join(d, sizeof d, dir, "update");
        SDL_RemovePath(d);
    }
    return true;
}

/* ---- SHA-256 ------------------------------------------------------------------------------ */

typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t n; } Sha256;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha_block(Sha256 *s, const unsigned char *p)
{
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof a[0]);
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}

static void sha_init(Sha256 *s)
{
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0);
    s->len = 0;
    s->n = 0;
}

static void sha_add(Sha256 *s, const unsigned char *p, size_t n)
{
    s->len += n;
    while (n) {
        size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, k);
        s->n += k;
        p += k;
        n -= k;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}

static void sha_hex(Sha256 *s, char out[65])
{
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80, zero = 0, len[8];
    sha_add(s, &pad, 1);
    while (s->n != 56) sha_add(s, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha_add(s, len, 8);
    for (int i = 0; i < 8; i++) snprintf(out + i * 8, 9, "%08x", (unsigned)s->h[i]);
}

static bool file_sha256(const char *path, char out[65])
{
    SDL_IOStream *f = SDL_IOFromFile(path, "rb");
    if (!f) return false;
    Sha256 s;
    sha_init(&s);
    static unsigned char buf[1 << 16];
    size_t got;
    while ((got = SDL_ReadIO(f, buf, sizeof buf)) > 0) sha_add(&s, buf, got);
    SDL_CloseIO(f);
    sha_hex(&s, out);
    return true;
}

/* ---- unpacking --------------------------------------------------------------------------- */

/* A path inside the package, made relative to its top folder (RondoRecomp/). False for one
 * that would land outside DIR/update/new. */
static bool package_path(const char *name, char *out, size_t n)
{
    while (name[0] == '.' && name[1] == '/') name += 2;
    if (!strncmp(name, "RondoRecomp/", 12)) name += 12;
    if (!*name || name[0] == '/' || name[0] == '\\' || strchr(name, ':')) return false;
    for (const char *p = name; *p;) {   /* no ".." part */
        const char *e = p + strcspn(p, "/\\");
        if (e - p == 2 && p[0] == '.' && p[1] == '.') return false;
        p = *e ? e + 1 : e;
    }
    snprintf(out, n, "%s", name);
    return true;
}

static bool unpack_zip(const char *archive, const char *dest)
{
    Zip z;
    if (!zip_open(&z, archive, g_err, sizeof g_err)) return false;
    bool ok = true;
    for (int i = 0; i < z.n && ok; i++) {
        const ZipEntry *e = &z.entries[i];
        size_t l = strlen(e->name);
        if (!l || e->name[l - 1] == '/') continue;     /* folders come with their files */
        char rel[600], path[1700];
        if (!package_path(e->name, rel, sizeof rel)) {
            snprintf(g_err, sizeof g_err, "the package has an unsafe path: %s", e->name);
            ok = false;
            break;
        }
        join(path, sizeof path, dest, rel);
        if (!make_dirs_for(path)) { snprintf(g_err, sizeof g_err, "cannot make the folder for %s", path); ok = false; break; }
        ok = zip_extract(&z, e, path, NULL, NULL, g_err, sizeof g_err);
        if (i % 50 == 0) setup_progress_hook("Unpacking", i * 100 / z.n);
    }
    zip_close(&z);
    return ok;
}

typedef struct Gz {
    SDL_IOStream *in;
    z_stream zs;
    unsigned char buf[1 << 16];
    long long read, total;
} Gz;

/* exactly n bytes of the unpacked stream */
static bool gz_read(Gz *g, void *out, size_t n)
{
    g->zs.next_out = out;
    g->zs.avail_out = (uInt)n;
    while (g->zs.avail_out) {
        if (!g->zs.avail_in) {
            size_t got = SDL_ReadIO(g->in, g->buf, sizeof g->buf);
            if (!got) return false;
            g->read += (long long)got;
            g->zs.next_in = g->buf;
            g->zs.avail_in = (uInt)got;
        }
        int r = inflate(&g->zs, Z_NO_FLUSH);
        if (r == Z_STREAM_END) return g->zs.avail_out == 0;
        if (r != Z_OK && r != Z_BUF_ERROR) return false;
    }
    return true;
}

static bool gz_skip(Gz *g, unsigned long long n)
{
    static unsigned char scratch[1 << 14];
    while (n) {
        size_t k = n < sizeof scratch ? (size_t)n : sizeof scratch;
        if (!gz_read(g, scratch, k)) return false;
        n -= k;
    }
    return true;
}

static unsigned long long tar_number(const char *p, size_t n)
{
    if ((unsigned char)p[0] & 0x80) {   /* base-256 (GNU, large sizes) */
        unsigned long long v = (unsigned char)p[0] & 0x7f;
        for (size_t i = 1; i < n; i++) v = v << 8 | (unsigned char)p[i];
        return v;
    }
    unsigned long long v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (unsigned long long)(p[i] - '0');
    return v;
}

static bool unpack_tgz(const char *archive, const char *dest)
{
    static Gz g;
    memset(&g, 0, sizeof g);
    g.in = SDL_IOFromFile(archive, "rb");
    if (!g.in) { snprintf(g_err, sizeof g_err, "cannot open %s", archive); return false; }
    g.total = SDL_GetIOSize(g.in);
    if (inflateInit2(&g.zs, 16 + MAX_WBITS) != Z_OK) { SDL_CloseIO(g.in); snprintf(g_err, sizeof g_err, "zlib"); return false; }
    char hdr[512], longname[1024] = "";
    bool ok = false;
    int files = 0;
    for (;;) {
        if (!gz_read(&g, hdr, 512)) { snprintf(g_err, sizeof g_err, "%s ends too early or is damaged", archive); break; }
        bool zero = true;
        for (int i = 0; i < 512 && zero; i++) zero = hdr[i] == 0;
        if (zero) { ok = true; break; }                 /* the end */
        unsigned long long sum = 0;
        for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : (unsigned char)hdr[i];
        if (sum != tar_number(hdr + 148, 8)) { snprintf(g_err, sizeof g_err, "%s is damaged (a header's checksum)", archive); break; }
        char type = hdr[156];
        unsigned long long size = tar_number(hdr + 124, 12), pad = (512 - size % 512) % 512;
        unsigned mode = (unsigned)tar_number(hdr + 100, 8);
        if (type == 'L' || type == 'x') {             /* the next entry's long name (GNU, POSIX) */
            char *data = malloc(size + 1);
            if (!data || !gz_read(&g, data, size) || !gz_skip(&g, pad)) { free(data); snprintf(g_err, sizeof g_err, "%s is damaged", archive); break; }
            data[size] = 0;
            if (type == 'L') snprintf(longname, sizeof longname, "%s", data);
            else {
                for (char *rec = data; rec < data + size;) {   /* "<length> key=value\n" */
                    long rl = strtol(rec, NULL, 10);
                    char *kv = strchr(rec, ' ');
                    if (rl <= 0 || !kv) break;
                    if (!strncmp(kv + 1, "path=", 5)) {
                        size_t vl = (size_t)(rec + rl - 1 - (kv + 6));
                        if (vl < sizeof longname) { memcpy(longname, kv + 6, vl); longname[vl] = 0; }
                    }
                    rec += rl;
                }
            }
            free(data);
            continue;
        }
        char name[1100];
        if (longname[0]) {
            snprintf(name, sizeof name, "%s", longname);
            longname[0] = 0;
        } else if (!memcmp(hdr + 257, "ustar", 5) && hdr[345]) {
            snprintf(name, sizeof name, "%.155s/%.100s", hdr + 345, hdr);
        } else {
            snprintf(name, sizeof name, "%.100s", hdr);
        }
        char rel[1100], path[1700];
        bool is_dir = type == '5';
        size_t nl = strlen(name);
        if (nl && name[nl - 1] == '/') { name[nl - 1] = 0; is_dir = true; }
        if (!strcmp(name, "RondoRecomp") || !strcmp(name, ".") || !strcmp(name, "./RondoRecomp")) {
            if (!gz_skip(&g, size + pad)) break;
            continue;
        }
        if (!package_path(name, rel, sizeof rel)) { snprintf(g_err, sizeof g_err, "the package has an unsafe path: %s", name); break; }
        join(path, sizeof path, dest, rel);
        if (is_dir) {
            SDL_CreateDirectory(path);
        } else if (type == '0' || type == 0 || type == '7') {
            if (!make_dirs_for(path)) { snprintf(g_err, sizeof g_err, "cannot make the folder for %s", path); break; }
            SDL_IOStream *out = SDL_IOFromFile(path, "wb");
            if (!out) { snprintf(g_err, sizeof g_err, "cannot write %s", path); break; }
            static unsigned char chunk[1 << 16];
            unsigned long long left = size;
            bool good = true;
            while (left && good) {
                size_t k = left < sizeof chunk ? (size_t)left : sizeof chunk;
                good = gz_read(&g, chunk, k) && SDL_WriteIO(out, chunk, k) == k;
                left -= k;
            }
            good = SDL_CloseIO(out) && good;
            if (!good || !gz_skip(&g, pad)) { snprintf(g_err, sizeof g_err, "cannot unpack %s", path); break; }
#ifndef _WIN32
            chmod(path, (mode & 0111) ? 0755 : 0644);
#endif
            if (++files % 100 == 0 && g.total > 0) setup_progress_hook("Unpacking", (int)(g.read * 100 / g.total));
            continue;
        } else if (type == '2') {
#ifndef _WIN32
            char target[101];
            snprintf(target, sizeof target, "%.100s", hdr + 157);
            if (!make_dirs_for(path)) break;
            unlink(path);
            if (symlink(target, path) != 0) { snprintf(g_err, sizeof g_err, "cannot make the link %s", path); break; }
#endif
        }
        if (!gz_skip(&g, size + pad)) { snprintf(g_err, sizeof g_err, "%s is damaged", archive); break; }
        (void)mode;
    }
    inflateEnd(&g.zs);
    SDL_CloseIO(g.in);
    return ok;
}

/* ---- installing ---------------------------------------------------------------------- */

typedef struct Entry { char name[256], aside[1200]; } Entry;   /* aside: where ours went, "" none */
typedef struct Entries { Entry e[64]; int n; } Entries;

static SDL_EnumerationResult SDLCALL collect(void *userdata, const char *dir, const char *name)
{
    (void)dir;
    Entries *list = userdata;
    if (list->n < (int)(sizeof list->e / sizeof list->e[0])) snprintf(list->e[list->n++].name, sizeof list->e[0].name, "%s", name);
    return SDL_ENUM_CONTINUE;
}

/* each top-level file and folder of `staged` back out of DIR, and DIR's old one back */
static void put_back(const char *dir, const char *staged, const Entries *done)
{
    for (int i = done->n - 1; i >= 0; i--) {
        char now[1200], back[1200];
        join(now, sizeof now, dir, done->e[i].name);
        join(back, sizeof back, staged, done->e[i].name);
        rename_retry(now, back);
        if (done->e[i].aside[0]) rename_retry(done->e[i].aside, now);
    }
}

static bool install(const char *dir, const char *staged, Entries *done)
{
    Entries list = {0};
    SDL_EnumerateDirectory(staged, collect, &list);
    done->n = 0;
    for (int i = 0; i < list.n; i++) {
        char now[1200], fresh[1200];
        join(now, sizeof now, dir, list.e[i].name);
        join(fresh, sizeof fresh, staged, list.e[i].name);
        Entry *e = &done->e[done->n];
        snprintf(e->name, sizeof e->name, "%s", list.e[i].name);
        e->aside[0] = 0;
        if (exists(now)) {
            aside_path(dir, list.e[i].name, e->aside, sizeof e->aside);
            if (!rename_retry(now, e->aside)) {
                snprintf(g_err, sizeof g_err, "cannot move %s out of the way: %s", now, SDL_GetError());
                put_back(dir, staged, done);
                return false;
            }
        }
        if (!rename_retry(fresh, now)) {
            snprintf(g_err, sizeof g_err, "cannot put %s in place: %s", now, SDL_GetError());
            if (e->aside[0]) rename_retry(e->aside, now);
            put_back(dir, staged, done);
            return false;
        }
        done->n++;
        say("  %s\n", list.e[i].name);
    }
    return true;
}

/* ---- the update ---------------------------------------------------------------------------- */

static int last_pct = -1;

static bool on_download(long long got, long long total, void *userdata)
{
    (void)userdata;
    int pct = total > 0 ? (int)(got * 100 / total) : 0;
    if (pct > 100) pct = 100;
    if (pct != last_pct && pct < 100) {   /* 100 is said once it is checked */
        setup_progress_hook("Downloading", pct);
        last_pct = pct;
    }
    return true;
}

static int failed(const char *step)
{
    say("\nSetup failed while %s:\n  %s\n", step, g_err);
    return 1;
}

/* the release's package into `path`, its size and SHA-256 checked */
static bool download(const UpdateInfo *info, const char *path)
{
    char err[256];
    last_pct = -1;
    int status = http_get_file(info->url, update_user_agent(), path, info->size, on_download, NULL, err, sizeof err);
    if (status != 200) { snprintf(g_err, sizeof g_err, "%s (%s)", err, info->url); return false; }
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(path, &pi) || (info->size > 0 && (long long)pi.size != info->size)) {
        snprintf(g_err, sizeof g_err, "the download is %lld bytes, not %lld", (long long)pi.size, info->size);
        return false;
    }
    setup_progress_hook("Downloading", 100);
    if (info->sha256[0]) {
        char sum[65];
        if (!file_sha256(path, sum) || strcmp(sum, info->sha256)) {
            snprintf(g_err, sizeof g_err, "the download is damaged: its SHA-256 is not the one GitHub gives");
            return false;
        }
        say("  SHA-256 checked: %s\n", sum);
    }
    return true;
}

/* From the Linux AppImage (programs.h, tools/linux/AppRun): the new AppImage is downloaded next
 * to this one and builds the game; only then does it take this one's place (a rename: this one
 * keeps running from the old file). A failed build leaves the game as it was. */
static int update_appimage(const char *dir, const UpdateInfo *info)
{
#ifdef _WIN32
    (void)dir;
    (void)info;
    return 1;
#else
    const char *app = rondo_appimage();
    char fresh[1100];
    snprintf(fresh, sizeof fresh, "%s.new", app);
    say("\n[2/4] Downloading %s\n", info->asset);
    if (!download(info, fresh)) { SDL_RemovePath(fresh); return failed("downloading"); }
    if (chmod(fresh, 0755) != 0) {
        snprintf(g_err, sizeof g_err, "cannot make %s executable: %s", fresh, strerror(errno));
        SDL_RemovePath(fresh);
        return failed("downloading");
    }

    say("\n[3/4] Rebuilding " GAME_PROGRAM " with the new version\n");
    const char *args[] = {fresh, "--setup-cli", "--rebuild", "--out", dir, NULL};
    if (!setup_run(args)) {
        char why[512];
        snprintf(why, sizeof why, "%s", g_err);
        SDL_RemovePath(fresh);
        snprintf(g_err, sizeof g_err, "%s; the game was left as it was", why);
        return failed("rebuilding");
    }

    say("\n[4/4] Putting the new AppImage in place of %s\n", app);
    if (rename(fresh, app) != 0) {
        snprintf(g_err, sizeof g_err, "cannot replace %s: %s", app, strerror(errno));
        SDL_RemovePath(fresh);
        return failed("installing");
    }
    say("\nUpdated to RondoRecomp %s.\n", info->version);
    return 0;
#endif
}

int setup_update(const char *dir, bool check_only)
{
    say("RondoRecomp Setup " RONDO_VERSION " (update)\n\n[1/5] Checking for a new version\n");
    char *body = NULL, err[256];
    size_t len = 0;
    int status = http_get_text(update_api_url(), update_user_agent(), &body, &len, err, sizeof err);
    if (status != 200) {
        free(body);
        snprintf(g_err, sizeof g_err, "%s (%s)", err, update_api_url());
        return failed("checking for a new version");
    }
    UpdateInfo info;
    bool parsed = update_parse(body, len, &info, err, sizeof err);
    free(body);
    if (!parsed) { snprintf(g_err, sizeof g_err, "%s", err); return failed("checking for a new version"); }
    const char *have = update_current_version();
    if (update_compare(info.version, have) <= 0) {
        say("  This is the latest version (%s; the newest release is %s).\n", have, info.version);
        return 3;
    }
    if (!info.url[0]) {
        say("  RondoRecomp %s is out, but has no package for this system (" UPDATE_SYSTEM ").\n", info.version);
        return 3;
    }
    say("  RondoRecomp %s is available (this is %s): %s, %.1f MB\n", info.version, have, info.asset,
        (double)info.size / 1e6);
    for (int i = 0; i < 4; i++)
        if (info.notes[i][0]) say("    - %s\n", info.notes[i]);
    if (check_only) return 0;
    if (rondo_appimage()) return update_appimage(dir, &info);

    char upd[1100], archive[1300], staged[1200];
    join(upd, sizeof upd, dir, "update");
    remove_tree(upd);                              /* whatever an earlier update left */
    SDL_CreateDirectory(upd);
    join(archive, sizeof archive, upd, info.asset);
    join(staged, sizeof staged, upd, "new");

    say("\n[2/5] Downloading %s\n", info.asset);
    if (!download(&info, archive)) { remove_tree(upd); return failed("downloading"); }

    say("\n[3/5] Unpacking\n");
    SDL_CreateDirectory(staged);
    bool unpacked = SDL_strcasecmp(info.asset + strlen(info.asset) - 4, ".zip") == 0 ? unpack_zip(archive, staged)
                                                                                   : unpack_tgz(archive, staged);
    if (!unpacked) { remove_tree(upd); return failed("unpacking"); }
    setup_progress_hook("Unpacking", 100);
    char check1[1300], check2[1300];
    join(check1, sizeof check1, staged, "sdk/sources.txt");
    join(check2, sizeof check2, staged, SETUP_CLI_PROGRAM);
    if (!exists(check1) || !exists(check2)) {
        snprintf(g_err, sizeof g_err, "%s is not a RondoRecomp package (no sdk/sources.txt or Setup)", info.asset);
        remove_tree(upd);
        return failed("unpacking");
    }
    remove_tree(archive);

    say("\n[4/5] Installing RondoRecomp %s\n", info.version);
    Entries done;
    if (!install(dir, staged, &done)) { remove_tree(upd); return failed("installing"); }

    say("\n[5/5] Rebuilding " GAME_PROGRAM " with the new version\n");
    char setup[1200];
    join(setup, sizeof setup, dir, SETUP_CLI_PROGRAM);
    const char *args[] = {setup, "--rebuild", "--out", dir, NULL};
    if (!setup_run(args)) {
        char why[512];
        snprintf(why, sizeof why, "%s", g_err);
        put_back(dir, staged, &done);
        remove_tree(upd);
        snprintf(g_err, sizeof g_err, "%s; the previous version was put back", why);
        return failed("rebuilding");
    }
    remove_tree(upd);   /* all but files still in use (this Setup, the game): the game removes them later */
    say("\nUpdated to RondoRecomp %s.\n", info.version);
    return 0;
}
