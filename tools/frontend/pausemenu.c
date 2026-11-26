/* See pausemenu.h. */
#include "pausemenu.h"

#include <string.h>

#include "game.h"
#include "pce/disc.h"

/* ---- the game's letters and heart, from the disc ------------------------------------- */

/* The GAME OVER continue screen loads its letters from a data sector (USA disc: sector 89
 * of the data track, into bank $78) and unpacks them to VRAM $1100 with 6C:81CD: a stream of
 * runs of 16-bit words, ended by $FF:
 *   $00 n hi, then n bytes      n words, each a byte and the high byte hi
 *   $01-$7F lo hi               that many words lo hi
 *   $80-$FE (but $FD, $FE)      the low 7 bits' words, each lo hi
 * The 49 tiles it makes ($110-$140): letters at $117-$128 in colours 9-12. The heart cursor
 * is a 16 x 16 sprite pattern stored as it is (USA disc: sector 526). Both are looked for
 * again if not where the USA disc has them (another disc), and checked by their shape. */
#define FONT_SECTOR 89
#define HEART_SECTOR 526
#define FONT_WORDS 784
#define SCAN_SECTORS 4000
static const char LETTERS[] = "CONTISAVEPLYU&DRHG";   /* tiles $117 on */

static uint16_t font[FONT_WORDS];
static uint16_t heart[64];
static int font_state, heart_state;   /* 0 not tried, 1 loaded, -1 not found */
static const Disc *font_disc;

static int unpack(const uint8_t *d, int n, uint16_t *out, int max)
{
    int i = 0, w = 0;
    while (i < n) {
        uint8_t b = d[i];
        if (b == 0xFF) return w;
        if (b == 0xFE || b == 0xFD) return -1;
        if (b == 0) {
            if (i + 3 > n) return -1;
            int c = d[i + 1], hi = d[i + 2];
            i += 3;
            for (int k = 0; k < c; k++, i++) {
                if (i >= n || w >= max) return -1;
                out[w++] = (uint16_t)(d[i] | hi << 8);
            }
        } else if (b & 0x80) {
            int c = b & 0x7F;
            i++;
            for (int k = 0; k < c; k++, i += 2) {
                if (i + 2 > n || w >= max) return -1;
                out[w++] = (uint16_t)(d[i] | d[i + 1] << 8);
            }
        } else {
            if (i + 3 > n) return -1;
            for (int k = 0; k < b; k++) {
                if (w >= max) return -1;
                out[w++] = (uint16_t)(d[i + 1] | d[i + 2] << 8);
            }
            i += 3;
        }
    }
    return -1;
}

static int tile_pixel(const uint16_t *t, int x, int y)
{
    int b = 7 - x;
    return ((t[y] >> b) & 1) | ((t[y] >> (8 + b)) & 1) << 1 | ((t[8 + y] >> b) & 1) << 2 | ((t[8 + y] >> (8 + b)) & 1) << 3;
}

/* the continue screen's tiles: the first blank, the letters in colours 9-12 only, each drawn */
static bool font_ok(const uint16_t *f)
{
    for (int i = 0; i < 16; i++)
        if (f[i]) return false;
    for (int l = 0; l < (int)sizeof LETTERS - 1; l++) {
        const uint16_t *t = &f[(7 + l) * 16];
        int drawn = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int c = tile_pixel(t, x, y);
                if (c && (c < 9 || c > 12)) return false;
                drawn += c != 0;
            }
        if (drawn < 8) return false;
    }
    return true;
}

static int heart_pixel(const uint16_t *h, int x, int y)
{
    int b = 15 - x, c = 0;
    for (int pl = 0; pl < 4; pl++) c |= ((h[pl * 16 + y] >> b) & 1) << pl;
    return c;
}

/* the heart: drawn in rows 7-14 only, in colours 1-3, 14 and 15 */
static bool heart_ok(const uint16_t *h)
{
    int drawn = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int c = heart_pixel(h, x, y);
            if (!c) continue;
            if (y < 7 || y > 14 || (c > 3 && c < 14)) return false;
            drawn++;
        }
    return drawn >= 30 && drawn <= 70;
}

static const DiscTrack *data_track(const Disc *d)
{
    for (int i = 0; i < d->n_tracks; i++)
        if (d->tracks[i].data) return &d->tracks[i];
    return NULL;
}

static bool try_font(const Disc *d, int lba)
{
    uint8_t sec[2048];
    uint16_t f[FONT_WORDS + 1];
    if (!disc_read_data(d, lba, 1, sec)) return false;
    if (unpack(sec, (int)sizeof sec, f, FONT_WORDS + 1) != FONT_WORDS || !font_ok(f)) return false;
    memcpy(font, f, sizeof font);
    return true;
}

static bool try_heart(const Disc *d, int lba)
{
    uint8_t sec[2048];
    if (!disc_read_data(d, lba, 1, sec)) return false;
    uint16_t h[64];
    for (int i = 0; i < 64; i++) h[i] = (uint16_t)(sec[2 * i] | sec[2 * i + 1] << 8);
    if (!heart_ok(h)) return false;
    memcpy(heart, h, sizeof heart);
    return true;
}

static void load(const Pce *p)
{
    const Disc *d = &p->disc;
    if (font_disc != d) font_state = heart_state = 0;
    font_disc = d;
    const DiscTrack *t = data_track(d);
    if (!t) {
        font_state = heart_state = -1;
        return;
    }
    if (!font_state) {
        font_state = -1;
        if (try_font(d, t->lba + FONT_SECTOR)) font_state = 1;
        for (int s = 0; s < SCAN_SECTORS && s < t->sectors && font_state < 0; s++)
            if (s != FONT_SECTOR && try_font(d, t->lba + s)) font_state = 1;
    }
    if (!heart_state) {
        heart_state = -1;
        if (try_heart(d, t->lba + HEART_SECTOR)) heart_state = 1;
        for (int s = 0; s < SCAN_SECTORS && s < t->sectors && heart_state < 0; s++)
            if (s != HEART_SECTOR && try_heart(d, t->lba + s)) heart_state = 1;
    }
}

/* ---- the menu ------------------------------------------------------------------------- */

#define PAUSED 0x02E1        /* 1 while the game is on its pause screen */
#define MENU_AFTER 4         /* frames paused before the menu comes up (a save state loaded
                              * mid-frame can read paused for a frame or two) */

/* PLAYER CHANGE, as the GAME OVER screen's (69:538D, then 5393 and 51F0) but for its
 * continue's resetting of the score and lives (54C9): the other character, the stage
 * started again from its start. The game does it itself: for one frame main state 5's entry
 * in the main loop's table (69:510E, JMP (,X) at 69:510B) is pointed at this routine in our
 * bank $00 (RTS filler, mapped at $E000 throughout), which then starts the stage. */
#define CHANGE_CODE 0x1B00        /* bank $00 offset ($FB00) */
#define CHANGE_DONE 0x1BF0        /* ... set to 1 by it once run ($FBF0) */
#define STATE5_ENTRY 0x1118       /* bank $69 offset: state 5's entry (69:510E + 2 * 5) */
static const uint8_t CHANGE[] = {
    0xA5, 0x9F, 0x49, 0x01, 0x85, 0x9F,   /* LDA $9F / EOR #1 / STA $9F   the other character */
    0xA5, 0x81, 0x10, 0x04,               /* LDA $81 / BPL +4             the stage's step ...  */
    0xA5, 0xA1, 0x29, 0x7F,               /* LDA $A1 / AND #$7F                                  */
    0x09, 0x40, 0x85, 0x81,               /* ORA #$40 / STA $81           ... from its start      */
    0xC9, 0x43, 0xD0, 0x04,               /* CMP #$43 / BNE +4                                   */
    0xA9, 0x42, 0x85, 0x81,               /* LDA #$42 / STA $81                                  */
    0x20, 0xA6, 0x6C,                     /* JSR $6CA6                    the stage's state reset */
    0x9C, 0xE1, 0x22,                     /* STZ $22E1                    not paused              */
    0xA9, 0x05, 0x85, 0x30,               /* LDA #5 / STA $30             playing, ...            */
    0x64, 0x37, 0x64, 0x31, 0x64, 0x38,   /* STZ $37 / STZ $31 / STZ $38  ... from the start      */
    0xA9, 0x01, 0x8D, 0xF0, 0xFB,         /* LDA #1 / STA $FBF0           done                    */
    0x60,                                 /* RTS                                                  */
};
static const uint8_t STATE5[2] = {0x65, 0x52};   /* $5265 */

static bool maria_unlocked(const Pce *p) { return (p->wram[0x1F40] & 1) != 0; }

/* the menu's lines: the GAME OVER screen's, PLAYER CHANGE with Maria unlocked */
static int lines(const Pce *p) { return maria_unlocked(p) ? 3 : 2; }

/* put the change in (on) or take it out; false if the game's code is not as expected */
static bool change_patch(Pce *p, bool on)
{
    uint8_t *z = p->page[0x00], *g = p->page[0x69];
    if (!z || !g) return false;
    uint8_t *e = g + STATE5_ENTRY;
    if (!on) {
        if (e[0] == (uint8_t)(0xE000 + CHANGE_CODE) && e[1] == (uint8_t)((0xE000 + CHANGE_CODE) >> 8)) {
            memcpy(e, STATE5, 2);
            p->cpu.page_gen[0x69]++;
        }
        return true;
    }
    if (memcmp(e, STATE5, 2) || g[0x110B] != 0x7C || g[0x110C] != 0x0E || g[0x110D] != 0x51) return false;   /* (JMP ($510E,X) at 69:510B) */
    uint8_t *c = z + CHANGE_CODE;
    for (size_t i = 0; i < sizeof CHANGE; i++)
        if (c[i] != 0x60 && c[i] != CHANGE[i]) return false;   /* (not our free space) */
    memcpy(c, CHANGE, sizeof CHANGE);
    z[CHANGE_DONE] = 0;
    e[0] = (uint8_t)(0xE000 + CHANGE_CODE);
    e[1] = (uint8_t)((0xE000 + CHANGE_CODE) >> 8);
    p->cpu.page_gen[0x00]++;
    p->cpu.page_gen[0x69]++;
    return true;
}

bool pause_menu_game_paused(const Pce *p)
{
    return game_playing(p) && p->wram[RONDO_SUB] == 0x01 && p->wram[PAUSED] == 1;
}

uint8_t pause_menu_pad(PauseMenu *m, Pce *p, uint8_t pad, bool live)
{
    /* PLAYER CHANGE under way: the table put back once the game has run it */
    if (m->change) {
        uint8_t *z = p->page[0x00];
        if ((z && z[CHANGE_DONE] == 1) || ++m->change > 10) {
            change_patch(p, false);
            if (z) z[CHANGE_DONE] = 0x60;
            m->change = 0;
        } else {
            return 0;
        }
    }
    if (!live || !pause_menu_game_paused(p)) {
        m->paused_frames = 0;
        m->active = false;
        m->sel = 0;
        m->prev = pad;
        return pad;
    }
    if (m->paused_frames < MENU_AFTER) m->paused_frames++;
    if (m->paused_frames < MENU_AFTER) {
        m->prev = pad;   /* (the RUN that paused, still held, is no choice) */
        return pad;
    }
    m->active = true;
    uint8_t press = (uint8_t)(pad & ~m->prev);
    m->prev = pad;
    int n = lines(p);
    if (m->sel >= n) m->sel = 0;
    if (press & (PAD_DOWN | PAD_SELECT)) m->sel = (m->sel + 1) % n;
    if (press & PAD_UP) m->sel = (m->sel + n - 1) % n;
    uint8_t out = (uint8_t)(pad & ~(PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_I | PAD_II | PAD_SELECT | PAD_RUN));
    if (press & (PAD_RUN | PAD_I)) {
        if (m->sel == 0) out |= PAD_RUN;   /* CONTINUE: the game's own end of its pause */
        else if (m->sel == 1) m->want_exit = true;
        else if (change_patch(p, true)) {
            m->change = 1;
            m->active = false;
        }
    }
    return out;
}

/* colours (9-bit GRB, as the VCE takes them): the continue screen's palettes 0 (the chosen
 * line, red) and 1 (white) for the letters' colours 9-12, and its sprite palette 3 for the
 * heart's 1-3, 14 and 15 */
static const uint16_t RED[4] = {0x010, 0x020, 0x038, 0x078};
static const uint16_t WHITE[4] = {0x049, 0x092, 0x124, 0x1FF};
static const uint16_t HEART[16] = {0x000, 0x000, 0x060, 0x0FB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x028, 0x1FF};

static uint32_t argb(uint16_t c)
{
    uint32_t b = c & 7, r = (c >> 3) & 7, g = (c >> 6) & 7;
    return 0xFF000000u | (r * 255 / 7) << 16 | (g * 255 / 7) << 8 | (b * 255 / 7);
}

static void put(Pce *p, int x, int y, uint32_t c)
{
    Vdc *d = &p->vdc;
    int left = (d->fb_w - 256) / 2;   /* (centred in the whole picture, wide or not) */
    int fx = left + x;
    if (y < 0 || y >= d->fb_h || fx < 0 || fx >= d->fb_w) return;
    d->fb[y * PCE_FB_MAX_W + fx] = c;
}

static void text(Pce *p, int x, int y, const char *s, const uint16_t *pal)
{
    for (; *s; s++, x += 8) {
        const char *at = *s == ' ' ? NULL : strchr(LETTERS, *s);
        if (!at) continue;
        const uint16_t *t = &font[(7 + (int)(at - LETTERS)) * 16];
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++) {
                int c = tile_pixel(t, xx, yy);
                if (c >= 9 && c <= 12) put(p, x + xx, y + yy, argb(pal[c - 9]));
            }
    }
}

bool pause_menu_draw(PauseMenu *m, Pce *p)
{
    if (!m->active) return true;
    load(p);
    if (font_state != 1) return false;
    /* a black panel in the middle of the game's screen, the two lines as on the continue
     * screen (16 px apart), the heart beside the chosen one */
    static const char *const LINES[3] = {"CONTINUE", "SAVE & END", "PLAYER CHANGE"};
    int n = lines(p);
    int w = n == 3 ? 160 : 128, h = 16 * n + 16, x0 = (256 - w) / 2, y0 = 112 - h / 2;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) put(p, x, y, 0xFF000000u);
    int tx = x0 + 34, ty = y0 + 12;
    for (int i = 0; i < n; i++) text(p, tx, ty + i * 16, LINES[i], i == m->sel ? RED : WHITE);
    if (heart_state == 1) {
        int hx = tx - 22, hy = ty + m->sel * 16 - 7;   /* (its drawn rows 7-14 on the line's 0-7) */
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                int c = heart_pixel(heart, x, y);
                if (c) put(p, hx + x, hy + y, argb(HEART[c]));
            }
    }
    return true;
}
