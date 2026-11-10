/* See vc.h. What is inside:
 *
 * Wii U (WUP): title.tik holds the title key, encrypted with the Wii U common key (IV: the
 * title id); title.tmd lists the contents (NNNNNNNN.app, each encrypted with the title key).
 * Content 0 is the file system (FST); the others hold its files, "hashed" ones in 0x10000-byte
 * blocks of 0x400 bytes of hashes and 0xFC00 of data. The game is /content/pceemu/pce.pkg: a
 * u32 (little-endian) length, then files, each a u32 size, a NUL-terminated name and the data.
 *
 * Wii (WAD): a header, the certificates, the ticket and the TMD, then the contents (each
 * 64-byte aligned, AES-CBC with the title key, IV: the content's index), checked against the
 * SHA-1 the TMD gives. The game is content 5, a U8 archive (possibly LZ77-compressed). With
 * no key, the title installed in Dolphin is read instead: Dolphin keeps it decrypted.
 *
 * Either way the disc is a track list (CDROM.hcd, KMCD3005.hcd), a line per track:
 * "track02,code ,track02.bin,004040,010299" (number, audio or code, file, start as LBA + 150,
 * length in sectors), with data tracks as zlib-compressed blocks of 128 2048-byte sectors (a
 * u32 count, then count pairs of u32 offset and size) and audio tracks as Ogg Vorbis. The
 * Wii U puts the audio track after a data track at its 2-second pregap; the Wii leaves a gap.
 * The Wii lacks track 1 (silence) and track 22, which on the discs copies track 2.
 *
 * Setup writes 2048-byte data tracks (.iso) and 44.1 kHz WAV audio, laid out with PREGAP and
 * INDEX lines so that every track starts where it does on the original disc. */
#include "vc.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "../frontend/programs.h"
#include "setup.h"
#include "sevenzip.h"
#include "vc_crypto.h"
#include "zip.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"   /* third_party/stb_vorbis; its code is in vorbis.c */

const char *vc_keys_file;

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t align64(uint64_t x) { return (x + 63) & ~(uint64_t)63; }

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !SDL_strcasecmp(s + a - b, suffix);
}

static const char *base_of(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

static void dir_of(char *out, size_t n, const char *p)
{
    snprintf(out, n, "%s", p);
    char *a = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > a) a = b;
    if (a) *a = 0; else snprintf(out, n, ".");
}

/* ---- where the files come from: a folder, a zip or a 7z ---------------------------------- */

enum { SRC_DIR, SRC_ZIP, SRC_7Z };

typedef struct Src {
    int kind;
    char dir[1100];      /* SRC_DIR: the folder holding them */
    Zip zip;
    SevenZip *sz;
    char prefix[700];    /* in an archive: the folder holding them ("" or ending in '/') */
    char wad[700];       /* the .wad (its name in dir, or in the archive); "" for a WUP */
    char cue[700];       /* a 7z holding a disc image instead: its .cue */
} Src;

typedef struct Blob {
    uint8_t *data;
    size_t size;
    int owner;           /* 0: not ours, 1: malloc, 2: SDL */
} Blob;

static void blob_free(Blob *b)
{
    if (b->owner == 1) free(b->data);
    else if (b->owner == 2) SDL_free(b->data);
    memset(b, 0, sizeof *b);
}

static int sz_find(const SevenZip *z, const char *name)
{
    for (int i = 0; i < sz_count(z); i++)
        if (!sz_is_dir(z, i) && !SDL_strcasecmp(sz_name(z, i), name)) return i;
    return -1;
}

static bool src_read(Src *s, const char *name, Blob *b)
{
    memset(b, 0, sizeof *b);
    char full[1500];
    char err[300];
    if (s->kind == SRC_DIR) {
        join(full, sizeof full, s->dir, name);
        size_t n = 0;
        void *d = SDL_LoadFile(full, &n);
        if (!d) {
            /* a file system that minds case: 0000000A.app or 0000000a.app */
            char alt[1500];
            snprintf(alt, sizeof alt, "%s", full);
            char *base = (char *)base_of(alt);
            bool upper = false;
            for (char *c = base; *c; c++) upper |= isupper((unsigned char)*c) != 0;
            for (char *c = base; *c && *c != '.'; c++) *c = (char)(upper ? tolower((unsigned char)*c) : toupper((unsigned char)*c));
            d = SDL_LoadFile(alt, &n);
        }
        if (!d) { snprintf(g_err, sizeof g_err, "cannot read %s", full); return false; }
        b->data = d;
        b->size = n;
        b->owner = 2;
        return true;
    }
    snprintf(full, sizeof full, "%s%s", s->prefix, name);
    if (s->kind == SRC_ZIP) {
        const ZipEntry *e = zip_find(&s->zip, full);
        if (!e) { snprintf(g_err, sizeof g_err, "%s is not in the zip", full); return false; }
        char *d = zip_read_text(&s->zip, e, SIZE_MAX - 1, err, sizeof err);
        if (!d) { snprintf(g_err, sizeof g_err, "%s", err); return false; }
        b->data = (uint8_t *)d;
        b->size = (size_t)e->size;
        b->owner = 1;
        return true;
    }
    int i = sz_find(s->sz, full);
    if (i < 0) { snprintf(g_err, sizeof g_err, "%s is not in the 7z", full); return false; }
    size_t n = 0;
    const uint8_t *d = sz_read(s->sz, i, &n, err, sizeof err);
    if (!d) { snprintf(g_err, sizeof g_err, "%s", err); return false; }
    b->data = (uint8_t *)d;
    b->size = n;
    return true;
}

/* one name from the input's list: the WUP's title.tmd or a .wad (or, in a 7z, a .cue) */
static void consider(Src *s, const char *name)
{
    size_t l = strlen(name);
    if (!s->prefix[0] && !s->wad[0] && ends_with_ci(name, "title.tmd") &&
        (l == 9 || name[l - 10] == '/' || name[l - 10] == '\\')) {
        snprintf(s->prefix, sizeof s->prefix, "%.*s", (int)(l - 9), name);
        if (!s->prefix[0]) snprintf(s->prefix, sizeof s->prefix, "/");   /* (at the root: marked, cleared below) */
    } else if (!s->wad[0] && !s->prefix[0] && ends_with_ci(name, ".wad")) {
        snprintf(s->wad, sizeof s->wad, "%s", name);
    } else if (!s->cue[0] && ends_with_ci(name, ".cue")) {
        snprintf(s->cue, sizeof s->cue, "%s", name);
    }
}

static void src_close(Src *s)
{
    if (s->kind == SRC_ZIP) zip_close(&s->zip);
    if (s->kind == SRC_7Z) sz_close(s->sz);
    memset(s, 0, sizeof *s);
}

/* the input's WUP or .wad (or a 7z's disc image): false if it has none */
static bool src_open(const char *input, Src *s, bool quiet)
{
    memset(s, 0, sizeof *s);
    char err[300];
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(input, &info)) return false;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        s->kind = SRC_DIR;
        int count = 0;
        char **files = SDL_GlobDirectory(input, NULL, 0, &count);
        for (int i = 0; files && i < count; i++)
            if (ends_with_ci(files[i], "title.tmd") || ends_with_ci(files[i], ".wad")) consider(s, files[i]);
        SDL_free(files);
        char rel[700];
        if (s->prefix[0]) {
            snprintf(rel, sizeof rel, "%s", s->prefix[0] == '/' && !s->prefix[1] ? "" : s->prefix);
            join(s->dir, sizeof s->dir, input, rel);
            s->prefix[0] = 0;
            return true;
        }
        if (s->wad[0]) {
            char path[1500];
            join(path, sizeof path, input, s->wad);
            dir_of(s->dir, sizeof s->dir, path);
            snprintf(rel, sizeof rel, "%s", base_of(path));
            snprintf(s->wad, sizeof s->wad, "%s", rel);
            return true;
        }
        return false;
    }
    if (ends_with_ci(input, ".wad")) {
        s->kind = SRC_DIR;
        dir_of(s->dir, sizeof s->dir, input);
        snprintf(s->wad, sizeof s->wad, "%s", base_of(input));
        return true;
    }
    if (ends_with_ci(input, ".zip")) {
        if (!zip_open(&s->zip, input, err, sizeof err)) return false;
        s->kind = SRC_ZIP;
        for (int i = 0; i < s->zip.n; i++) consider(s, s->zip.entries[i].name);
        s->cue[0] = 0;   /* a zipped disc image is main.c's */
    } else if (ends_with_ci(input, ".7z")) {
        s->sz = sz_open(input, err, sizeof err);
        if (!s->sz) {
            if (!quiet) snprintf(g_err, sizeof g_err, "%s", err);
            return false;
        }
        s->kind = SRC_7Z;
        for (int i = 0; i < sz_count(s->sz); i++)
            if (!sz_is_dir(s->sz, i)) consider(s, sz_name(s->sz, i));
    } else {
        return false;
    }
    if (s->prefix[0] == '/' && !s->prefix[1]) s->prefix[0] = 0;
    else if (s->prefix[0]) {
        for (char *c = s->prefix; *c; c++) if (*c == '\\') *c = '/';
    }
    /* title.tmd in a folder (prefix), or at the archive's root */
    bool wup = s->prefix[0] || (s->kind == SRC_ZIP ? zip_find(&s->zip, "title.tmd") != NULL
                                                   : sz_find(s->sz, "title.tmd") >= 0);
    if (wup || s->wad[0] || s->cue[0]) return true;
    src_close(s);
    return false;
}

bool vc_detect(const char *input)
{
    Src s;
    if (!src_open(input, &s, true)) return false;
    src_close(&s);
    return true;
}

/* ---- keys ---------------------------------------------------------------------------------- */

typedef struct Keys {
    uint8_t (*k)[16];
    int n, cap;
} Keys;

static void key_add(Keys *ks, const uint8_t *k)
{
    for (int i = 0; i < ks->n; i++)
        if (!memcmp(ks->k[i], k, 16)) return;
    if (ks->n == ks->cap) {
        int cap = ks->cap ? ks->cap * 2 : 64;
        void *p = realloc(ks->k, (size_t)cap * 16);
        if (!p) return;
        ks->k = p;
        ks->cap = cap;
    }
    memcpy(ks->k[ks->n++], k, 16);
}

static int hexval(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

/* a text file: every run of exactly 32 hex digits (keys.txt, as Cemu's). Anything else small
 * (a Wii U's otp.bin, a Wii's keys.bin, a bare key): every 16 bytes at a multiple of 4. */
static void keys_from_file(Keys *ks, const char *path)
{
    size_t n = 0;
    uint8_t *d = SDL_LoadFile(path, &n);
    if (!d) return;
    bool text = n > 0;
    for (size_t i = 0; i < n && text; i++) text = d[i] == '\t' || d[i] == '\r' || d[i] == '\n' || d[i] >= 32;
    int before = ks->n;
    if (text) {
        for (size_t i = 0; i < n;) {
            size_t j = i;
            while (j < n && isxdigit(d[j])) j++;
            if (j - i == 32) {
                uint8_t k[16];
                for (int b = 0; b < 16; b++) k[b] = (uint8_t)(hexval(d[i + 2 * b]) << 4 | hexval(d[i + 2 * b + 1]));
                key_add(ks, k);
            }
            i = j > i ? j : i + 1;
        }
    } else if (n <= 4096) {
        for (size_t o = 0; o + 16 <= n; o += 4) key_add(ks, d + o);
    }
    SDL_free(d);
    if (ks->n > before) say("  keys: %s\n", path);
}

static void keys_from_dir(Keys *ks, const char *dir)
{
    static const char *names[] = {"keys.txt", "otp.bin", "keys.bin", "common-key.bin"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[1300];
        join(p, sizeof p, dir, names[i]);
        if (SDL_GetPathInfo(p, NULL)) keys_from_file(ks, p);
    }
}

static void keys_from(Keys *ks, const char *path)
{
    SDL_PathInfo info;
    if (!path || !*path || !SDL_GetPathInfo(path, &info)) return;
    if (info.type == SDL_PATHTYPE_DIRECTORY) keys_from_dir(ks, path);
    else keys_from_file(ks, path);
}

static void keys_gather(Keys *ks, const char *input)
{
    keys_from(ks, vc_keys_file);
    keys_from(ks, SDL_getenv("RONDO_KEYS"));
    char dir[1100], parent[1100];
    SDL_PathInfo info;
    if (SDL_GetPathInfo(input, &info) && info.type == SDL_PATHTYPE_DIRECTORY) snprintf(dir, sizeof dir, "%s", input);
    else dir_of(dir, sizeof dir, input);
    keys_from_dir(ks, dir);
    dir_of(parent, sizeof parent, dir);
    keys_from_dir(ks, parent);
    keys_from_dir(ks, rondo_tools_dir());
    keys_from_dir(ks, rondo_home_dir());
}

/* The title key: a key that, as the common key decrypting the ticket's title key (or as the
 * title key itself), decrypts the start of content 0 to what it must be. */
static bool unlock(const Keys *ks, const uint8_t enc_tk[16], const uint8_t tid[8], uint16_t index,
                   const uint8_t *first, size_t len, bool (*ok)(const uint8_t *plain), uint8_t tk_out[16])
{
    uint8_t buf[0x60];
    if (len > sizeof buf) len = sizeof buf;
    for (int i = 0; i < ks->n; i++) {
        for (int as_title = 0; as_title < 2; as_title++) {
            uint8_t tk[16], iv[16] = {0};
            if (as_title) {
                memcpy(tk, ks->k[i], 16);
            } else {
                memcpy(tk, enc_tk, 16);
                memcpy(iv, tid, 8);
                aes128_cbc_decrypt(ks->k[i], iv, tk, 16);
            }
            memset(iv, 0, 16);
            iv[0] = (uint8_t)(index >> 8);
            iv[1] = (uint8_t)index;
            memcpy(buf, first, len);
            aes128_cbc_decrypt(tk, iv, buf, len);
            if (ok(buf)) {
                memcpy(tk_out, tk, 16);
                return true;
            }
        }
    }
    return false;
}

static bool is_fst(const uint8_t *p) { return !memcmp(p, "FST", 4); }
/* a Wii title's game content: a U8 archive, or LZ77-compressed */
static bool is_u8(const uint8_t *p) { return be32(p) == 0x55AA382D || !memcmp(p, "LZ77", 4); }

/* ---- the files of the disc: a name, its data ------------------------------------------------ */

typedef struct VcFile {
    char name[160];
    const uint8_t *data;
    size_t size;
} VcFile;

typedef struct VcFiles {
    VcFile *f;
    int n, cap;
    Blob *keep;          /* buffers the files point into */
    int n_keep, cap_keep;
} VcFiles;

static void files_add(VcFiles *fs, const char *name, const uint8_t *data, size_t size)
{
    if (fs->n == fs->cap) {
        int cap = fs->cap ? fs->cap * 2 : 64;
        void *p = realloc(fs->f, (size_t)cap * sizeof *fs->f);
        if (!p) return;
        fs->f = p;
        fs->cap = cap;
    }
    VcFile *f = &fs->f[fs->n++];
    snprintf(f->name, sizeof f->name, "%s", name);
    f->data = data;
    f->size = size;
}

static void files_keep(VcFiles *fs, Blob b)
{
    if (fs->n_keep == fs->cap_keep) {
        int cap = fs->cap_keep ? fs->cap_keep * 2 : 16;
        void *p = realloc(fs->keep, (size_t)cap * sizeof *fs->keep);
        if (!p) { blob_free(&b); return; }
        fs->keep = p;
        fs->cap_keep = cap;
    }
    fs->keep[fs->n_keep++] = b;
}

static void files_free(VcFiles *fs)
{
    for (int i = 0; i < fs->n_keep; i++) blob_free(&fs->keep[i]);
    free(fs->keep);
    free(fs->f);
    memset(fs, 0, sizeof *fs);
}

/* pce.pkg (Wii U): u32 length, then (u32 size, name, NUL, data) for each file */
static void pkg_files(VcFiles *fs, const uint8_t *d, size_t n)
{
    size_t o = 4;
    while (o + 5 <= n) {
        uint32_t size = le32(d + o);
        const uint8_t *nm = d + o + 4;
        size_t room = n - (o + 4) < 256 ? n - (o + 4) : 256;
        const uint8_t *z = memchr(nm, 0, room);
        if (!z) break;
        size_t at = (size_t)(z - d) + 1;
        if (at + size > n) break;
        files_add(fs, (const char *)nm, d + at, size);
        o = at + size;
    }
}

/* ---- the Wii U --------------------------------------------------------------------------- */

typedef struct Content {
    uint32_t id;
    uint16_t index, type;
    uint64_t size;
    uint8_t sha1[20];
} Content;

static void app_name(char *out, size_t n, uint32_t id) { snprintf(out, n, "%08X.app", (unsigned)id); }

/* `size` bytes at `offset` of a file in a content (the whole .app in `app`) */
static bool wup_file_data(const uint8_t tk[16], const Content *c, const Blob *app, uint64_t offset, uint64_t size,
                          uint8_t *out)
{
    if (c->type & 2) {   /* hashed: 0x10000-byte blocks, 0x400 bytes of hashes, then 0xFC00 of data */
        static uint8_t hashes[0x400], data[0xFC00];
        uint64_t block = offset / 0xFC00, within = offset % 0xFC00, got = 0;
        int last_pct = -1;
        while (got < size) {
            uint64_t at = block * 0x10000;
            if (at + 0x10000 > app->size) { snprintf(g_err, sizeof g_err, "content %08X is cut short", (unsigned)c->id); return false; }
            uint8_t iv[16] = {0};
            memcpy(hashes, app->data + at, 0x400);
            aes128_cbc_decrypt(tk, iv, hashes, 0x400);
            memcpy(iv, hashes + 0x14 * (block % 16), 16);
            memcpy(data, app->data + at + 0x400, 0xFC00);
            aes128_cbc_decrypt(tk, iv, data, 0xFC00);
            uint64_t take = 0xFC00 - within < size - got ? 0xFC00 - within : size - got;
            memcpy(out + got, data + within, (size_t)take);
            got += take;
            within = 0;
            block++;
            int pct = (int)(got * 100 / size);
            if (pct != last_pct) { setup_progress_hook("Decrypting", pct); last_pct = pct; }
        }
        return true;
    }
    uint64_t start = offset / 16 * 16, span = (offset - start + size + 15) / 16 * 16;
    if (start + span > app->size) { snprintf(g_err, sizeof g_err, "content %08X is cut short", (unsigned)c->id); return false; }
    uint8_t iv[16] = {0};
    if (start) memcpy(iv, app->data + start - 16, 16);
    else { iv[0] = (uint8_t)(c->index >> 8); iv[1] = (uint8_t)c->index; }
    uint8_t *tmp = malloc((size_t)span);
    if (!tmp) { snprintf(g_err, sizeof g_err, "not enough memory"); return false; }
    memcpy(tmp, app->data + start, (size_t)span);
    aes128_cbc_decrypt(tk, iv, tmp, (size_t)span);
    memcpy(out, tmp + (offset - start), (size_t)size);
    free(tmp);
    return true;
}

static bool wup_read(Src *s, const Keys *ks, VcFiles *fs, bool *need_key)
{
    Blob tmd = {0}, tik = {0}, c0 = {0}, app = {0};
    Content *cs = NULL;
    bool ok = false;
    if (!src_read(s, "title.tmd", &tmd) || !src_read(s, "title.tik", &tik)) goto done;
    if (tik.size < 0x1E4 || tmd.size < 0xB04) { snprintf(g_err, sizeof g_err, "title.tmd or title.tik is too short"); goto done; }
    int n = be16(tmd.data + 0x1DE);
    if (n < 1 || tmd.size < 0xB04 + 0x30 * (size_t)n) { snprintf(g_err, sizeof g_err, "title.tmd lists no contents"); goto done; }
    cs = calloc((size_t)n, sizeof *cs);
    if (!cs) goto done;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = tmd.data + 0xB04 + 0x30 * i;
        cs[i].id = be32(r);
        cs[i].index = be16(r + 4);
        cs[i].type = be16(r + 6);
        cs[i].size = be64(r + 8);
    }
    say("  Wii U title %016llX, %d contents\n", (unsigned long long)be64(tmd.data + 0x18C), n);
    char name[32];
    app_name(name, sizeof name, cs[0].id);
    if (!src_read(s, name, &c0)) goto done;
    if (c0.size < 0x40) { snprintf(g_err, sizeof g_err, "%s is too short", name); goto done; }
    uint8_t tk[16];
    if (!unlock(ks, tik.data + 0x1BF, tik.data + 0x1DC, cs[0].index, c0.data, 16, is_fst, tk)) {
        *need_key = true;
        snprintf(g_err, sizeof g_err, "no key opens this Wii U title: Setup needs the Wii U common key, in a keys.txt (as "
                 "Cemu's) or a Wii U's otp.bin, next to it or to Setup, or chosen with --keys");
        goto done;
    }
    say("  the key opens it\n");

    /* the file system */
    uint8_t iv[16] = {0};
    iv[0] = (uint8_t)(cs[0].index >> 8);
    iv[1] = (uint8_t)cs[0].index;
    size_t fst_size = c0.size / 16 * 16;
    if (c0.owner == 0) {   /* (a 7z's buffer: decrypt a copy) */
        uint8_t *copy = malloc(fst_size);
        if (!copy) goto done;
        memcpy(copy, c0.data, fst_size);
        c0.data = copy;
        c0.owner = 1;
    }
    aes128_cbc_decrypt(tk, iv, c0.data, fst_size);
    const uint8_t *fst = c0.data;
    uint32_t factor = be32(fst + 4), nclusters = be32(fst + 8);
    size_t ent0 = 0x20 + 0x20 * (size_t)nclusters;
    if (ent0 + 16 > fst_size) { snprintf(g_err, sizeof g_err, "the title's file system is damaged"); goto done; }
    uint32_t nent = be32(fst + ent0 + 8);
    size_t names = ent0 + 16 * (size_t)nent;
    if (names >= fst_size) { snprintf(g_err, sizeof g_err, "the title's file system is damaged"); goto done; }
    struct { uint32_t end; size_t len; } stack[32];
    int depth = 0;
    char path[1024] = "";
    const uint8_t *pkg = NULL;
    for (uint32_t i = 1; i < nent && !pkg; i++) {
        while (depth && i >= stack[depth - 1].end) path[stack[--depth].len] = 0;
        const uint8_t *e = fst + ent0 + 16 * (size_t)i;
        size_t noff = (size_t)e[1] << 16 | (size_t)e[2] << 8 | e[3];
        if (names + noff >= fst_size || !memchr(fst + names + noff, 0, fst_size - names - noff)) break;
        size_t len = strlen(path);
        snprintf(path + len, sizeof path - len, "/%s", (const char *)fst + names + noff);
        if (e[0] & 1) {
            if (depth < 32) { stack[depth].end = be32(e + 8); stack[depth].len = len; depth++; }
            else path[len] = 0;
        } else {
            if (!SDL_strcasecmp(path, "/content/pceemu/pce.pkg")) pkg = e;
            else path[len] = 0;
        }
    }
    if (!pkg) { snprintf(g_err, sizeof g_err, "this Wii U title has no /content/pceemu/pce.pkg: not a PC Engine Virtual Console game"); goto done; }
    uint64_t off = be32(pkg + 4), size = be32(pkg + 8);
    uint16_t flags = be16(pkg + 12), ci = be16(pkg + 14);
    if (!(flags & 4)) off *= factor;
    if (ci >= n) { snprintf(g_err, sizeof g_err, "the title's file system is damaged"); goto done; }
    app_name(name, sizeof name, cs[ci].id);
    say("  /content/pceemu/pce.pkg: %.1f MB in %s\n", (double)size / 1e6, name);
    if (!src_read(s, name, &app)) goto done;
    Blob out = {malloc((size_t)size + 1), (size_t)size, 1};
    if (!out.data) { snprintf(g_err, sizeof g_err, "not enough memory"); goto done; }
    if (!wup_file_data(tk, &cs[ci], &app, off, size, out.data)) { blob_free(&out); goto done; }
    blob_free(&app);
    files_keep(fs, out);
    pkg_files(fs, out.data, out.size);
    ok = true;
done:
    blob_free(&tmd);
    blob_free(&tik);
    blob_free(&c0);
    blob_free(&app);
    free(cs);
    return ok;
}

/* ---- the Wii ------------------------------------------------------------------------------- */

/* Nintendo's LZ77 ("LZ77" then 0x10 or 0x11, or a bare 0x10/0x11 header): NULL if it is not */
static uint8_t *lz77(const uint8_t *d, size_t n, size_t *out_n)
{
    if (n >= 4 && !memcmp(d, "LZ77", 4)) { d += 4; n -= 4; }
    if (n < 4 || (d[0] != 0x10 && d[0] != 0x11)) return NULL;
    int type = d[0];
    size_t size = (size_t)d[1] | (size_t)d[2] << 8 | (size_t)d[3] << 16, i = 4;
    if (!size && n >= 8) { size = le32(d + 4); i = 8; }
    if (!size || size > (1u << 30)) return NULL;
    uint8_t *out = malloc(size);
    if (!out) return NULL;
    size_t o = 0;
    while (o < size && i < n) {
        uint8_t flags = d[i++];
        for (int b = 0; b < 8 && o < size; b++, flags <<= 1) {
            if (!(flags & 0x80)) {
                if (i >= n) break;
                out[o++] = d[i++];
                continue;
            }
            size_t len, disp;
            if (type == 0x10) {
                if (i + 2 > n) { free(out); return NULL; }
                len = (size_t)(d[i] >> 4) + 3;
                disp = ((size_t)(d[i] & 15) << 8 | d[i + 1]) + 1;
                i += 2;
            } else {
                if (i >= n) { free(out); return NULL; }
                int ind = d[i] >> 4;
                if (ind == 0) {
                    if (i + 3 > n) { free(out); return NULL; }
                    len = ((size_t)(d[i] & 15) << 4 | d[i + 1] >> 4) + 0x11;
                    disp = ((size_t)(d[i + 1] & 15) << 8 | d[i + 2]) + 1;
                    i += 3;
                } else if (ind == 1) {
                    if (i + 4 > n) { free(out); return NULL; }
                    len = ((size_t)(d[i] & 15) << 12 | (size_t)d[i + 1] << 4 | d[i + 2] >> 4) + 0x111;
                    disp = ((size_t)(d[i + 2] & 15) << 8 | d[i + 3]) + 1;
                    i += 4;
                } else {
                    if (i + 2 > n) { free(out); return NULL; }
                    len = (size_t)ind + 1;
                    disp = ((size_t)(d[i] & 15) << 8 | d[i + 1]) + 1;
                    i += 2;
                }
            }
            if (disp > o) { free(out); return NULL; }
            for (size_t k = 0; k < len && o < size; k++, o++) out[o] = out[o - disp];
        }
    }
    *out_n = o;
    return out;
}

/* a U8 archive's files into fs (its buffer kept by the caller) */
static bool u8_files(VcFiles *fs, const uint8_t *d, size_t n)
{
    if (n < 0x20 || be32(d) != 0x55AA382D) return false;
    size_t root = be32(d + 4);
    if (root + 12 > n) return false;
    uint32_t count = be32(d + root + 8);
    size_t names = root + 12 * (size_t)count;
    if (!count || names > n) return false;
    struct { uint32_t end; size_t len; } stack[32];
    int depth = 0;
    char path[512] = "";
    for (uint32_t i = 1; i < count; i++) {
        while (depth && i >= stack[depth - 1].end) path[stack[--depth].len] = 0;
        const uint8_t *e = d + root + 12 * (size_t)i;
        size_t noff = (size_t)e[1] << 16 | (size_t)e[2] << 8 | e[3];
        if (names + noff >= n || !memchr(d + names + noff, 0, n - names - noff)) return false;
        size_t len = strlen(path);
        snprintf(path + len, sizeof path - len, "%s%s", len ? "/" : "", (const char *)d + names + noff);
        if (e[0] == 1) {
            if (depth < 32) { stack[depth].end = be32(e + 8); stack[depth].len = len; depth++; }
            else path[len] = 0;
        } else {
            uint32_t off = be32(e + 4), size = be32(e + 8);
            if ((uint64_t)off + size <= n) files_add(fs, path, d + off, size);
            path[len] = 0;
        }
    }
    return true;
}

/* a Wii TMD's contents (records of 36 bytes from 0x1E4): their number, 0 if it is not one */
static int wii_tmd(const uint8_t *tmd, size_t size, Content *cs, int max)
{
    if (size < 0x1E4) return 0;
    int n = be16(tmd + 0x1DE);
    if (n < 1 || 0x1E4 + 36 * (uint64_t)n > size) return 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = tmd + 0x1E4 + 36 * i;
        cs[i].id = be32(r);
        cs[i].index = be16(r + 4);
        cs[i].type = be16(r + 6);
        cs[i].size = be64(r + 8);
        memcpy(cs[i].sha1, r + 16, 20);
    }
    return n;
}

/* One decrypted content of a Wii title (c, taken over), checked against the TMD's SHA-1: the
 * game if it is a U8 archive (LZ77-compressed or not) with the disc's track list. 1 found,
 * 0 not this one, -1 damaged. */
static int wii_game(VcFiles *fs, Blob c, const Content *ct, const char *what)
{
    uint8_t sum[20];
    if (c.size < ct->size) { blob_free(&c); snprintf(g_err, sizeof g_err, "%s: content %08x is cut short", what, (unsigned)ct->id); return -1; }
    sha1(c.data, (size_t)ct->size, sum);
    if (memcmp(sum, ct->sha1, 20)) {
        blob_free(&c);
        snprintf(g_err, sizeof g_err, "%s: content %08x is not the one its list gives (damaged)", what, (unsigned)ct->id);
        return -1;
    }
    c.size = (size_t)ct->size;
    size_t un = 0;
    uint8_t *u = lz77(c.data, c.size, &un);
    if (u) { blob_free(&c); c = (Blob){u, un, 1}; }
    int before = fs->n;
    bool found = false;
    if (u8_files(fs, c.data, c.size)) {
        for (int k = before; k < fs->n; k++) {   /* files inside may be compressed too */
            size_t fn = 0;
            uint8_t *fu = lz77(fs->f[k].data, fs->f[k].size, &fn);
            if (fu) { files_keep(fs, (Blob){fu, fn, 1}); fs->f[k].data = fu; fs->f[k].size = fn; }
        }
        for (int k = before; k < fs->n; k++) found |= ends_with_ci(fs->f[k].name, ".hcd");
        if (!found) fs->n = before;
    }
    if (found) files_keep(fs, c);
    else blob_free(&c);
    return found;
}

/* the game among a title's decrypted contents in folder `dir` (as a Wii, or Dolphin, keeps an
 * installed title: title/<id>/content/NNNNNNNN.app), checked against `cs` */
static bool wii_installed(VcFiles *fs, const char *dir, const Content *cs, int n, const char *what)
{
    Src s = {0};
    s.kind = SRC_DIR;
    snprintf(s.dir, sizeof s.dir, "%s", dir);
    for (int i = 0; i < n; i++) {
        if (cs[i].type & 0x8000) continue;   /* shared contents (the system's): not the game */
        char name[32];
        snprintf(name, sizeof name, "%08x.app", (unsigned)cs[i].id);
        Blob c;
        if (!src_read(&s, name, &c)) continue;
        int r = wii_game(fs, c, &cs[i], what);
        if (r) return r > 0;
    }
    snprintf(g_err, sizeof g_err, "%s: no PC Engine CD track list (.hcd) in its contents (are they all there?)", what);
    return false;
}

/* Dolphin's Wii storage (where it installs WADs, decrypted): its own setting
 * (Dolphin.ini: NANDRootPath), then the usual places, on Windows and Linux (Flatpak too) */
static int dolphin_nands(char out[][1100], int max)
{
    int n = 0;
    const char *home = SDL_getenv("HOME"), *appdata = SDL_getenv("APPDATA"), *profile = SDL_getenv("USERPROFILE");
    char cfg[6][1100], def[6][1100];
    int nc = 0, nd = 0;
    if (appdata) {
        snprintf(cfg[nc++], sizeof cfg[0], "%s/Dolphin Emulator/Config/Dolphin.ini", appdata);
        snprintf(def[nd++], sizeof def[0], "%s/Dolphin Emulator/Wii", appdata);
    }
    if (profile) {
        snprintf(cfg[nc++], sizeof cfg[0], "%s/Documents/Dolphin Emulator/Config/Dolphin.ini", profile);
        snprintf(def[nd++], sizeof def[0], "%s/Documents/Dolphin Emulator/Wii", profile);
    }
    if (home) {
        snprintf(cfg[nc++], sizeof cfg[0], "%s/.config/dolphin-emu/Dolphin.ini", home);
        snprintf(cfg[nc++], sizeof cfg[0], "%s/.var/app/org.DolphinEmu.dolphin-emu/config/dolphin-emu/Dolphin.ini", home);
        snprintf(def[nd++], sizeof def[0], "%s/.local/share/dolphin-emu/Wii", home);
        snprintf(def[nd++], sizeof def[0], "%s/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu/Wii", home);
    }
    for (int i = 0; i < nc && n < max; i++) {
        size_t len = 0;
        char *ini = SDL_LoadFile(cfg[i], &len);
        if (!ini) continue;
        for (char *line = strtok(ini, "\r\n"); line && n < max; line = strtok(NULL, "\r\n")) {
            while (*line == ' ' || *line == '\t') line++;
            if (SDL_strncasecmp(line, "NANDRootPath", 12)) continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            eq++;
            while (*eq == ' ') eq++;
            size_t l = strlen(eq);
            while (l && (eq[l - 1] == ' ' || eq[l - 1] == '/' || eq[l - 1] == '\\')) eq[--l] = 0;
            if (l) snprintf(out[n++], 1100, "%s", eq);
        }
        SDL_free(ini);
    }
    for (int i = 0; i < nd && n < max; i++) snprintf(out[n++], 1100, "%s", def[i]);
    return n;
}

/* a Wii title installed in Dolphin (decrypted) with these contents: its content folder */
static bool dolphin_title(uint64_t tid, const Content *cs, char *dir, size_t dir_len)
{
    char nands[8][1100];
    int n = dolphin_nands(nands, 8);
    for (int i = 0; i < n; i++) {
        char d[1300], first[1400];
        snprintf(d, sizeof d, "%s/title/%08x/%08x/content", nands[i], (unsigned)(tid >> 32), (unsigned)tid);
        snprintf(first, sizeof first, "%s/%08x.app", d, (unsigned)cs[0].id);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(first, &info) && info.size == cs[0].size) {
            snprintf(dir, dir_len, "%s", d);
            return true;
        }
    }
    return false;
}

static bool wad_read(Src *s, const Keys *ks, VcFiles *fs, bool *need_key)
{
    Blob w = {0};
    bool ok = false;
    if (!src_read(s, s->wad, &w)) return false;
    const uint8_t *d = w.data;
    if (w.size < 0x40 || be32(d) != 0x20) { snprintf(g_err, sizeof g_err, "%s is not a WAD", s->wad); goto done; }
    uint32_t certsz = be32(d + 8), tiksz = be32(d + 0x10), tmdsz = be32(d + 0x14);
    uint64_t tik_off = align64(align64(0x20) + certsz), tmd_off = align64(tik_off + tiksz), data_off = align64(tmd_off + tmdsz);
    if (tiksz < 0x2A4 || tmdsz < 0x1E4 || data_off > w.size) { snprintf(g_err, sizeof g_err, "%s is damaged", s->wad); goto done; }
    const uint8_t *tik = d + tik_off, *tmd = d + tmd_off;
    Content cs[64];
    uint64_t at[64];
    int n = wii_tmd(tmd, tmdsz, cs, 64);
    if (!n) { snprintf(g_err, sizeof g_err, "%s lists no contents", s->wad); goto done; }
    uint64_t tid = be64(tmd + 0x18C);
    say("  Wii title %016llX, %d contents\n", (unsigned long long)tid, n);
    uint64_t pos = data_off;
    int big = 0;
    for (int i = 0; i < n; i++) {
        at[i] = pos;
        pos = align64(pos + ((cs[i].size + 15) & ~(uint64_t)15));
        if (cs[i].size > cs[big].size) big = i;
    }
    if (pos > align64(w.size)) { snprintf(g_err, sizeof g_err, "%s is cut short", s->wad); goto done; }
    uint8_t tk[16];
    if (!unlock(ks, tik + 0x1BF, tik + 0x1DC, cs[big].index, d + at[big], 16, is_u8, tk)) {
        /* no key: the same title installed in Dolphin is decrypted already */
        char dir[1300];
        if (dolphin_title(tid, cs, dir, sizeof dir)) {
            say("  no key here, but Dolphin has the title installed: %s\n", dir);
            ok = wii_installed(fs, dir, cs, n, s->wad);
            goto done;
        }
        *need_key = true;
        snprintf(g_err, sizeof g_err, "no key opens this WAD: Setup needs the Wii common key (in a Wii's keys.bin, a Wii "
                 "U's otp.bin or a keys.txt, next to it or to Setup, or chosen with --keys), or the WAD installed in Dolphin");
        goto done;
    }
    say("  the key opens it\n");
    for (int i = 0; i < n && !ok; i++) {   /* the game first: the largest content */
        int k = i == 0 ? big : i == big ? 0 : i;
        if (cs[k].type & 0x8000 && k != big) continue;
        uint64_t len = (cs[k].size + 15) & ~(uint64_t)15;
        if (at[k] + len > w.size) { snprintf(g_err, sizeof g_err, "%s is cut short", s->wad); goto done; }
        Blob c = {malloc((size_t)len), (size_t)len, 1};
        if (!c.data) { snprintf(g_err, sizeof g_err, "not enough memory"); goto done; }
        memcpy(c.data, d + at[k], (size_t)len);
        uint8_t iv[16] = {0};
        iv[0] = (uint8_t)(cs[k].index >> 8);
        iv[1] = (uint8_t)cs[k].index;
        aes128_cbc_decrypt(tk, iv, c.data, (size_t)len);
        int r = wii_game(fs, c, &cs[k], s->wad);
        if (r < 0) goto done;
        ok = r > 0;
    }
    if (!ok) snprintf(g_err, sizeof g_err, "no PC Engine CD track list (.hcd) in %s: a layout Setup does not know", s->wad);
done:
    blob_free(&w);
    return ok;
}

/* A folder holding an installed Wii title (title.tmd and its decrypted contents, as Dolphin's
 * title/<id>/content): no key needed. */
static bool wii_title_read(Src *s, VcFiles *fs)
{
    Blob tmd;
    if (!src_read(s, "title.tmd", &tmd)) return false;
    Content cs[64];
    int n = wii_tmd(tmd.data, tmd.size, cs, 64);
    uint64_t tid = tmd.size >= 0x194 ? be64(tmd.data + 0x18C) : 0;
    blob_free(&tmd);
    if (!n) { snprintf(g_err, sizeof g_err, "title.tmd lists no contents"); return false; }
    say("  Wii title %016llX, installed (decrypted): %s\n", (unsigned long long)tid, s->dir);
    return wii_installed(fs, s->dir, cs, n, "the installed title");
}

/* title.tmd: a Wii title's (an installed one) rather than a Wii U's (a WUP) */
static bool is_wii_tmd(Src *s)
{
    Blob tmd;
    if (!src_read(s, "title.tmd", &tmd)) return false;
    bool wii = tmd.size >= 0x194 && (be32(tmd.data + 0x18C) >> 16) == 0x0001;
    blob_free(&tmd);
    g_err[0] = 0;
    return wii;
}

/* ---- the disc ------------------------------------------------------------------------------ */

typedef struct VcTrack {
    int number;
    bool data;
    char file[160];
    int start, length, sectors;
} VcTrack;

static const VcFile *find_file(const VcFiles *fs, const char *name)
{
    for (int i = 0; i < fs->n; i++)
        if (!SDL_strcasecmp(fs->f[i].name, name)) return &fs->f[i];
    return NULL;
}

static void msf(char *out, size_t n, int frames)
{
    snprintf(out, n, "%02d:%02d:%02d", frames / 4500, frames / 75 % 60, frames % 75);
}

/* a data track: zlib blocks (a u32 count, then count x (u32 offset, u32 size)) or plain sectors */
static bool data_track(const VcFile *f, uint8_t *out, size_t want)
{
    memset(out, 0, want);
    const uint8_t *d = f->data;
    size_t n = f->size;
    uint32_t count = n >= 12 ? le32(d) : 0;
    if (!(count && 4 + 8 * (uint64_t)count <= n && le32(d + 4) == 4 + 8 * count)) {
        memcpy(out, d, n < want ? n : want);
        return true;
    }
    static uint8_t buf[1 << 20];
    size_t at = 0;
    for (uint32_t i = 0; i < count && at < want; i++) {
        uint32_t off = le32(d + 4 + 8 * i), size = le32(d + 8 + 8 * i);
        uLongf got = sizeof buf;
        if ((uint64_t)off + size > n || uncompress(buf, &got, d + off, size) != Z_OK) {
            snprintf(g_err, sizeof g_err, "%s is damaged (block %u)", f->name, (unsigned)i);
            return false;
        }
        size_t take = got < want - at ? got : want - at;
        memcpy(out + at, buf, take);
        at += take;
    }
    return true;
}

static long gcd(long a, long b)
{
    while (b) { long t = a % b; a = b; b = t; }
    return a;
}

/* 44.1 kHz stereo from `frames` frames of `ch` channels at `rate`: windowed-sinc interpolation,
 * polyphase (rate and 44100 reduced by their gcd: 441 phases from 32 kHz) */
static void to_cd_rate(const short *in, long frames, int ch, int rate, short *out, long out_frames)
{
    enum { T = 12 };   /* taps on each side */
    const double pi = 3.14159265358979323846;
    long g = gcd(rate, 44100), L = 44100 / g, M = rate / g;
    double fc = (rate < 44100 ? 0.5 : 0.5 * 44100.0 / rate) * 0.95;   /* cycles per input sample */
    float *h = malloc(sizeof *h * (size_t)L * 2 * T);
    if (!h) return;
    for (long p = 0; p < L; p++)
        for (int j = 0; j < 2 * T; j++) {
            double t = (j - (T - 1)) - (double)p / L, x = 2 * fc * t;
            double sinc = fabs(x) < 1e-12 ? 1.0 : sin(pi * x) / (pi * x);
            double w = fabs(t) < T ? 0.5 * (1 + cos(pi * t / T)) : 0;
            h[p * 2 * T + j] = (float)(2 * fc * sinc * w);
        }
    for (long k = 0; k < out_frames; k++) {
        long long num = (long long)k * M;
        long n = (long)(num / L);
        const float *hp = h + (num % L) * 2 * T;
        for (int c = 0; c < 2; c++) {
            int sc = ch == 1 ? 0 : c;
            float acc = 0;
            long i0 = n - (T - 1);
            for (int j = 0; j < 2 * T; j++) {
                long i = i0 + j;
                if (i >= 0 && i < frames) acc += (float)in[i * ch + sc] * hp[j];
            }
            long v = lrintf(acc);
            out[k * 2 + c] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    free(h);
}

static bool write_wav(const char *path, const short *pcm, long frames)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) return false;
    uint32_t bytes = (uint32_t)frames * 4;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                     0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a'};
    uint32_t riff = bytes + 36;
    for (int i = 0; i < 4; i++) { h[4 + i] = (uint8_t)(riff >> (8 * i)); h[40 + i] = (uint8_t)(bytes >> (8 * i)); }
    bool ok = SDL_WriteIO(io, h, 44) == 44 && SDL_WriteIO(io, pcm, bytes) == bytes;
    return SDL_CloseIO(io) && ok;
}

/* an audio track: Ogg Vorbis (or a WAV), as `sectors` CD sectors of 44.1 kHz stereo */
static bool audio_track(const VcFile *f, const char *path, int sectors, int *rate_seen)
{
    long out_frames = (long)sectors * 588;
    short *out = calloc((size_t)out_frames * 2 + 2, sizeof *out);
    if (!out) { snprintf(g_err, sizeof g_err, "not enough memory"); return false; }
    int ch = 0, rate = 0;
    short *pcm = NULL;
    long frames = 0;
    if (f->size > 4 && !memcmp(f->data, "OggS", 4)) {
        frames = stb_vorbis_decode_memory(f->data, (int)f->size, &ch, &rate, &pcm);
        if (frames < 0 || ch < 1) { free(out); snprintf(g_err, sizeof g_err, "%s cannot be decoded", f->name); return false; }
    } else if (f->size > 44 && !memcmp(f->data, "RIFF", 4) && !memcmp(f->data + 8, "WAVE", 4)) {
        ch = f->data[22];
        rate = (int)le32(f->data + 24);
        size_t o = 12;
        while (o + 8 <= f->size && memcmp(f->data + o, "data", 4)) o += 8 + le32(f->data + o + 4);
        if (o + 8 > f->size || ch < 1 || f->data[34] != 16) { free(out); snprintf(g_err, sizeof g_err, "%s is a WAV Setup cannot read", f->name); return false; }
        size_t bytes = le32(f->data + o + 4);
        if (o + 8 + bytes > f->size) bytes = f->size - o - 8;
        frames = (long)(bytes / (2 * (size_t)ch));
        pcm = malloc(bytes + 2);
        if (pcm) memcpy(pcm, f->data + o + 8, bytes);
    } else {
        free(out);
        snprintf(g_err, sizeof g_err, "%s is audio in a format Setup does not know", f->name);
        return false;
    }
    if (!pcm) { free(out); snprintf(g_err, sizeof g_err, "not enough memory"); return false; }
    *rate_seen = rate;
    to_cd_rate(pcm, frames, ch, rate, out, out_frames);
    free(pcm);
    bool ok = write_wav(path, out, out_frames);
    free(out);
    if (!ok) snprintf(g_err, sizeof g_err, "cannot write %s", path);
    return ok;
}

static bool build_disc(const VcFiles *fs, const char *stage, const char *what, char *cue_out, size_t cue_len)
{
    const VcFile *hcd = NULL;
    for (int i = 0; i < fs->n && !hcd; i++)
        if (ends_with_ci(fs->f[i].name, ".hcd")) hcd = &fs->f[i];
    if (!hcd) { snprintf(g_err, sizeof g_err, "no PC Engine CD track list (.hcd): not a PC Engine CD game"); return false; }
    char dir[160];
    snprintf(dir, sizeof dir, "%s", hcd->name);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;

    /* the track list */
    VcTrack tr[100];
    int n = 0;
    char *text = malloc(hcd->size + 1);
    if (!text) return false;
    memcpy(text, hcd->data, hcd->size);
    text[hcd->size] = 0;
    for (char *line = strtok(text, "\r\n"); line && n < 99; line = strtok(NULL, "\r\n")) {
        char f[5][160] = {{0}};
        int k = 0;
        for (char *p = line; k < 5;) {
            char *c = strchr(p, ',');
            size_t len = c ? (size_t)(c - p) : strlen(p);
            while (len && isspace((unsigned char)p[len - 1])) len--;
            while (len && isspace((unsigned char)*p)) { p++; len--; }
            snprintf(f[k++], sizeof f[0], "%.*s", (int)len, p);
            if (!c) break;
            p = c + 1;
        }
        if (k < 5 || SDL_strncasecmp(f[0], "track", 5)) continue;
        VcTrack *t = &tr[n++];
        t->number = atoi(f[0] + 5);
        t->data = !SDL_strcasecmp(f[1], "code");
        snprintf(t->file, sizeof t->file, "%s%s", dir, f[2]);
        t->start = atoi(f[3]) - 150;
        t->length = atoi(f[4]);
    }
    free(text);
    if (!n) { snprintf(g_err, sizeof g_err, "%s lists no tracks", hcd->name); return false; }
    for (int i = 0; i < n; i++) {
        int next = i + 1 < n ? tr[i + 1].start : tr[i].start + tr[i].length;
        tr[i].sectors = tr[i].length < next - tr[i].start ? tr[i].length : next - tr[i].start;
        if (tr[i].sectors <= 0 || tr[i].start < 0) { snprintf(g_err, sizeof g_err, "%s: track %d is out of order", hcd->name, tr[i].number); return false; }
    }

    SDL_CreateDirectory(stage);
    char cue_path[1300];
    snprintf(cue_out, cue_len, "%s", stage);
    join(cue_path, sizeof cue_path, stage, "Rondo of Blood (Virtual Console).cue");
    SDL_IOStream *cue = SDL_IOFromFile(cue_path, "w");
    if (!cue) { snprintf(g_err, sizeof g_err, "cannot write %s", cue_path); return false; }
    SDL_IOprintf(cue, "REM made by RondoRecomp Setup from the %s Virtual Console release\n", what);
    int prev_end = 0, audio = 0, data = 0, rate = 0, silent = 0;
    uint8_t *first_data = NULL;   /* the first data track: later ones are copies of it */
    size_t first_bytes = 0;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        const VcTrack *t = &tr[i];
        const VcFile *f = find_file(fs, t->file);
        char name[64], path[1400], m[16];
        snprintf(name, sizeof name, "Track %02d.%s", t->number, t->data ? "iso" : "wav");
        join(path, sizeof path, stage, name);
        if (t->data) {
            /* On both discs the second data track (22) is a copy of the first. The Wii leaves
             * it out and the Wii U has the untranslated one: it is made from the first. */
            size_t bytes = (size_t)t->sectors * 2048;
            uint8_t *buf = calloc(1, bytes);
            if (!buf) { snprintf(g_err, sizeof g_err, "not enough memory"); ok = false; break; }
            if (first_data) memcpy(buf, first_data, bytes < first_bytes ? bytes : first_bytes);
            else if (!f) { snprintf(g_err, sizeof g_err, "%s is missing", t->file); free(buf); ok = false; break; }
            else ok = data_track(f, buf, bytes);
            ok = ok && SDL_SaveFile(path, buf, bytes);
            if (!ok && !g_err[0]) snprintf(g_err, sizeof g_err, "cannot write %s", path);
            if (!first_data) { first_data = buf; first_bytes = bytes; }
            else free(buf);
            data++;
        } else if (!f) {
            /* (the Wii leaves out track 1, the warning a CD player plays: silence) */
            short *zero = calloc((size_t)t->sectors * 588 * 2 + 2, sizeof *zero);
            ok = zero && write_wav(path, zero, (long)t->sectors * 588);
            if (!ok) snprintf(g_err, sizeof g_err, "cannot write %s", path);
            free(zero);
            silent++;
        } else {
            ok = audio_track(f, path, t->sectors, &rate);
            audio++;
        }
        setup_progress_hook("Converting the disc", (i + 1) * 100 / n);
        SDL_IOprintf(cue, "FILE \"%s\" %s\n  TRACK %02d %s\n", name, t->data ? "BINARY" : "WAVE", t->number,
                     t->data ? "MODE1/2048" : "AUDIO");
        /* An audio track straight after a data track starts with its 2-second pregap (INDEX 00,
         * as on the disc) when the list puts it where the data ends (the Wii U); with a gap
         * before it (the Wii), the gap is the pregap. */
        bool split = !t->data && i > 0 && tr[i - 1].data && t->start == prev_end && t->sectors > 150;
        if (t->start > prev_end) {
            msf(m, sizeof m, t->start - prev_end);
            SDL_IOprintf(cue, "    PREGAP %s\n", m);
        }
        SDL_IOprintf(cue, split ? "    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n" : "    INDEX 01 00:00:00\n");
        prev_end = t->start + t->sectors;
    }
    free(first_data);
    SDL_CloseIO(cue);
    if (!ok) return false;
    say("  %d tracks: %d data, %d audio%s", n, data, audio, rate && rate != 44100 ? " (resampled to 44.1 kHz)" : "");
    if (silent) say(", %d silent (not included in the release)", silent);
    say("\n");
    snprintf(cue_out, cue_len, "%s", cue_path);
    return true;
}

/* ---- a 7z holding a disc image ------------------------------------------------------------- */

static bool sevenzip_cue(Src *s, const char *stage, char *cue_out, size_t cue_len)
{
    char dir[700];
    snprintf(dir, sizeof dir, "%s", s->cue);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    SDL_CreateDirectory(stage);
    /* the files next to the .cue: it and its tracks */
    size_t dl = strlen(dir);
    for (int i = 0; i < sz_count(s->sz); i++) {
        const char *nm = sz_name(s->sz, i);
        if (sz_is_dir(s->sz, i) || SDL_strncasecmp(nm, dir, dl) || strchr(nm + dl, '/')) continue;
        char err[300], path[1300];
        size_t size = 0;
        const uint8_t *d = sz_read(s->sz, i, &size, err, sizeof err);
        if (!d) { snprintf(g_err, sizeof g_err, "%s", err); return false; }
        join(path, sizeof path, stage, nm + dl);
        say("  %s\n", nm + dl);
        if (!SDL_SaveFile(path, d, size)) { snprintf(g_err, sizeof g_err, "cannot write %s", path); return false; }
    }
    join(cue_out, cue_len, stage, s->cue + dl);
    return true;
}

bool vc_import(const char *input, const char *stage, char *cue_out, size_t cue_out_len, bool *need_key)
{
    *need_key = false;
    g_err[0] = 0;
    Src s;
    if (!src_open(input, &s, false)) {
        if (!g_err[0]) snprintf(g_err, sizeof g_err, "no Wii U title (title.tmd) or Wii .wad in %s", input);
        return false;
    }
    bool ok = false;
    VcFiles fs = {0};
    Keys ks = {0};
    if (!s.prefix[0] && !s.wad[0] && s.kind == SRC_7Z && sz_find(s.sz, "title.tmd") < 0) {
        ok = sevenzip_cue(&s, stage, cue_out, cue_out_len);   /* a disc image, not a Virtual Console one */
        src_close(&s);
        return ok;
    }
    if (s.wad[0]) {
        say("  Wii: %s\n", s.wad);
        keys_gather(&ks, input);
        ok = wad_read(&s, &ks, &fs, need_key) && build_disc(&fs, stage, "Wii", cue_out, cue_out_len);
    } else if (is_wii_tmd(&s)) {
        /* a Wii title installed (as Dolphin's title/<id>/content folder): decrypted already */
        if (s.kind != SRC_DIR) snprintf(g_err, sizeof g_err, "an installed Wii title in an archive: unpack it first");
        else ok = wii_title_read(&s, &fs) && build_disc(&fs, stage, "Wii", cue_out, cue_out_len);
    } else {
        say("  Wii U title (WUP)%s%s\n", s.prefix[0] ? ": " : "", s.prefix);
        keys_gather(&ks, input);
        ok = wup_read(&s, &ks, &fs, need_key) && build_disc(&fs, stage, "Wii U", cue_out, cue_out_len);
    }
    files_free(&fs);
    free(ks.k);
    src_close(&s);
    return ok;
}
