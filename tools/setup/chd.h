/* A CD image in MAME's compressed format (.chd, as chdman createcd makes it) as Setup's
 * input. Setup turns it back into a cue sheet with a .bin per track, as chdman extractcd
 * would: the files a Redump-style disc image has, checked hunk by hunk against the CRCs the
 * .chd keeps. Reads CHD version 5 with the CD codecs chdman uses by default (LZMA, zlib and
 * FLAC) or none; not one that needs a parent .chd. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pce/discid.h"

/* What its first data track is (discid_identify_sectors), read without unpacking the rest:
 * DISC_INVALID with g_err saying why when it cannot be read or is not a PC Engine CD. */
DiscKind chd_identify(const char *path, uint64_t *fingerprint);
/* The disc as a cue sheet and its tracks, written to `stage` (created); the cue's path goes
 * in cue_out. False with g_err set on failure. */
bool chd_import(const char *path, const char *stage, char *cue_out, size_t cue_out_len);
