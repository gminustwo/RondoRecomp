/* Virtual Console releases of Rondo of Blood as Setup's input: the Wii's (a .wad) and the Wii
 * U's (a WUP: title.tmd, title.tik and NNNNNNNN.app files), each as it is or in a .zip or .7z.
 * Setup turns the disc inside back into a cue sheet with 2048-byte data tracks and 44.1 kHz
 * WAV audio, which the game reads like any disc image (docs/distribution.md, Virtual
 * Console). Both are encrypted with their console's common key, which the player supplies:
 * a keys.txt (32 hex digits per key, as Cemu's), a Wii U's otp.bin or a Wii's keys.bin, next
 * to the input or to Setup, or named by --keys / RONDO_KEYS. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* a key file named by the player (--keys, or Setup's window): looked at first */
extern const char *vc_keys_file;

/* Whether input is one: a .wad, a WUP folder (or a folder above one), or a .zip or .7z
 * holding either. */
bool vc_detect(const char *input);
/* Its disc, written to `stage` (created) as a cue sheet, whose path goes in cue_out. False
 * with g_err set on failure; *need_key: no key found opens it. */
bool vc_import(const char *input, const char *stage, char *cue_out, size_t cue_out_len, bool *need_key);
