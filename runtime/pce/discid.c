/* See discid.h. */
#include "discid.h"

#include <string.h>

#include "disc.h"

/* FNV-1a 64 of the boot program sector (first data track, record 2) of known releases */
static const uint64_t known_boot[] = {
    0xDB4D9415EC501573ull,   /* Akumajou Dracula X: Chi no Rondo (JP), and its English translation */
};

/* FNV-1a 64 of sectors 5-10 of the first data track (text the translation changes) of the
 * original Japanese release; the English translation gives 24EDDDB2B9F204E9 */
#define ORIGINAL_JA_TEXT 0x267F7AAFEB11FB21ull

static uint64_t fnv1a_more(uint64_t h, const uint8_t *p, size_t n)
{
    while (n--) { h ^= *p++; h *= 0x100000001b3ull; }
    return h;
}

static uint64_t fnv1a(const uint8_t *p, size_t n) { return fnv1a_more(0xcbf29ce484222325ull, p, n); }

DiscKind discid_identify(const char *cue_path, uint64_t *fingerprint)
{
    static Disc disc;
    char err[256];
    uint8_t sec[2048];
    if (fingerprint) *fingerprint = 0;
    if (!disc_open(&disc, cue_path, err, sizeof err)) return DISC_INVALID;
    const DiscTrack *t = NULL;
    for (int i = 0; i < disc.n_tracks; i++)
        if (disc.tracks[i].data) { t = &disc.tracks[i]; break; }
    uint8_t boot[2048];
    if (!t || !disc_read_data(&disc, t->lba + 1, 1, sec) || !disc_read_data(&disc, t->lba + 2, 1, boot))
        return DISC_INVALID;
    return discid_identify_sectors(sec, boot, fingerprint);
}

DiscKind discid_identify_sectors(const uint8_t *sector1, const uint8_t *sector2, uint64_t *fingerprint)
{
    if (fingerprint) *fingerprint = 0;
    if (memcmp(sector1 + 0x20, "PC Engine CD-ROM SYSTEM", 23)) return DISC_INVALID;
    uint64_t h = fnv1a(sector2, 2048);
    if (fingerprint) *fingerprint = h;
    for (size_t i = 0; i < sizeof known_boot / sizeof known_boot[0]; i++)
        if (h == known_boot[i]) return DISC_RONDO;
    return DISC_PCE_UNKNOWN;
}

DiscLanguage discid_language(const char *cue_path)
{
    static Disc disc;
    char err[256];
    uint8_t sec[2048];
    if (!disc_open(&disc, cue_path, err, sizeof err)) return DISC_LANG_UNKNOWN;
    const DiscTrack *t = NULL;
    for (int i = 0; i < disc.n_tracks; i++)
        if (disc.tracks[i].data) { t = &disc.tracks[i]; break; }
    if (!t) return DISC_LANG_UNKNOWN;
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 5; i <= 10; i++) {
        if (!disc_read_data(&disc, t->lba + i, 1, sec)) return DISC_LANG_UNKNOWN;
        h = fnv1a_more(h, sec, sizeof sec);
    }
    return h == ORIGINAL_JA_TEXT ? DISC_LANG_JA : DISC_LANG_EN;
}
