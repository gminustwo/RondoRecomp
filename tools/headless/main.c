/* Headless runner: boots the disc on the interpreter, saves screenshots, prints progress.
 *
 * usage: rondo_headless <game.cue> [--frames N] [--shot-every N] [--out DIR]
 *                       [--log-bios] [--press FRAME:BUTTONS[:LEN]]...
 *   BUTTONS: comma list of i,ii,select,run,up,down,left,right
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pce/pce.h"
#include "pce/state.h"
#include "../frontend/game.h"
#include "../frontend/moves.h"
#include "../frontend/prompt.h"
#include "recomp/recomp.h"
#include "../recomp/capture_script.h"
#include "util/png.h"

typedef struct { long frame; int len; uint8_t mask; } Press;

/* PC histogram (bank:addr), excluding the idle loop, for --profile */
static uint32_t *prof;
/* --watch BANK:ADDR: print A/X/Y whenever that instruction runs with different X than last time */
static int n_watch;
static struct { uint8_t bank; uint16_t addr; int last_x; } watch[8];

/* --trace FRAME:COUNT: log COUNT instructions from the start of FRAME to work/trace.txt */
static long trace_frame = -1, trace_left;
static FILE *trace_f;

/* --write-watch ADDR,...: report which instruction changed these WRAM bytes */
static int n_ww;
static unsigned ww_addr[16];
static uint8_t ww_last[16];
static long ww_from = 0, ww_to = 0x7FFFFFFF;
static uint16_t ww_prev_pc;
static uint8_t ww_prev_bank;
static int ww_prev_a, ww_prev_x, ww_prev_y;
static long ww_count, ww_max = 4000;

static void write_watch(Pce *p)
{
    long f = (long)p->frame;
    for (int i = 0; i < n_ww; i++) {
        uint8_t v = p->wram[ww_addr[i]];
        if (v != ww_last[i]) {
            if (f >= ww_from && f < ww_to && ww_count < ww_max) {
                printf("  [write %03X] f%ld l%03d %02X:%04X  %02X->%02X  A=%02X X=%02X Y=%02X\n", ww_addr[i], f, p->line,
                       ww_prev_bank, ww_prev_pc, ww_last[i], v, ww_prev_a, ww_prev_x, ww_prev_y);
                ww_count++;
            }
            ww_last[i] = v;
        }
    }
    ww_prev_pc = p->cpu.pc;
    ww_prev_bank = p->cpu.mpr[p->cpu.pc >> 13];
    ww_prev_a = p->cpu.a;
    ww_prev_x = p->cpu.x;
    ww_prev_y = p->cpu.y;
}

/* --pal-watch LO:HI: report which instruction changed these VCE palette entries (frame,
 * scanline, the new values), within --watch-frames */
static int pw_lo = -1, pw_hi = -1;
static uint16_t pw_last[512];
static uint16_t pw_prev_pc;
static uint8_t pw_prev_bank;

static void pal_watch(Pce *p)
{
    long f = (long)p->frame;
    int changed = 0, first = -1;
    for (int i = pw_lo; i <= pw_hi; i++)
        if (p->vce.pal[i] != pw_last[i]) {
            if (first < 0) first = i;
            changed++;
        }
    if (changed && f >= ww_from && f < ww_to && ww_count < ww_max) {
        printf("  [pal] f%ld l%03d %02X:%04X  %d changed from %03X:", f, p->line, pw_prev_bank, pw_prev_pc, changed, first);
        for (int i = first; i <= pw_hi && i < first + 16; i++) printf(" %03X", p->vce.pal[i]);
        printf("\n");
        ww_count++;
    }
    if (changed) memcpy(&pw_last[pw_lo], &p->vce.pal[pw_lo], (size_t)(pw_hi - pw_lo + 1) * sizeof pw_last[0]);
    pw_prev_pc = p->cpu.pc;
    pw_prev_bank = p->cpu.mpr[p->cpu.pc >> 13];
}

/* --vram-watch LO:HI: count, per instruction address, VRAM writes landing in [LO, HI]
 * (seen as the write address register stepping), printed at the end */
static unsigned vw_lo = 1, vw_hi = 0;
static long vw_from = 0, vw_to = 0x7FFFFFFF;
static uint16_t vw_last_mawr;
static uint32_t *vw_count;   /* bank << 16 | pc */

static void vram_watch(Pce *p)
{
    uint16_t m = p->vdc.reg[0];
    if (m != vw_last_mawr) {
        uint16_t prev = vw_last_mawr;
        vw_last_mawr = m;
        long f = (long)p->frame;
        /* a data write advances the address from where it was */
        if (f >= vw_from && f < vw_to && prev >= vw_lo && prev <= vw_hi && (uint16_t)(m - prev) <= 128)
            vw_count[(uint32_t)ww_prev_bank << 16 | ww_prev_pc]++;
    }
}

/* --scroll-log FILE: BXR, BYR (16-bit each) as set when display row 120 starts, per frame */
static FILE *scroll_log;
static long scroll_last = -1;

static void step_hook(Pce *p, void *ctx)
{
    (void)ctx;
    if (vw_count) vram_watch(p);
    if (n_ww) write_watch(p);
    if (pw_lo >= 0) pal_watch(p);
    else if (vw_count) {
        ww_prev_pc = p->cpu.pc;
        ww_prev_bank = p->cpu.mpr[p->cpu.pc >> 13];
    }
    if (scroll_log && (long)p->frame != scroll_last && p->line >= p->vdc.display_start + 120) {
        uint16_t v[2] = {p->vdc.reg[7], p->vdc.reg[8]};
        fwrite(v, 2, 2, scroll_log);
        scroll_last = (long)p->frame;
    }
    uint16_t pc = p->cpu.pc;
    uint8_t bank = p->cpu.mpr[pc >> 13];
    if (prof) prof[(uint32_t)bank << 16 | pc]++;
    if (trace_left > 0 && (long)p->frame >= trace_frame) {
        if (!trace_f) trace_f = fopen("work/trace.txt", "w");
        if (trace_f) {
            HuC6280 *c = &p->cpu;
            fprintf(trace_f, "f%llu l%03d %02X:%04X  A=%02X X=%02X Y=%02X S=%02X P=%02X  bytes %02X %02X %02X\n",
                    (unsigned long long)p->frame, p->line, bank, pc, c->a, c->x, c->y, c->s, c->p,
                    pce_read8(p, pc), pce_read8(p, (uint16_t)(pc + 1)), pce_read8(p, (uint16_t)(pc + 2)));
        }
        if (--trace_left == 0 && trace_f) fclose(trace_f);
    }
    for (int i = 0; i < n_watch; i++) {
        if (watch[i].addr == pc && watch[i].bank == bank && watch[i].last_x != p->cpu.x) {
            printf("  [watch %02X:%04X] frame %llu line %d  A=%02X X=%02X Y=%02X\n", bank, pc,
                   (unsigned long long)p->frame, p->line, p->cpu.a, p->cpu.x, p->cpu.y);
            watch[i].last_x = p->cpu.x;
        }
    }
}

static uint8_t parse_buttons(const char *s)
{
    static const struct { const char *n; uint8_t m; } map[] = {
        {"i", PAD_I}, {"ii", PAD_II}, {"select", PAD_SELECT}, {"run", PAD_RUN},
        {"up", PAD_UP}, {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"right", PAD_RIGHT},
    };
    uint8_t m = 0;
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", s);
    for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(tok, map[i].n)) m |= map[i].m;
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <game.cue> [--frames N] [--shot-every N] [--out DIR] [--log-bios] [--press F:BUTTONS[:LEN]]\n", argv[0]);
        return 2;
    }
    long frames = 600, shot_every = 60;
    const char *out = "work/shots";
    bool log_bios = false, log_all = false;
    const char *bios_reset = NULL, *wav_path = NULL, *seeds_path = NULL, *misses_path = NULL;
    int verify = 0;
    bool no_native = false, use_script = false, mute_cd = false;
    Press presses[64];
    int n_press = 0;
    struct { long from, to, period; int len; uint8_t mask; } every[16];
    int n_every = 0;
    /* investigation helpers: WRAM watch, pokes, save states, WRAM dumps */
    int n_ram = 0, n_poke = 0, n_dump = 0;
    unsigned ram_addr[32];
    struct { unsigned addr, val; long from, to; } poke[16];   /* --poke ADDR=VAL[@FROM:TO] */
    struct { long frame; char path[256]; } dump[16], save_at[4];
    int n_save_at = 0;
    struct { long frame; char path[256]; } mem_at[4];   /* --dump-mem: all 256 banks + MPRs */
    int n_mem = 0;
    const char *load_state = NULL;
    unsigned long long state_disc = 0;   /* --state-disc HEX: the disc id a state was saved with (rondo.exe's) */
    const char *bram_file = NULL;        /* --bram FILE: backup RAM to start with (a player's bram.bin), not written */
    GameCheats cheats = {0};
    GameWatch gw;
    MoveOptions move_opts = {0};   /* --moves eight,limp: moves.h, Attack being the pad's II */
    static Moves moves;
    bool skip_cine = false;
    long skipped = 0;
    FILE *ram_trace = NULL;   /* --ram-trace FILE: WRAM after every frame */
    long start_frame = 0;
    int prompt_dev = -1;   /* --prompt pad|keys: the title's PUSH RUN BUTTON! as rondo.exe shows it */
    long vram_dump_at = -1;            /* --dump-vram FRAME:FILE: all of VRAM after that frame */
    long adpcm_dump_at = -1;           /* --dump-adpcm FRAME:FILE: the 64 KiB of ADPCM RAM */
    char adpcm_dump_path[256] = "";
    char vram_dump_path[256] = "";
    long run_at[16][2]; /* --run FRAME:LEN: hold the run button */
    int n_run = 0;
    long rows_from = -1, rows_to = -1;   /* --row-scroll A:B: each frame's bands of rows by X scroll */
    long sat_at[8]; /* --dump-sat FRAME: print the sprite table after that frame */
    int n_sat = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) shot_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--log-bios")) log_bios = true;
        else if (!strcmp(argv[i], "--log-bios-all")) log_bios = log_all = true;
        else if (!strcmp(argv[i], "--profile")) prof = calloc(1u << 24, sizeof *prof);
        else if (!strcmp(argv[i], "--bios-reset") && i + 1 < argc) bios_reset = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--record-seeds") && i + 1 < argc) seeds_path = argv[++i];
        else if (!strcmp(argv[i], "--misses") && i + 1 < argc) misses_path = argv[++i];
        else if (!strcmp(argv[i], "--verify") && i + 1 < argc) verify = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-native")) no_native = true;
        else if (!strcmp(argv[i], "--script")) use_script = true;
        else if (!strcmp(argv[i], "--mute-cd")) mute_cd = true;
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc) {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%s", argv[++i]);
            for (char *t = strtok(tmp, ","); t && n_ram < 32; t = strtok(NULL, ","))
                ram_addr[n_ram++] = (unsigned)strtoul(t, NULL, 16) & 0x1FFF;
        }
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc && n_poke < 16) {
            unsigned a, v;
            long from = 0, to = 0x7FFFFFFF;
            const char *spec = argv[++i], *at = strchr(spec, '@');
            if (at) sscanf(at + 1, "%ld:%ld", &from, &to);
            if (sscanf(spec, "%x=%x", &a, &v) == 2) {
                poke[n_poke].addr = a & 0x1FFF;
                poke[n_poke].val = v;
                poke[n_poke].from = from;
                poke[n_poke++].to = to;
            }
        }
        else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc && n_dump < 16) {
            char *c = argv[++i];
            dump[n_dump].frame = atol(c);
            char *colon = strchr(c, ':');
            snprintf(dump[n_dump++].path, sizeof dump[0].path, "%s", colon ? colon + 1 : "work/ram.bin");
        }
        else if (!strcmp(argv[i], "--save-state") && i + 1 < argc && n_save_at < 4) {
            char *c = argv[++i];
            save_at[n_save_at].frame = atol(c);
            char *colon = strchr(c, ':');
            snprintf(save_at[n_save_at++].path, sizeof save_at[0].path, "%s", colon ? colon + 1 : "work/s.state");
        }
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) load_state = argv[++i];
        else if (!strcmp(argv[i], "--state-disc") && i + 1 < argc) state_disc = strtoull(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--bram") && i + 1 < argc) bram_file = argv[++i];
        else if (!strcmp(argv[i], "--cheats") && i + 1 < argc) {
            const char *c = argv[++i];
            cheats.health = strstr(c, "health") != NULL;
            cheats.hearts = strstr(c, "hearts") != NULL;
            cheats.lives = strstr(c, "lives") != NULL;
            cheats.maria_unlocked = strstr(c, "maria") != NULL;
            cheats.play_maria = strstr(c, "play-maria") != NULL;
        }
        else if (!strcmp(argv[i], "--skip-cinematics")) skip_cine = true;
        else if (!strcmp(argv[i], "--moves") && i + 1 < argc) {
            const char *c = argv[++i];
            move_opts.eight_way = strstr(c, "eight") != NULL;
            move_opts.limp_whip = strstr(c, "limp") != NULL;
        }
        else if (!strcmp(argv[i], "--ram-trace") && i + 1 < argc) ram_trace = fopen(argv[++i], "wb");
        else if (!strcmp(argv[i], "--write-watch") && i + 1 < argc) {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%s", argv[++i]);
            for (char *t = strtok(tmp, ","); t && n_ww < 16; t = strtok(NULL, ","))
                ww_addr[n_ww++] = (unsigned)strtoul(t, NULL, 16) & 0x1FFF;
        }
        else if (!strcmp(argv[i], "--prompt") && i + 1 < argc) prompt_dev = !strcmp(argv[++i], "pad");
        else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc) sscanf(argv[++i], "%ld:%255s", &vram_dump_at, vram_dump_path);
        else if (!strcmp(argv[i], "--dump-adpcm") && i + 1 < argc) sscanf(argv[++i], "%ld:%255s", &adpcm_dump_at, adpcm_dump_path);
        else if (!strcmp(argv[i], "--run") && i + 1 < argc && n_run < 16) {
            if (sscanf(argv[++i], "%ld:%ld", &run_at[n_run][0], &run_at[n_run][1]) == 2) n_run++;
        }
        else if (!strcmp(argv[i], "--row-scroll") && i + 1 < argc) sscanf(argv[++i], "%ld:%ld", &rows_from, &rows_to);
        else if (!strcmp(argv[i], "--dump-sat") && i + 1 < argc && n_sat < 8) sat_at[n_sat++] = atol(argv[++i]);
        else if (!strcmp(argv[i], "--vram-watch") && i + 1 < argc) {
            if (sscanf(argv[++i], "%x:%x", &vw_lo, &vw_hi) == 2) vw_count = calloc(1u << 24, sizeof *vw_count);
        }
        else if (!strcmp(argv[i], "--vram-frames") && i + 1 < argc) sscanf(argv[++i], "%ld:%ld", &vw_from, &vw_to);
        else if (!strcmp(argv[i], "--scroll-log") && i + 1 < argc) scroll_log = fopen(argv[++i], "wb");
        else if (!strcmp(argv[i], "--pal-watch") && i + 1 < argc) {
            if (sscanf(argv[++i], "%x:%x", (unsigned *)&pw_lo, (unsigned *)&pw_hi) != 2 || pw_lo < 0 || pw_hi > 511 || pw_lo > pw_hi)
                pw_lo = pw_hi = -1;
        }
        else if (!strcmp(argv[i], "--watch-frames") && i + 1 < argc) sscanf(argv[++i], "%ld:%ld", &ww_from, &ww_to);
        else if (!strcmp(argv[i], "--watch-max") && i + 1 < argc) ww_max = atol(argv[++i]);
        else if (!strcmp(argv[i], "--dump-mem") && i + 1 < argc && n_mem < 4) {
            char *c = argv[++i];
            mem_at[n_mem].frame = atol(c);
            char *colon = strchr(c, ':');
            snprintf(mem_at[n_mem++].path, sizeof mem_at[0].path, "%s", colon ? colon + 1 : "work/mem.bin");
        }
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) {
            sscanf(argv[++i], "%ld:%ld", &trace_frame, &trace_left);
        }
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc && n_watch < 8) {
            unsigned b, a;
            if (sscanf(argv[++i], "%x:%x", &b, &a) == 2)
                watch[n_watch++] = (typeof(watch[0])){(uint8_t)b, (uint16_t)a, -1};
        }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc && n_press < 64) {
            char spec[128], *a, *b;
            snprintf(spec, sizeof spec, "%s", argv[++i]);
            a = strchr(spec, ':');
            if (!a) continue;
            *a++ = 0;
            b = strchr(a, ':');
            if (b) *b++ = 0;
            presses[n_press++] = (Press){atol(spec), b ? atoi(b) : 6, parse_buttons(a)};
        }
        else if (!strcmp(argv[i], "--every") && i + 1 < argc && n_every < 16) {
            /* --every FROM:TO:PERIOD:BUTTONS[:LEN]: press for LEN frames (default 1) every
             * PERIOD frames from FROM to TO (PERIOD 1: held) */
            char btn[64] = "";
            long a, b, per;
            int len = 1;
            if (sscanf(argv[++i], "%ld:%ld:%ld:%63[^:]:%d", &a, &b, &per, btn, &len) >= 4 && per > 0)
                every[n_every++] = (typeof(every[0])){a, b, per, len, parse_buttons(btn)};
        }
    }

    static Pce pce;
    char err[256];
    if (bios_reset) {
        /* Run the original System Card reset and report the hardware state it sets up. */
        if (!pce_init(&pce, argv[1], err, sizeof err) || !pce_map_real_bios(&pce, bios_reset, err, sizeof err)) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        hu_reset(&pce.cpu);
        for (long f = 0; f < frames; f++) {
            pce_run_frame(&pce);
            if ((f + 1) % 30 == 0 || f + 1 == frames) {
                printf("frame %3ld PC=%02X:%04X timer %s reload=$%02X irq_disable=%X irq_m=%02X VCE=%02X  "
                       "VDC CR=%04X MWR=%04X HSR=%04X HDR=%04X VSR=%04X VDR=%04X VCR=%04X  irqs %u/%u/%u\n",
                       f + 1, pce.cpu.mpr[pce.cpu.pc >> 13], pce.cpu.pc, pce.timer_on ? "on" : "off",
                       pce.timer_reload, pce.irq_disable, pce.wram[0xF5], pce.vce.ctrl,
                       pce.vdc.reg[5], pce.vdc.reg[9], pce.vdc.reg[10], pce.vdc.reg[11],
                       pce.vdc.reg[12], pce.vdc.reg[13], pce.vdc.reg[14],
                       pce.irq_taken[0], pce.irq_taken[1], pce.irq_taken[2]);
            }
        }
        return 0;
    }
    if (!pce_init(&pce, argv[1], err, sizeof err) || !pce_boot(&pce, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    if (bram_file) {
        FILE *bf = fopen(bram_file, "rb");
        size_t got = bf ? fread(pce.bram, 1, 0x800, bf) : 0;
        if (bf) fclose(bf);
        printf("backup RAM: %zu bytes from %s\n", got, bram_file);
    }
    pce.log_bios = log_bios;
    pce.log_bios_all = log_all;
    pce.mute_cdda = mute_cd;
    recomp_set_verify(verify);
    if (no_native || seeds_path) recomp_set_enabled(false);   /* seeds: record every entry point */
    if (seeds_path) recomp_load_misses(seeds_path);
    if (prof || n_watch || trace_left || n_ww || scroll_log || vw_count || pw_lo >= 0) pce.on_step = step_hook;
    if (load_state) {
        if (!pce_state_load(&pce, load_state, state_disc, err, sizeof err)) { fprintf(stderr, "load state: %s\n", err); return 1; }
        start_frame = (long)pce.frame;
        printf("state loaded: frame %ld\n", start_frame);
    }
    game_watch_reset(&gw, &pce);
    moves_reset(&moves);
    TitlePrompt tp = {.pad = prompt_dev == 1};
    for (int i = 0; i < n_ww; i++) ww_last[i] = pce.wram[ww_addr[i]];
    memcpy(pw_last, pce.vce.pal, sizeof pw_last);
    uint8_t ram_last[32];
    for (int i = 0; i < n_ram; i++) ram_last[i] = pce.wram[ram_addr[i]];

    /* --wav: 44.1 kHz stereo 16-bit output; header sizes patched at the end */
    FILE *wav = wav_path ? fopen(wav_path, "wb") : NULL;
    uint32_t wav_frames = 0;
    double audio_frac = 0;
    if (wav) { uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, wav); }

    uint64_t start_master = pce.master;
    for (long f = start_frame; f < start_frame + frames; f++) {
        if (wav) {
            static int16_t s[2048 * 2];
            audio_frac += 44100.0 / (7159090.0 / (455.0 * 263.0));
            int n = (int)audio_frac;
            audio_frac -= n;
            pce_audio_render(&pce, s, n);
            fwrite(s, 4, (size_t)n, wav);
            wav_frames += (uint32_t)n;
        }
        pce.pad = use_script ? capture_pad((int)f) : 0;
        for (int i = 0; i < n_press; i++)
            if (f >= presses[i].frame && f < presses[i].frame + presses[i].len) pce.pad |= presses[i].mask;
        for (int i = 0; i < n_every; i++)
            if (f >= every[i].from && f <= every[i].to && (f - every[i].from) % every[i].period < every[i].len)
                pce.pad |= every[i].mask;
        for (int i = 0; i < n_poke; i++)
            if (f >= poke[i].from && f <= poke[i].to) pce.wram[poke[i].addr] = (uint8_t)poke[i].val;
        if (skip_cine && game_in_cinematic(&pce)) { pce.pad = 0; skipped++; }
        pce.pad = moves_pad(&moves, &pce, &move_opts, pce.pad, pce.pad & PAD_II);
        if (prompt_dev >= 0) prompt_before_frame(&tp, &pce);
        moves_before_frame(&moves, &pce, &move_opts, true);
        pce_run_frame(&pce);
        if (prompt_dev >= 0) prompt_after_frame(&tp, &pce);
        moves_after_frame(&moves, &pce, &move_opts, true);
        game_apply_cheats(&pce, &cheats, &gw);
        {
            bool run = false;
            for (int i = 0; i < n_run; i++) run |= f >= run_at[i][0] && f < run_at[i][0] + run_at[i][1];
            game_apply_run(&pce, run, &gw);
        }
        if (n_ram) {
            bool changed = false;
            for (int i = 0; i < n_ram; i++) changed |= pce.wram[ram_addr[i]] != ram_last[i];
            if (changed) {
                printf("frame %5ld pad %02X:", f + 1, pce.pad);
                for (int i = 0; i < n_ram; i++) {
                    printf(" %03X=%02X%s", ram_addr[i], pce.wram[ram_addr[i]], pce.wram[ram_addr[i]] != ram_last[i] ? "*" : " ");
                    ram_last[i] = pce.wram[ram_addr[i]];
                }
                printf("\n");
            }
        }
        if (ram_trace) { fwrite(pce.wram, 1, 0x2000, ram_trace); fputc(pce.pad, ram_trace); }
        for (int i = 0; i < n_dump; i++)
            if (dump[i].frame == f + 1) {
                FILE *df = fopen(dump[i].path, "wb");
                if (df) { fwrite(pce.wram, 1, 0x2000, df); fclose(df); }
            }
        if (f + 1 >= rows_from && f + 1 <= rows_to) {
            const uint16_t *rs = vdc_row_scroll();
            printf("rows %ld:", f + 1);
            for (int y = 0; y < pce.vdc.fb_h;) {
                int e = y;
                while (e + 1 < pce.vdc.fb_h && rs[e + 1] == rs[y]) e++;
                printf(" %d-%d=%d", y, e, rs[y]);
                y = e + 1;
            }
            printf("\n");
        }
        for (int i = 0; i < n_sat; i++)
            if (sat_at[i] == f + 1) {
                printf("sprites after frame %ld (screen x, y; pattern; attr):\n", f + 1);
                for (int k = 0; k < 64; k++) {
                    const uint16_t *e = &pce.vdc.satb[k * 4];
                    if (!(e[0] & 0x3FF) && !(e[1] & 0x3FF)) continue;
                    printf("  %2d: x %4d y %4d  pat %03X attr %04X  raw x %04X\n", k, (e[1] & 0x3FF) - 32,
                           (e[0] & 0x3FF) - 64, (e[2] >> 1) & 0x3FF, e[3], e[1]);
                }
                printf("palettes (VCE: background 00-0F, sprites 10-1F; 9-bit GRB):\n");
                for (int pal = 0; pal < 32; pal++) {
                    printf("  %02X:", pal);
                    for (int c = 0; c < 16; c++) printf(" %03X", pce.vce.pal[pal * 16 + c]);
                    printf("\n");
                }
            }
        if (vram_dump_at == f + 1) {
            FILE *vf = fopen(vram_dump_path, "wb");
            if (vf) { fwrite(pce.vdc.vram, 2, 0x8000, vf); fclose(vf); }
        }
        if (adpcm_dump_at == f + 1) {
            FILE *af = fopen(adpcm_dump_path, "wb");
            if (af) { fwrite(pce.adpcm, 1, sizeof pce.adpcm, af); fclose(af); }
        }
        for (int i = 0; i < n_mem; i++)
            if (mem_at[i].frame == f + 1) {
                /* 256 pages of 8 KiB (zeros where unmapped), then the 8 MPRs */
                FILE *mf = fopen(mem_at[i].path, "wb");
                static const uint8_t zero[0x2000];
                if (mf) {
                    for (int b = 0; b < 256; b++) fwrite(pce.page[b] ? pce.page[b] : zero, 1, 0x2000, mf);
                    fwrite(pce.cpu.mpr, 1, 8, mf);
                    fclose(mf);
                    printf("memory dumped at frame %ld: %s\n", f + 1, mem_at[i].path);
                }
            }
        for (int i = 0; i < n_save_at; i++)
            if (save_at[i].frame == f + 1 && pce_state_save(&pce, save_at[i].path, 0, err, sizeof err))
                printf("state saved at frame %ld: %s\n", f + 1, save_at[i].path);
        if ((shot_every > 0 && (f + 1) % shot_every == 0) || f + 1 == start_frame + frames) {
            char path[512];
            snprintf(path, sizeof path, "%s/frame_%05ld.png", out, f + 1);
            if (!png_write_argb(path, pce.vdc.fb, pce.vdc.fb_w, pce.vdc.fb_h, PCE_FB_MAX_W))
                fprintf(stderr, "cannot write %s (does the directory exist?)\n", path);
            HuC6280 *c = &pce.cpu;
            printf("frame %5ld  PC=%02X:%04X  CR=%04X MWR=%04X SATB=%04X  %dx%d  irq_m=%02X  vec1=%02X%02X  io?=%u\n",
                   f + 1, c->mpr[c->pc >> 13], c->pc, pce.vdc.reg[5], pce.vdc.reg[9], pce.vdc.reg[0x13], pce.vdc.fb_w, pce.vdc.fb_h,
                   pce.wram[0xF5], pce.wram[0x203], pce.wram[0x202], pce.unhandled_io);
            fflush(stdout);
        }
    }
    if (wav) {
        uint32_t data = wav_frames * 4;
        uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                         16, 0, 0, 0, 1, 0, 2, 0, 0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0, 4, 0, 16, 0,
                         'd', 'a', 't', 'a', 0, 0, 0, 0};
        uint32_t riff = data + 36;
        memcpy(h + 4, &riff, 4);
        memcpy(h + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    {
        const RecompStats *rs = recomp_stats();
        uint64_t tot = rs->native_cycles + rs->interp_cycles;
        printf("\nRecompiled code: %s, %d regions; %.2f%% of CPU cycles native; %llu region calls; "
               "%u entry points without a region",
               recomp_build_id, recomp_chunk_count, tot ? 100.0 * (double)rs->native_cycles / (double)tot : 0.0,
               (unsigned long long)rs->calls, rs->miss_entries);
        if (verify) printf("; verified %llu, failures %llu", (unsigned long long)rs->verify_runs,
                           (unsigned long long)rs->verify_failures);
        printf("\n");
        if (seeds_path && recomp_write_misses(seeds_path)) printf("Seeds written to %s\n", seeds_path);
        if (misses_path && recomp_write_misses(misses_path)) printf("Misses written to %s\n", misses_path);
    }
    if (skip_cine) printf("Cinematic frames run with input ignored: %ld\n", skipped);
    if (pce.frame > (uint64_t)start_frame)
        printf("Master clock: %.1f cycles a frame over this run (nominal %d)\n",
               (double)(pce.master - start_master) / (double)(pce.frame - (uint64_t)start_frame),
               PCE_MASTER_PER_LINE * PCE_LINES_PER_FRAME);
    printf("Sound: %llu PSG register writes; PSG audible for %.1f s, ADPCM for %.1f s\n",
           (unsigned long long)pce.snd.psg_writes, (double)pce.snd.psg_samples / SND_RATE,
           (double)pce.snd.adpcm_samples / SND_RATE);
    printf("\nBIOS calls:");
    for (int i = 0; i < PCE_BIOS_ENTRIES; i++)
        if (pce.bios_calls[i]) printf(" %s x%u", bios_entry_name(i), pce.bios_calls[i]);
    printf("\nInterrupts taken: IRQ2 %u, IRQ1 %u, timer %u; timer %s reload %u; irq_disable %X\n",
           pce.irq_taken[0], pce.irq_taken[1], pce.irq_taken[2], pce.timer_on ? "on" : "off",
           pce.timer_reload, pce.irq_disable);
    if (vw_count) {
        printf("VRAM writes to %04X-%04X by instruction:\n", vw_lo, vw_hi);
        for (int k = 0; k < 24; k++) {
            uint32_t best = 0, at = 0;
            for (uint32_t i = 0; i < (1u << 24); i++)
                if (vw_count[i] > best) { best = vw_count[i]; at = i; }
            if (!best) break;
            printf("  %02X:%04X  %u\n", at >> 16, at & 0xFFFF, best);
            vw_count[at] = 0;
        }
    }
    if (prof) {
        printf("Hottest PCs outside the idle loop:\n");
        for (int k = 0; k < 16; k++) {
            uint32_t best = 0, at = 0;
            for (uint32_t i = 0; i < (1u << 24); i++)
                if (prof[i] > best && (i & 0xFFFF) != 0x4174) { best = prof[i]; at = i; }
            if (!best) break;
            printf("  %02X:%04X  %u\n", at >> 16, at & 0xFFFF, best);
            prof[at] = 0;
        }
    }
    return 0;
}
