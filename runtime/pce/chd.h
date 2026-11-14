/* A CD image in MAME's compressed format (.chd, as chdman createcd makes it), read in place:
 * the game plays it as it is, and Setup checks one with it. Reads CHD version 5 with the CD
 * codecs chdman uses by default (cdlz: LZMA, cdzl: Deflate, cdfl: FLAC), plain lzma and zlib
 * hunks, or none; not one that needs a parent .chd. Each hunk is checked against the CRC-16
 * its map keeps as it is read. See docs/distribution.md, CHD. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CHD_FRAME 2448      /* a CD frame in a .chd: a 2352-byte sector, then 96 bytes of subcode */

typedef struct ChdTrack {
    int number;
    int frames;             /* frames stored, with the pregap if pregap_stored */
    int pregap, postgap;    /* frames; a postgap is never stored, a pregap is if pregap_stored */
    int size;               /* bytes of each frame it uses: 2352, or 2048 (MODE1) */
    bool audio, pregap_stored;
    const char *cue_type;   /* AUDIO, MODE1/2352 or MODE1/2048 */
    uint64_t first;         /* its first frame in the .chd */
} ChdTrack;

typedef struct Chd Chd;

/* NULL, with err saying why, if it cannot be read or is not a CD image */
Chd *chd_open(const char *path, char *err, int err_len);
void chd_close(Chd *c);
int chd_track_count(const Chd *c);
const ChdTrack *chd_track(const Chd *c, int i);
/* Frame `frame` of the .chd (audio as big-endian samples, as a .chd keeps it), good until the
 * next read; NULL if it cannot be read (chd_error says why). */
const uint8_t *chd_frame(Chd *c, uint64_t frame);
/* Reads every hunk, checking each; progress (if not NULL) gets 0 to 100 as it goes. */
bool chd_verify(Chd *c, void (*progress)(int pct, void *ctx), void *ctx);
const char *chd_error(const Chd *c);
