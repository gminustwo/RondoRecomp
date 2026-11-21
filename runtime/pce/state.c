/* Save states and rewind. See state.h. */
#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/recomp.h"

#define STATE_MAGIC "RRSTATE"
#define STATE_FORMAT 1

size_t pce_state_size(void) { return sizeof(Pce); }

/* Host-side fields: not part of the emulated machine. */
static void clear_host_fields(Pce *q)
{
    memset(&q->disc, 0, sizeof q->disc);
    memset(q->page, 0, sizeof q->page);
    memset(q->page_ro, 0, sizeof q->page_ro);
    q->real_bios = NULL;
    memset(&q->cpu.bus, 0, sizeof q->cpu.bus);
    memset(q->cpu.page_ptr, 0, sizeof q->cpu.page_ptr);
    memset(q->cpu.page_ram, 0, sizeof q->cpu.page_ram);
    memset(q->vdc.fb, 0, sizeof q->vdc.fb);
    q->vdc.wide = q->vdc.fb_wide = 0;
    q->vdc.wide_x0 = q->vdc.wide_x1 = 0;
    q->vdc.fb_w = 256;
    q->snd.ram = NULL;
    q->snd.disc = NULL;
    memset(q->snd.ring, 0, sizeof q->snd.ring);
    q->snd.head = q->snd.count = 0;
    q->snd.psg_writes = q->snd.psg_samples = q->snd.adpcm_samples = 0;
    q->snd.lfo_used = false;
    q->pad = 0;
    q->bram_dirty = false;
    q->mute_cdda = false;
    q->log_bios = q->log_bios_all = false;
    memset(q->bios_calls, 0, sizeof q->bios_calls);
    q->unhandled_io = 0;
    memset(q->irq_taken, 0, sizeof q->irq_taken);
    q->joy_reads = 0;
    q->on_step = NULL;
    q->on_step_ctx = NULL;
    q->on_entry = NULL;
    q->on_entry_ctx = NULL;
}

void pce_state_capture(const Pce *p, uint8_t *out)
{
    memcpy(out, p, sizeof(Pce));
    clear_host_fields((Pce *)out);
}

void pce_state_restore(Pce *p, const uint8_t *in)
{
    static Pce keep;
    memcpy(&keep, p, sizeof(Pce));
    memcpy(p, in, sizeof(Pce));
    /* put the host-side fields back */
    memcpy(&p->disc, &keep.disc, sizeof p->disc);
    memcpy(p->page, keep.page, sizeof p->page);
    memcpy(p->page_ro, keep.page_ro, sizeof p->page_ro);
    p->real_bios = keep.real_bios;
    p->cpu.bus = keep.cpu.bus;
    memcpy(p->cpu.page_ptr, keep.cpu.page_ptr, sizeof p->cpu.page_ptr);
    memcpy(p->cpu.page_ram, keep.cpu.page_ram, sizeof p->cpu.page_ram);
    memcpy(p->vdc.fb, keep.vdc.fb, sizeof p->vdc.fb);
    p->vdc.wide = keep.vdc.wide;
    p->vdc.fb_wide = keep.vdc.fb_wide;
    p->vdc.wide_x0 = keep.vdc.wide_x0;
    p->vdc.wide_x1 = keep.vdc.wide_x1;
    p->vdc.fb_w = keep.vdc.fb_w;
    p->snd.ram = keep.snd.ram;
    p->snd.disc = &p->disc;
    memcpy(p->snd.ring, keep.snd.ring, sizeof p->snd.ring);
    p->snd.head = keep.snd.head;
    p->snd.count = keep.snd.count;
    p->snd.psg_writes = keep.snd.psg_writes;
    p->snd.psg_samples = keep.snd.psg_samples;
    p->snd.adpcm_samples = keep.snd.adpcm_samples;
    p->snd.lfo_used = keep.snd.lfo_used;
    p->pad = keep.pad;
    p->bram_dirty = keep.bram_dirty;
    p->mute_cdda = keep.mute_cdda;
    p->log_bios = keep.log_bios;
    p->log_bios_all = keep.log_bios_all;
    memcpy(p->bios_calls, keep.bios_calls, sizeof p->bios_calls);
    p->unhandled_io = keep.unhandled_io;
    memcpy(p->irq_taken, keep.irq_taken, sizeof p->irq_taken);
    p->joy_reads = keep.joy_reads;
    p->on_step = keep.on_step;
    p->on_step_ctx = keep.on_step_ctx;
    p->on_entry = keep.on_entry;
    p->on_entry_ctx = keep.on_entry_ctx;
    /* memory changed behind the recompiler's back: its code checks must be redone */
    recomp_invalidate();
}

/* ---- XOR-delta codec ------------------------------------------------------------------ */

static uint8_t *put_varint(uint8_t *o, size_t v)
{
    while (v >= 0x80) { *o++ = (uint8_t)(v | 0x80); v >>= 7; }
    *o++ = (uint8_t)v;
    return o;
}

static const uint8_t *get_varint(const uint8_t *i, const uint8_t *end, size_t *v)
{
    size_t r = 0;
    int shift = 0;
    while (i < end) {
        uint8_t b = *i++;
        r |= (size_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    *v = r;
    return i;
}

size_t delta_bound(size_t n) { return n + n / 8 + 64; }

/* first index >= i where a and b differ (word at a time) */
static size_t next_diff(const uint8_t *a, const uint8_t *b, size_t i, size_t n)
{
    while (i + 8 <= n) {
        uint64_t x, y;
        memcpy(&x, a + i, 8);
        memcpy(&y, b + i, 8);
        if (x != y) break;
        i += 8;
    }
    while (i < n && a[i] == b[i]) i++;
    return i;
}

size_t delta_encode(const uint8_t *a, const uint8_t *b, size_t n, uint8_t *out)
{
    uint8_t *o = out;
    size_t i = 0;
    while (i < n) {
        size_t d = next_diff(a, b, i, n);
        if (d == n) break;
        /* literal run: until 8 equal bytes in a row (or the end) */
        size_t e = d, same = 0;
        while (e < n) {
            if (a[e] == b[e]) { if (++same == 8) { e -= 7; break; } }
            else same = 0;
            e++;
        }
        if (e > n) e = n;
        while (e > d && a[e - 1] == b[e - 1]) e--;   /* trailing equal bytes belong to the next gap */
        o = put_varint(o, d - i);
        o = put_varint(o, e - d);
        for (size_t k = d; k < e; k++) *o++ = (uint8_t)(a[k] ^ b[k]);
        i = e;
    }
    return (size_t)(o - out);
}

void delta_apply(uint8_t *dst, size_t n, const uint8_t *code, size_t len)
{
    const uint8_t *i = code, *end = code + len;
    size_t pos = 0;
    while (i < end) {
        size_t gap, lit;
        i = get_varint(i, end, &gap);
        i = get_varint(i, end, &lit);
        pos += gap;
        if (pos + lit > n || i + lit > end) return;   /* corrupt: stop */
        for (size_t k = 0; k < lit; k++) dst[pos + k] ^= i[k];
        i += lit;
        pos += lit;
    }
}

/* ---- save-state files ------------------------------------------------------------------ */

typedef struct StateHeader {
    char magic[8];
    uint32_t format;
    uint32_t state_size;
    uint64_t disc_id;
    uint64_t code_len;
} StateHeader;

bool pce_state_save(const Pce *p, const char *path, uint64_t disc_id, char *err, int err_len)
{
    size_t n = sizeof(Pce);
    uint8_t *snap = malloc(n), *zero = calloc(1, n), *code = malloc(delta_bound(n));
    bool ok = false;
    if (snap && zero && code) {
        pce_state_capture(p, snap);
        StateHeader h;
        memset(&h, 0, sizeof h);
        memcpy(h.magic, STATE_MAGIC, sizeof STATE_MAGIC);
        h.format = STATE_FORMAT;
        h.state_size = (uint32_t)n;
        h.disc_id = disc_id;
        h.code_len = delta_encode(snap, zero, n, code);
        FILE *f = fopen(path, "wb");
        if (f) {
            ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(code, 1, h.code_len, f) == h.code_len;
            ok = (fclose(f) == 0) && ok;
        }
        if (!ok) snprintf(err, (size_t)err_len, "cannot write %s", path);
    } else {
        snprintf(err, (size_t)err_len, "out of memory");
    }
    free(snap);
    free(zero);
    free(code);
    return ok;
}

bool pce_state_load(Pce *p, const char *path, uint64_t disc_id, char *err, int err_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, (size_t)err_len, "no saved state in this slot"); return false; }
    StateHeader h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, STATE_MAGIC, sizeof STATE_MAGIC);
    if (!ok) { fclose(f); snprintf(err, (size_t)err_len, "not a RondoRecomp save state"); return false; }
    if (h.format != STATE_FORMAT || h.state_size != sizeof(Pce)) {
        fclose(f);
        snprintf(err, (size_t)err_len, "saved by a different version of RondoRecomp");
        return false;
    }
    if (h.disc_id != disc_id) { fclose(f); snprintf(err, (size_t)err_len, "saved from a different disc"); return false; }
    size_t n = sizeof(Pce);
    uint8_t *snap = calloc(1, n), *code = malloc(h.code_len ? h.code_len : 1);
    ok = snap && code && fread(code, 1, h.code_len, f) == h.code_len;
    fclose(f);
    if (ok) {
        delta_apply(snap, n, code, h.code_len);
        pce_state_restore(p, snap);
    } else {
        snprintf(err, (size_t)err_len, "cannot read %s", path);
    }
    free(snap);
    free(code);
    return ok;
}

/* ---- rewind ------------------------------------------------------------------------------ */

struct RewindFrame {
    uint32_t len;
    uint8_t pad;
    uint8_t data[];
};

bool rewind_init(Rewind *r, size_t budget_bytes, int max_frames)
{
    memset(r, 0, sizeof *r);
    r->size = sizeof(Pce);
    r->cur = malloc(r->size);
    r->tmp = malloc(r->size);
    r->code = malloc(delta_bound(r->size));
    r->frames = calloc((size_t)max_frames, sizeof *r->frames);
    r->cap = max_frames;
    r->budget = budget_bytes;
    return r->cur && r->tmp && r->code && r->frames;
}

static void drop_oldest(Rewind *r)
{
    int i = (r->head - r->count + r->cap) % r->cap;
    r->bytes -= r->frames[i]->len;
    free(r->frames[i]);
    r->frames[i] = NULL;
    r->count--;
}

void rewind_free(Rewind *r)
{
    while (r->count) drop_oldest(r);
    free(r->cur);
    free(r->tmp);
    free(r->code);
    free(r->frames);
    memset(r, 0, sizeof *r);
}

void rewind_reset(Rewind *r, const Pce *p)
{
    while (r->count) drop_oldest(r);
    r->head = 0;
    pce_state_capture(p, r->cur);
}

void rewind_record(Rewind *r, const Pce *p, uint8_t pad)
{
    pce_state_capture(p, r->tmp);
    size_t len = delta_encode(r->tmp, r->cur, r->size, r->code);
    RewindFrame *fr = malloc(sizeof *fr + len);
    if (!fr) return;
    fr->len = (uint32_t)len;
    fr->pad = pad;
    memcpy(fr->data, r->code, len);
    while (r->count && (r->count == r->cap || r->bytes + len > r->budget)) drop_oldest(r);
    r->frames[r->head] = fr;
    r->head = (r->head + 1) % r->cap;
    r->count++;
    r->bytes += len;
    uint8_t *t = r->cur;
    r->cur = r->tmp;
    r->tmp = t;
}

bool rewind_step(Rewind *r, uint8_t *pad)
{
    if (!r->count) return false;
    r->head = (r->head - 1 + r->cap) % r->cap;
    RewindFrame *fr = r->frames[r->head];
    delta_apply(r->cur, r->size, fr->data, fr->len);
    if (pad) *pad = fr->pad;
    r->bytes -= fr->len;
    free(fr);
    r->frames[r->head] = NULL;
    r->count--;
    return true;
}

int rewind_frames(const Rewind *r) { return r->count; }
