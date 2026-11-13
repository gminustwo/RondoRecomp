/* Identifying a disc image as a supported Rondo of Blood release. */
#pragma once

#include <stdint.h>

typedef enum {
    DISC_INVALID,       /* unreadable, or not a PC Engine CD */
    DISC_PCE_UNKNOWN,   /* a PC Engine CD, but not a known Rondo of Blood release */
    DISC_RONDO,         /* recognised Rondo of Blood */
} DiscKind;

/* Checks the PC Engine CD signature, then fingerprints the boot program sector.
 * If fingerprint is non-NULL it receives the fingerprint (0 when unreadable). */
DiscKind discid_identify(const char *cue_path, uint64_t *fingerprint);
/* The same check from the first data track's sectors 1 and 2 (2048 bytes each), for a disc
 * image read some other way (Setup's .chd reader). */
DiscKind discid_identify_sectors(const uint8_t *sector1, const uint8_t *sector2, uint64_t *fingerprint);

typedef enum {
    DISC_LANG_UNKNOWN,  /* unreadable */
    DISC_LANG_EN,       /* a translated disc: the English fan translation (any disc whose text
                         * differs from the original's) */
    DISC_LANG_JA,       /* the original Japanese release, Akumajou Dracula X: Chi no Rondo */
} DiscLanguage;

/* The original Japanese disc and the English translation share the boot sector; the
 * translation changes the data track from its sixth sector on, which this checks. */
DiscLanguage discid_language(const char *cue_path);
