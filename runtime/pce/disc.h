/* CD image access, addressed by absolute LBA: a CUE sheet with ISO/BIN/WAV tracks, or a .chd
 * (chd.h), read where it is. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define DISC_MAX_TRACKS 99

/* Save states hold the whole machine, this included (cleared), so these keep their size:
 * a larger Disc would make every state saved before unreadable (disc.c checks). */
typedef struct DiscTrack {
    int number;
    bool data;
    char path[1008];
    long data_offset;     /* byte offset of the file's first sector */
    int sector_size;      /* 2048 or 2352 */
    int user_offset;      /* 16 for MODE1/2352 */
    int file_index;       /* INDEX 01 position in the file (in a .chd: in the track), sectors */
    int sectors;
    int lba;              /* absolute LBA of INDEX 01 */
    int chd_first;        /* a .chd's: the track's first frame in it, how many it stores */
    int chd_frames;
    uint32_t chd[2];      /* and the open .chd (struct Chd *, in two halves), 0 for a .cue */
} DiscTrack;

typedef struct Disc {
    DiscTrack tracks[DISC_MAX_TRACKS];
    int n_tracks;
} Disc;

/* A .cue or a .chd. A disc opened has to be closed (a .chd stays open), and is not opened
 * again before it is. */
bool disc_open(Disc *disc, const char *path, char *err, int err_len);
void disc_close(Disc *disc);
const DiscTrack *disc_track(const Disc *disc, int number);
const DiscTrack *disc_track_at(const Disc *disc, int lba);
/* Read `count` 2048-byte data sectors starting at absolute `lba`. */
bool disc_read_data(const Disc *disc, int lba, int count, uint8_t *out);
/* Read `count` 2352-byte audio sectors (16-bit stereo PCM) starting at absolute `lba`. */
bool disc_read_audio(const Disc *disc, int lba, int count, uint8_t *out);
/* fopen, for a path in UTF-8 (on Windows, then as the system's code page, as fopen takes it) */
FILE *disc_fopen(const char *path, const char *mode);
/* .cue or .chd */
bool disc_is_image(const char *name);
