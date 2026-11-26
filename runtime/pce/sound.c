/* PC Engine PSG + ADPCM. See sound.h. */
#include "sound.h"

#include "disc.h"

#include <math.h>
#include <string.h>

#define MASTER_HZ 21477270.0
#define PSG_HZ (MASTER_HZ / 6.0)              /* 3.58 MHz */
#define PSG_PER_SAMPLE (PSG_HZ / SND_RATE)
#define PSG_GAIN 300.0f                        /* per channel, per step of the 5-bit wave */
#define ADPCM_GAIN 8                           /* 12-bit full scale -> 16-bit full scale, at half
                                                * the CD audio's level (as MAME's PC Engine CD mixes it) */

/* ---- PSG ------------------------------------------------------------------------- */

/* Band-limiting. A tone channel is a stepped wave (32 steps a cycle). The hardware's output
 * reaches the speakers through analog filtering, so only the note's harmonics are heard.
 * Taking the step under each 44.1 kHz output sample instead folds everything above 22 kHz
 * back into the audible band as tones that are not the note's: in the game's chip music
 * (the boss and pre-boss rooms, the stage clear, death and game over jingles) 11-27 dB
 * under the notes, which made them sound sour and out of tune (issue #8). So each output
 * sample is the stepped wave low-pass filtered: a windowed sinc (cutoff 20 kHz, Kaiser
 * window, beta 8, BL_TAPS output samples long; flat to 15 kHz, -1 dB at 18 kHz, -50 dB at
 * 24 kHz, -80 dB from 26 kHz) applied to the wave as it has played. That needs no state of
 * its own (nothing new in save states): the wave's recent past follows from where it is now
 * (phase) and how fast it moves (period). Any alias left is 88 dB or more under the notes up
 * to 16 kHz. The tone channels come out BL_TAPS / 2 samples (0.27 ms) late, all alike. */
#define BL_TAPS 24                    /* the filter's length, in output samples */
#define BL_RES 256                    /* table entries per output sample */
#define BL_CUTOFF (20000.0 / SND_RATE)
#define BL_BETA 8.0
/* Tones above this many wave steps an output sample (period under 27: above 4.1 kHz, sound
 * effects only) are averaged over their output sample instead, which costs a step at most
 * per step played rather than one per step in the filter's length. */
#define BL_MAX_STEPS 3.0
#define BL_PI 3.14159265358979323846

static float bl_step[BL_TAPS * BL_RES + 1];   /* the filter's integral from its start: 0 .. 1 */

static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 60 && term > 1e-12 * sum; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

static void bl_init(void)
{
    static bool done;
    if (done) return;
    static double cum[BL_TAPS * BL_RES + 1];
    double prev = 0, sum = 0;
    for (int i = 0; i <= BL_TAPS * BL_RES; i++) {
        double tau = (double)i / BL_RES, x = tau - BL_TAPS / 2.0, r = 2.0 * tau / BL_TAPS - 1.0;
        double sinc = fabs(x) < 1e-12 ? 2.0 * BL_CUTOFF : sin(2.0 * BL_PI * BL_CUTOFF * x) / (BL_PI * x);
        double h = sinc * bessel_i0(BL_BETA * sqrt(fmax(0.0, 1.0 - r * r))) / bessel_i0(BL_BETA);
        if (i) sum += (prev + h) * 0.5 / BL_RES;
        cum[i] = sum;
        prev = h;
    }
    for (int i = 0; i <= BL_TAPS * BL_RES; i++) bl_step[i] = (float)(cum[i] / sum);
    done = true;
}

/* the filter's integral up to x output samples into it */
static inline float bl_at(double x)
{
    if (x >= BL_TAPS) return 1.0f;
    double p = x * BL_RES;
    int i = (int)p;
    float f = (float)(p - i);
    return bl_step[i] + (bl_step[i + 1] - bl_step[i]) * f;
}

/* the wave (centered on 0) summed over steps a .. b of its play (a < b; before 0 is earlier
 * cycles) */
static double wave_area(const PsgChannel *c, double a, double b)
{
    double acc = 0;
    for (double x = a; x < b;) {
        double step = floor(x), next = step + 1 < b ? step + 1 : b;
        acc += (double)(c->wave[(int)step & 31] - 16) * (next - x);
        x = next;
    }
    return acc;
}

/* A tone channel's output sample, `d` wave steps an output sample: its steps, each weighted
 * by how much of the filter it fell in, back from the current one. */
static float tone_sample(const PsgChannel *c, double d)
{
    if (d > BL_MAX_STEPS)   /* the output sample BL_TAPS / 2 back, averaged */
        return (float)(wave_area(c, c->phase - d * (BL_TAPS / 2 + 0.5), c->phase - d * (BL_TAPS / 2 - 0.5)) / d);
    int k = (int)c->phase;
    double to = (c->phase - k) / d, per_step = 1.0 / d;   /* where the current step began, in output samples back */
    float acc = 0, prev = 0;
    for (int i = k;; i--) {
        float cur = bl_at(to);
        acc += (float)(c->wave[i & 31] - 16) * (cur - prev);
        if (to >= BL_TAPS) break;
        prev = cur;
        to += per_step;
    }
    return acc;
}

/* Volume steps are 1.5 dB (5-bit channel volume) and 3 dB (4-bit balances); a 0 mutes. */
static float level(int vol, int bal, int global_bal)
{
    if (!vol || !bal || !global_bal) return 0.0f;
    double db = (31 - vol) * 1.5 + (15 - bal) * 3.0 + (15 - global_bal) * 3.0;
    return (float)pow(10.0, -db / 20.0);
}

static void update_amp(Sound *s, PsgChannel *c)
{
    int vol = c->control & 0x1F;
    c->amp_l = level(vol, c->balance >> 4, s->global_balance >> 4);
    c->amp_r = level(vol, c->balance & 15, s->global_balance & 15);
}

void sound_reset(Sound *s, const uint8_t *adpcm_ram, const struct Disc *disc)
{
    memset(s, 0, sizeof *s);
    s->ram = adpcm_ram;
    s->disc = disc;
    s->ad_sec_lba = -1;
    for (int i = 0; i < 6; i++) s->ch[i].lfsr = 1;
    s->ad_volume = 256;
}

void sound_psg_write(Sound *s, uint64_t now, uint8_t reg, uint8_t v)
{
    sound_advance(s, now);
    s->psg_writes++;
    PsgChannel *c = s->select < 6 ? &s->ch[s->select] : NULL;
    switch (reg & 15) {
    case 0: s->select = v & 7; break;
    case 1:
        s->global_balance = v;
        for (int i = 0; i < 6; i++) update_amp(s, &s->ch[i]);
        break;
    case 2: if (c) c->freq = (uint16_t)((c->freq & 0xF00) | v); break;
    case 3: if (c) c->freq = (uint16_t)((c->freq & 0x0FF) | (v & 0x0F) << 8); break;
    case 4:
        if (!c) break;
        if ((v & 0xC0) == 0x40) c->wave_index = 0;       /* off + DDA: reset the wave write index */
        c->control = v;
        update_amp(s, c);
        break;
    case 5: if (c) { c->balance = v; update_amp(s, c); } break;
    case 6:
        if (!c) break;
        if (c->control & 0x40) {
            c->dda = v & 0x1F;
        } else {
            c->wave[c->wave_index] = v & 0x1F;
            c->wave_index = (c->wave_index + 1) & 31;
        }
        break;
    case 7: if (c && s->select >= 4) c->noise = v; break;
    case 8: s->lfo_freq = v; break;
    case 9:
        s->lfo_ctrl = v;
        if (v & 3) s->lfo_used = true;
        if (v & 0x80) s->ch[1].phase = 0;                /* LFO reset */
        break;
    }
}

static float psg_channel(PsgChannel *c, int index, double period_override)
{
    if (!(c->control & 0x80)) return 0.0f;
    int sample;
    if (c->control & 0x40) {
        sample = c->dda;
    } else if (index >= 4 && (c->noise & 0x80)) {
        double period = (double)(((c->noise & 0x1F) ^ 0x1F) + 1) * 64.0;
        c->noise_clock += PSG_PER_SAMPLE;
        while (c->noise_clock >= period) {
            c->noise_clock -= period;
            uint32_t bit = ((c->lfsr >> 0) ^ (c->lfsr >> 1) ^ (c->lfsr >> 11) ^ (c->lfsr >> 12) ^ (c->lfsr >> 17)) & 1;
            c->lfsr = (c->lfsr >> 1) | bit << 17;
        }
        sample = (c->lfsr & 1) ? 0x1F : 0x00;
    } else {
        double period = period_override > 0 ? period_override : (c->freq ? c->freq : 4096);
        if (period < 8) return 0.0f;                     /* above audible range: silent */
        double d = PSG_PER_SAMPLE / period;               /* wave steps an output sample */
        c->phase += d;
        if (c->phase >= 32.0) c->phase = fmod(c->phase, 32.0);
        return tone_sample(c, d);                         /* band-limited (above) */
    }
    return (float)(sample - 16);
}

/* ---- ADPCM (OKI MSM5205, each byte's high nibble first) ----------------------------
 * The order is the hardware's (MAME's PC Engine CD plays bits 4-7, then 0-3). Played the
 * other way round the game's clips decode as a slow random walk that keeps hitting the
 * 12-bit limits (Death's first line in the Prologue: 92 times); this way none do, and they
 * have speech's spectrum and pitch. */

static const int16_t step_table[49] = {
    16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157,
    173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552,
};
static const int8_t index_adjust[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static uint8_t adpcm_byte(Sound *s)
{
    if (!s->ad_stream) return s->ram[s->ad_addr];
    if (s->ad_sec_lba != s->ad_stream_lba) {
        if (!s->disc || !disc_read_data(s->disc, s->ad_stream_lba, 1, s->ad_sec)) memset(s->ad_sec, 0x88, sizeof s->ad_sec);
        s->ad_sec_lba = s->ad_stream_lba;
    }
    return s->ad_sec[s->ad_stream_off];
}

static void adpcm_advance_byte(Sound *s)
{
    s->ad_left--;
    if (!s->ad_stream) { s->ad_addr++; return; }
    if (++s->ad_stream_off == 2048) { s->ad_stream_off = 0; s->ad_stream_lba++; }
}

static int adpcm_next(Sound *s)
{
    uint8_t b = adpcm_byte(s);
    int n = s->ad_low ? b & 15 : b >> 4;
    if (s->ad_low) adpcm_advance_byte(s);
    s->ad_low = !s->ad_low;
    int st = step_table[s->ad_index];
    int diff = st >> 3;
    if (n & 1) diff += st >> 2;
    if (n & 2) diff += st >> 1;
    if (n & 4) diff += st;
    if (n & 8) diff = -diff;
    s->ad_signal += diff;
    if (s->ad_signal > 2047) s->ad_signal = 2047;
    if (s->ad_signal < -2048) s->ad_signal = -2048;
    s->ad_index += index_adjust[n & 7];
    if (s->ad_index < 0) s->ad_index = 0;
    if (s->ad_index > 48) s->ad_index = 48;
    return s->ad_signal;
}

static void adpcm_restart(Sound *s)
{
    s->ad_addr = s->ad_start;
    s->ad_left = s->ad_len;
    s->ad_low = false;
    s->ad_signal = 0;
    s->ad_index = 0;
    s->ad_prev = s->ad_cur = 0;
    s->ad_phase = 0;
}

void sound_adpcm_play(Sound *s, uint64_t now, uint16_t start, uint32_t len, uint8_t rate, bool repeat)
{
    sound_advance(s, now);
    s->ad_stream = false;
    s->ad_start = start;
    s->ad_len = len;
    s->ad_rate = rate & 15;
    s->ad_repeat = repeat;
    adpcm_restart(s);
    s->ad_playing = len > 0;
}

void sound_adpcm_stop(Sound *s, uint64_t now)
{
    sound_advance(s, now);
    s->ad_playing = false;
}

void sound_adpcm_stream(Sound *s, uint64_t now, int lba, uint32_t bytes, uint8_t rate)
{
    sound_advance(s, now);
    s->ad_stream = true;
    s->ad_stream_lba = lba;
    s->ad_stream_off = 0;
    s->ad_sec_lba = -1;
    s->ad_len = bytes;
    s->ad_rate = rate & 15;
    s->ad_repeat = false;
    s->ad_left = bytes;
    s->ad_low = false;
    s->ad_signal = 0;
    s->ad_index = 0;
    s->ad_prev = s->ad_cur = 0;
    s->ad_phase = 0;
    s->ad_playing = bytes > 0;
}

void sound_adpcm_fade(Sound *s, int step)
{
    s->ad_fade_step = step;
    if (!step) s->ad_volume = 256;
}

static int adpcm_sample(Sound *s)
{
    if (!s->ad_playing) return 0;
    double hz = 32000.0 / (16 - s->ad_rate);
    s->ad_phase += hz / SND_RATE;
    while (s->ad_phase >= 1.0) {
        s->ad_phase -= 1.0;
        if (s->ad_left == 0) {
            if (!s->ad_repeat) { s->ad_playing = false; return 0; }
            adpcm_restart(s);
        }
        s->ad_prev = s->ad_cur;
        s->ad_cur = adpcm_next(s);
    }
    return (int)(s->ad_prev + (s->ad_cur - s->ad_prev) * s->ad_phase);
}

/* ---- the host voices (sound_extra_play) ------------------------------------------- */
/* Two, mixed: a sound started while one plays takes the other (a yell over the thunder);
 * with both busy, the one started first gives way. */

typedef struct {
    const uint8_t *data;
    uint32_t len, pos;     /* bytes */
    bool low, on;
    int signal, index, prev, cur;
    double phase, hz;
    int volume, soften;
    float lp;
    uint32_t started;
} ExtraVoice;
static ExtraVoice xvs[2];
static uint32_t xv_count;

void sound_extra_play_at(const uint8_t *data, uint32_t len, uint8_t rate, int volume, int soften)
{
    ExtraVoice *xv = !xvs[0].on ? &xvs[0] : !xvs[1].on ? &xvs[1] : xvs[0].started < xvs[1].started ? &xvs[0] : &xvs[1];
    memset(xv, 0, sizeof *xv);
    xv->data = data;
    xv->len = len;
    xv->hz = 32000.0 / (16 - (rate & 15));
    xv->volume = volume;
    xv->soften = soften;
    xv->started = ++xv_count;
    xv->on = data && len;
}

void sound_extra_play(const uint8_t *data, uint32_t len, uint8_t rate) { sound_extra_play_at(data, len, rate, 256, 0); }

void sound_extra_stop(void) { xvs[0].on = xvs[1].on = false; }

static int voice_sample(ExtraVoice *xv)
{
    if (!xv->on) return 0;
    xv->phase += xv->hz / SND_RATE;
    while (xv->phase >= 1.0) {
        xv->phase -= 1.0;
        if (xv->pos >= xv->len) {
            xv->on = false;
            return 0;
        }
        uint8_t b = xv->data[xv->pos];
        int n = xv->low ? b & 15 : b >> 4;
        if (xv->low) xv->pos++;
        xv->low = !xv->low;
        int st = step_table[xv->index], diff = st >> 3;
        if (n & 1) diff += st >> 2;
        if (n & 2) diff += st >> 1;
        if (n & 4) diff += st;
        if (n & 8) diff = -diff;
        xv->signal += diff;
        if (xv->signal > 2047) xv->signal = 2047;
        if (xv->signal < -2048) xv->signal = -2048;
        xv->index += index_adjust[n & 7];
        if (xv->index < 0) xv->index = 0;
        if (xv->index > 48) xv->index = 48;
        xv->prev = xv->cur;
        xv->cur = xv->signal;
    }
    float v = (float)(xv->prev + (xv->cur - xv->prev) * xv->phase);
    xv->lp += (v - xv->lp) * (float)(256 - xv->soften) / 256.0f;
    return (int)(xv->lp * (float)xv->volume / 256.0f);
}

static int extra_sample(void) { return voice_sample(&xvs[0]) + voice_sample(&xvs[1]); }

/* ---- output ---------------------------------------------------------------------- */

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

void sound_advance(Sound *s, uint64_t now)
{
    if (now <= s->last_master) return;
    bl_init();
    s->frac += (double)(now - s->last_master) * SND_RATE / MASTER_HZ;
    s->last_master = now;
    int n = (int)s->frac;
    s->frac -= n;
    for (int i = 0; i < n; i++) {
        float l = 0, r = 0;
        double ch0_period = 0;
        bool lfo = (s->lfo_ctrl & 3) && !(s->lfo_ctrl & 0x80);
        if (lfo) {
            /* LFO: channel 1's waveform, clocked lfo_freq times slower, is added to channel 0's
             * period (x1, x16 or x256); channel 1 is not heard */
            static const int shift[4] = {0, 0, 4, 8};
            PsgChannel *m = &s->ch[1];
            double mp = (double)(m->freq ? m->freq : 4096) * (s->lfo_freq ? s->lfo_freq : 256);
            m->phase += PSG_PER_SAMPLE / mp;
            if (m->phase >= 32.0) m->phase = fmod(m->phase, 32.0);
            int mod = (int)m->wave[(int)m->phase & 31] - 16;
            ch0_period = (double)(s->ch[0].freq ? s->ch[0].freq : 4096) + (double)(mod * (1 << shift[s->lfo_ctrl & 3]));
            if (ch0_period < 1) ch0_period = 1;
        }
        for (int k = 0; k < 6; k++) {
            if (lfo && k == 1) continue;
            PsgChannel *c = &s->ch[k];
            float v = psg_channel(c, k, k == 0 ? ch0_period : 0);
            l += v * c->amp_l;
            r += v * c->amp_r;
        }
        int ad = adpcm_sample(s) * ADPCM_GAIN * s->ad_volume / 256;
        ad += extra_sample() * ADPCM_GAIN;
        if (l != 0 || r != 0) s->psg_samples++;
        if (s->ad_playing) s->adpcm_samples++;
        if (s->ad_fade_step && (i & 63) == 0) {
            s->ad_volume += s->ad_fade_step;
            if (s->ad_volume <= 0) { s->ad_volume = 0; s->ad_fade_step = 0; }
        }
        uint32_t at = (s->head + s->count) % SND_RING;
        if (s->count == SND_RING) {                  /* nobody is reading: drop the oldest */
            s->head = (s->head + 1) % SND_RING;
            s->count--;
            at = (s->head + s->count) % SND_RING;
        }
        s->ring[at * 2] = clamp16((int)(l * PSG_GAIN) + ad);
        s->ring[at * 2 + 1] = clamp16((int)(r * PSG_GAIN) + ad);
        s->count++;
    }
}

void sound_trim(Sound *s, uint32_t keep)
{
    if (s->count <= keep) return;
    s->head = (s->head + (s->count - keep)) % SND_RING;
    s->count = keep;
}

int sound_take_fit(Sound *s, int16_t *out, int n)
{
    if (n <= 0) return 0;
    if (s->count > (uint32_t)(n + n / 4)) sound_trim(s, (uint32_t)n);
    uint32_t m = s->count;
    if (m < (uint32_t)n / 2) return sound_take(s, out, n);   /* too little to stretch: as it is */
    for (int i = 0; i < n; i++) {
        double pos = (double)i * m / n;
        uint32_t j = (uint32_t)pos, j1 = j + 1 < m ? j + 1 : m - 1;
        double f = pos - j;
        const int16_t *a = &s->ring[((s->head + j) % SND_RING) * 2];
        const int16_t *b = &s->ring[((s->head + j1) % SND_RING) * 2];
        out[i * 2] = clamp16(out[i * 2] + (int)(a[0] + (b[0] - a[0]) * f));
        out[i * 2 + 1] = clamp16(out[i * 2 + 1] + (int)(a[1] + (b[1] - a[1]) * f));
    }
    s->head = (s->head + m) % SND_RING;
    s->count = 0;
    return n;
}

int sound_take(Sound *s, int16_t *out, int n)
{
    int taken = 0;
    while (taken < n && s->count) {
        out[taken * 2] = clamp16(out[taken * 2] + s->ring[s->head * 2]);
        out[taken * 2 + 1] = clamp16(out[taken * 2 + 1] + s->ring[s->head * 2 + 1]);
        s->head = (s->head + 1) % SND_RING;
        s->count--;
        taken++;
    }
    return taken;
}
