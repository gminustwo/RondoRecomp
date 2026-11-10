/* Reading a .7z archive (the LZMA SDK's decoder, third_party/lzma, public domain): its file
 * list, and a file's contents in memory. A solid block is decoded once and kept, so reading
 * several files from it costs one decode. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SevenZip SevenZip;

SevenZip *sz_open(const char *path, char *err, int err_len);
void sz_close(SevenZip *z);
int sz_count(const SevenZip *z);
/* entry i: its path ('/' separated, UTF-8), size and whether it is a folder */
const char *sz_name(const SevenZip *z, int i);
uint64_t sz_size(const SevenZip *z, int i);
bool sz_is_dir(const SevenZip *z, int i);
/* entry i's contents: valid until the next sz_read or sz_close (do not free) */
const uint8_t *sz_read(SevenZip *z, int i, size_t *size, char *err, int err_len);
