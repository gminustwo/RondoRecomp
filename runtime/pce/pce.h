/* PC Engine Super CD-ROM² system: memory map, I/O, timer, interrupts, frame loop.
 * System Card functions are replaced by C implementations (bios_hle.c). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cpu/huc6280.h"
#include "disc.h"
#include "sound.h"
#include "video.h"

#define PCE_MASTER_PER_LINE 1365
#define PCE_BIOS_ENTRIES 81

enum {  /* pad buttons, active high */
    PAD_I = 0x01, PAD_II = 0x02, PAD_SELECT = 0x04, PAD_RUN = 0x08,
    PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80,
};

typedef struct Pce {
    HuC6280 cpu;
    Vdc vdc;
    Vce vce;
    Sound snd;                  /* PSG + ADPCM */
    Disc disc;

    uint8_t *page[256];         /* physical 8 KiB pages; NULL = unmapped */
    bool page_ro[256];
    uint8_t rom[0x2000];        /* bank $00: interrupt stubs + bank info */
    uint8_t wram[0x2000];       /* $F8 */
    uint8_t bram[0x2000];       /* $F7 (2 KiB used) */
    uint8_t cdram[0x10000];     /* $80-$87 */
    uint8_t superram[0x30000];  /* $68-$7F */
    uint8_t adpcm[0x10000];
    uint8_t *real_bios;         /* diagnostics: original System Card mapped, no HLE */

    uint8_t io_buffer;
    uint8_t irq_disable;        /* $1402: bit0 IRQ2, bit1 IRQ1, bit2 timer */
    bool timer_irq, timer_on;
    uint8_t timer_reload, timer_count;
    int timer_clock;

    uint8_t joy_sel, joy_clr;
    uint8_t pad;                /* PAD_* currently held */

    int cd_base[2];             /* LBA bases set by CD_BASE */
    uint8_t mwr_shadow;
    bool bram_dirty;            /* save data changed since last written to disk */

    /* CD-DA playback (driven by CD_PLAY / CD_SEARCH / CD_PAUSE / CD_FADE) */
    struct {
        bool playing;
        bool repeat;
        int start_lba, end_lba; /* end exclusive */
        int64_t pos;            /* current position in stereo samples from LBA 0 */
        int volume;             /* 0..256 */
        int fade_step;          /* volume change per output sample block, 0 = none */
    } cdda;
    bool mute_cdda;             /* diagnostics: leave CD audio out of the mix */
    uint16_t ad_last_start;     /* AD_PLAY parameters, reused when replaying */
    uint16_t ad_last_len;
    uint8_t ad_last_rate;

    uint64_t master;            /* master clock (21.477 MHz) */
    uint64_t frame;
    int line;
    bool at_entry;              /* next instruction is a dispatch point */

    /* diagnostics */
    bool log_bios, log_bios_all;
    uint32_t bios_calls[PCE_BIOS_ENTRIES];
    uint32_t unhandled_io;
    uint32_t irq_taken[3];      /* IRQ2, IRQ1, timer */
    uint32_t joy_reads;
    void (*on_step)(struct Pce *p, void *ctx);   /* optional per-instruction hook */
    void (*on_entry)(struct Pce *p, void *ctx);  /* optional: at each dispatch point */
    void *on_entry_ctx;
    void *on_step_ctx;
} Pce;

/* `cue`: the disc image, a .cue or a .chd (disc.h). Close a machine started before
 * starting it again. */
bool pce_init(Pce *p, const char *cue, char *err, int err_len);
/* Closes its disc (a .chd stays open while the machine runs). */
void pce_close(Pce *p);
/* Diagnostics only: map a real System Card image (banks $00-$1F) and disable the
 * C replacements, so the original reset code can be observed. */
bool pce_map_real_bios(Pce *p, const char *path, char *err, int err_len);
/* Do what the System Card does before handing over to the disc: load the IPL and jump. */
bool pce_boot(Pce *p, char *err, int err_len);
void pce_run_frame(Pce *p);
/* Calls a game routine (JSR-like) with A set and interrupts off, until it returns.
 * Used by the code generator to run the game's own overlay loader. */
bool pce_call(Pce *p, uint16_t addr, uint8_t a, long max_instructions);
/* Mix `frames` stereo 44.1 kHz samples of audio output into out[frames*2]. */
void pce_audio_render(Pce *p, int16_t *out, int frames);

/* logical-address helpers through the current MPRs */
uint8_t pce_read8(Pce *p, uint16_t addr);
void pce_write8(Pce *p, uint16_t addr, uint8_t v);

/* bram.c: backup RAM file store (System Card format) */
bool bram_formatted(const Pce *p);
void bram_format(Pce *p);
int bram_free(const Pce *p);
int bram_read(Pce *p, const uint8_t name[12], uint8_t *out, int len, int offset, int *done);
int bram_write(Pce *p, const uint8_t name[12], const uint8_t *in, int len, int offset);
int bram_delete(Pce *p, const uint8_t name[12]);
int bram_file_name(const Pce *p, int index, uint8_t name[12]);

/* bios_hle.c */
int bios_hle_call(Pce *p, int index);
const char *bios_entry_name(int index);
