/* Code generator. See gen.h and docs/recompiler.md.
 *
 * 1. Boot the game headless (interpreter) and run its own overlay loader for every
 *    group, collecting each distinct RAM bank image.
 * 2. Match entry seeds against the images and form regions: code reachable from the
 *    entry through branches and jumps inside the same 8 KiB window.
 * 3. Expand from region exits (call targets, return points, out-of-region targets).
 * 4. Emit C: one function per region calling the shared semantics in huc6280_ops.h,
 *    plus a table with the code-byte hash each region was compiled from.
 */
#include "gen.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/huc6280.h"
#include "cpu/huc6280_info.h"
#include "pce/pce.h"
#include "recomp/recomp.h"

#define RONDO_GROUPS 95          /* main loader table entries (see docs/findings.md) */
#define RONDO_LOADER 0x6FD8      /* 6A:6FD8, A = group */
#define REGIONS_PER_FILE 800

static GenLog g_log;
static bool g_verbose;

static void say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log(buf);
}

/* ---- instruction set ------------------------------------------------------------ */

enum { IMP, ACC, IMM, ZP, ZPX, ZPY, IZX, IZY, IZP, ABS, ABX, ABY, IND, IAX, REL, ZPR, IMZP, IMZPX, IMAB, IMABX, BLK };
static const uint8_t mode_len[] = {1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 2, 3, 3, 3, 4, 4, 7};

static const char *const op_table =
    "00 imp 01 izx 02 imp 03 imm 04 zp 05 zp 06 zp 07 zp 08 imp 09 imm 0A acc 0C abs 0D abs 0E abs 0F zpr "
    "10 rel 11 izy 12 izp 13 imm 14 zp 15 zpx 16 zpx 17 zp 18 imp 19 aby 1A acc 1C abs 1D abx 1E abx 1F zpr "
    "20 abs 21 izx 22 imp 23 imm 24 zp 25 zp 26 zp 27 zp 28 imp 29 imm 2A acc 2C abs 2D abs 2E abs 2F zpr "
    "30 rel 31 izy 32 izp 34 zpx 35 zpx 36 zpx 37 zp 38 imp 39 aby 3A acc 3C abx 3D abx 3E abx 3F zpr "
    "40 imp 41 izx 42 imp 43 imm 44 rel 45 zp 46 zp 47 zp 48 imp 49 imm 4A acc 4C abs 4D abs 4E abs 4F zpr "
    "50 rel 51 izy 52 izp 53 imm 54 imp 55 zpx 56 zpx 57 zp 58 imp 59 aby 5A imp 5D abx 5E abx 5F zpr "
    "60 imp 61 izx 62 imp 64 zp 65 zp 66 zp 67 zp 68 imp 69 imm 6A acc 6C ind 6D abs 6E abs 6F zpr "
    "70 rel 71 izy 72 izp 73 blk 74 zpx 75 zpx 76 zpx 77 zp 78 imp 79 aby 7A imp 7C iax 7D abx 7E abx 7F zpr "
    "80 rel 81 izx 82 imp 83 imzp 84 zp 85 zp 86 zp 87 zp 88 imp 89 imm 8A imp 8C abs 8D abs 8E abs 8F zpr "
    "90 rel 91 izy 92 izp 93 imab 94 zpx 95 zpx 96 zpy 97 zp 98 imp 99 aby 9A imp 9C abs 9D abx 9E abx 9F zpr "
    "A0 imm A1 izx A2 imm A3 imzpx A4 zp A5 zp A6 zp A7 zp A8 imp A9 imm AA imp AC abs AD abs AE abs AF zpr "
    "B0 rel B1 izy B2 izp B3 imabx B4 zpx B5 zpx B6 zpy B7 zp B8 imp B9 aby BA imp BC abx BD abx BE aby BF zpr "
    "C0 imm C1 izx C2 imp C3 blk C4 zp C5 zp C6 zp C7 zp C8 imp C9 imm CA imp CC abs CD abs CE abs CF zpr "
    "D0 rel D1 izy D2 izp D3 blk D4 imp D5 zpx D6 zpx D7 zp D8 imp D9 aby DA imp DD abx DE abx DF zpr "
    "E0 imm E1 izx E3 blk E4 zp E5 zp E6 zp E7 zp E8 imp E9 imm EA imp EC abs ED abs EE abs EF zpr "
    "F0 rel F1 izy F2 izp F3 blk F4 imp F5 zpx F6 zpx F7 zp F8 imp F9 aby FA imp FD abx FE abx FF zpr ";

static uint8_t op_mode[256];
static bool op_defined[256];

static void init_ops(void)
{
    static const char *const names[] = {"imp", "acc", "imm", "zp", "zpx", "zpy", "izx", "izy", "izp", "abs",
                                        "abx", "aby", "ind", "iax", "rel", "zpr", "imzp", "imzpx", "imab", "imabx", "blk"};
    for (int i = 0; i < 256; i++) { op_mode[i] = IMP; op_defined[i] = false; }
    const char *p = op_table;
    while (*p) {
        unsigned op;
        char m[8];
        int used;
        if (sscanf(p, "%x %7s %n", &op, m, &used) != 2) break;
        for (int k = 0; k < (int)(sizeof names / sizeof names[0]); k++)
            if (!strcmp(m, names[k])) { op_mode[op] = (uint8_t)k; op_defined[op] = true; }
        p += used;
    }
}

enum { F_SEQ, F_BRANCH, F_JUMP, F_CALL, F_EXIT, F_EXIT_AFTER, F_SET };

static int flow_of(uint8_t op)
{
    switch (op) {
    case 0x20: case 0x44: return F_CALL;
    case 0x60: case 0x40: case 0x00: case 0x6C: case 0x7C: return F_EXIT;
    case 0x4C: case 0x80: return F_JUMP;
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xB0: case 0xD0: case 0xF0: return F_BRANCH;
    case 0x53: case 0x73: case 0xC3: case 0xD3: case 0xE3: case 0xF3:
    case 0x58: case 0x78: case 0x28: case 0x54: case 0xD4: return F_EXIT_AFTER;
    case 0xF4: return F_SET;
    default: return (op & 0x0F) == 0x0F ? F_BRANCH : F_SEQ;
    }
}

typedef struct Ins {
    uint16_t addr;
    uint8_t op, len;
    uint8_t b[7];
    uint16_t target;
    bool set_inline;             /* SET: the next instruction is emitted here in T mode */
    uint8_t vol;                 /* bit i: operand byte i is patched at runtime (read from memory) */
} Ins;

static bool decode(const uint8_t *img, uint16_t base, uint16_t addr, Ins *in)
{
    uint32_t off = (uint32_t)(addr - base);
    if (addr < base || off >= 0x2000) return false;
    uint8_t op = img[off];
    int len = mode_len[op_mode[op]];
    if (off + (uint32_t)len > 0x2000) return false;      /* would cross the window */
    memset(in, 0, sizeof *in);
    in->addr = addr;
    in->op = op;
    in->len = (uint8_t)len;
    memcpy(in->b, img + off, (size_t)len);
    switch (op_mode[op]) {
    case REL: in->target = (uint16_t)(addr + 2 + (int8_t)in->b[1]); break;
    case ZPR: in->target = (uint16_t)(addr + 3 + (int8_t)in->b[2]); break;
    default:
        if (op == 0x4C || op == 0x20) in->target = (uint16_t)(in->b[1] | in->b[2] << 8);
        break;
    }
    return true;
}

/* ---- images ---------------------------------------------------------------------- */

typedef struct Image {
    uint8_t bank;
    uint64_t hash;
    uint8_t data[0x2000];
} Image;

static Image **images;
static int n_images, cap_images;

static uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    while (n--) { h ^= *p++; h *= 0x100000001b3ull; }
    return h;
}

static int add_image(uint8_t bank, const uint8_t *data)
{
    uint64_t h = fnv64(data, 0x2000);
    for (int i = 0; i < n_images; i++)
        if (images[i]->bank == bank && images[i]->hash == h) return i;
    if (n_images == cap_images) {
        cap_images = cap_images ? cap_images * 2 : 256;
        images = realloc(images, (size_t)cap_images * sizeof *images);
    }
    Image *im = malloc(sizeof *im);
    im->bank = bank;
    im->hash = h;
    memcpy(im->data, data, 0x2000);
    images[n_images++] = im;
    return n_images - 1;
}

static void collect(Pce *p)
{
    for (int b = 0x68; b <= 0x87; b++) add_image((uint8_t)b, p->page[b]);
    add_image(0xF8, p->wram);
}

/* ---- capture run ------------------------------------------------------------------ */

#include "capture_script.h"

typedef struct Seed { uint8_t bank; uint16_t addr; uint32_t h; int img; } Seed;  /* img: capture snapshot or -1 */
static Seed *seeds;
static int n_seeds, cap_seeds;

static const Seed *g_sort_seeds;
static int cmp_seed_key(const void *a, const void *b)
{
    const Seed *x = &g_sort_seeds[*(const int *)a], *y = &g_sort_seeds[*(const int *)b];
    uint32_t kx = (uint32_t)x->bank << 16 | x->addr, ky = (uint32_t)y->bank << 16 | y->addr;
    return kx < ky ? -1 : kx > ky;
}

static void add_seed(uint8_t bank, uint16_t addr, uint32_t h, int img)
{
    if (n_seeds == cap_seeds) {
        cap_seeds = cap_seeds ? cap_seeds * 2 : 8192;
        seeds = realloc(seeds, (size_t)cap_seeds * sizeof *seeds);
    }
    seeds[n_seeds++] = (Seed){bank, addr, h, img};
}

/* (bank, addr, block hash) triples already seen during capture */
static uint64_t *seen_tab;
static uint32_t seen_cap, seen_n;

static bool seen_insert(uint64_t v)
{
    if (!v) v = 1;
    if (seen_n * 2 >= seen_cap) {
        uint32_t oc = seen_cap;
        uint64_t *old = seen_tab;
        seen_cap = oc ? oc * 2 : 1 << 15;
        seen_tab = calloc(seen_cap, sizeof *seen_tab);
        seen_n = 0;
        for (uint32_t i = 0; i < oc; i++) if (old[i]) seen_insert(old[i]);
        free(old);
    }
    uint32_t i = (uint32_t)(v * 0x9E3779B97F4A7C15ull >> 40) & (seen_cap - 1);
    while (seen_tab[i]) {
        if (seen_tab[i] == v) return false;
        i = (i + 1) & (seen_cap - 1);
    }
    seen_tab[i] = v;
    seen_n++;
    return true;
}

static void on_entry(Pce *p, void *ctx)
{
    (void)ctx;
    HuC6280 *c = &p->cpu;
    uint16_t pc = c->pc;
    uint8_t bank = c->mpr[pc >> 13];
    if (bank == 0x00 || !c->page_ram[bank]) return;
    const uint8_t *page = c->page_ptr[bank];
    uint32_t h = hu_block_hash(page, pc & 0x1FFF);
    if (!seen_insert((uint64_t)bank << 56 | (uint64_t)pc << 32 | h)) return;
    add_seed(bank, pc, h, add_image(bank, page));   /* with the bank as it is when this code runs */
}

static bool materialize(const char *cue, GenStats *st, char *err, int err_len)
{
    Pce *p = malloc(sizeof(Pce)), *s0 = malloc(sizeof(Pce));
    if (!p || !s0) { snprintf(err, (size_t)err_len, "out of memory"); return false; }
    if (!pce_init(p, cue, err, err_len) || !pce_boot(p, err, err_len)) { pce_close(p); free(p); free(s0); return false; }
    recomp_set_enabled(false);
    for (int f = 0; f < 40; f++) pce_run_frame(p);       /* boot + main init (groups 0, 4, 6) */
    collect(p);
    memcpy(s0, p, sizeof(Pce));

    /* capture run: play the intro and attract sequence, recording every entry point and
     * the bank it runs from (this includes code the game assembles at runtime) */
    p->on_entry = on_entry;
    for (int f = 40; f < CAPTURE_FRAMES; f++) {
        p->pad = capture_pad(f);
        pce_run_frame(p);
    }
    p->on_entry = NULL;
    st->capture_seeds = n_seeds;

    /* each group from the post-init state, then all groups in order (later groups can
     * depend on data from earlier ones, e.g. a stage's small loads before its big one) */
    for (int pass = 0; pass < 2; pass++) {
        memcpy(p, s0, sizeof(Pce));
        for (int g = 0; g < RONDO_GROUPS; g++) {
            if (pass == 0) memcpy(p, s0, sizeof(Pce));
            p->cpu.mpr[2] = 0x69;
            p->cpu.mpr[3] = 0x6A;
            if (pce_call(p, RONDO_LOADER, (uint8_t)g, 50000000)) {
                st->groups_loaded++;
                collect(p);
            } else if (g_verbose) {
                say("  group %d did not return from the loader", g);
            }
        }
    }
    pce_close(p);   /* (s0 shares its disc) */
    free(s0);
    free(p);
    st->images = n_images;
    return true;
}

/* ---- chunks ------------------------------------------------------------------------ */

/* All code found in one bank image within one 8 KiB window. */
typedef struct Chunk {
    int img;
    uint16_t base;
    int n_ins;
    uint8_t inc[0x2000 / 8];     /* instruction starts */
    uint8_t cov[0x2000 / 8];     /* bytes compiled (incl. instructions inlined after SET) */
    uint8_t setinl[0x2000 / 8];  /* SET whose next instruction is emitted in T mode */
    uint8_t ent[0x2000 / 8];     /* entry points */
    uint8_t vol[0x2000 / 8];     /* operand bytes the game patches at runtime */
    bool has_vol;
} Chunk;

#define BIT_GET(a, i) ((a)[(i) >> 3] >> ((i) & 7) & 1)
#define BIT_SET(a, i) ((a)[(i) >> 3] |= (uint8_t)(1u << ((i) & 7)))

static Chunk **chunks;
static int n_chunks, cap_chunks;
static int *first_chunk;         /* [bank * 8 + window] -> head of a chunk list (index + 1) */
static int *next_chunk;          /* per chunk: next chunk of the same bank/window (index + 1) */

/* operand bytes known to be patched at runtime, per bank window: [bank * 8 + window] */
static uint8_t (*bank_vol)[0x2000 / 8];

typedef struct Work { int img; uint16_t addr; } Work;
static Work *work;
static int n_work, cap_work;

static void push_work(int img, uint16_t addr)
{
    if (n_work == cap_work) {
        cap_work = cap_work ? cap_work * 2 : 65536;
        work = realloc(work, (size_t)cap_work * sizeof *work);
    }
    work[n_work++] = (Work){img, addr};
}

/* Closure of one entry, computed in a scratch chunk before it is placed. */
static Chunk scratch;

static void closure(int img_index, uint16_t base, uint16_t entry)
{
    const uint8_t *img = images[img_index]->data;
    memset(&scratch, 0, sizeof scratch);
    scratch.img = img_index;
    scratch.base = base;
    static uint16_t queue[0x4000];
    int qh = 0, qt = 0;
    queue[qt++] = entry;
    while (qh < qt) {
        uint16_t a = queue[qh++];
        uint32_t off = (uint32_t)(a - base);
        if (a < base || off >= 0x2000 || BIT_GET(scratch.inc, off)) continue;
        Ins in;
        if (!decode(img, base, a, &in)) continue;                 /* becomes an exit */
        BIT_SET(scratch.inc, off);
        for (int b = 0; b < in.len; b++) BIT_SET(scratch.cov, off + b);
        scratch.n_ins++;
        uint16_t next = (uint16_t)(a + in.len);
        switch (flow_of(in.op)) {
        case F_SEQ: queue[qt++] = next; break;
        case F_BRANCH: queue[qt++] = next; queue[qt++] = in.target; break;
        case F_JUMP: queue[qt++] = in.target; break;
        case F_SET: {
            Ins nx;
            if (decode(img, base, next, &nx) && flow_of(nx.op) == F_SEQ) {
                BIT_SET(scratch.setinl, off);
                for (int b = 0; b < nx.len; b++) BIT_SET(scratch.cov, (next - base) + b);
                queue[qt++] = (uint16_t)(next + nx.len);
            }
            break;
        }
        case F_CALL:
            if ((in.target & 0xE000) == base) push_work(img_index, in.target);
            break;
        case F_EXIT_AFTER:
            push_work(img_index, next);
            break;
        default:
            break;
        }
        if (qt > (int)(sizeof queue / sizeof queue[0]) - 4) break;
    }
}

/* Do two images agree on every byte covered by cov_a or cov_b? */
static bool agree(const uint8_t *a, const uint8_t *b, const uint8_t *cov_a, const uint8_t *cov_b,
                  const uint8_t *skip)
{
    for (int w = 0; w < 0x2000 / 8; w++) {
        uint8_t m = (uint8_t)((cov_a[w] | (cov_b ? cov_b[w] : 0)) & ~skip[w]);
        if (!m) continue;
        for (int k = 0; k < 8; k++)
            if ((m >> k & 1) && a[w * 8 + k] != b[w * 8 + k]) return false;
    }
    return true;
}

/* An entry captured with several variants: compare all of them over the code reachable from
 * the entry. Bytes that differ only as operands of ordinary instructions are recorded as
 * patched at runtime for that bank window (read from memory by the generated code and left
 * out of code checks). If opcodes, branch offsets or jump targets differ, the variants are
 * different code and nothing is recorded. */
static bool find_patched_operands(const int *imgs, int n_imgs, uint16_t entry)
{
    const Image *base_im = images[imgs[0]];
    uint16_t base = entry & 0xE000;
    Ins first;
    if (!decode(base_im->data, base, entry, &first)) return false;
    int saved_work = n_work;
    closure(imgs[0], base, entry);
    n_work = saved_work;                         /* analysis only: no new work */
    static uint8_t vol[0x2000 / 8];
    memset(vol, 0, sizeof vol);
    bool any = false;
    for (int v = 1; v < n_imgs; v++) {
        const uint8_t *d = images[imgs[v]]->data;
        for (int off = 0; off < 0x2000; off++)
            if (BIT_GET(scratch.cov, off) && d[off] != base_im->data[off]) { BIT_SET(vol, off); any = true; }
    }
    if (!any) return false;
    for (int off = 0; off < 0x2000; off++) {
        if (!BIT_GET(scratch.inc, off)) continue;
        if (BIT_GET(vol, off)) return false;                        /* opcode */
        Ins in;
        decode(base_im->data, base, (uint16_t)(base + off), &in);
        int fl = flow_of(in.op);
        bool ok_operand = fl == F_SEQ || fl == F_EXIT_AFTER || in.op == 0x4C || in.op == 0x20;
        for (int b = 1; b < in.len; b++)
            if (BIT_GET(vol, off + b) && !ok_operand) return false;
    }
    int slot = base_im->bank * 8 + (base >> 13);
    for (int w = 0; w < 0x2000 / 8; w++) bank_vol[slot][w] |= vol[w];
    return true;
}

/* Place an entry: into an existing chunk of the same bank/window whose reference image
 * has the same bytes wherever either has code, or into a new chunk. */
static void add_entry(int img_index, uint16_t entry, int *total_ins)
{
    const Image *im = images[img_index];
    uint16_t base = entry & 0xE000;
    uint32_t eoff = (uint32_t)(entry - base);
    int slot = im->bank * 8 + (base >> 13);

    /* already an entry of a chunk this image agrees with? */
    for (int ci = first_chunk[slot]; ci; ci = next_chunk[ci - 1]) {
        Chunk *k = chunks[ci - 1];
        if (BIT_GET(k->ent, eoff) && agree(images[k->img]->data, im->data, k->cov, NULL, bank_vol[slot])) return;
    }
    Ins first;
    if (!decode(im->data, base, entry, &first)) return;
    closure(img_index, base, entry);

    Chunk *target = NULL;
    for (int ci = first_chunk[slot]; ci; ci = next_chunk[ci - 1]) {
        Chunk *k = chunks[ci - 1];
        if (agree(images[k->img]->data, im->data, k->cov, scratch.cov, bank_vol[slot])) { target = k; break; }
    }
    if (!target) {
        if (n_chunks == cap_chunks) {
            cap_chunks = cap_chunks ? cap_chunks * 2 : 256;
            chunks = realloc(chunks, (size_t)cap_chunks * sizeof *chunks);
            next_chunk = realloc(next_chunk, (size_t)cap_chunks * sizeof *next_chunk);
        }
        target = calloc(1, sizeof *target);
        target->img = img_index;
        target->base = base;
        chunks[n_chunks] = target;
        next_chunk[n_chunks] = first_chunk[slot];
        first_chunk[slot] = ++n_chunks;
    }
    int added = 0;
    for (int w = 0; w < 0x2000 / 8; w++) {
        uint8_t fresh = (uint8_t)(scratch.inc[w] & ~target->inc[w]);
        while (fresh) { added += fresh & 1; fresh >>= 1; }
        target->inc[w] |= scratch.inc[w];
        target->cov[w] |= scratch.cov[w];
        target->setinl[w] |= scratch.setinl[w];
    }
    BIT_SET(target->ent, eoff);
    target->n_ins += added;
    *total_ins += added;
}

typedef struct View {
    const Image *img;
    uint16_t base;
    const uint8_t *inc;
    const uint8_t *vol;          /* patched operand bytes, or NULL */
    Ins *ins;
    int n;
} View;

static bool included(const View *r, uint16_t a)
{
    uint32_t off = (uint32_t)(a - r->base);
    return a >= r->base && off < 0x2000 && BIT_GET(r->inc, off);
}

/* ---- C emission -------------------------------------------------------------------- */

/* Operand bytes: constants, or reads from the code itself when the game patches them. */
static void opnd8(char *o, size_t n, const Ins *in, int i)
{
    if (in->vol >> i & 1) snprintf(o, n, "hu_rd(c, 0x%04X)", (unsigned)(uint16_t)(in->addr + i));
    else snprintf(o, n, "0x%02X", in->b[i]);
}

static void opnd16(char *o, size_t n, const Ins *in, int i)
{
    if (in->vol >> i & 3)
        snprintf(o, n, "(uint16_t)(hu_rd(c, 0x%04X) | hu_rd(c, 0x%04X) << 8)",
                 (unsigned)(uint16_t)(in->addr + i), (unsigned)(uint16_t)(in->addr + i + 1));
    else snprintf(o, n, "0x%04X", (unsigned)(in->b[i] | in->b[i + 1] << 8));
}

static void zp_addr(char *o, size_t n, const Ins *in, int i)
{
    if (in->vol >> i & 1) snprintf(o, n, "(uint16_t)(0x2000 | hu_rd(c, 0x%04X))", (unsigned)(uint16_t)(in->addr + i));
    else snprintf(o, n, "0x20%02X", in->b[i]);
}

static void ea_expr(char *o, size_t n, const Ins *in)
{
    char v[96];
    switch (op_mode[in->op]) {
    case ZP:    zp_addr(o, n, in, 1); break;
    case ZPX:   opnd8(v, sizeof v, in, 1); snprintf(o, n, "hu_ea_zpx(c, %s)", v); break;
    case ZPY:   opnd8(v, sizeof v, in, 1); snprintf(o, n, "hu_ea_zpy(c, %s)", v); break;
    case IZX:   opnd8(v, sizeof v, in, 1); snprintf(o, n, "hu_ea_izx(c, %s)", v); break;
    case IZY:   opnd8(v, sizeof v, in, 1); snprintf(o, n, "hu_ea_izy(c, %s)", v); break;
    case IZP:   opnd8(v, sizeof v, in, 1); snprintf(o, n, "hu_ea_izp(c, %s)", v); break;
    case ABS:   opnd16(o, n, in, 1); break;
    case ABX:   opnd16(v, sizeof v, in, 1); snprintf(o, n, "hu_ea_abx(c, %s)", v); break;
    case ABY:   opnd16(v, sizeof v, in, 1); snprintf(o, n, "hu_ea_aby(c, %s)", v); break;
    case IMZP:  zp_addr(o, n, in, 2); break;
    case IMZPX: opnd8(v, sizeof v, in, 2); snprintf(o, n, "hu_ea_zpx(c, %s)", v); break;
    case IMAB:  opnd16(o, n, in, 2); break;
    case IMABX: opnd16(v, sizeof v, in, 2); snprintf(o, n, "hu_ea_abx(c, %s)", v); break;
    default:    snprintf(o, n, "0"); break;
    }
}

static void val_expr(char *o, size_t n, const Ins *in)
{
    if (op_mode[in->op] == IMM) { opnd8(o, n, in, 1); return; }
    char ea[200];
    ea_expr(ea, sizeof ea, in);
    snprintf(o, n, "hu_rd(c, %s)", ea);
}

/* Chunk context for stores that may patch the chunk's own code. */
typedef struct SmcCtx {
    int win;                 /* code window (address >> 13) */
    uint8_t bank;
    int u;                   /* chunk index: R<u> are its code ranges */
    int nr;
    const uint8_t *cov;      /* code bytes of the chunk */
    uint16_t next;           /* where execution continues after this instruction */
} SmcCtx;

static bool const_ea(const Ins *in, uint16_t *addr)
{
    const uint8_t *b = in->b;
    if (in->vol) return false;
    switch (op_mode[in->op]) {
    case ZP:   *addr = (uint16_t)(0x2000 | b[1]); return true;
    case ABS:  *addr = (uint16_t)(b[1] | b[2] << 8); return true;
    case IMZP: *addr = (uint16_t)(0x2000 | b[2]); return true;
    case IMAB: *addr = (uint16_t)(b[2] | b[3] << 8); return true;
    default:   return false;
    }
}

/* Emit a store-like operation `op` that uses `ea_` as its address. After it, leave the
 * chunk if the store may have changed one of its own instructions. */
static void emit_store(FILE *f, const Ins *in, const char *ea, const char *op, const SmcCtx *x)
{
    uint16_t a;
    if (const_ea(in, &a)) {
        bool hit = (a >> 13) == x->win && BIT_GET(x->cov, a & 0x1FFF);
        if (!hit) { fprintf(f, " { const uint16_t ea_ = 0x%04X; %s; }\n", a, op); return; }
        fprintf(f, " { const uint16_t ea_ = 0x%04X; %s; if (c->mpr[%d] == 0x%02X) { c->pc = 0x%04X; return cyc; } }\n",
                a, op, x->win, x->bank, x->next);
        return;
    }
    fprintf(f, " { const uint16_t ea_ = %s; %s; if ((ea_ >> 13) == %d && c->mpr[%d] == 0x%02X && "
               "recomp_hits(R%d, %d, ea_ & 0x1FFF)) { c->pc = 0x%04X; return cyc; } }\n",
            ea, op, x->win, x->win, x->bank, x->u, x->nr, x->next);
}

/* Data effects of one instruction (no control flow). */
static void emit_body(FILE *f, const Ins *in, bool t, const SmcCtx *x)
{
    uint8_t op = in->op;
    char V[256], A[220], I1[96], I2[96], I3[96];
    val_expr(V, sizeof V, in);
    ea_expr(A, sizeof A, in);
    opnd8(I1, sizeof I1, in, 1);
    fprintf(f, "    cyc += %d;", hu_base_cycles[op]);
    static const char *const alu[8] = {"HU_ORA", "HU_AND", "HU_EOR", "HU_ADC", NULL, NULL, NULL, "HU_SBC"};
    uint8_t lo = op & 0x1F, grp = op >> 5;
    if (alu[grp] && op_defined[op] && (lo == 0x09 || lo == 0x05 || lo == 0x15 || lo == 0x0D || lo == 0x1D ||
                                      lo == 0x19 || lo == 0x01 || lo == 0x11 || lo == 0x12)) {
        if (t && x->win == 1) {   /* T mode writes zero page [X]: same window as the code */
            char op_[160];
            snprintf(op_, sizeof op_, "cyc += hu_alu(c, %s, %s, 1)", alu[grp], V);
            Ins z = *in;
            z.op = 0x95;                                  /* treat as a zp,X store for the check */
            z.b[1] = 0;
            emit_store(f, &z, "hu_ea_zpx(c, 0x00)", op_, x);
            return;
        }
        fprintf(f, " cyc += hu_alu(c, %s, %s, %d);\n", alu[grp], V, t ? 1 : 0);
        return;
    }
    switch (op) {
    case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xA1: case 0xB1: case 0xB2:
        fprintf(f, " c->a = %s; hu_set_nz(c, c->a);\n", V); return;
    case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE:
        fprintf(f, " c->x = %s; hu_set_nz(c, c->x);\n", V); return;
    case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC:
        fprintf(f, " c->y = %s; hu_set_nz(c, c->y);\n", V); return;
    case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81: case 0x91: case 0x92:
        emit_store(f, in, A, "hu_wr(c, ea_, c->a)", x); return;
    case 0x86: case 0x96: case 0x8E: emit_store(f, in, A, "hu_wr(c, ea_, c->x)", x); return;
    case 0x84: case 0x94: case 0x8C: emit_store(f, in, A, "hu_wr(c, ea_, c->y)", x); return;
    case 0x64: case 0x74: case 0x9C: case 0x9E: emit_store(f, in, A, "hu_wr(c, ea_, 0)", x); return;
    case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1: case 0xD2:
        fprintf(f, " hu_cmp(c, c->a, %s);\n", V); return;
    case 0xE0: case 0xE4: case 0xEC: fprintf(f, " hu_cmp(c, c->x, %s);\n", V); return;
    case 0xC0: case 0xC4: case 0xCC: fprintf(f, " hu_cmp(c, c->y, %s);\n", V); return;
    case 0x89: case 0x24: case 0x34: case 0x2C: case 0x3C: fprintf(f, " hu_bit(c, %s);\n", V); return;
    case 0x83: case 0xA3: case 0x93: case 0xB3: fprintf(f, " hu_tst(c, %s, hu_rd(c, %s));\n", I1, A); return;
    case 0x04: case 0x0C: emit_store(f, in, A, "hu_tsb_trb(c, ea_, 1)", x); return;
    case 0x14: case 0x1C: emit_store(f, in, A, "hu_tsb_trb(c, ea_, 0)", x); return;
    case 0x0A: fprintf(f, " c->a = hu_shift(c, HU_ASL, c->a);\n"); return;
    case 0x2A: fprintf(f, " c->a = hu_shift(c, HU_ROL, c->a);\n"); return;
    case 0x4A: fprintf(f, " c->a = hu_shift(c, HU_LSR, c->a);\n"); return;
    case 0x6A: fprintf(f, " c->a = hu_shift(c, HU_ROR, c->a);\n"); return;
    case 0x06: case 0x16: case 0x0E: case 0x1E: emit_store(f, in, A, "hu_rmw_shift(c, HU_ASL, ea_)", x); return;
    case 0x26: case 0x36: case 0x2E: case 0x3E: emit_store(f, in, A, "hu_rmw_shift(c, HU_ROL, ea_)", x); return;
    case 0x46: case 0x56: case 0x4E: case 0x5E: emit_store(f, in, A, "hu_rmw_shift(c, HU_LSR, ea_)", x); return;
    case 0x66: case 0x76: case 0x6E: case 0x7E: emit_store(f, in, A, "hu_rmw_shift(c, HU_ROR, ea_)", x); return;
    case 0x1A: fprintf(f, " c->a++; hu_set_nz(c, c->a);\n"); return;
    case 0x3A: fprintf(f, " c->a--; hu_set_nz(c, c->a);\n"); return;
    case 0xE6: case 0xF6: case 0xEE: case 0xFE: emit_store(f, in, A, "hu_rmw_inc(c, ea_, 1)", x); return;
    case 0xC6: case 0xD6: case 0xCE: case 0xDE: emit_store(f, in, A, "hu_rmw_inc(c, ea_, -1)", x); return;
    case 0xE8: fprintf(f, " c->x++; hu_set_nz(c, c->x);\n"); return;
    case 0xCA: fprintf(f, " c->x--; hu_set_nz(c, c->x);\n"); return;
    case 0xC8: fprintf(f, " c->y++; hu_set_nz(c, c->y);\n"); return;
    case 0x88: fprintf(f, " c->y--; hu_set_nz(c, c->y);\n"); return;
    case 0xAA: fprintf(f, " c->x = c->a; hu_set_nz(c, c->x);\n"); return;
    case 0xA8: fprintf(f, " c->y = c->a; hu_set_nz(c, c->y);\n"); return;
    case 0x8A: fprintf(f, " c->a = c->x; hu_set_nz(c, c->a);\n"); return;
    case 0x98: fprintf(f, " c->a = c->y; hu_set_nz(c, c->a);\n"); return;
    case 0xBA: fprintf(f, " c->x = c->s; hu_set_nz(c, c->x);\n"); return;
    case 0x9A: fprintf(f, " c->s = c->x;\n"); return;
    case 0x22: fprintf(f, " { uint8_t s_ = c->a; c->a = c->x; c->x = s_; }\n"); return;
    case 0x42: fprintf(f, " { uint8_t s_ = c->a; c->a = c->y; c->y = s_; }\n"); return;
    case 0x02: fprintf(f, " { uint8_t s_ = c->x; c->x = c->y; c->y = s_; }\n"); return;
    case 0x62: fprintf(f, " c->a = 0;\n"); return;
    case 0x82: fprintf(f, " c->x = 0;\n"); return;
    case 0xC2: fprintf(f, " c->y = 0;\n"); return;
    case 0x48: fprintf(f, " hu_push(c, c->a);\n"); return;
    case 0xDA: fprintf(f, " hu_push(c, c->x);\n"); return;
    case 0x5A: fprintf(f, " hu_push(c, c->y);\n"); return;
    case 0x08: fprintf(f, " hu_push(c, (uint8_t)(c->p | HU_FB));\n"); return;
    case 0x68: fprintf(f, " c->a = hu_pull(c); hu_set_nz(c, c->a);\n"); return;
    case 0xFA: fprintf(f, " c->x = hu_pull(c); hu_set_nz(c, c->x);\n"); return;
    case 0x7A: fprintf(f, " c->y = hu_pull(c); hu_set_nz(c, c->y);\n"); return;
    case 0x28: fprintf(f, " c->p = (uint8_t)(hu_pull(c) & ~HU_FB);\n"); return;
    case 0x18: fprintf(f, " c->p &= (uint8_t)~HU_FC;\n"); return;
    case 0x38: fprintf(f, " c->p |= HU_FC;\n"); return;
    case 0x58: fprintf(f, " c->p &= (uint8_t)~HU_FI;\n"); return;
    case 0x78: fprintf(f, " c->p |= HU_FI;\n"); return;
    case 0xD8: fprintf(f, " c->p &= (uint8_t)~HU_FD;\n"); return;
    case 0xF8: fprintf(f, " c->p |= HU_FD;\n"); return;
    case 0xB8: fprintf(f, " c->p &= (uint8_t)~HU_FV;\n"); return;
    case 0x53: fprintf(f, " hu_tam(c, %s);\n", I1); return;
    case 0x43: fprintf(f, " hu_tma(c, %s);\n", I1); return;
    case 0x03: fprintf(f, " hu_st(c, 0, %s);\n", I1); return;
    case 0x13: fprintf(f, " hu_st(c, 1, %s);\n", I1); return;
    case 0x23: fprintf(f, " hu_st(c, 2, %s);\n", I1); return;
    case 0x54: fprintf(f, " c->high_speed = false;\n"); return;
    case 0xD4: fprintf(f, " c->high_speed = true;\n"); return;
    case 0x73: case 0xC3: case 0xD3: case 0xE3: case 0xF3:
        opnd16(I1, sizeof I1, in, 1);
        opnd16(I2, sizeof I2, in, 3);
        opnd16(I3, sizeof I3, in, 5);
        fprintf(f, " hu_block_push(c); cyc += hu_block_run(c, 0x%02X, %s, %s, %s);\n", op, I1, I2, I3);
        return;
    default:
        if ((op & 0x0F) == 0x07) {
            char op_[64];
            if (op < 0x80) snprintf(op_, sizeof op_, "hu_rmb(c, ea_, %d)", op >> 4);
            else snprintf(op_, sizeof op_, "hu_smb(c, ea_, %d)", (op >> 4) - 8);
            emit_store(f, in, A, op_, x);
            return;
        }
        fprintf(f, "\n");   /* NOP / undefined */
        return;
    }
}

static void emit_goto(FILE *f, const View *r, uint16_t from, uint16_t to)
{
    if (included(r, to)) {
        if (to <= from) fprintf(f, "    if (cyc >= budget) { c->pc = 0x%04X; return cyc; }\n", to);
        fprintf(f, "    goto L%04X;\n", to);
    } else {
        fprintf(f, "    c->pc = 0x%04X; return cyc;\n", to);
    }
}

static void emit_branch_cond(char *o, size_t n, const Ins *in)
{
    switch (in->op) {
    case 0x10: snprintf(o, n, "!(c->p & HU_FN)"); return;
    case 0x30: snprintf(o, n, "(c->p & HU_FN)"); return;
    case 0x50: snprintf(o, n, "!(c->p & HU_FV)"); return;
    case 0x70: snprintf(o, n, "(c->p & HU_FV)"); return;
    case 0x90: snprintf(o, n, "!(c->p & HU_FC)"); return;
    case 0xB0: snprintf(o, n, "(c->p & HU_FC)"); return;
    case 0xD0: snprintf(o, n, "!(c->p & HU_FZ)"); return;
    case 0xF0: snprintf(o, n, "(c->p & HU_FZ)"); return;
    default: {
        int bit = (in->op >> 4) & 7;
        char z[96];
        zp_addr(z, sizeof z, in, 1);
        if (in->op < 0x80) snprintf(o, n, "!(hu_rd(c, %s) & 0x%02X)", z, 1u << bit);
        else snprintf(o, n, "(hu_rd(c, %s) & 0x%02X)", z, 1u << bit);
        return;
    }
    }
}

/* Continue at `next` after instruction index i (fall through, goto or exit). */
static void emit_continue(FILE *f, const View *r, int i, uint16_t next)
{
    if (!included(r, next)) { fprintf(f, "    c->pc = 0x%04X; return cyc;\n", next); return; }
    if (i + 1 < r->n && r->ins[i + 1].addr == next) return;   /* falls through */
    fprintf(f, "    goto L%04X;\n", next);
}

static void emit_chunk(FILE *f, const View *r, const char *name, int u, int nr, const uint8_t *cov, uint8_t bank)
{
    SmcCtx x = {r->base >> 13, bank, u, nr, cov, 0};
    fprintf(f, "int %s(HuC6280 *c, int budget)\n{\n    int cyc = 0;\n    (void)budget;\n    switch (c->pc) {\n", name);
    for (int i = 0; i < r->n; i++) fprintf(f, "    case 0x%04X: goto L%04X;\n", r->ins[i].addr, r->ins[i].addr);
    fprintf(f, "    default: return -1;\n    }\n");
    for (int i = 0; i < r->n; i++) {
        const Ins *in = &r->ins[i];
        uint16_t next = (uint16_t)(in->addr + in->len);
        fprintf(f, "L%04X:\n", in->addr);
        switch (flow_of(in->op)) {
        case F_SEQ:
            x.next = next;
            emit_body(f, in, false, &x);
            emit_continue(f, r, i, next);
            break;
        case F_EXIT_AFTER:
            x.next = next;
            emit_body(f, in, false, &x);
            fprintf(f, "    c->pc = 0x%04X; return cyc;\n", next);
            break;
        case F_SET:
            fprintf(f, "    cyc += 2; c->p |= HU_FT;\n");
            if (in->set_inline) {
                Ins nx;
                decode(r->img->data, r->base, next, &nx);
                nx.vol = 0;
                if (r->vol)
                    for (int b = 1; b < nx.len && (next - r->base) + b < 0x2000; b++)
                        if (BIT_GET(r->vol, (next - r->base) + b)) nx.vol |= (uint8_t)(1u << b);
                fprintf(f, "    c->p &= (uint8_t)~HU_FT;\n");
                x.next = (uint16_t)(next + nx.len);
                emit_body(f, &nx, true, &x);
                emit_continue(f, r, i, (uint16_t)(next + nx.len));
            } else {
                fprintf(f, "    c->pc = 0x%04X; return cyc;\n", next);
            }
            break;
        case F_BRANCH: {
            char cond[200];
            emit_branch_cond(cond, sizeof cond, in);
            fprintf(f, "    cyc += %d;\n    if (%s) {\n        cyc += 2;\n", hu_base_cycles[in->op], cond);
            emit_goto(f, r, in->addr, in->target);
            fprintf(f, "    }\n");
            emit_continue(f, r, i, next);
            break;
        }
        case F_JUMP:
            fprintf(f, "    cyc += %d;\n", hu_base_cycles[in->op]);
            if (in->op == 0x4C && in->vol) {
                char t[96];
                opnd16(t, sizeof t, in, 1);
                fprintf(f, "    c->pc = %s; return cyc;\n", t);
            } else {
                emit_goto(f, r, in->addr, in->target);
            }
            break;
        case F_CALL:
            if (in->op == 0x20 && in->vol) {
                char t[96];
                opnd16(t, sizeof t, in, 1);
                fprintf(f, "    cyc += 7; { uint16_t t_ = %s; hu_push_return(c, 0x%04X); c->pc = t_; } return cyc;\n",
                        t, (uint16_t)(in->addr + 2));
            } else if (in->op == 0x20)
                fprintf(f, "    cyc += 7; hu_push_return(c, 0x%04X); c->pc = 0x%04X; return cyc;\n",
                        (uint16_t)(in->addr + 2), in->target);
            else
                fprintf(f, "    cyc += 8; hu_push_return(c, 0x%04X); c->pc = 0x%04X; return cyc;\n",
                        (uint16_t)(in->addr + 1), in->target);
            break;
        case F_EXIT:
            switch (in->op) {
            case 0x60: fprintf(f, "    cyc += 7; c->pc = hu_rts(c); return cyc;\n"); break;
            case 0x40: fprintf(f, "    cyc += 7; c->pc = hu_rti(c); return cyc;\n"); break;
            case 0x00: fprintf(f, "    cyc += 8; c->pc = hu_brk(c, 0x%04X); return cyc;\n", (uint16_t)(in->addr + 2)); break;
            case 0x6C: fprintf(f, "    cyc += 7; c->pc = hu_rd16(c, 0x%04X); return cyc;\n", (unsigned)(in->b[1] | in->b[2] << 8)); break;
            default:   fprintf(f, "    cyc += 7; c->pc = hu_rd16(c, hu_ea_abx(c, 0x%04X)); return cyc;\n", (unsigned)(in->b[1] | in->b[2] << 8)); break;
            }
            break;
        }
    }
    fprintf(f, "}\n\n");
}

/* ---- driver ------------------------------------------------------------------------ */

typedef struct Unique {
    uint8_t bank;
    uint16_t base;
    uint16_t n_ranges;
    uint16_t *ranges;
    uint64_t hash;
    uint64_t sig;
    int *comp_of;                /* offset -> component record */
} Unique;

typedef struct EntryRec { uint32_t key; uint32_t chunk; } EntryRec;

/* A component: instructions of a chunk connected by fallthrough, branches and jumps.
 * Execution entering a component stays inside it until it leaves the chunk, so each
 * component is checked against memory on its own (one table record per component). */
typedef struct CompRec {
    uint8_t bank;
    int u;                       /* chunk function */
    uint16_t n_ranges;
    uint16_t *ranges;
    uint64_t hash;
} CompRec;

static CompRec *comps;
static int n_comps, cap_comps;

static int uf_find(int *parent, int x)
{
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
}

/* Split chunk u (its sorted instructions) into components; fills comp_of[offset]. */
static void build_components(const Image *im, uint16_t base, const Ins *ins, int n, const uint8_t *inc,
                             const uint8_t *vol, int u, int *comp_of)
{
    static int parent[0x2000], comp_index[0x2000];
    for (int o = 0; o < 0x2000; o++) { parent[o] = o; comp_index[o] = -1; comp_of[o] = -1; }
#define INC(o) ((o) >= 0 && (o) < 0x2000 && BIT_GET(inc, (o)))
#define UNION(a, b) do { int ra_ = uf_find(parent, (a)), rb_ = uf_find(parent, (b)); if (ra_ != rb_) parent[ra_] = rb_; } while (0)
    for (int i = 0; i < n; i++) {
        const Ins *in = &ins[i];
        int off = in->addr - base, next = off + in->len;
        int tgt = (int)in->target - (int)base;
        switch (flow_of(in->op)) {
        case F_SEQ: if (INC(next)) UNION(off, next); break;
        case F_BRANCH:
            if (INC(next)) UNION(off, next);
            if ((in->target & 0xE000) == base && INC(tgt)) UNION(off, tgt);
            break;
        case F_JUMP:
            if (!(in->op == 0x4C && in->vol) && (in->target & 0xE000) == base && INC(tgt)) UNION(off, tgt);
            break;
        case F_SET:
            if (in->set_inline) {
                Ins nx;
                decode(im->data, base, (uint16_t)(in->addr + 1), &nx);
                int after = next + nx.len;
                if (INC(after)) UNION(off, after);
            }
            break;
        default:
            break;
        }
    }
    /* bytes per component (an instruction's own bytes, plus the instruction inlined after SET) */
    static uint8_t mark[0x2000 / 8];
    for (int i = 0; i < n; i++) {
        int off = ins[i].addr - base;
        int r = uf_find(parent, off);
        if (comp_index[r] < 0) {
            if (n_comps == cap_comps) {
                cap_comps = cap_comps ? cap_comps * 2 : 1024;
                comps = realloc(comps, (size_t)cap_comps * sizeof *comps);
            }
            comp_index[r] = n_comps;
            comps[n_comps] = (CompRec){im->bank, u, 0, NULL, 0};
            n_comps++;
        }
        comp_of[off] = comp_index[r];
    }
    for (int c0 = 0; c0 < n_comps; c0++) {
        if (comps[c0].u != u || comps[c0].ranges) continue;
        memset(mark, 0, sizeof mark);
        for (int i = 0; i < n; i++) {
            int off = ins[i].addr - base;
            if (comp_of[off] != c0) continue;
            int len = ins[i].len;
            if (flow_of(ins[i].op) == F_SET && ins[i].set_inline) {
                Ins nx;
                decode(im->data, base, (uint16_t)(ins[i].addr + 1), &nx);
                len += nx.len;
            }
            for (int b = 0; b < len && off + b < 0x2000; b++)
                if (!(vol && BIT_GET(vol, off + b))) BIT_SET(mark, off + b);
        }
        static uint16_t rr[2 * 0x2000];
        int nr = 0;
        for (int o = 0; o < 0x2000;) {
            if (!BIT_GET(mark, o)) { o++; continue; }
            int st0 = o;
            while (o < 0x2000 && BIT_GET(mark, o)) o++;
            rr[2 * nr] = (uint16_t)st0;
            rr[2 * nr + 1] = (uint16_t)(o - st0);
            nr++;
        }
        comps[c0].n_ranges = (uint16_t)nr;
        comps[c0].ranges = malloc(sizeof(uint16_t) * 2 * (size_t)(nr ? nr : 1));
        memcpy(comps[c0].ranges, rr, sizeof(uint16_t) * 2 * (size_t)nr);
        comps[c0].hash = recomp_code_hash(im->data, rr, nr);
    }
#undef INC
#undef UNION
}

static int cmp_entry(const void *a, const void *b)
{
    const EntryRec *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->chunk < y->chunk ? -1 : x->chunk > y->chunk;
}

static FILE *open_out(const char *dir, const char *name, char *err, int err_len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) snprintf(err, (size_t)err_len, "cannot write %s", path);
    return f;
}

static const char *const GEN_HEADER = "/* Generated from the player's disc by RondoRecomp. Do not distribute. */\n";

bool gen_run(const GenOptions *o, GenStats *st, GenLog log, char *err, int err_len)
{
    memset(st, 0, sizeof *st);
    g_log = log;
    g_verbose = o->verbose;
    int max_total = o->max_regions > 0 ? o->max_regions * 16 : 2000000;   /* instruction safety cap */
    init_ops();

    say("Running the game headless to find its code (intro, attract, menus, Stage 0; %d overlay groups)...", RONDO_GROUPS);
    if (!materialize(o->cue, st, err, err_len)) return false;
    say("  capture run: %d entry points; %d overlay loads; %d distinct bank images",
        st->capture_seeds, st->groups_loaded, n_images);

    /* seeds: those found by the capture run, plus seed files (bank, address, block hash) */
    for (int fi = 0; fi < 4 && o->seed_files[fi]; fi++) {
        FILE *in = fopen(o->seed_files[fi], "r");
        if (!in) { say("  (no seed file %s)", o->seed_files[fi]); continue; }
        unsigned b, a, h;
        while (fscanf(in, "%x %x %x", &b, &a, &h) == 3) add_seed((uint8_t)b, (uint16_t)a, h, -1);
        fclose(in);
    }
    st->seeds = n_seeds;

    first_chunk = calloc(256 * 8, sizeof *first_chunk);
    bank_vol = calloc(256 * 8, sizeof *bank_vol);
    int unmatched[256] = {0};
    int total_ins = 0;

    /* pass 1: entries captured with several variants -> bytes the game patches */
    {
        int *order = malloc(sizeof(int) * (size_t)(n_seeds ? n_seeds : 1)), n_order = 0;
        for (int s = 0; s < n_seeds; s++) if (seeds[s].img >= 0) order[n_order++] = s;
        g_sort_seeds = seeds;
        qsort(order, (size_t)n_order, sizeof *order, cmp_seed_key);
        int *imgs = malloc(sizeof(int) * (size_t)(n_order ? n_order : 1));
        for (int i = 0; i < n_order;) {
            int j = i, n = 0;
            while (j < n_order && seeds[order[j]].bank == seeds[order[i]].bank && seeds[order[j]].addr == seeds[order[i]].addr)
                imgs[n++] = seeds[order[j++]].img;
            if (n >= 4 && find_patched_operands(imgs, n, seeds[order[i]].addr)) st->volatile_entries++;
            i = j;
        }
        free(imgs);
        free(order);
    }

    for (int s = 0; s < n_seeds; s++) {
        if (seeds[s].img >= 0) {                 /* found by the capture run: its own snapshot */
            push_work(seeds[s].img, seeds[s].addr);
            st->seeds_matched++;
            continue;
        }
        bool matched = false;                    /* from a file: every image with that block */
        for (int i = 0; i < n_images; i++) {
            if (images[i]->bank != seeds[s].bank) continue;
            if (hu_block_hash(images[i]->data, seeds[s].addr & 0x1FFF) == seeds[s].h) {
                push_work(i, seeds[s].addr);
                matched = true;
            }
        }
        if (matched) st->seeds_matched++;
        else unmatched[seeds[s].bank]++;
    }
    if (g_verbose)
        for (int b = 0; b < 256; b++)
            if (unmatched[b]) say("  bank %02X: %d seeds without a matching image", b, unmatched[b]);
    free(seeds);
    seeds = NULL;
    n_seeds = cap_seeds = 0;

    for (int w = 0; w < n_work && total_ins < max_total; w++) add_entry(work[w].img, work[w].addr, &total_ins);
    if (g_verbose) {
        int with = 0;
        for (int ci = 0; ci < n_chunks; ci++) with += chunks[ci]->n_ins > 0;
        say("  work items %d, chunks %d (%d with code), instructions %d (cap %d)", n_work, n_chunks, with, total_ins, max_total);
    }
    free(work);
    work = NULL;
    n_work = cap_work = 0;

    /* stores to constant addresses that land on an operand of the chunk's own code: the
     * game patches that instruction while running, so read the operand at runtime */
    for (int ci = 0; ci < n_chunks; ci++) {
        Chunk *k = chunks[ci];
        const Image *im = images[k->img];
        uint8_t *bv = bank_vol[im->bank * 8 + (k->base >> 13)];
        for (int off = 0; off < 0x2000; off++) {
            if (!BIT_GET(k->inc, off)) continue;
            Ins in;
            decode(im->data, k->base, (uint16_t)(k->base + off), &in);
            bool store = false;
            switch (in.op) {
            case 0x8D: case 0x8E: case 0x8C: case 0x9C: case 0x0E: case 0x2E: case 0x4E: case 0x6E:
            case 0xEE: case 0xCE: case 0x0C: case 0x1C:
                store = true;
                break;
            default:
                break;
            }
            if (!store) continue;
            uint16_t t = (uint16_t)(in.b[1] | in.b[2] << 8);
            if ((t & 0xE000) != k->base) continue;
            uint32_t to = (uint32_t)(t - k->base);
            if (!BIT_GET(k->cov, to) || BIT_GET(k->inc, to)) continue;      /* not code, or an opcode */
            /* find the instruction owning that byte (within 6 bytes before it) */
            for (int back = 1; back <= 6 && (int)to - back >= 0; back++) {
                uint32_t io = to - (uint32_t)back;
                if (!BIT_GET(k->inc, io)) continue;
                Ins owner;
                decode(im->data, k->base, (uint16_t)(k->base + io), &owner);
                if ((int)owner.len <= back) break;
                int fl = flow_of(owner.op);
                if (fl == F_SEQ || fl == F_EXIT_AFTER || owner.op == 0x4C || owner.op == 0x20) {
                    BIT_SET(bv, to);
                    st->self_patched++;
                }
                break;
            }
        }
    }

    /* emit unique chunks */
    FILE *hdr = open_out(o->out_dir, "regions.h", err, err_len);
    if (!hdr) return false;
    fprintf(hdr, "%s#pragma once\n#include \"cpu/huc6280.h\"\n\n", GEN_HEADER);
    Unique *uniq = calloc((size_t)(n_chunks ? n_chunks : 1), sizeof *uniq);
    int n_uniq = 0;
    EntryRec *entries = NULL;
    int n_entries = 0, cap_entries = 0;
    Ins *ins = malloc(sizeof(Ins) * 0x2000);
    static uint16_t ranges[2 * 0x2000];
    FILE *f = NULL;
    int file_index = 0, in_file = 0;
    size_t bytes = 0;

    for (int ci = 0; ci < n_chunks; ci++) {
        Chunk *k = chunks[ci];
        if (!k->n_ins) continue;
        const Image *im = images[k->img];
        {
            const uint8_t *bv = bank_vol[im->bank * 8 + (k->base >> 13)];
            for (int off = 0; off < 0x2000; off++) {
                if (!BIT_GET(k->inc, off)) continue;
                Ins in;
                decode(im->data, k->base, (uint16_t)(k->base + off), &in);
                int fl = flow_of(in.op);
                int len = in.len;
                int from = 1;
                if (fl == F_SET && BIT_GET(k->setinl, off)) {      /* the inlined instruction after SET */
                    Ins nx;
                    decode(im->data, k->base, (uint16_t)(k->base + off + 1), &nx);
                    from = 1;
                    len = 1 + nx.len;
                    fl = F_SEQ;
                    for (int b = 2; b < len && off + b < 0x2000; b++)
                        if (BIT_GET(bv, off + b)) { BIT_SET(k->vol, off + b); k->has_vol = true; }
                    continue;
                }
                if (fl != F_SEQ && fl != F_EXIT_AFTER && in.op != 0x4C && in.op != 0x20) continue;
                for (int b = from; b < len && off + b < 0x2000; b++)
                    if (BIT_GET(bv, off + b)) { BIT_SET(k->vol, off + b); k->has_vol = true; }
            }
        }
        int nr = 0;
#define CODE_BYTE(o) (BIT_GET(k->cov, (o)) && !(k->has_vol && BIT_GET(k->vol, (o))))
        for (int off = 0; off < 0x2000;) {
            if (!CODE_BYTE(off)) { off++; continue; }
            int start = off;
            while (off < 0x2000 && CODE_BYTE(off)) off++;
            ranges[2 * nr] = (uint16_t)start;
            ranges[2 * nr + 1] = (uint16_t)(off - start);
            nr++;
        }
        uint64_t code_hash = recomp_code_hash(im->data, ranges, nr);
        uint64_t sig = code_hash ^ ((uint64_t)im->bank << 56) ^ ((uint64_t)k->base << 40);
        sig = (sig ^ fnv64(k->inc, sizeof k->inc)) * 0x100000001b3ull;
        sig = (sig ^ fnv64(k->setinl, sizeof k->setinl)) * 0x100000001b3ull;
        if (k->has_vol) sig = (sig ^ fnv64(k->vol, sizeof k->vol)) * 0x100000001b3ull;

        int u = -1;
        for (int j = 0; j < n_uniq; j++)
            if (uniq[j].sig == sig && uniq[j].bank == im->bank && uniq[j].base == k->base) { u = j; break; }
        if (u < 0) {
            u = n_uniq++;
            uniq[u].bank = im->bank;
            uniq[u].base = k->base;
            uniq[u].n_ranges = (uint16_t)nr;
            uniq[u].ranges = malloc(sizeof(uint16_t) * 2 * (size_t)(nr ? nr : 1));
            memcpy(uniq[u].ranges, ranges, sizeof(uint16_t) * 2 * (size_t)nr);
            uniq[u].hash = code_hash;
            uniq[u].sig = sig;

            int n = 0;
            for (int off = 0; off < 0x2000; off++) {
                if (!BIT_GET(k->inc, off)) continue;
                decode(im->data, k->base, (uint16_t)(k->base + off), &ins[n]);
                ins[n].set_inline = BIT_GET(k->setinl, off);
                ins[n].vol = 0;
                if (k->has_vol)
                    for (int b = 1; b < ins[n].len && off + b < 0x2000; b++)
                        if (BIT_GET(k->vol, off + b)) ins[n].vol |= (uint8_t)(1u << b);
                n++;
            }
            View v = {im, k->base, k->inc, k->has_vol ? k->vol : NULL, ins, n};
            if (!f || in_file >= 40) {
                if (f) { bytes += (size_t)ftell(f); fclose(f); }
                char name[64];
                snprintf(name, sizeof name, "regions_%03d.c", file_index++);
                f = open_out(o->out_dir, name, err, err_len);
                if (!f) return false;
                fprintf(f, "%s#include \"cpu/huc6280_ops.h\"\n#include \"recomp/recomp.h\"\n#include \"regions.h\"\n\n", GEN_HEADER);
                in_file = 0;
            }
            char fname[48];
            snprintf(fname, sizeof fname, "ck_%02X_%04X_%d", im->bank, k->base, u);
            emit_chunk(f, &v, fname, u, nr, k->cov, im->bank);
            uniq[u].comp_of = malloc(sizeof(int) * 0x2000);
            build_components(im, k->base, ins, n, k->inc, k->has_vol ? k->vol : NULL, u, uniq[u].comp_of);
            fprintf(hdr, "int %s(HuC6280 *c, int budget);\nextern const uint16_t R%d[];\n", fname, u);
            in_file++;
            st->instructions += n;
        }
        for (int off = 0; off < 0x2000; off++) {
            /* every instruction of a chunk can be entered (its function switches on the PC), so
             * exits mid-loop (cycle budget, self-patching stores) resume natively */
            if (!BIT_GET(k->inc, off)) continue;
            if (n_entries == cap_entries) {
                cap_entries = cap_entries ? cap_entries * 2 : 16384;
                entries = realloc(entries, (size_t)cap_entries * sizeof *entries);
            }
            int comp = uniq[u].comp_of[off];
            if (comp < 0) continue;
            entries[n_entries++] = (EntryRec){(uint32_t)im->bank << 16 | (uint32_t)(k->base + off), (uint32_t)comp};
        }
    }
    if (f) { bytes += (size_t)ftell(f); fclose(f); }
    fclose(hdr);
    free(ins);

    qsort(entries, (size_t)n_entries, sizeof *entries, cmp_entry);
    int ne = 0;
    for (int i = 0; i < n_entries; i++)
        if (!ne || entries[i].key != entries[ne - 1].key || entries[i].chunk != entries[ne - 1].chunk)
            entries[ne++] = entries[i];

    FILE *t = open_out(o->out_dir, "table.c", err, err_len);
    if (!t) return false;
    fprintf(t, "%s#include \"recomp/recomp.h\"\n#include \"regions.h\"\n\n", GEN_HEADER);
    for (int u = 0; u < n_uniq; u++) {
        fprintf(t, "const uint16_t R%d[] = {", u);
        for (int i = 0; i < 2 * uniq[u].n_ranges; i++) fprintf(t, "%s%u", i ? "," : "", uniq[u].ranges[i]);
        fprintf(t, "};\n");
    }
    for (int c0 = 0; c0 < n_comps; c0++) {
        fprintf(t, "static const uint16_t C%d[] = {", c0);
        for (int i = 0; i < 2 * comps[c0].n_ranges; i++) fprintf(t, "%s%u", i ? "," : "", comps[c0].ranges[i]);
        fprintf(t, "%s};\n", comps[c0].n_ranges ? "" : "0");
    }
    fprintf(t, "\nconst RecompChunk recomp_chunks[%d] = {\n", n_comps ? n_comps : 1);
    for (int c0 = 0; c0 < n_comps; c0++) {
        int u = comps[c0].u;
        fprintf(t, "    {0x%02X, %u, C%d, 0x%016llXull, ck_%02X_%04X_%d},\n", comps[c0].bank, comps[c0].n_ranges, c0,
                (unsigned long long)comps[c0].hash, uniq[u].bank, uniq[u].base, u);
    }
    if (!n_comps) fprintf(t, "    {0}\n");
    fprintf(t, "};\nconst int recomp_chunk_count = %d;\n\n", n_comps);
    fprintf(t, "const RecompEntry recomp_entries[%d] = {\n", ne ? ne : 1);
    for (int i = 0; i < ne; i++) fprintf(t, "    {0x%06X, %u},\n", entries[i].key, entries[i].chunk);
    if (!ne) fprintf(t, "    {0}\n");
    fprintf(t, "};\nconst int recomp_entry_count = %d;\n", ne);
    fprintf(t, "const char recomp_build_id[] = \"%s\";\n", o->build_id ? o->build_id : "generated");
    bytes += (size_t)ftell(t);
    fclose(t);

    st->regions = n_uniq;
    st->entries = ne;
    st->files = file_index + 2;
    st->c_bytes = bytes;
    say("  %d self-patching stores found", st->self_patched);
    say("  %d chunks in %d independently checked components (%d entries with runtime-patched operands), "
        "%d entry points, %d instructions from %d of %d seeds; %d C files, %.1f MB of C",
        n_uniq, n_comps, st->volatile_entries, ne, st->instructions, st->seeds_matched, st->seeds, st->files,
        (double)bytes / 1e6);
    for (int u = 0; u < n_uniq; u++) { free(uniq[u].ranges); free(uniq[u].comp_of); }
    for (int c0 = 0; c0 < n_comps; c0++) free(comps[c0].ranges);
    free(comps);
    comps = NULL;
    n_comps = cap_comps = 0;
    free(uniq);
    free(entries);
    return true;
}
