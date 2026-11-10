/* See sevenzip.h. The archive is read through SDL (UTF-8 paths on every system), not 7zFile.c. */
#include "sevenzip.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"

typedef struct SdlInStream {
    ISeekInStream vt;
    SDL_IOStream *io;
} SdlInStream;

static SRes sdl_read(ISeekInStreamPtr pp, void *buf, size_t *size)
{
    SdlInStream *p = Z7_CONTAINER_FROM_VTBL(pp, SdlInStream, vt);
    if (!*size) return SZ_OK;
    size_t got = SDL_ReadIO(p->io, buf, *size);
    *size = got;
    return got || SDL_GetIOStatus(p->io) == SDL_IO_STATUS_EOF ? SZ_OK : SZ_ERROR_READ;
}

static SRes sdl_seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
    SdlInStream *p = Z7_CONTAINER_FROM_VTBL(pp, SdlInStream, vt);
    SDL_IOWhence w = origin == SZ_SEEK_SET ? SDL_IO_SEEK_SET : origin == SZ_SEEK_CUR ? SDL_IO_SEEK_CUR : SDL_IO_SEEK_END;
    Sint64 r = SDL_SeekIO(p->io, *pos, w);
    if (r < 0) return SZ_ERROR_READ;
    *pos = r;
    return SZ_OK;
}

struct SevenZip {
    SdlInStream in;
    CLookToRead2 look;
    CSzArEx db;
    char **names;
    UInt32 block;           /* the decoded solid block (0xFFFFFFFF: none yet) */
    Byte *out;
    size_t out_size;
};

static const ISzAlloc alloc_main = {SzAlloc, SzFree};
static const ISzAlloc alloc_temp = {SzAllocTemp, SzFreeTemp};

/* a UTF-16 name as UTF-8, with '/' between folders */
static char *utf8_name(const UInt16 *s, size_t n)
{
    char *out = malloc(n * 3 + 1), *o = out;
    if (!out) return NULL;
    for (size_t i = 0; i < n && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c == '\\') c = '/';
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | ((c >> 6) & 63)); *o++ = (char)(0x80 | (c & 63)); }
        else { *o++ = (char)(0xF0 | c >> 18); *o++ = (char)(0x80 | ((c >> 12) & 63)); *o++ = (char)(0x80 | ((c >> 6) & 63)); *o++ = (char)(0x80 | (c & 63)); }
    }
    *o = 0;
    return out;
}

SevenZip *sz_open(const char *path, char *err, int err_len)
{
    static bool crc_ready;
    if (!crc_ready) { CrcGenerateTable(); crc_ready = true; }
    SevenZip *z = calloc(1, sizeof *z);
    if (!z) return NULL;
    z->in.io = SDL_IOFromFile(path, "rb");
    if (!z->in.io) { snprintf(err, (size_t)err_len, "cannot open %s", path); free(z); return NULL; }
    z->in.vt.Read = sdl_read;
    z->in.vt.Seek = sdl_seek;
    LookToRead2_CreateVTable(&z->look, False);
    z->look.buf = malloc(1 << 18);
    z->look.bufSize = 1 << 18;
    z->look.realStream = &z->in.vt;
    LookToRead2_INIT(&z->look);
    z->block = 0xFFFFFFFF;
    SzArEx_Init(&z->db);
    SRes r = SzArEx_Open(&z->db, &z->look.vt, &alloc_main, &alloc_temp);
    if (r != SZ_OK) {
        snprintf(err, (size_t)err_len, "%s is not a 7z archive this can read (error %d)", path, (int)r);
        sz_close(z);
        return NULL;
    }
    z->names = calloc(z->db.NumFiles ? z->db.NumFiles : 1, sizeof *z->names);
    UInt16 tmp[1024];
    for (UInt32 i = 0; i < z->db.NumFiles; i++) {
        size_t len = SzArEx_GetFileNameUtf16(&z->db, i, NULL);
        if (len > sizeof tmp / sizeof tmp[0]) { z->names[i] = calloc(1, 1); continue; }
        SzArEx_GetFileNameUtf16(&z->db, i, tmp);
        z->names[i] = utf8_name(tmp, len);
    }
    return z;
}

void sz_close(SevenZip *z)
{
    if (!z) return;
    if (z->names)
        for (UInt32 i = 0; i < z->db.NumFiles; i++) free(z->names[i]);
    free(z->names);
    ISzAlloc_Free(&alloc_main, z->out);
    SzArEx_Free(&z->db, &alloc_main);
    free(z->look.buf);
    if (z->in.io) SDL_CloseIO(z->in.io);
    free(z);
}

int sz_count(const SevenZip *z) { return (int)z->db.NumFiles; }
const char *sz_name(const SevenZip *z, int i) { return z->names[i] ? z->names[i] : ""; }
uint64_t sz_size(const SevenZip *z, int i) { return SzArEx_GetFileSize(&z->db, (UInt32)i); }
bool sz_is_dir(const SevenZip *z, int i) { return SzArEx_IsDir(&z->db, (UInt32)i) != 0; }

const uint8_t *sz_read(SevenZip *z, int i, size_t *size, char *err, int err_len)
{
    size_t offset = 0, got = 0;
    SRes r = SzArEx_Extract(&z->db, &z->look.vt, (UInt32)i, &z->block, &z->out, &z->out_size, &offset, &got,
                            &alloc_main, &alloc_temp);
    if (r != SZ_OK) {
        snprintf(err, (size_t)err_len, "cannot unpack %s from the 7z (error %d%s)", sz_name(z, i), (int)r,
                 r == SZ_ERROR_MEM ? ": not enough memory" : r == SZ_ERROR_CRC ? ": the archive is damaged" : "");
        return NULL;
    }
    *size = got;
    return z->out + offset;
}
