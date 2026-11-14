/* PC Engine system. See pce.h. */
#include "pce.h"
#include "recomp/recomp.h"

#include <stdio.h>
#include <string.h>

#define TIMER_PERIOD 3072   /* master clocks per timer decrement (1024 CPU cycles at 7.16 MHz) */

/* ---- I/O page ($FF) -------------------------------------------------------- */

static uint8_t io_read(Pce *p, uint16_t off)
{
    switch (off >> 10) {
    case 0: return vdc_read(&p->vdc, off);
    case 1: return vce_read(&p->vce, off);
    case 2: return p->io_buffer;                                     /* PSG: write-only */
    case 3: return (uint8_t)((p->io_buffer & 0x80) | (p->timer_count & 0x7F));
    case 4: {                                                        /* joypad */
        p->joy_reads++;
        uint8_t nib;
        if (p->joy_clr) nib = 0;
        else if (p->joy_sel) nib = (uint8_t)(~(p->pad >> 4) & 0x0F);
        else nib = (uint8_t)(~p->pad & 0x0F);
        /* bit 6: Japanese unit, bit 7 clear: CD-ROM attached */
        p->io_buffer = (uint8_t)(0x40 | 0x30 | nib);
        return p->io_buffer;
    }
    case 5:
        if ((off & 3) == 2) return (uint8_t)((p->io_buffer & 0xF8) | p->irq_disable);
        if ((off & 3) == 3)
            return (uint8_t)((p->io_buffer & 0xF8) | (p->timer_irq ? 4 : 0) | (p->vdc.irq ? 2 : 0));
        return p->io_buffer;
    case 6:                                                          /* CD-ROM interface */
        if (off == 0x18C1) return 0xAA;                              /* Super System Card signature */
        if (off == 0x18C2) return 0x55;
        if (off == 0x18C3) return 0x03;
        p->unhandled_io++;
        return 0x00;
    default:
        return 0xFF;
    }
}

static void io_write(Pce *p, uint16_t off, uint8_t v)
{
    switch (off >> 10) {
    case 0: vdc_write(&p->vdc, off, v); return;
    case 1: vce_write(&p->vce, off, v); return;
    case 2:                                                          /* PSG */
        p->io_buffer = v;
        sound_psg_write(&p->snd, p->master, (uint8_t)(off & 15), v);
        return;
    case 3:
        p->io_buffer = v;
        if (off & 1) {
            bool on = v & 1;
            if (on && !p->timer_on) { p->timer_count = p->timer_reload; p->timer_clock = TIMER_PERIOD; }
            p->timer_on = on;
        } else {
            p->timer_reload = v & 0x7F;
        }
        return;
    case 4:
        p->io_buffer = v;
        p->joy_sel = v & 1;
        p->joy_clr = (v >> 1) & 1;
        return;
    case 5:
        p->io_buffer = v;
        if ((off & 3) == 2) p->irq_disable = v & 7;
        else if ((off & 3) == 3) p->timer_irq = false;
        return;
    case 6:
        p->unhandled_io++;
        return;
    default:
        return;
    }
}

/* ---- bus ------------------------------------------------------------------- */

static uint8_t bus_read(void *ctx, uint32_t a)
{
    Pce *p = ctx;
    uint32_t bank = a >> 13;
    if (bank == 0xFF) return io_read(p, (uint16_t)(a & 0x1FFF));
    uint8_t *pg = p->page[bank];
    return pg ? pg[a & 0x1FFF] : 0xFF;
}

static void bus_write(void *ctx, uint32_t a, uint8_t v)
{
    Pce *p = ctx;
    uint32_t bank = a >> 13;
    if (bank == 0xFF) { io_write(p, (uint16_t)(a & 0x1FFF), v); return; }
    uint8_t *pg = p->page[bank];
    if (pg && !p->page_ro[bank]) {
        pg[a & 0x1FFF] = v;
        /* work RAM mirrors share $F8's write counter */
        p->cpu.page_gen[(bank >= 0xF8 && bank <= 0xFB) ? 0xF8 : bank]++;
    }
}

/* Fast-path pointers for plain memory (see HuC6280.page_ptr). Mirrors of work RAM
 * stay on the slow path so their writes are counted against bank $F8. */
static void map_fast_pages(Pce *p)
{
    for (int b = 0; b < 256; b++) {
        bool mirror = b >= 0xF9 && b <= 0xFB;
        p->cpu.page_ptr[b] = mirror ? NULL : p->page[b];
        p->cpu.page_ram[b] = (uint8_t)(p->page[b] && !p->page_ro[b] && !mirror);
    }
}

uint8_t pce_read8(Pce *p, uint16_t addr) { return bus_read(p, hu_phys(&p->cpu, addr)); }
void pce_write8(Pce *p, uint16_t addr, uint8_t v) { bus_write(p, hu_phys(&p->cpu, addr), v); }

/* ---- bank $00: our own interrupt stubs ----------------------------------------
 * Same dispatch rule as the System Card: if the interrupt's bit in irq_m (zp $F5)
 * is set, jump through the work-RAM vector; otherwise acknowledge and return. */
static void build_rom(Pce *p)
{
    uint8_t *r = p->rom;            /* offset = address - $E000 */
    memset(r, 0x60, sizeof p->rom); /* jump-table area: RTS (entries are intercepted) */
    static const uint8_t irq1[] = {
        0x9F, 0xF5, 0x06,           /* BBS1 <$F5, user        */
        0x48,                       /* PHA                    */
        0xAD, 0x00, 0x00,           /* LDA $0000 (ack VDC)    */
        0x68,                       /* PLA                    */
        0x40,                       /* RTI                    */
        0x6C, 0x02, 0x22,           /* user: JMP ($2202)      */
    };
    static const uint8_t timer[] = {
        0xAF, 0xF5, 0x06,           /* BBS2 <$F5, user        */
        0x48,                       /* PHA                    */
        0x8D, 0x03, 0x14,           /* STA $1403 (ack timer)  */
        0x68,                       /* PLA                    */
        0x40,                       /* RTI                    */
        0x6C, 0x04, 0x22,           /* user: JMP ($2204)      */
    };
    static const uint8_t irq2[] = {
        0x8F, 0xF5, 0x01,           /* BBS0 <$F5, user        */
        0x40,                       /* RTI                    */
        0x6C, 0x00, 0x22,           /* user: JMP ($2200)      */
    };
    memcpy(r + 0x1F00, irq1, sizeof irq1);
    memcpy(r + 0x1F10, timer, sizeof timer);
    memcpy(r + 0x1F20, irq2, sizeof irq2);
    r[0x1F30] = 0x40;                                   /* NMI: RTI */
    r[0x1F38] = 0x4C; r[0x1F39] = 0x38; r[0x1F3A] = 0xFF; /* reset: JMP * */
    r[0x1FF4] = 0x68;                                   /* first Super RAM bank */
    r[0x1FF5] = 0x80;                                   /* first CD RAM bank */
    static const uint16_t vec[5] = {0xFF20, 0xFF00, 0xFF10, 0xFF30, 0xFF38};
    for (int i = 0; i < 5; i++) {
        r[0x1FF6 + 2 * i] = (uint8_t)vec[i];
        r[0x1FF7 + 2 * i] = (uint8_t)(vec[i] >> 8);
    }
}

bool pce_init(Pce *p, const char *cue, char *err, int err_len)
{
    memset(p, 0, sizeof *p);
    if (!disc_open(&p->disc, cue, err, err_len)) return false;
    build_rom(p);
    p->page[0x00] = p->rom;
    p->page_ro[0x00] = true;
    for (int i = 0; i < 24; i++) p->page[0x68 + i] = p->superram + i * 0x2000;
    for (int i = 0; i < 8; i++) p->page[0x80 + i] = p->cdram + i * 0x2000;
    p->page[0xF7] = p->bram;
    for (int i = 0xF8; i <= 0xFB; i++) p->page[i] = p->wram;   /* work RAM mirrors */
    hu_init(&p->cpu, (HuBus){p, bus_read, bus_write});
    map_fast_pages(p);
    recomp_init();
    sound_reset(&p->snd, p->adpcm, &p->disc);
    bram_format(p);                 /* as the System Card does for empty backup RAM */
    p->bram_dirty = false;
    vdc_reset(&p->vdc);
    vce_reset(&p->vce);
    p->timer_clock = TIMER_PERIOD;
    return true;
}

void pce_close(Pce *p) { disc_close(&p->disc); }

bool pce_map_real_bios(Pce *p, const char *path, char *err, int err_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, (size_t)err_len, "cannot open %s", path); return false; }
    static uint8_t img[0x40000];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    if (n != sizeof img) { snprintf(err, (size_t)err_len, "%s: expected 256 KiB", path); return false; }
    p->real_bios = img;
    for (int i = 0; i < 32; i++) {
        p->page[i] = img + i * 0x2000;
        p->page_ro[i] = true;
    }
    map_fast_pages(p);
    return true;
}

bool pce_boot(Pce *p, char *err, int err_len)
{
    const DiscTrack *t = NULL;
    for (int i = 0; i < p->disc.n_tracks; i++)
        if (p->disc.tracks[i].data) { t = &p->disc.tracks[i]; break; }
    if (!t) { snprintf(err, (size_t)err_len, "no data track"); return false; }

    uint8_t hdr[2048];
    if (!disc_read_data(&p->disc, t->lba + 1, 1, hdr) || memcmp(hdr + 0x20, "PC Engine CD-ROM SYSTEM", 23)) {
        snprintf(err, (size_t)err_len, "IPL header not found");
        return false;
    }
    int iplblk = hdr[0] << 16 | hdr[1] << 8 | hdr[2];
    int iplbln = hdr[3];
    uint16_t iplsta = (uint16_t)(hdr[4] | hdr[5] << 8);
    uint16_t ipljmp = (uint16_t)(hdr[6] | hdr[7] << 8);

    HuC6280 *c = &p->cpu;
    c->mpr[0] = 0xFF;
    c->mpr[1] = 0xF8;
    for (int i = 0; i < 5; i++) c->mpr[2 + i] = (uint8_t)(0x80 + hdr[8 + i]);
    c->mpr[7] = 0x00;

    static uint8_t buf[256 * 2048];
    if (!disc_read_data(&p->disc, t->lba + iplblk, iplbln, buf)) {
        snprintf(err, (size_t)err_len, "cannot read IPL program");
        return false;
    }
    for (int i = 0; i < iplbln * 2048; i++) pce_write8(p, (uint16_t)(iplsta + i), buf[i]);

    p->cd_base[0] = p->cd_base[1] = t->lba;
    c->s = 0xFF;
    c->p = 0;
    c->high_speed = true;
    c->pc = ipljmp;
    return true;
}

/* ---- timing ------------------------------------------------------------------ */

static void advance(Pce *p, int master)
{
    p->master += (uint64_t)master;
    p->timer_clock -= master;
    while (p->timer_clock <= 0) {
        p->timer_clock += TIMER_PERIOD;
        if (!p->timer_on) continue;
        if (p->timer_count == 0) {
            p->timer_count = p->timer_reload;
            p->timer_irq = true;
        } else {
            p->timer_count--;
        }
    }
}

/* If the CPU is at a System Card jump-table entry, run the C replacement and return
 * from it; returns the cycles used, or -1 if the PC is not a BIOS entry. */
static int try_hle(Pce *p)
{
    HuC6280 *c = &p->cpu;
    if (p->real_bios || c->mpr[7] != 0x00 || c->pc < 0xE000 || c->pc >= 0xE000 + 3 * PCE_BIOS_ENTRIES ||
        (c->pc - 0xE000) % 3 != 0)
        return -1;
    int cycles = bios_hle_call(p, (c->pc - 0xE000) / 3);
    c->s++;
    uint8_t lo = pce_read8(p, (uint16_t)(0x2100 | c->s));
    c->s++;
    uint8_t hi = pce_read8(p, (uint16_t)(0x2100 | c->s));
    c->pc = (uint16_t)((lo | hi << 8) + 1);
    return cycles;
}

bool pce_call(Pce *p, uint16_t addr, uint8_t a, long max_instructions)
{
    HuC6280 *c = &p->cpu;
    const uint16_t done = 0xFF40;                       /* inside our bank-$00 ROM, never executed */
    pce_write8(p, (uint16_t)(0x2100 | c->s), (uint8_t)((done - 1) >> 8));
    c->s--;
    pce_write8(p, (uint16_t)(0x2100 | c->s), (uint8_t)(done - 1));
    c->s--;
    c->pc = addr;
    c->a = a;
    c->p |= HU_FLAG_I;
    for (long i = 0; i < max_instructions; i++) {
        if (c->pc == done && c->mpr[7] == 0x00) return true;
        if (try_hle(p) >= 0) continue;
        c->irq_pending = 0;
        hu_step(c);
    }
    return false;
}

/* Opcodes after which execution continues at a dispatch point: control transfers and
 * the instructions that end a recompiled region. (RTI is left out: it returns to
 * wherever an interrupt happened.) */
static bool creates_entry(uint8_t op)
{
    switch (op) {
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xB0: case 0xD0: case 0xF0:
    case 0x80: case 0x4C: case 0x6C: case 0x7C: case 0x20: case 0x44: case 0x60: case 0x00:
    case 0x53: case 0x73: case 0xC3: case 0xD3: case 0xE3: case 0xF3:
    case 0x58: case 0x78: case 0x28: case 0x54: case 0xD4:
        return true;
    default:
        return (op & 0x0F) == 0x0F;   /* BBR / BBS */
    }
}

/* run the CPU (and HLE) up to master clock `end` */
static void run_until(Pce *p, uint64_t end)
{
    HuC6280 *c = &p->cpu;
    bool native = recomp_enabled();
    while (p->master < end) {
        int hle = try_hle(p);
        if (hle >= 0) {
            advance(p, hle * 3);
            p->at_entry = true;
            continue;
        }
        uint8_t lines = (uint8_t)((p->vdc.irq ? HU_IRQ1 : 0) | (p->timer_irq ? HU_TIRQ : 0));
        c->irq_pending = lines & (uint8_t)~p->irq_disable;
        bool irq = c->irq_pending && !(c->p & HU_FLAG_I);
        if (p->at_entry && !irq && p->on_entry) p->on_entry(p, p->on_entry_ctx);

        if (native && !irq && !(c->p & HU_FLAG_T)) {
            const RecompChunk *k = recomp_lookup(c);
            if (k) {
                int mult = c->high_speed ? 3 : 12;
                int budget = (int)((end - p->master + (uint64_t)mult - 1) / (uint64_t)mult);
                int n = recomp_run(p, k, budget);
                if (n >= 0) {
                    advance(p, n * mult);
                    p->at_entry = true;
                    continue;
                }
            }
        }
        if (p->at_entry && !irq) recomp_note_miss(c);
        if (irq) p->irq_taken[(c->irq_pending & HU_TIRQ) ? 2 : (c->irq_pending & HU_IRQ1) ? 1 : 0]++;
        if (p->on_step) p->on_step(p, p->on_step_ctx);
        const uint8_t *pg = c->page_ptr[c->mpr[c->pc >> 13]];
        uint8_t op = pg ? pg[c->pc & 0x1FFF] : 0xEA;
        int n = hu_step(c);
        recomp_note_interp(n);
        p->at_entry = irq || creates_entry(op);
        advance(p, n * (c->high_speed ? 3 : 12));
    }
}

/* ---- audio ------------------------------------------------------------------- */

static void cdda_mix(Pce *p, int16_t *out, int frames)
{
    if (!p->cdda.playing) return;
    static uint8_t sector[2352];
    int cached = -1;
    for (int i = 0; i < frames; i++) {
        if (p->cdda.pos >= (int64_t)p->cdda.end_lba * 588) {
            if (!p->cdda.repeat) { p->cdda.playing = false; return; }
            p->cdda.pos = (int64_t)p->cdda.start_lba * 588;
        }
        int lba = (int)(p->cdda.pos / 588);
        int idx = (int)(p->cdda.pos % 588);
        if (lba != cached) {
            if (!disc_read_audio(&p->disc, lba, 1, sector)) { p->cdda.playing = false; return; }
            cached = lba;
        }
        int16_t l = (int16_t)(sector[idx * 4] | sector[idx * 4 + 1] << 8);
        int16_t r = (int16_t)(sector[idx * 4 + 2] | sector[idx * 4 + 3] << 8);
        out[i * 2] = (int16_t)(l * p->cdda.volume / 256);
        out[i * 2 + 1] = (int16_t)(r * p->cdda.volume / 256);
        p->cdda.pos++;
        if (p->cdda.fade_step && (i & 63) == 0) {
            p->cdda.volume += p->cdda.fade_step;
            if (p->cdda.volume <= 0) { p->cdda.volume = 0; p->cdda.fade_step = 0; }
            if (p->cdda.volume >= 256) { p->cdda.volume = 256; p->cdda.fade_step = 0; }
        }
    }
}

void pce_audio_render(Pce *p, int16_t *out, int frames)
{
    memset(out, 0, (size_t)frames * 2 * sizeof *out);
    if (!p->mute_cdda) cdda_mix(p, out, frames);
    sound_advance(&p->snd, p->master);
    sound_take_fit(&p->snd, out, frames);   /* what the frame made, as `frames`: no delay builds up */
}

void pce_run_frame(Pce *p)
{
    for (int line = 0; line < PCE_LINES_PER_FRAME; line++) {
        p->line = line;
        vdc_line_start(&p->vdc, line);
        uint64_t end = p->master + PCE_MASTER_PER_LINE;
        if (line == p->vdc.display_end) {
            /* vertical blank half a line in, after the raster compare's interrupt has read
             * the status (vdc_vblank) */
            run_until(p, p->master + PCE_MASTER_PER_LINE / 2);
            vdc_vblank(&p->vdc);
        }
        run_until(p, end);
        vdc_line_end(&p->vdc, &p->vce, line);
    }
    p->frame++;
}
