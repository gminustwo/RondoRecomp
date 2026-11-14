/* A CD image in MAME's compressed format (.chd, as chdman createcd makes it) as Setup's
 * input. The game reads a .chd as it is (runtime/pce/chd.c), so Setup checks every hunk of
 * it and copies it in; nothing is unpacked. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pce/discid.h"

/* What its first data track is (discid_identify_sectors), read without checking the rest:
 * DISC_INVALID with g_err saying why when it cannot be read or is not a PC Engine CD. */
DiscKind chd_identify(const char *path, uint64_t *fingerprint);
/* Checks every hunk, then copies the .chd into `stage` (created); the copy's path goes in
 * out. False with g_err set on failure. */
bool chd_install(const char *path, const char *stage, char *out, size_t out_len);
