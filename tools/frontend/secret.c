/* The Konami code door in Stage 1's house and the hidden room behind it (secret.h). */
#include "secret.h"

#include <math.h>
#include <string.h>

#include "game.h"
#include "pce/disc.h"
#include "pce/sound.h"
#include "pce/video.h"

/* work RAM (offsets in bank $F8) */
#define W_EVENT 0x38        /* an object's request to the main loop: 3 leaves the room */
#define W_EXIT 0x8C         /* the way out: bit 7, walking off the room's edge */
#define W_STEP 0x82         /* the route step (the grid comes from it, the room from the route) */
#define W_ROOM 0x83
#define W_AREA 0xA2
#define W_CAM_PAGE 0x1E96   /* the camera's X page and X */
#define W_CAM_X 0x1E08
#define W_PX_HI 0x0B70      /* the player (object 0): X, Y (picture coordinates) */
#define W_PX 0x0BC8
#define W_PY_HI 0x0C78
#define W_PY 0x0CD0

/* Stage 1's first area (area $10), as the USA disc loads it */
#define AREA 0x10
#define HOUSE 1             /* the house's room, and its route step */
#define HOUSE_ALT 4         /* the house too: the same grid and header but for where the player
                             * comes in (room 4: reached another way, as a save state had it) */
#define B_ROOMS 0x6B        /* the area's room tables, at $8000 */
#define GRID_PTRS 0x9C30    /* a grid list for each route step */
#define HEAD_PTRS 0x9C71    /* a header for each room */
#define HOUSE_GRID 0x9C44
#define HOUSE_HEAD 0x9C91
#define B_SCREENS 0x6F      /* the area's screens: 64 block numbers each, at $A9EC, and a bit
                             * more for each block (block numbers are 9 bits) at $AFEC */
#define SCREENS 0xA9EC
#define SCREEN_HI 0xAFEC
/* ours, in the room bank's free space after the room tables (all $FF on the disc) */
#define OUR_GRID 0x9E00
#define OUR_HEAD_IN 0x9E10
#define OUR_HEAD_OUT 0x9E20
#define STASH 0x9F00        /* "RSEC", then the street's two screens and their bits */
#define FREE_END 0x9FE0
#define BORROW_A 1          /* the street's screens lent to the hidden room */
#define N_SCREENS 3        /* the hidden room's screens (level X pages 2-4) */
#define STASH_LEN (N_SCREENS * 64 + N_SCREENS * 8)

static const uint8_t HOUSE_HEADER[14] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
                                         0x01, 0x01, 0x20, 0xAA, 0x00, 0x00};
static const uint8_t HOUSE_SCREEN[8] = {0x90, 0x91, 0x92, 0x93, 0x9D, 0x9E, 0x9F, 0xA0};

/* The hidden room: two screens side by side at the level's X pages 2 and 3, Y page 1 (the
 * house's row; the house is page 0), so the house's own objects, all on page 0, stay out of
 * reach. Stage 1's blocks: E8-ED stone and the dark interior's brick (E9 the ceiling, EA/ED
 * the wall's edge), EE/EF the dark brick wall (EF with a pillar, EB/EC above them), F1 a
 * barred window (two blocks tall), F0 a chair, 118 the floor's ledge, 119 below the floor,
 * 9F/A1 the house's arched door (the way back). Row 7 is below the picture. */
static uint16_t layout[8][8 * N_SCREENS];
static void make_layout(void)
{
    static const uint8_t wall[8] = {0xEE, 0xEF, 0xEE, 0xEE, 0xEE, 0xEF, 0xEE, 0xEE};
    for (int c = 0; c < 8 * N_SCREENS; c++) {
        uint16_t lw = wall[c & 7];
        layout[0][c] = 0xE9;
        layout[1][c] = lw == 0xEF ? 0xEC : 0xEB;
        for (int r = 2; r <= 5; r++) layout[r][c] = lw;
        layout[6][c] = 0x118;
        layout[7][c] = 0x119;
    }
    layout[1][5] = layout[2][5] = 0xF1;   /* windows */
    layout[1][12] = layout[2][12] = 0xF1;
    layout[5][6] = 0xF0;                  /* a chair (the far end is the card table's) */
    layout[4][2] = 0x9F;                  /* the door back */
    layout[5][2] = 0xA1;
    layout[1][19] = layout[2][19] = 0xF1;
    for (int r = 0; r <= 5; r++) layout[r][8 * N_SCREENS - 1] = 0xE8;
    layout[0][0] = 0xE8;
    layout[1][0] = 0xEA;
    for (int r = 2; r <= 5; r++) layout[r][0] = 0xED;
}
/* the grid list (6B:8882's format: a grid position, then screen numbers, $FF ends) and the
 * headers (6B:8954's: camera X page, Y page, X, Y, camera mode, columns, left page, right
 * page, top and bottom pages, the player's X and Y, $45, the player's facing) */
static const uint8_t GRID[5] = {0x12, BORROW_A, BORROW_A + 1, BORROW_A + 2, 0xFF};
static const uint8_t HEAD_IN[14] = {0x02, 0x01, 0x00, 0x00, 0x00, 0x08, 0x02, 0x03,
                                    0x01, 0x01, 0x50, 0xAA, 0x00, 0x00};
/* back in the house, by the door, facing the stairs */
static const uint8_t HEAD_OUT[14] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
                                     0x01, 0x01, 0xD0, 0x2A, 0x00, 0x01};

/* The door: its tiles (VRAM patterns, palette $B) in five rows of two, 16 x 40 pixels: two
 * wooden leaves hinged at the sides (columns 0-7 and 8-15), a bar across them (rows 19-23)
 * and a padlock on it (columns 9-13). The house's map has the door at map rows 34-38,
 * columns 25-26.
 *
 * Opening (the user's pick of five mock-ups, "creaky"), at 60 frames a second: the padlock's
 * shackle pops, the lock drops out of sight, the bar slides off to the left; then, with the
 * game's own creaking door (the sound of RUN on its title screen), the left leaf swings
 * slowly in, the right one gives a little, sticks, and swings in after it. A leaf turning
 * away narrows toward its hinge and darkens. */
static const uint16_t DOOR_TILES[5][2] = {{0x245, 0x246}, {0x248, 0x249}, {0x24A, 0x24B},
                                          {0x24C, 0x24D}, {0x24E, 0x24F}};
#define DOOR_H 40
#define T_POP 16      /* the shackle pops */
#define T_FALL 24     /* the lock drops */
#define T_BAR 44      /* the bar slides off */
#define T_LEFT 60     /* the left leaf swings; the creak starts */
#define T_PAUSE 150
#define T_GIVE 170    /* the right leaf gives a little and settles back */
#define T_RIGHT 200   /* the right leaf swings */
#define T_OPEN 320
#define DOOR_MAP_ROW 34
#define DOOR_MAP_COL 25

static const uint8_t KONAMI[10] = {PAD_UP, PAD_UP, PAD_DOWN, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
                                   PAD_LEFT, PAD_RIGHT, PAD_II, PAD_I};

static struct {
    uint8_t prev;              /* the pad last frame */
    int code;                  /* how much of the code has been entered */
    bool open;                 /* the door opened (this visit to the house) */
    int anim;                  /* frames into the opening, 0 - T_OPEN */
    bool have_orig;
    uint16_t orig[5][2][16], shown[5][2][16];
    int pending;               /* 1 going in, 2 coming out (the room change under way) */
    int pending_frames;
} s;

static void fight_reset(void);

static bool hidden_seen;   /* (see in_hidden) */

void secret_reset(void)
{
    hidden_seen = false;
    s.prev = 0;
    s.code = 0;
    s.open = false;
    s.anim = 0;
    s.have_orig = false;
    s.pending = 0;
    sound_extra_stop();
    fight_reset();
}

static uint8_t *at(Pce *p, int bank, uint16_t a) { return p->page[bank] ? p->page[bank] + (a & 0x1FFF) : NULL; }
static int rd16(Pce *p, int bank, uint16_t a)
{
    uint8_t *m = at(p, bank, a);
    return m ? m[0] | m[1] << 8 : -1;
}
static void wr16(Pce *p, int bank, uint16_t a, uint16_t v)
{
    uint8_t *m = at(p, bank, a);
    if (m) m[0] = (uint8_t)v, m[1] = (uint8_t)(v >> 8);
}

/* the stage's data is the one this was made for, and our space in it is free or ours */
static bool data_ok(Pce *p)
{
    if (p->wram[W_AREA] != AREA) return false;
    uint8_t *h = at(p, B_ROOMS, HOUSE_HEAD), *sc = at(p, B_SCREENS, SCREENS + 7 * 64), *st = at(p, B_ROOMS, STASH);
    if (!h || !sc || !st || memcmp(h, HOUSE_HEADER, sizeof HOUSE_HEADER) || memcmp(sc, HOUSE_SCREEN, sizeof HOUSE_SCREEN))
        return false;
    int g = rd16(p, B_ROOMS, GRID_PTRS + 2 * HOUSE), hd = rd16(p, B_ROOMS, HEAD_PTRS + 2 * HOUSE);
    if ((g != HOUSE_GRID && g != OUR_GRID) || (hd != HOUSE_HEAD && hd != OUR_HEAD_IN && hd != OUR_HEAD_OUT)) return false;
    if (!memcmp(st, "RSEC", 4)) return true;
    for (uint16_t a = OUR_GRID; a < FREE_END; a++) {
        uint8_t v = *at(p, B_ROOMS, a);
        if (v != 0xFF && !(a >= OUR_GRID && a < STASH)) return false;
    }
    return true;
}

static int cam_page(Pce *p) { return p->wram[W_CAM_PAGE] & 0x0F; }
static bool playing(Pce *p) { return game_state(p) == RONDO_PLAYING && p->wram[RONDO_SUB] == 1; }
static bool house_room(Pce *p) { return p->wram[W_ROOM] == HOUSE || p->wram[W_ROOM] == HOUSE_ALT; }
static bool in_house(Pce *p) { return p->wram[W_AREA] == AREA && house_room(p) && cam_page(p) == 0; }
/* (walking in from the street, the room is the house's two frames before the camera leaves the
 * street's page 3, so the hidden room counts only once the game has played in it; then it holds
 * through pauses and the like: hidden_seen) */
static bool in_hidden(Pce *p)
{
    int c = cam_page(p);
    if (p->wram[W_AREA] != AREA || !house_room(p) || (c != 2 && c != 3)) return hidden_seen = false;
    if (playing(p)) hidden_seen = true;
    return hidden_seen;
}
static int px(Pce *p) { return p->wram[W_PX_HI] << 8 | p->wram[W_PX]; }
static int py(Pce *p) { return p->wram[W_PY_HI] << 8 | p->wram[W_PY]; }

/* the hidden room's screens in the street's two slots (the street's kept in the stash) */
static void borrow(Pce *p)
{
    uint8_t *st = at(p, B_ROOMS, STASH), *sc = at(p, B_SCREENS, SCREENS + BORROW_A * 64),
            *hi = at(p, B_SCREENS, SCREEN_HI + BORROW_A * 8);
    if (!st || !sc || !hi) return;
    if (memcmp(st, "RSEC", 4)) {
        memcpy(st + 4, sc, N_SCREENS * 64);
        memcpy(st + 4 + N_SCREENS * 64, hi, N_SCREENS * 8);
        memcpy(st, "RSEC", 4);
    }
    for (int k = 0; k < N_SCREENS; k++)
        for (int r = 0; r < 8; r++) {
            uint8_t bits = 0;
            for (int c = 0; c < 8; c++) {
                uint16_t b = layout[r][k * 8 + c];
                sc[k * 64 + r * 8 + c] = (uint8_t)b;
                bits |= (uint8_t)((b >> 8) & 1) << c;
            }
            hi[k * 8 + r] = bits;
        }
}

static void give_back(Pce *p)
{
    uint8_t *st = at(p, B_ROOMS, STASH), *sc = at(p, B_SCREENS, SCREENS + BORROW_A * 64),
            *hi = at(p, B_SCREENS, SCREEN_HI + BORROW_A * 8);
    if (!st || !sc || !hi || memcmp(st, "RSEC", 4)) return;
    memcpy(sc, st + 4, N_SCREENS * 64);
    memcpy(hi, st + 4 + N_SCREENS * 64, N_SCREENS * 8);
    memset(st, 0xFF, 4 + STASH_LEN);
}

static void pointers_back(Pce *p)
{
    wr16(p, B_ROOMS, GRID_PTRS + 2 * HOUSE, HOUSE_GRID);
    wr16(p, B_ROOMS, HEAD_PTRS + 2 * HOUSE, HOUSE_HEAD);
}

/* the game's own room change into the house's route step, with our grid and header */
static void change_room(Pce *p, uint16_t grid, uint16_t head)
{
    memcpy(at(p, B_ROOMS, OUR_GRID), GRID, sizeof GRID);
    memcpy(at(p, B_ROOMS, OUR_HEAD_IN), HEAD_IN, sizeof HEAD_IN);
    memcpy(at(p, B_ROOMS, OUR_HEAD_OUT), HEAD_OUT, sizeof HEAD_OUT);
    wr16(p, B_ROOMS, GRID_PTRS + 2 * HOUSE, grid);
    wr16(p, B_ROOMS, HEAD_PTRS + 2 * HOUSE, head);
    p->wram[W_STEP] = HOUSE - 1;   /* (leaving steps it to the house's again) */
    p->wram[W_EXIT] = 0x80;
    p->wram[W_EVENT] = 3;
    s.pending_frames = 0;
}

/* a pattern's pixel (colour 0-15) */
static int pix(const uint16_t *t, int x, int y)
{
    int b = 7 - x;
    return (t[y] >> b & 1) | (t[y] >> (8 + b) & 1) << 1 | (t[8 + y] >> b & 1) << 2 | (t[8 + y] >> (8 + b) & 1) << 3;
}
static void set_pix(uint16_t *t, int x, int y, int v)
{
    int b = 7 - x;
    t[y] = (uint16_t)((t[y] & ~(1u << b | 1u << (8 + b))) | (v & 1) << b | (v >> 1 & 1) << (8 + b));
    t[8 + y] = (uint16_t)((t[8 + y] & ~(1u << b | 1u << (8 + b))) | (v >> 2 & 1) << b | (v >> 3 & 1) << (8 + b));
}
static int door_orig(int x, int y) { return pix(s.orig[y >> 3][x >> 3], x & 7, y & 7); }

static double ease(double t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 0.5 - 0.5 * cos(3.14159265358979 * t);
}
static bool in_lock(int x, int y) { return x >= 9 && x <= 13 && y >= 19 && y <= 23 && door_orig(x, y) != 1; }
/* a leaf's own pixels (behind the bar: its boards, as above it) */
static int leaf_px(int x, int y) { return door_orig(x, y >= 19 && y <= 23 ? (y & 1 ? 17 : 18) : y); }
/* the bar (behind the lock: the bar as beside it) */
static int bar_px(int x, int y) { return in_lock(x, y) ? door_orig(6, y) : door_orig(x, y); }
static int darker(int c)
{
    switch (c) {
    case 7: return 5;
    case 5: return 6;
    case 6: case 2: return 1;
    case 3: return 2;
    case 4: return 3;
    default: return c;
    }
}

/* the door `t` frames into its opening, as colours (16 x 40) */
static void door_at(int t, uint8_t g[DOOR_H][16])
{
    double lw = 8, rw = 8;
    if (t >= T_LEFT) lw = 8 * (1 - ease((t - T_LEFT) / (double)(T_PAUSE - T_LEFT - 1)));
    if (t >= T_GIVE && t < T_RIGHT) rw = 8 - 1.8 * sin(3.14159265358979 * (t - T_GIVE) / (T_RIGHT - T_GIVE));
    if (t >= T_RIGHT) rw = 8 * (1 - ease((t - T_RIGHT) / (double)(T_OPEN - T_RIGHT - 1)));
    for (int side = 0; side < 2; side++) {
        double w = side ? rw : lw;
        int dark = w < 8 ? (int)((1 - w / 8) * 3 + 0.5) : 0;
        for (int y = 0; y < DOOR_H; y++)
            for (int j = 0; j < 8; j++) {
                int x = side ? 15 - j : j, c = 1;
                if (j < w) {   /* (the leaf squeezed toward its hinge) */
                    int sj = (int)(j * 8 / w);
                    if (sj > 7) sj = 7;
                    c = leaf_px(side ? 15 - sj : sj, y);
                    for (int k = 0; k < dark && c != 1; k++) c = darker(c);
                }
                g[y][x] = (uint8_t)c;
            }
    }
    if (t < T_LEFT) {   /* the bar, sliding off to the left */
        int bx = t < T_BAR ? 0 : -(int)(16 * ease((t - T_BAR) / 15.0));
        for (int y = 19; y <= 23; y++)
            for (int x = 0; x < 16; x++) {
                int sx = x - bx;
                if (sx < 16 && (bx == 0 || bar_px(sx, y) != 1)) g[y][x] = (uint8_t)bar_px(sx, y);
            }
    }
    if (t < T_BAR) {    /* the lock: its shackle popped, then falling */
        int dy = 0;
        if (t >= T_FALL) {
            int k = (t - T_FALL) / 2;
            dy = (int)(0.3 * (k + 1) * (k + 2));
        }
        for (int y = 19; y <= 23; y++)
            for (int x = 9; x <= 13; x++) {
                if (!in_lock(x, y)) continue;
                int ny = y + dy - (t >= T_POP && t < T_FALL && y <= 20 ? 1 : 0);
                if (ny < DOOR_H) g[ny][x] = (uint8_t)door_orig(x, y);
            }
    }
}

/* the door's tiles in VRAM `t` frames into its opening (0: the game's own) */
static void door_show(Pce *p, int t)
{
    uint16_t *v = p->vdc.vram;
    bool reloaded = false;
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 2; k++)
            if (!s.have_orig || memcmp(&v[DOOR_TILES[r][k] * 16], s.shown[r][k], 32)) reloaded = true;
    if (reloaded) {   /* (first time, or the game loaded the tiles again: they are its own) */
        for (int r = 0; r < 5; r++)
            for (int k = 0; k < 2; k++) memcpy(s.orig[r][k], &v[DOOR_TILES[r][k] * 16], 32);
        s.have_orig = true;
    }
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 2; k++) memcpy(s.shown[r][k], s.orig[r][k], 32);
    if (t > 0) {
        uint8_t g[DOOR_H][16];
        door_at(t, g);
        for (int y = 0; y < DOOR_H; y++)
            for (int x = 0; x < 16; x++) set_pix(s.shown[y >> 3][x >> 3], x & 7, y & 7, g[y][x]);
    }
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 2; k++) memcpy(&v[DOOR_TILES[r][k] * 16], s.shown[r][k], 32);
}

/* The creak: the ADPCM clip the game plays for RUN on its title screen, read from the disc
 * (the title's sound bank: data track 2, record $ED on, loaded whole into ADPCM RAM; the clip
 * at $50CA, $5B8A bytes, rate 12: 8 kHz). Its first 0.55 s are silence, skipped. Checked by
 * its CRC: on another disc there is no creak. */
#define CREAK_REC 0xED
#define CREAK_AT 0x50CA
#define CREAK_LEN 0x5B8A
#define CREAK_CRC 0xFA003458u
#define CREAK_SKIP 2200
static uint8_t creak[CREAK_LEN];
static int creak_state;   /* 0 not read yet, 1 read, -1 not there */

static uint32_t crc32_of(const uint8_t *b, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void play_creak(Pce *p)
{
    if (creak_state == 0) {
        static uint8_t buf[12 * 2048];
        const DiscTrack *tr = disc_track(&p->disc, 2);
        int first = CREAK_AT / 2048;
        creak_state = -1;
        if (tr && disc_read_data(&p->disc, tr->lba + CREAK_REC + first, 12, buf)) {
            memcpy(creak, buf + (CREAK_AT - first * 2048), CREAK_LEN);
            if (crc32_of(creak, CREAK_LEN) == CREAK_CRC) creak_state = 1;
        }
    }
    if (creak_state == 1) sound_extra_play(creak + CREAK_SKIP, CREAK_LEN - CREAK_SKIP, 12);
}

/* the house's door is where it should be in the map (the house as drawn) */
static bool door_in_map(Pce *p)
{
    const uint16_t *v = p->vdc.vram;
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 2; k++)
            if ((v[(DOOR_MAP_ROW + r) * 64 + DOOR_MAP_COL + k] & 0x7FF) != DOOR_TILES[r][k]) return false;
    return true;
}

static void scene_hits(Pce *p);
static void fight_frame(Pce *p);
static void orbs_step(Pce *p);
static void grave_step(Pce *p);
static void grave_reset(void);

/* A sound effect of the game's own, played by its sound driver: before a frame, the CPU is
 * sent through a few bytes in free work RAM ($29E0) that ask for it as the game does (LDA #id,
 * JSR $401B: 69:4393, the driver's request) and return to where it was (its PC pushed as a
 * JSR would). Only while the game's usual banks are in (work RAM at $2000, bank $69 at $4000,
 * $401B a JMP), the driver not busy ($34 bit 7), and the CPU not in work RAM or the top of
 * bank 0. (Sound sets are the stage's: the id is the current stage's.) */
#define SFX_TRAMP 0x09E0   /* work RAM offset ($29E0) */
static int sfx_want = -1;
static void sfx_request(int id) { sfx_want = id; }
static void sfx_inject(Pce *p)
{
    if (sfx_want < 0) return;
    HuC6280 *c = &p->cpu;
    if (c->mpr[1] != 0xF8 || c->mpr[2] != 0x69 || pce_read8(p, 0x401B) != 0x4C || (p->wram[0x34] & 0x80)) return;
    if ((c->pc >= 0x2000 && c->pc < 0x4000) || (c->pc >= 0xFA00 && c->mpr[7] == 0x00)) return;
    static const uint8_t T[14] = {0x08, 0x48, 0xDA, 0x5A, 0xA9, 0x00, 0x20, 0x1B, 0x40, 0x7A, 0xFA, 0x68, 0x28, 0x60};
    memcpy(&p->wram[SFX_TRAMP], T, sizeof T);
    p->wram[SFX_TRAMP + 5] = (uint8_t)sfx_want;
    uint16_t ret = (uint16_t)(c->pc - 1);
    p->wram[0x100 + c->s] = (uint8_t)(ret >> 8), c->s--;
    p->wram[0x100 + c->s] = (uint8_t)ret, c->s--;
    c->pc = 0x2000 + SFX_TRAMP;
    c->page_gen[0xF8]++;
    sfx_want = -1;
}

void secret_frame(Pce *p, bool on)
{
    static bool made;
    if (!made) make_layout(), made = true;
    uint8_t pad = p->pad, press = pad & (uint8_t)~s.prev;
    s.prev = pad;
    if (p->wram[W_AREA] != AREA || !data_ok(p)) {
        if (s.pending == 0) { s.open = false; s.code = 0; }
        return;
    }
    /* the room change under way: our pointers back once the room it went to is playing */
    if (s.pending) {
        s.pending_frames++;
        bool done = playing(p) && (s.pending == 1 ? in_hidden(p) : in_house(p));
        if (done || s.pending_frames > 600) {
            pointers_back(p);
            s.pending = 0;
        }
    } else if (rd16(p, B_ROOMS, HEAD_PTRS + 2 * HOUSE) != HOUSE_HEAD || rd16(p, B_ROOMS, GRID_PTRS + 2 * HOUSE) != HOUSE_GRID) {
        pointers_back(p);   /* (a state saved in the middle of one) */
    }
    /* the street's screens lent only while the hidden room is played (or being entered) */
    if (s.pending == 1 || (in_hidden(p) && playing(p))) borrow(p);
    else if (s.pending == 0 && !in_hidden(p)) give_back(p);
    if (!on) {
        s.code = 0;
        if (s.open && s.have_orig) door_show(p, 0);
        sound_extra_stop();
        s.open = false;
        return;
    }
    if (!house_room(p)) {
        s.open = false;
        s.code = 0;
        return;
    }
    if (in_hidden(p)) {
        fight_frame(p);
        scene_hits(p);
        sfx_inject(p);
        s.open = true;   /* (it is open on this side, and stays open behind) */
        s.anim = T_OPEN;
        door_show(p, s.anim);
        /* Up at the door back */
        int wx = cam_page(p) * 256 + p->wram[W_CAM_X] + px(p);
        if (s.pending == 0 && playing(p) && (press & PAD_UP) && wx >= 576 && wx <= 608 && py(p) >= 0x80)
            s.pending = 2, change_room(p, HOUSE_GRID, OUR_HEAD_OUT);
        return;
    }
    if (!in_house(p) || !door_in_map(p)) return;
    /* (the code works anywhere in the house, not only by the door) */
    if (!s.open) {
        if (!playing(p)) s.code = 0;
        else if (press) {
            /* (a press holding the button wanted counts: the frontend's Sub-weapon control,
             * a gamepad's B, reaches the game as Up and II together; for a direction, no other
             * direction with it) */
            uint8_t want = KONAMI[s.code], dirs = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
            bool ok = (press & want) && (!(want & dirs) || !(press & dirs & (uint8_t)~want));
            if (want & (PAD_I | PAD_II)) ok = press & want;
            if (ok) s.code++;
            else s.code = press == KONAMI[0] ? 1 : 0;
            if (s.code == 10) s.open = true, s.anim = 0, s.code = 0;
        }
    }
    if (s.open) {
        if (s.anim < T_OPEN && ++s.anim == T_LEFT) play_creak(p);
        door_show(p, s.anim);
        if (s.pending == 0 && s.anim == T_OPEN && playing(p) && (press & PAD_UP) && px(p) >= 192 && px(p) <= 224 && py(p) < 0x50) {
            borrow(p);
            s.pending = 1;
            change_room(p, OUR_GRID, OUR_HEAD_IN);
        }
    }
}

/* ---- the scene: Death and Maria at cards --------------------------------------------- */
/* At the room's far right a card table (the host's own drawing), Death hovering at its left
 * end and Maria standing at its right, facing him, each holding cards. They are drawn by the
 * host over the picture after each frame, behind the game's sprites (Richter walks in front),
 * in the game's own sprite graphics and palettes read from the disc's data track: Death as
 * the boss of the Death fight, Maria's standing frame from her own sprite set. Whipping Death
 * makes him flash. */

/* where on the data track (byte offsets from its start): 16 x 16 sprite cells, 128 bytes each,
 * by cell number from a set's start, and palettes (16 entries packed in 18 bytes: a byte of
 * the 8 entries' bit 8, MSB first, then their low bytes, twice) */
#define DEATH_CELLS 0x838800
#define DEATH_CELLS2 0x84E800
#define DEATH_PAL 0x8614B7
#define DEATH_PAL2 0x861523
#define MARIA_CELLS 0xEC600
#define MARIA_PAL 0x1293C9
#define DEATH_CRC 0x5AF32AD3u
#define MARIA_CRC 0xE27CAA86u

/* a frame: sprites as the game's sprite table has them (x, y from the frame's top left;
 * cell number; attributes: palette, width and height, flips); listed front to back */
typedef struct { int8_t x, y; uint16_t cell, attr; } Piece;
/* Death as he hovers in his fight (palette 0 here: his palette 9; 1: his palette 15, the
 * glint in his ribs; cell 0x1F4 stands for cell 0xF4 of his second set) */
static const Piece DEATH[] = {
    {16, 48, 0x14D, 0x1800}, {0, 48, 0x14D, 0x1000}, {13, 24, 0x145, 0x0000}, {7, 28, 0x147, 0x0000},
    {0, 0, 0x140, 0x1100},   {0, 32, 0x14B, 0x0000}, {16, 32, 0x14B, 0x0800}, {9, 36, 0x1F4, 0x0001},
};
#define DEATH_W 32
#define DEATH_H 80
/* Maria standing (facing right; drawn flipped, facing left) */
static const Piece MARIA[] = {{0, 0, 0x185, 0x0000}, {0, 16, 0x184, 0x1000}};
#define MARIA_W 16
#define MARIA_H 48

static struct {
    int state;                       /* 0 not read, 1 read, -1 not there */
    uint8_t death[16 * 128 + 128], maria[4 * 128];
    uint32_t death_pal[2][16], maria_pal[1][16];
    int flash;                       /* frames of Death's white flash left */
    int hit_wait;                    /* frames before he can be hit again */
    /* the card game (host-side, not in save states) */
    uint64_t last_frame;
    uint32_t rng;
    int hand[2];                     /* cards in Death's (0) and Maria's (1) hands */
    int deck;                        /* cards in the deck */
    int n_table;                     /* cards played on the table: their slots and faces */
    int8_t table_x[6];
    uint8_t table_kind[6];
    int wait;                        /* frames to the next move */
    int turn;                        /* whose move */
    /* a card on its way: from (fx, fy) to (tx, ty) over `len` frames, relative to the table's
     * middle at the table top; `to`: what it lands as (0 a hand, 1 the table, 2 the deck) */
    bool moving;
    int mv_t, mv_len, mv_from_x, mv_from_y, mv_to_x, mv_to_y, mv_to, mv_who, mv_kind, mv_sweep;
    int glow;                        /* the candle's light this frame, 0-255 */
} sc;

static bool disc_bytes(Pce *p, uint32_t off, uint32_t len, uint8_t *out)
{
    static uint8_t buf[4 * 2048];
    const DiscTrack *tr = disc_track(&p->disc, 2);
    uint32_t first = off / 2048, n = (off % 2048 + len + 2047) / 2048;
    if (!tr || n > 4 || !disc_read_data(&p->disc, tr->lba + (int)first, (int)n, buf)) return false;
    memcpy(out, buf + off % 2048, len);
    return true;
}

static uint32_t argb9(uint16_t c)
{
    uint32_t b = c & 7, r = (c >> 3) & 7, g = (c >> 6) & 7;
    return 0xFF000000u | (r * 255 / 7) << 16 | (g * 255 / 7) << 8 | (b * 255 / 7);
}
static void unpack_pal(const uint8_t *b, uint32_t *out)
{
    for (int g = 0; g < 2; g++)
        for (int i = 0; i < 8; i++) out[g * 8 + i] = argb9((uint16_t)(b[g * 9 + 1 + i] | ((b[g * 9] >> (7 - i)) & 1) << 8));
}

static bool scene_load(Pce *p)
{
    if (sc.state) return sc.state > 0;
    sc.state = -1;
    uint8_t dp[2][18], mp[18], all[16 * 128 + 128 + 36], mall[4 * 128 + 18];
    if (!disc_bytes(p, DEATH_CELLS + 0x140 * 128, 16 * 128, sc.death) ||
        !disc_bytes(p, DEATH_CELLS2 + 0xF4 * 128, 128, sc.death + 16 * 128) || !disc_bytes(p, DEATH_PAL, 18, dp[0]) ||
        !disc_bytes(p, DEATH_PAL2, 18, dp[1]) || !disc_bytes(p, MARIA_CELLS + 0x184 * 128, 4 * 128, sc.maria) ||
        !disc_bytes(p, MARIA_PAL, 18, mp))
        return false;
    memcpy(all, sc.death, sizeof sc.death);
    memcpy(all + sizeof sc.death, dp[0], 18);
    memcpy(all + sizeof sc.death + 18, dp[1], 18);
    memcpy(mall, sc.maria, sizeof sc.maria);
    memcpy(mall + sizeof sc.maria, mp, 18);
    if (crc32_of(all, sizeof all) != DEATH_CRC || crc32_of(mall, sizeof mall) != MARIA_CRC) return false;
    unpack_pal(dp[0], sc.death_pal[0]);
    unpack_pal(dp[1], sc.death_pal[1]);
    unpack_pal(mp, sc.maria_pal[0]);
    sc.state = 1;
    return true;
}

/* a sprite cell's pixel (colour 0-15): the game's 4 planes of 16 rows */
static int cell_px(const uint8_t *cell, int x, int y)
{
    int b = 15 - x, c = 0;
    for (int pl = 0; pl < 4; pl++) {
        uint16_t w = (uint16_t)(cell[(pl * 16 + y) * 2] | cell[(pl * 16 + y) * 2 + 1] << 8);
        c |= ((w >> b) & 1) << pl;
    }
    return c;
}

/* the picture: a pixel at screen x, y (the game's 256 from x 0), unless a sprite of the
 * game's is there */
typedef struct { uint32_t *fb; const uint8_t *spr; int org, w, h; } Canvas;
static void plot(const Canvas *cv, int x, int y, uint32_t c)
{
    int fx = x + cv->org;
    if (fx < 0 || fx >= cv->w || y < 0 || y >= cv->h) return;
    if (cv->spr[y * PCE_FB_MAX_W + fx]) return;
    cv->fb[y * PCE_FB_MAX_W + fx] = c;
}

/* (0-16: how much of a frame draw_frame leaves out, by a 4 x 4 ordered dither) */
static int g_dissolve;
static const uint8_t DITHER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* a frame of pieces with its top left at screen x, y; flip: mirrored within width w; cells
 * from `base` in `cells`, from `base2` in `cells2` */
static void draw_frame(const Canvas *cv, const Piece *pc, int n, const uint8_t *cells, uint16_t base, uint16_t base2,
                       const uint8_t *cells2, uint32_t pal[][16], int x, int y, int w, bool flip, bool white)
{
    for (int i = n - 1; i >= 0; i--) {   /* (back to front) */
        const Piece *e = &pc[i];
        int cgy = (e->attr >> 12) & 3, cw = e->attr & 0x100 ? 2 : 1, ch = cgy == 0 ? 1 : cgy == 1 ? 2 : 4;
        int sw = cw * 16, sh = ch * 16;
        bool xf = ((e->attr & 0x800) != 0) != flip, yf = (e->attr & 0x8000) != 0;
        int ex = flip ? w - (e->x + sw) : e->x;
        uint16_t first = (uint16_t)(e->cell & ~((cw - 1) | (ch - 1) << 1));
        for (int yy = 0; yy < sh; yy++)
            for (int xx = 0; xx < sw; xx++) {
                int cx = xf ? sw - 1 - xx : xx, cy = yf ? sh - 1 - yy : yy;
                uint16_t cell = (uint16_t)(first + (cx >> 4) + (cy >> 4) * 2);
                const uint8_t *src = cells2 && cell >= base2 ? cells2 + (cell - base2) * 128 : cells + (cell - base) * 128;
                int c = cell_px(src, cx & 15, cy & 15);
                int px = x + ex + xx, py = y + e->y + yy;
                if (c && DITHER[py & 3][px & 3] >= g_dissolve) plot(cv, px, py, white ? 0xFFFFFFFFu : pal[e->attr & 15][c]);
            }
    }
}

/* the table and the cards: the host's own drawing */
static void rect(const Canvas *cv, int x0, int y0, int x1, int y1, uint32_t c)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) plot(cv, x, y, c);
}
#define WOOD_D 0xFF482400u
#define WOOD 0xFF6D4824u
#define WOOD_L 0xFF916D24u
static void card(const Canvas *cv, int x, int y, int w, int h, int kind)
{
    if (kind == 0) {   /* face down */
        rect(cv, x, y, x + w - 1, y + h - 1, 0xFFDADAECu);
        if (w > 2 && h > 2) rect(cv, x + 1, y + 1, x + w - 2, y + h - 2, 0xFF242491u);
    } else {           /* face up: a red or a black pip */
        rect(cv, x, y, x + w - 1, y + h - 1, 0xFFECECECu);
        plot(cv, x + w / 2, y + h / 2, kind == 1 ? 0xFFC80000u : 0xFF000000u);
    }
}

/* the scene's places in the room (room X: the level's X - 512, from the room's left wall) */
#define DEATH_X 350
#define DEATH_BOTTOM 186
#define TABLE_X0 390
#define TABLE_X1 444
#define TABLE_Y 166
#define MARIA_X 452
#define ROOM_FLOOR 192

static int scene_cam(Pce *p) { return cam_page(p) * 256 + p->wram[W_CAM_X] - 512; }

/* Death's place on screen this frame (his top left; he hovers up and down a little) */
static void death_at(Pce *p, int cam, int *x, int *y)
{
    *x = DEATH_X - cam;
    *y = DEATH_BOTTOM - DEATH_H + (int)lround(2 * sin((double)p->frame * 0.05));
}

/* the whip striking Death (before a frame: its box as the last frame left it) */
static void fight_start(Pce *p);
static bool fight_busy(void);

static void scene_hits(Pce *p)
{
    if (sc.hit_wait) sc.hit_wait--;
    if (sc.flash) sc.flash--;
    if (sc.state <= 0 || sc.hit_wait || fight_busy()) return;
    int dx, dy;
    death_at(p, scene_cam(p), &dx, &dy);
    int x0 = dx + 2, x1 = dx + DEATH_W - 2, y0 = dy + 6, y1 = dy + DEATH_H - 6;
    for (int sl = 1; sl < 16; sl++) {
        if (!p->wram[0x0A10 + sl] || p->wram[0x1828 + sl] != 5) continue;   /* (the whip) */
        int fr = p->wram[0x0A68 + sl];
        if (fr < 5 || fr > 7) continue;                                         /* (its strike) */
        int ox = (int16_t)(p->wram[W_PX_HI + sl] << 8 | p->wram[W_PX + sl]);
        int oy = (int16_t)(p->wram[W_PY_HI + sl] << 8 | p->wram[W_PY + sl]);
        int ahead = (int8_t)p->wram[0x1930 + sl], hw = p->wram[0x1988 + sl];
        int below = (int8_t)p->wram[0x19E0 + sl], hh = p->wram[0x1A38 + sl];
        int cx = ox + (p->wram[0x0AC0] ? -ahead : ahead), cy = oy + below;
        if (cx + hw >= x0 && cx - hw <= x1 && cy + hh >= y0 && cy - hh <= y1) {
            sc.flash = 8;
            sc.hit_wait = 20;
            fight_start(p);
            return;
        }
    }
}

/* The card game: each in turn plays a card from their hand onto the table, or draws one from
 * the deck; with enough on the table, the trick is swept back onto the deck. A move every
 * second or three, a card flying over in a little arc. */
static const int8_t SLOTS[5] = {-13, -7, 6, 12, 18};
static int rnd(int n)
{
    sc.rng = sc.rng * 1103515245u + 12345u;
    return (int)((sc.rng >> 16) % (uint32_t)n);
}
static void move_card(int fx, int fy, int tx, int ty, int to, int who, int kind, int sweep, int len)
{
    sc.moving = true;
    sc.mv_t = 0;
    sc.mv_len = len;
    sc.mv_from_x = fx, sc.mv_from_y = fy, sc.mv_to_x = tx, sc.mv_to_y = ty;
    sc.mv_to = to, sc.mv_who = who, sc.mv_kind = kind, sc.mv_sweep = sweep;
}
static void cards_step(const int hand_x[2], const int hand_y[2])
{
    if (!sc.deck && !sc.hand[0] && !sc.hand[1] && !sc.n_table) {   /* (a new game) */
        sc.rng = 0x2D2A5u;
        sc.hand[0] = sc.hand[1] = 3;
        sc.deck = 10;
        sc.n_table = 2;
        sc.table_x[0] = SLOTS[1], sc.table_kind[0] = 2;
        sc.table_x[1] = SLOTS[3], sc.table_kind[1] = 1;
        sc.wait = 60;
    }
    if (sc.moving) {
        if (++sc.mv_t < sc.mv_len) return;
        sc.moving = false;
        if (sc.mv_to == 0) sc.hand[sc.mv_who]++;
        else if (sc.mv_to == 2) sc.deck += sc.mv_sweep;
        else if (sc.n_table < 5) {
            sc.table_x[sc.n_table] = (int8_t)sc.mv_to_x;
            sc.table_kind[sc.n_table++] = (uint8_t)sc.mv_kind;
        }
        return;
    }
    if (--sc.wait > 0) return;
    int w = sc.turn;
    if (sc.n_table >= 4 || (sc.n_table >= 2 && rnd(4) == 0)) {   /* the trick swept up */
        move_card(-4, -2, -22, -2, 2, w, 0, sc.n_table, 24);
        sc.n_table = 0;
    } else if (sc.deck && (!sc.hand[w] || (sc.hand[w] < 4 && rnd(10) < 3))) {   /* a card drawn */
        sc.deck--;
        move_card(-22, -2, hand_x[w], hand_y[w], 0, w, 0, 0, 20);
    } else if (sc.hand[w]) {   /* a card played, face up */
        int x, tries = 0;
        bool used;
        do {
            x = SLOTS[rnd(5)];
            used = false;
            for (int i = 0; i < sc.n_table; i++) used |= sc.table_x[i] == x;
        } while (used && ++tries < 20);
        sc.hand[w]--;
        move_card(hand_x[w], hand_y[w], x, -2, 1, w, 1 + rnd(2), 0, 18);
    }
    sc.turn ^= 1;
    sc.wait = 50 + rnd(120);
}

/* the candle's light: warm, flickering, over everything near it (Richter too) */
static void candle_glow(const Canvas *cv, int x, int y, uint64_t frame)
{
    if ((frame % 3) == 0 || !sc.glow) sc.glow = 150 + rnd(60);
    int r = 30 + (sc.glow - 150) / 12;
    for (int yy = y - r; yy <= y + r; yy++) {
        if (yy < 0 || yy >= cv->h) continue;
        for (int xx = x - r; xx <= x + r; xx++) {
            int fx = xx + cv->org;
            if (fx < 0 || fx >= cv->w) continue;
            double dd = sqrt((double)(xx - x) * (xx - x) + (double)(yy - y) * (yy - y)) / r;
            if (dd >= 1) continue;
            double f = (1 - dd) * (1 - dd) * sc.glow / 255.0;
            uint32_t *px = &cv->fb[yy * PCE_FB_MAX_W + fx];
            int cr = (int)(*px >> 16 & 255) + (int)(f * 120), cg = (int)(*px >> 8 & 255) + (int)(f * 62),
                cb = (int)(*px & 255) + (int)(f * 14);
            *px = 0xFF000000u | (uint32_t)(cr > 255 ? 255 : cr) << 16 | (uint32_t)(cg > 255 ? 255 : cg) << 8 |
                  (uint32_t)(cb > 255 ? 255 : cb);
        }
    }
}


/* ---- the fight: Death's end and the arena ------------------------------------------------ */
/* Whipping Death ends the card game: he shakes and flashes, the game's blood sprays from him
 * and he comes apart (FIGHT_DYING); the room then fades into the arena (FIGHT_FADE), the
 * camera locked at the room's right end so the arena is exactly the 16:9 picture: the title
 * screen's sky (its castle, moon and lightning, the logo left out; widened by the title art's
 * sides) behind the castle approach's brown wall, gate and cobbles. Maria stands facing
 * Richter with a boss gauge (FIGHT_ARENA). All of it is the host's drawing behind the game's
 * sprites, from the disc: the title's map and tiles unpacked by the game's own run-length
 * scheme (6C:81CD), the wall's 32-pixel blocks and tiles, sprites and palettes as they are
 * stored. Not in save states (a state loaded mid-fight shows the card game again). */
enum { FIGHT_NONE, FIGHT_DYING, FIGHT_FADE, FIGHT_ARENA, FIGHT_WON };
#define DYING_T 150
#define FADE_T 60
#define ARENA_W 396
#define ARENA_H 224
#define SKY_LIFT 24          /* the sky raised: the castle over the wall */
#define DROP 17              /* the arena's ground this much lower than the room's: the floor's last
                              * row the picture's last; the camera as much higher in the fight */
#define WALL_TOP (108 + DROP) /* the wall band's top row in the arena */
#define ARENA_X0 698         /* the level's X of the arena's x 0 */
#define ARENA_FLOOR (ROOM_FLOOR + DROP)
#define MARIA_ARENA_X 300
#define GAUGE_ARENA_X 352
#define LIMIT_LEFT_PAGE 0x1EBE
#define CAM_LO 0x1E00

/* the title's packed streams: where on the data track, the VRAM address they unpack to, and
 * the flag the game passes (bit 0 clear: an address step wraps in 4 K words). The two that use
 * the second scheme ($FD: the logo's sprites) are left out. */
static const struct { uint32_t off; uint16_t vram; uint8_t flag; } TITLE_STREAMS[] = {
    {0x430B9, 0x1100, 1}, {0x3C800, 0x2000, 1}, {0x3DBFC, 0x3000, 1}, {0x64800, 0x4000, 1},
    {0x65DEE, 0x5000, 1}, {0x47792, 0x6000, 1}, {0x3F46D, 0x7800, 1}, {0x33B46, 0x0000, 0},
    {0x340D5, 0x0800, 0}, {0x340D5, 0x0020, 0}, {0x345DD, 0x0820, 0},
};
#define TITLE_STREAM_MAX 6144
#define TITLE_VRAM_CRC 0x569D9C32u
#define TITLE_PALS 0x63604        /* 8 palettes, then the lightning's 4-7 */
#define TITLE_PALS_CRC 0x996A041Du
/* the castle approach's wall: its blocks (16 map entries each), tiles and palettes */
#define WALL_BLOCKS_AT 0x19A000
#define WALL_TILES 0x1AC600       /* tile n at + n * 32 (n 0x110-0x20F) */
#define WALL_TILES2 0x2CE600      /* (n 0x210-0x216: the gate's) */
#define WALL_PALS 0x191340
#define WALL_CRC 0x6E9C0312u
/* the band of the map shown: block rows from map row 44, block columns from map column 12;
 * the arena's x 0 is the map's x 100, its row WALL_TOP the map's y 380 */
static const uint8_t WALL_BLOCK[5][13] = {
    {72, 80, 81, 82, 83, 88, 89, 90, 91, 64, 64, 64, 64},
    {93, 93, 93, 93, 98, 93, 93, 93, 101, 108, 93, 93, 93},
    {95, 95, 95, 95, 99, 95, 102, 103, 104, 109, 110, 111, 112},
    {97, 97, 97, 97, 100, 105, 105, 106, 107, 113, 106, 105, 105},
    {64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64},
};
/* the title's lightning bolt (sprites, as the title draws them; x, y on the title) */
#define BOLT_CELLS 0xAD800
#define BOLT_PAL 0x63342
#define BOLT_CRC 0x54665E56u
static const Piece BOLT[] = {{88, 28, 0x199, 0x8000}, {104, 32, 0x19F, 0x0000}, {88, 16, 0x19C, 0x0100},
                             {120, 80, 0x199, 0x0800}, {104, 64, 0x19A, 0x0000}, {104, 48, 0x19B, 0x0000}};
/* the game's blood (the splash of a beaten boss: 8 frames, 16 x 16) */
#define BLOOD_CELLS 0x23DC00
#define BLOOD_PAL 0x8614DB
#define BLOOD_CRC 0xF1D822E8u
/* a boss's gauge (the Death fight's, as it is stored: its column is the bar) */
#define GAUGE_CELLS 0x1B0800
#define GAUGE_PAL 0x861493
#define GAUGE_CRC 0x6E0C1153u
/* the thunder: the game's own, the Prologue's (its carriage chase's, with the lightning: its
 * sound effect $77 there, an ADPCM clip, 4 kHz, 3 s), read from the disc and played over the
 * game's sound by the host. (Stage 1's effect $77 plays whatever its ADPCM RAM holds at the
 * same place, not thunder.) */
#define THUNDER_DELAY 10      /* frames after the flash */
#define THUNDER_AT 0x146804
#define THUNDER_LEN 0x17A2
#define THUNDER_CRC 0x18AD4485u
#define THUNDER_VOLUME 256    /* (the ADPCM's own level) */
/* Maria's yell as she is hurt (when she is played, the stage's sound bank is hers, record $601
 * of the data track: the yell is its first clip, $326 bytes at rate code 8) */
#define MARIA_YELL_AT (0x601 * 2048)
#define MARIA_YELL_LEN 0x326
#define MARIA_YELL_CRC 0x9A54BF9Bu
#define MARIA_YELL_RATE 8
/* ...and her cry as she dies: further on in the same bank ($93C, $B32 bytes) */
#define MARIA_DEATH_AT (0x601 * 2048 + 0x93C)
#define MARIA_DEATH_LEN 0xB32
#define MARIA_DEATH_CRC 0x4D4CA4F8u
#define MARIA_DEATH_RATE 6
/* Death's cry as he falls (Stage 5's ship, his first form beaten: the sound the game plays
 * there (AD_PLAY from $6626 of its sound memory, $E54 bytes at rate code 8), from the data
 * track at $876E26) */
#define DEATH_CRY_AT 0x876E26
#define DEATH_CRY_LEN 0xE54
#define DEATH_CRY_CRC 0x1E3A59F5u
#define DEATH_CRY_RATE 8     /* (drawn out: slower and deeper than the game's 8) */
/* The finale, when her second gauge is emptied: she flashes, shakes and comes apart with her
 * cry (and its echo), red orbs (a beaten boss's, as the host draws them) rain down all over
 * the arena and bounce, the picture fades out and back into the room: the card game over,
 * a tombstone where Death was struck down. */
#define ORBS 48
#define FIN_ECHO 22            /* her cry's echo */
#define FIN_GONE 130           /* she has come apart */
#define FIN_RAIN0 30           /* the orbs: falling from then */
#define FIN_RAIN1 330          /* ...to then (5 s of them) */
#define FIN_AFTER_LAST 30      /* the fade out starts this long after the last one is gathered */
#define FIN_FADE_LEN 60        /* ...and takes this long; then back in the room */
#define FIN_GATHER_MAX 1200    /* (or 20 s after the rain, the orbs left where they are: one out of reach) */
/* The orbs are the one a beaten boss leaves (Death's, Stage 5), as the game draws it, from the
 * disc: cells $140-$147 (data track $2E934): $140 the white shining orb, shimmering (flipped
 * every 2 frames), its palette fading to red through the game's own steps ($191461 on, 19
 * bytes apart, 7 of them); then $141 the red orb with a fireball turning in it ($142-$147,
 * behind it) in its palette ($19156B), sparkles about it now and then ($1F3-$1F6, $104180, in
 * the doves' palette). Richter gathers them: each one the game's pickup blip (its sound effect
 * $0B, sfx_request). */
#define ORB_CELLS 0x2E934
#define ORB_FADE 0x191461
#define ORB_FADE_STEP 0x13
#define ORB_FADES 7
#define ORB_PAL 0x19156B
#define SPARK_CELLS 0x104180
#define ORB_ART_CRC 0xC8D3A101u
#define ORB_WHITE_T 42         /* frames white, fading to red */
#define PICKUP_SFX 0x0B
#define WON_FADE 60            /* and the room fades in */
#define THUNDER_SOFTEN 0
static const Piece GAUGE[] = {{16, 32, 0x1F9, 0x3000}, {16, 0, 0x1FC, 0x1000}, {0, 0, 0x1F8, 0x1000}};
/* the castle approach's standing torches: cells 0xB0-0xBB (with the wall's tiles), the flame in
 * its palette 9, the stand in its 14 */
#define TORCH_CELLS (WALL_TILES + 0xB0 * 128)
#define TORCH_PAL_FLAME 0x1915A2
#define TORCH_PAL_STAND 0x191364
#define TORCH_CRC 0xAF8C6D51u
static const uint16_t FLAME_CELLS[4] = {0xB1, 0xB3, 0xB4, 0xBB};
static const int TORCH_ARENA_X[2] = {44, 236};

static struct {
    int state, t;
    int loaded;                       /* 0 not yet, 1 ready, -1 not there */
    const TitleArt *arts;
    int n_arts;
    uint32_t sky[2][ARENA_H * ARENA_W];   /* the arena: its lightning's palette off and on */
    uint8_t solid[ARENA_H * ARENA_W];     /* (the wall's pixels: the sky is behind them) */
    uint8_t bolt[7 * 128], blood[8 * 128], gauge[8 * 128], torch[12 * 128];
    uint32_t torch_pal[2][16];
    uint32_t bolt_pal[1][16], blood_pal[1][16], gauge_pal[1][16];
    struct { float x, y, vx, vy; int age; bool on; } drop[24];
    /* the music: the stage's, kept while the fight's plays */
    bool music_ours;
    int music_start, music_end;
    int64_t music_pos;
    bool music_repeat, music_playing;
    uint8_t thunder[THUNDER_LEN];
    uint8_t yell[MARIA_YELL_LEN];
    bool have_yell;
    uint8_t death_yell[MARIA_DEATH_LEN];
    bool have_death_yell;
    uint8_t death_cry[DEATH_CRY_LEN];   /* Death's own, as he is struck down */
    bool have_death_cry;
    /* the finale: from her end (frames, -1 none); the orbs raining; the fade back in, won */
    int finale_t, won_t;
    int fade_at;               /* (the finale's fade out: its start, -1 until every orb is gathered) */
    struct { float x, y, vx, vy; int age, spark; bool on; } orb[ORBS];
    uint8_t orb_cells[8 * 128], spark_cells[4 * 128];
    uint32_t orb_pal[ORB_FADES + 1][16], spark_pal[1][16];
    bool have_orb_art;
    int gathered;
    bool have_thunder;
    /* the lightning: a strike now and then (7-15 s after the last), the bolt
     * somewhere over the sky, the game's thunder after it */
    int strike_at, next_strike, strike_dx;
    bool strike_flip;
    /* the second phase: the moonlit fade (0-256), the ledges (their tiles and palette, how far
     * they have come in: 0-256, solid at 256), the bolt that strikes Maria (its points) */
    int dim, ledge_a;
    bool strike_maria;
    int16_t bolt_pts[16][2];
    int n_bolt_pts;
    uint8_t stone[128];
    uint32_t stone_pal[1][16];
    uint32_t torch_dim[2][16];
    /* the map entries the ledges' floor replaced (restored when they go) */
    uint16_t bat_saved[3 * 2 * 6];
    int bat_at[3 * 2 * 6];
    int n_bat;
} fs;

/* The second phase's ledges: the castle stone block of Stage 1's stair room (area $10 room 5:
 * tiles $124, $125 over $134, $135, palette 6), three ledges of three blocks, solid (floor
 * tiles written into the game's map under them: the game's collision reads the map). Arena
 * x on the map's 8-px grid (the arena's x 0 is level X 698). */
#define STONE_TILES 0x278A80   /* $124, $125; $134, $135 at + $200 */
#define STONE_PAL 0x28949D
#define STONE_CRC 0xB66B192Cu
#define LEDGES 3
static const int16_t LEDGE_X[LEDGES] = {70, 174, 278}, LEDGE_Y[LEDGES] = {165, 130, 165};
#define LEDGE_BLOCKS 3
#define P2_TRACK 13            /* the second phase's music */
static void music_track(Pce *p, int track);

void secret_set_title_arts(const TitleArt *arts, int n)
{
    fs.arts = arts;
    fs.n_arts = n;
}

/* any length from the data track */
static bool disc_span(Pce *p, uint32_t off, uint32_t len, uint8_t *out)
{
    while (len) {
        uint32_t k = len > 4096 ? 4096 : len;
        if (!disc_bytes(p, off, k, out)) return false;
        off += k, out += k, len -= k;
    }
    return true;
}

/* the game's run-length scheme into VRAM (6C:81CD): $00 n hi: n bytes, each with that high
 * byte; $01-$7F: a word that many times; $80-$FC: that many words (& $7F); $FE lo hi: the
 * address from the stream's base; $FF: the end */
static void unpack(const uint8_t *s, uint32_t n, uint16_t base, uint8_t flag, uint16_t *vram)
{
    uint32_t p = 0;
    uint16_t a = base;
    while (p < n) {
        uint8_t op = s[p];
        if (op == 0xFF || op == 0xFD) return;
        if (op == 0) {
            if (p + 3 > n) return;
            int k = s[p + 1];
            uint8_t hi = s[p + 2];
            for (int i = 0; i < k && p + 3 + i < n; i++) vram[a++ & 0x7FFF] = (uint16_t)(s[p + 3 + i] | hi << 8);
            p += 3 + (uint32_t)k;
        } else if (op < 0x80) {
            if (p + 3 > n) return;
            uint16_t w = (uint16_t)(s[p + 1] | s[p + 2] << 8);
            for (int i = 0; i < op; i++) vram[a++ & 0x7FFF] = w;
            p += 3;
        } else if (op == 0xFE) {
            if (p + 3 > n) return;
            int lo = s[p + 1] + (base & 0xFF), hi = s[p + 2] + (base >> 8) + (lo >> 8);
            if (!(flag & 1)) hi &= 0x0F;
            a = (uint16_t)((hi & 0xFF) << 8 | (lo & 0xFF));
            p += 3;
        } else {
            int k = op & 0x7F;
            for (int i = 0; i < k && p + 2 + 2 * i < n; i++, a++)
                vram[a & 0x7FFF] = (uint16_t)(s[p + 1 + 2 * i] | s[p + 2 + 2 * i] << 8);
            p += 1 + 2 * (uint32_t)k;
        }
    }
}

/* a background tile's pixel (colour 0-15): 4 planes, 8 rows */
static int tile_px(const uint16_t *t, int x, int y)
{
    int b = 7 - x;
    return (t[y] >> b & 1) | (t[y] >> (8 + b) & 1) << 1 | (t[8 + y] >> b & 1) << 2 | (t[8 + y] >> (8 + b) & 1) << 3;
}


/* ---- Maria, the arena's boss -------------------------------------------------------------- */
/* Maria fights as the game draws her when she is played: her own sprites (her poses, read from
 * the disc: where each piece of each is, as the game's sprite table has it), moving as she does
 * (walk, jump and somersault, crouch and slide, her doves). The host plays her: an AI picks her
 * moves by Richter's distance, dodges his whip now and then, and keeps to the arena. Her hits
 * on Richter are the game's own (the player's hit, as an enemy's touch makes it: its type, its
 * damage, which way he is knocked: the game's code does the rest, his flinch, blinking and
 * health); his whip and sub-weapons hit her (her gauge empties). Facing left as stored; drawn
 * mirrored facing right. Arena coordinates: x 0-395 across the picture, y the picture's. */
/* her poses: the game's animation frames she uses (MP_xx is frame $xx) */
enum { MP_01, MP_02, MP_03, MP_08, MP_09, MP_0A, MP_0B, MP_0C, MP_0D, MP_0E, MP_0F, MP_10, MP_11, MP_12,
       MP_13, MP_14, MP_15, MP_17, MP_18, MP_19, MP_1A, MP_1B, MP_1C, MP_1D, MP_1F, MP_20, MP_25, MP_26,
       MP_27, MP_29, MP_2A, MP_2B, MP_33, MP_34, MP_35, MP_COUNT };
static const uint8_t MP_FRAME[MP_COUNT] = {
    0x01, 0x02, 0x03, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
    0x13, 0x14, 0x15, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1F, 0x20, 0x25, 0x26,
    0x27, 0x29, 0x2A, 0x2B, 0x33, 0x34, 0x35};
/* The game's tables for her, read from the disc:
 *  - her sprite frames (as at $9000): per frame a pointer to [count][5-byte pieces: flags (bit 0
 *    32 wide, bits 4-5 the height, bit 3 flipped), y, x (facing right), pattern low, pattern high
 *    in bits 4-6];
 *  - her cell uploads (as at $9800): per frame a pointer to a list of [bank][flags][VRAM address]
 *    [source address][32-byte blocks, when flags bit 7], $FF ending it; frames from 3 on reach
 *    their list through a byte table (MARIA_REMAP), frames 0-2 directly;
 *  - her cells (MARIA_GFX, the uploads' source: bank $1C at its start). */
#define MARIA_FRAMES 0xF2800
#define MARIA_FRAMES_LEN 0x400
#define MARIA_FRAMES_CRC 0x4CF29EECu
#define MARIA_LISTS 0xF3C00
#define MARIA_LISTS_LEN 0x400
#define MARIA_LISTS_CRC 0xEDBFC6EAu
#define MARIA_REMAP 0xF09A5
#define MARIA_REMAP_LEN 0x36
#define MARIA_REMAP_CRC 0xE369A5BCu
#define MP_MAXP 96
typedef struct { int8_t x, y; uint16_t attr; uint8_t n; uint16_t cell[8]; } MPiece;
static MPiece mpieces[MP_MAXP];
static uint16_t mpose_first[MP_COUNT + 1];
#define MARIA_GFX 0xF8800
#define MARIA_GFX_LEN 18432
#define MARIA_GFX_CRC 0xBEB819AEu
#define DOVE_CELLS 0x105800        /* 3 cells: the wings' beats */
#define DOVE_PAL 0x129392
#define DOVE_CRC 0x6B48488Cu
#define MARIA_FEET 22              /* her feet below her position */
#define MARIA_HP 32
#define MARIA_HP2 (MARIA_HP * 3 / 2)   /* the second phase's gauge: half as much again */
#define WHIP_DAMAGE 2
#define WEAPON_DAMAGE 3

enum { M_INTRO, M_IDLE, M_WALK, M_THROW, M_JUMP, M_CROUCH, M_SLIDE, M_HURT, M_DOWN };
/* The second phase: her gauge filled again, afterimages trailing her (fading magenta echoes
 * of where she was 6, 12 and 18 frames before), her doves thrown two at a time a second
 * apart. */
#define TRAIL 20
#define REFILL_T 90               /* frames her gauge takes to fill again */
#define DOVE_GAP 60               /* frames between the two doves of the second phase */
/* Her sub-weapons in the second phase, as she has them when she is played (her items, bank $82
 * of her CD-RAM, the data track at $F4800 on; her uploads list them at 6C:9C3E):
 *  - the cat: 30 cells from $1B0, packed as the game packs VRAM uploads (6C:81CD), with its
 *    palette at $F4121: running $1B0/$1B2/$1B4/$1B6 (32 x 16), leaping $1BC, pouncing $1B8
 *    (32 x 32); it leaps from her hand, runs at Richter along the ground and springs at him;
 *  - the red birds: her doves' cells in the birds' palette ($F4147), two at once, up and out
 *    to either side, then each turning on Richter.
 * Her cry with them is in her sound bank (record $601: $146E on, $355 bytes, rate code 8).
 * Their touch hurts him; the whip (or a sub-weapon of his) strikes them down. */
#define CAT_PACKED 0xF4800
#define CAT_PACKED_LEN 1036
#define CAT_PAL 0xF4121
#define CAT_CRC 0xB2039C28u        /* (the packed cells and the palette) */
#define CAT_CELLS 30
#define RED_PAL 0xF4147
#define RED_PAL_CRC 0x6EDE936Au
#define SUB_CRY_AT (0x601 * 2048 + 0x146E)
#define SUB_CRY_LEN 0x355
#define SUB_CRY_CRC 0x79B54028u
#define SUB_CHANCE 40             /* in 100: a throw of the second phase is a sub-weapon */
#define SUB_WAIT 300              /* frames before the next sub-weapon */
#define STRIKE2_T 36              /* the second strike, frames after the first */
#define STRIKE_HOLD 16            /* frames a strike holds her (white, shaking) and its bolt shows */
static const uint32_t ECHO_RGB = 0xC828DCu;   /* (200, 40, 220) */
static const uint8_t WALK_CYCLE[8] = {MP_09, MP_0A, MP_0B, MP_0C, MP_0D, MP_0E, MP_0F, MP_08};
static const uint8_t FLIP_CYCLE[6] = {MP_10, MP_12, MP_13, MP_14, MP_15, MP_18};

static struct {
    uint8_t gfx[MARIA_GFX_LEN], dove[3 * 128];
    uint32_t dove_pal[1][16];
    float x, y, vx, vy;       /* her feet, arena coordinates */
    bool right;               /* facing right */
    int state, t, pose, hp, inv, flash;
    bool second;              /* the somersault (second jump) done */
    bool fall;                /* off a ledge's edge: falling, no somersault */
    int shake;                /* drawn this far over (struck by the lightning) */
    int walk_for, throws, dir;
    int dove_wait;            /* frames before she may throw again */
    int phase;                /* 1, then 2 once her gauge has been emptied and filled again */
    struct { float x, y; uint8_t pose; bool right; } trail[TRAIL];   /* (her last frames, for
                              * the second phase's afterimages; trail_at the newest) */
    int trail_at;
    int flinch_wait;          /* frames before a hit makes her flinch again */
    int org, fb0;             /* the picture's, last frame */
    struct { float x, y, vx, vy; int t; bool on; } dove_[4];
    /* the second phase's sub-weapons (subw: the throw's, 0 doves, 1 the cat, 2 the birds) */
    uint8_t cat_cells[CAT_CELLS * 128], sub_cry[SUB_CRY_LEN];
    uint32_t cat_pal[1][16], red_pal[1][16];
    bool have_cat, have_red, have_sub_cry;
    int subw, sub_wait;
    struct { float x, y, vx, vy; int t, mode, hit; bool on; } cat;   /* mode 0 leap, 1 run, 2 pounce */
    struct { float x, y, vx, vy; int t, hit; bool on; } red[2];
} mf;

static void maria_reset(void)
{
    mf.x = MARIA_ARENA_X + 8;
    mf.y = ARENA_FLOOR;
    mf.vx = mf.vy = 0;
    mf.right = false;
    mf.state = M_INTRO;
    mf.t = 0;
    mf.pose = MP_01;
    mf.hp = MARIA_HP;
    mf.inv = mf.flash = 0;
    mf.dove_wait = mf.flinch_wait = 0;
    memset(mf.dove_, 0, sizeof mf.dove_);
    memset(&mf.cat, 0, sizeof mf.cat);
    memset(mf.red, 0, sizeof mf.red);
    mf.subw = mf.sub_wait = 0;
    mf.phase = 1;
    mf.fall = false;
    mf.shake = 0;
    memset(mf.trail, 0, sizeof mf.trail);
    mf.trail_at = -1;
}

/* The game's packed VRAM uploads (6C:81CD), unpacked into `out` (VRAM words from `base` on,
 * `cap` bytes): per control byte, $FF the end; $00 n, high byte, n low bytes (n words sharing
 * a high byte); $FE offset (the write address moved to base + offset); $80 + n, n words as
 * they are; 1-$7F n, a word written n times. (Not $FD: another routine.) */
static bool vram_unpack(const uint8_t *in, int len, int base, uint8_t *out, int cap)
{
    int pos = 0, a = base;
    memset(out, 0, (size_t)cap);
#define PUT(w) do { int o_ = (a - base) * 2; if (o_ >= 0 && o_ + 1 < cap) out[o_] = (uint8_t)(w), out[o_ + 1] = (uint8_t)((w) >> 8); a++; } while (0)
    while (pos < len) {
        int c = in[pos];
        if (c == 0xFF) return true;
        if (c == 0x00) {
            if (pos + 3 > len) return false;
            int n = in[pos + 1], hi = in[pos + 2];
            if (pos + 3 + n > len) return false;
            for (int k = 0; k < n; k++) PUT(in[pos + 3 + k] | hi << 8);
            pos += 3 + n;
        } else if (c == 0xFE) {
            if (pos + 3 > len) return false;
            a = base + (in[pos + 1] | in[pos + 2] << 8);
            pos += 3;
        } else if (c == 0xFD) {
            return false;
        } else if (c & 0x80) {
            int n = c & 0x7F;
            if (pos + 1 + 2 * n > len) return false;
            for (int k = 0; k < n; k++) PUT(in[pos + 1 + 2 * k] | in[pos + 2 + 2 * k] << 8);
            pos += 1 + 2 * n;
        } else {
            if (pos + 3 > len) return false;
            int w = in[pos + 1] | in[pos + 2] << 8;
            for (int k = 0; k < c; k++) PUT(w);
            pos += 3;
        }
    }
#undef PUT
    return false;
}

/* her poses from the game's tables: each piece's place, size and flip, and which of her cells
 * (128 bytes each, from MARIA_GFX) the frame's uploads put in the VRAM cells it shows */
static bool maria_poses(const uint8_t *fr, const uint8_t *ls, const uint8_t *rm)
{
    int np = 0;
    for (int k = 0; k < MP_COUNT; k++) {
        int f = MP_FRAME[k], vram[16];
        for (int i = 0; i < 16; i++) vram[i] = -1;
        int la = f < 3 ? 0x19 + 2 * f : 0x1F + 2 * rm[f];
        if (f >= MARIA_REMAP_LEN || la + 1 >= MARIA_LISTS_LEN) return false;
        int a = (ls[la] | ls[la + 1] << 8) - 0x9800;
        for (;;) {
            if (a < 0 || a >= MARIA_LISTS_LEN) return false;
            if (ls[a] == 0xFF) break;
            if (a + 6 > MARIA_LISTS_LEN) return false;
            int bank = ls[a], fl = ls[a + 1], v = ls[a + 2] | ls[a + 3] << 8, src = ls[a + 4] | ls[a + 5] << 8, cnt = 0;
            a += 6;
            if (fl & 0x80) {
                if (a >= MARIA_LISTS_LEN) return false;
                cnt = ls[a++];
            }
            long off = (long)(bank - 0x1C) * 0x2000 + (src - 0xA000);
            for (int c = 0; c < cnt / 4; c++) {
                int cell = v / 64 + c - 0x180;
                long o = off + c * 128L;
                if (cell < 0 || cell >= 16 || o < 0 || o % 128 || o + 128 > MARIA_GFX_LEN) return false;
                vram[cell] = (int)(o / 128);
            }
        }
        int pa = (fr[2 * f] | fr[2 * f + 1] << 8) - 0x9000;
        if (pa < 0 || pa >= MARIA_FRAMES_LEN) return false;
        int n = fr[pa];
        if (pa + 1 + 5 * n > MARIA_FRAMES_LEN || np + n > MP_MAXP) return false;
        mpose_first[k] = (uint16_t)np;
        for (int i = 0; i < n; i++) {
            const uint8_t *e = fr + pa + 1 + 5 * i;
            int cw = e[0] & 1 ? 2 : 1, cg = e[0] >> 4 & 3, ch = cg == 0 ? 1 : cg == 1 ? 2 : 4;
            int pat = e[3] | (e[4] >> 4 & 7) << 8, base = (pat >> 1) & ~((cw - 1) | (ch - 1) << 1);
            MPiece *m = &mpieces[np++];
            m->x = (int8_t)-((int8_t)e[2] + cw * 16);          /* facing left */
            m->y = (int8_t)e[1];
            m->attr = (uint16_t)((cw == 2 ? 0x100 : 0) | cg << 12 | (e[0] & 8 ? 0 : 0x800));
            m->n = (uint8_t)(cw * ch);
            for (int c = 0; c < cw * ch; c++) {
                int v = base + c % cw + c / cw * 2 - 0x180;
                if (v < 0 || v >= 16 || vram[v] < 0) return false;
                m->cell[c] = (uint16_t)vram[v];
            }
        }
    }
    mpose_first[MP_COUNT] = (uint16_t)np;
    return true;
}

static bool maria_load(Pce *p)
{
    uint8_t dv[3 * 128 + 18], fr[MARIA_FRAMES_LEN], ls[MARIA_LISTS_LEN], rm[MARIA_REMAP_LEN];
    if (!disc_span(p, MARIA_GFX, MARIA_GFX_LEN, mf.gfx) || crc32_of(mf.gfx, MARIA_GFX_LEN) != MARIA_GFX_CRC) return false;
    if (!disc_span(p, MARIA_FRAMES, sizeof fr, fr) || crc32_of(fr, sizeof fr) != MARIA_FRAMES_CRC ||
        !disc_span(p, MARIA_LISTS, sizeof ls, ls) || crc32_of(ls, sizeof ls) != MARIA_LISTS_CRC ||
        !disc_span(p, MARIA_REMAP, sizeof rm, rm) || crc32_of(rm, sizeof rm) != MARIA_REMAP_CRC ||
        !maria_poses(fr, ls, rm))
        return false;
    if (!disc_span(p, DOVE_CELLS, 3 * 128, dv) || !disc_span(p, DOVE_PAL, 18, dv + 3 * 128) || crc32_of(dv, sizeof dv) != DOVE_CRC)
        return false;
    memcpy(mf.dove, dv, 3 * 128);
    unpack_pal(dv + 3 * 128, mf.dove_pal[0]);
    /* (her sub-weapons: without them, doves only) */
    {
        uint8_t pk[CAT_PACKED_LEN + 18], rp[18];
        mf.have_cat = disc_span(p, CAT_PACKED, CAT_PACKED_LEN, pk) && disc_span(p, CAT_PAL, 18, pk + CAT_PACKED_LEN) &&
                      crc32_of(pk, sizeof pk) == CAT_CRC &&
                      vram_unpack(pk, CAT_PACKED_LEN, 0x6C00, mf.cat_cells, sizeof mf.cat_cells);
        if (mf.have_cat) unpack_pal(pk + CAT_PACKED_LEN, mf.cat_pal[0]);
        mf.have_red = disc_span(p, RED_PAL, 18, rp) && crc32_of(rp, 18) == RED_PAL_CRC;
        if (mf.have_red) unpack_pal(rp, mf.red_pal[0]);
        mf.have_sub_cry = disc_span(p, SUB_CRY_AT, SUB_CRY_LEN, mf.sub_cry) && crc32_of(mf.sub_cry, SUB_CRY_LEN) == SUB_CRY_CRC;
    }
    return true;
}

/* a pose at screen x, y (her position), mirrored when facing right */
/* (a pixel blended over what is there: the afterimages) */
static void blend(const Canvas *cv, int x, int y, uint32_t rgb, int alpha)
{
    int fx = x + cv->org;
    if (fx < 0 || fx >= cv->w || y < 0 || y >= cv->h) return;
    if (cv->spr[y * PCE_FB_MAX_W + fx]) return;
    uint32_t *d = &cv->fb[y * PCE_FB_MAX_W + fx], o = *d, c = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        int a = (int)(o >> sh & 0xFF), b = (int)(rgb >> sh & 0xFF);
        c |= (uint32_t)(a + (b - a) * alpha / 255) << sh;
    }
    *d = c;
}

static void draw_mpose_as(const Canvas *cv, int pose, int x, int y, bool right, bool white, int echo);
static void draw_mpose(const Canvas *cv, int pose, int x, int y, bool right, bool white)
{
    draw_mpose_as(cv, pose, x, y, right, white, 0);
}

/* echo: 0 herself; else her form in ECHO_RGB, that see-through (alpha 1-255) */
static void draw_mpose_as(const Canvas *cv, int pose, int x, int y, bool right, bool white, int echo)
{
    for (int i = mpose_first[pose + 1] - 1; i >= (int)mpose_first[pose]; i--) {
        const MPiece *e = &mpieces[i];
        int cgy = (e->attr >> 12) & 3, cw = e->attr & 0x100 ? 2 : 1, ch = cgy == 0 ? 1 : cgy == 1 ? 2 : 4;
        int sw = cw * 16, sh = ch * 16;
        bool xf = ((e->attr & 0x800) != 0) != right, yf = (e->attr & 0x8000) != 0;
        int ox = right ? -(e->x + sw) : e->x;
        for (int yy = 0; yy < sh; yy++)
            for (int xx = 0; xx < sw; xx++) {
                int sx = xf ? sw - 1 - xx : xx, sy = yf ? sh - 1 - yy : yy;
                uint16_t c = e->cell[(sy >> 4) * cw + (sx >> 4)];
                if (c == 0xFFFF) continue;
                int v = cell_px(mf.gfx + c * 128, sx & 15, sy & 15);
                int px = x + ox + xx, py = y + e->y + yy;
                if (!v || DITHER[py & 3][px & 3] < g_dissolve) continue;
                if (echo) blend(cv, px, py, ECHO_RGB, echo);
                else plot(cv, px, py, white ? 0xFFFFFFFFu : sc.maria_pal[0][v]);
            }
    }
}

/* Richter in arena coordinates (his feet), from the game's (screen) */
static void richter_at(Pce *p, float *x, float *y)
{
    int sx = (int16_t)(p->wram[W_PX_HI] << 8 | p->wram[W_PX]), sy = (int16_t)(p->wram[W_PY_HI] << 8 | p->wram[W_PY]);
    *x = (float)(sx + mf.org - mf.fb0);
    *y = (float)(sy + MARIA_FEET);
}

/* the game's own hit on the player: type 2, `dmg`, knocked away from x (as an enemy's touch
 * makes it: 6B:9855); not while he is already hit or blinking */
static void hurt_richter(Pce *p, int dmg, float from_x)
{
    if (p->wram[0x13B0] || p->wram[0x1460]) return;
    float rx, ry;
    richter_at(p, &rx, &ry);
    p->wram[0x13B0] = 2;
    p->wram[0x12A8] = (uint8_t)dmg;
    p->wram[0x1778] = from_x > rx ? 1 : 0;   /* (0: knocked right, 1: left) */
}

static bool overlap(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1)
{
    return ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1;
}

/* a ledge's top on the screen: on the map's 8-px rows, where its floor in the map stands (the
 * row its place falls in) */
static int ledge_top(int l)
{
    return LEDGE_Y[l] + 1 - (vdc_row_map_y()[LEDGE_Y[l] + 1] & 7);
}

/* the ground under her at x, from y on down: a ledge's top (once they are solid) or the floor */
static float ground_at(float x, float y)
{
    float g = (float)ARENA_FLOOR;
    if (fs.ledge_a >= 256)
        for (int l = 0; l < LEDGES; l++)
            if (x >= LEDGE_X[l] - 4 && x < LEDGE_X[l] + 16 * LEDGE_BLOCKS + 4 && ledge_top(l) >= y - 0.5f && ledge_top(l) < g)
                g = (float)ledge_top(l);
    return g;
}

/* her box: low while crouched or sliding */
static void maria_box(float *x0, float *y0, float *x1, float *y1)
{
    bool low = mf.state == M_SLIDE || mf.state == M_CROUCH;
    *x0 = mf.x - 8, *x1 = mf.x + 8, *y1 = mf.y, *y0 = mf.y - (low ? 20 : 44);
}

/* Richter's weapons on her: the whip's strike and his sub-weapons (his objects 4-15 with a box) */
static void maria_hits(Pce *p)
{
    if (mf.inv || mf.state == M_INTRO || mf.state == M_DOWN) return;
    float mx0, my0, mx1, my1;
    maria_box(&mx0, &my0, &mx1, &my1);
    for (int sl = 1; sl < 16; sl++) {
        if (!p->wram[0x0A10 + sl]) continue;
        int hw = p->wram[0x1988 + sl], hh = p->wram[0x1A38 + sl];
        if (!hw || !hh) continue;
        bool whip = p->wram[0x1828 + sl] == 5;   /* (its box is set only while it strikes: standing
                                                   * frames 05-07, crouching 0C-0E, in the air) */
        if (!whip && sl < 4) continue;
        int ox = (int16_t)(p->wram[W_PX_HI + sl] << 8 | p->wram[W_PX + sl]);
        int oy = (int16_t)(p->wram[W_PY_HI + sl] << 8 | p->wram[W_PY + sl]);
        int ahead = (int8_t)p->wram[0x1930 + sl], below = (int8_t)p->wram[0x19E0 + sl];
        float cx = (float)(ox + (whip && p->wram[0x0AC0] ? -ahead : ahead) + mf.org - mf.fb0), cy = (float)(oy + below);
        if (overlap(cx - hw, cy - hh, cx + hw, cy + hh, mx0, my0, mx1, my1)) {
            mf.hp -= whip ? WHIP_DAMAGE : WEAPON_DAMAGE;
            mf.inv = 40;
            mf.flash = 8;
            float rx, ry;
            richter_at(p, &rx, &ry);
            mf.dir = rx < mf.x ? 1 : -1;
            /* (a flinch now and then: hit again soon after, she only blinks and goes on;
             * in the air she only flashes) */
            if (mf.state != M_JUMP && !mf.flinch_wait) {
                mf.flinch_wait = 150;
                mf.state = M_HURT, mf.t = 0;
                mf.vx = 1.6f * (float)mf.dir;
            }
            if (mf.hp <= 0) mf.hp = 0, mf.state = M_DOWN, mf.t = 0, mf.vx = 0, mf.vy = 0;
            return;
        }
    }
}

/* away from Richter if there is room behind her; cornered, forward over him: a somersault
 * across (or now and then a slide past) */
#define CORNER 70
static void maria_escape(float rx)
{
    float away = rx < mf.x ? 1.0f : -1.0f;
    bool cornered = away > 0 ? mf.x > ARENA_W - CORNER : mf.x < CORNER;
    mf.t = 0;
    mf.second = false;
    if (!cornered) {
        mf.state = M_JUMP, mf.vy = -5.0f, mf.vx = 1.8f * away;
    } else if (rnd(3)) {
        mf.state = M_JUMP, mf.vy = -5.4f, mf.vx = -2.6f * away;   /* (over him) */
        mf.right = away < 0;
    } else {
        mf.state = M_CROUCH, mf.dir = away > 0 ? -1 : 1;           /* (a slide past him) */
    }
}

static void maria_choose(Pce *p)
{
    float rx, ry;
    richter_at(p, &rx, &ry);
    float d = rx - mf.x, ad = d < 0 ? -d : d;
    mf.right = d > 0;
    int r = rnd(100);
    mf.t = 0;
    /* (her doves spaced out: not while one is flying or soon after the last) */
    bool flying = false;
    for (int i = 0; i < 4; i++) flying |= mf.dove_[i].on;

    if (ad > 150) {
        if (r < 60) mf.state = M_WALK, mf.walk_for = 30 + rnd(40), mf.dir = d > 0 ? 1 : -1;
        else mf.state = M_THROW, mf.throws = 1;
    } else if (ad > 80) {
        if (r < 45) mf.state = M_THROW, mf.throws = 1;
        else if (r < 70) mf.state = M_CROUCH, mf.dir = d > 0 ? 1 : -1;
        else mf.state = M_JUMP, mf.vy = -5.0f, mf.vx = (d > 0 ? 1.6f : -1.6f), mf.second = false;
    } else {
        if (r < 40) maria_escape(rx);
        else if (r < 70) mf.state = M_CROUCH, mf.dir = d > 0 ? 1 : -1;
        else mf.state = M_THROW, mf.throws = 1;
    }
    if (mf.state == M_THROW && mf.phase == 2) mf.throws = 2;
    /* (the second phase: now and then a sub-weapon instead, one at a time) */
    mf.subw = 0;
    if (mf.state == M_THROW && mf.phase == 2 && !mf.sub_wait && !mf.cat.on && !mf.red[0].on && !mf.red[1].on &&
        (mf.have_cat || mf.have_red) && rnd(100) < SUB_CHANCE) {
        mf.subw = !mf.have_red ? 1 : !mf.have_cat ? 2 : 1 + rnd(2);
        mf.throws = 1;
    }
    if (mf.state == M_THROW && !mf.subw && (mf.dove_wait || flying)) {   /* (too soon: in close instead) */
        if (ad > 120) mf.state = M_WALK, mf.walk_for = 30 + rnd(40), mf.dir = d > 0 ? 1 : -1;
        else mf.state = M_CROUCH, mf.dir = d > 0 ? 1 : -1;
    }
}

/* a dove thrown from her hand toward Richter */
static void throw_dove(Pce *p)
{
    float rx, ry;
    richter_at(p, &rx, &ry);
    for (int i = 0; i < 4; i++)
        if (!mf.dove_[i].on) {
            mf.dove_[i].on = true;
            mf.dove_[i].t = 0;
            mf.dove_[i].x = mf.x + (mf.right ? 12.0f : -12.0f);
            mf.dove_[i].y = mf.y - 22;   /* (waist high, straight: to be jumped) */
            mf.dove_[i].vx = mf.right ? 2.2f : -2.2f;
            mf.dove_[i].vy = 0;
            return;
        }
}

/* her sub-weapon, from her hand */
static void throw_sub(Pce *p)
{
    float rx, ry;
    richter_at(p, &rx, &ry);
    float side = mf.right ? 1.0f : -1.0f;
    if (mf.have_sub_cry) sound_extra_play_at(mf.sub_cry, SUB_CRY_LEN, 8, 256, 0);
    mf.sub_wait = SUB_WAIT;
    if (mf.subw == 1) {   /* the cat: a leap out ahead of her */
        memset(&mf.cat, 0, sizeof mf.cat);
        mf.cat.on = true;
        mf.cat.x = mf.x + 12 * side, mf.cat.y = mf.y - 16;
        mf.cat.vx = 2.2f * side, mf.cat.vy = -2.6f;
    } else {              /* the birds: up and out, one each way */
        for (int i = 0; i < 2; i++) {
            float s2 = i ? -side : side;
            memset(&mf.red[i], 0, sizeof mf.red[i]);
            mf.red[i].on = true;
            mf.red[i].x = mf.x + 8 * s2, mf.red[i].y = mf.y - 30;
            mf.red[i].vx = 1.7f * s2, mf.red[i].vy = -2.1f;
        }
    }
}

/* Richter's whip or a sub-weapon of his on a box (arena coordinates) */
static bool richter_strikes(Pce *p, float x0, float y0, float x1, float y1)
{
    for (int sl = 1; sl < 16; sl++) {
        if (!p->wram[0x0A10 + sl]) continue;
        int hw = p->wram[0x1988 + sl], hh = p->wram[0x1A38 + sl];
        if (!hw || !hh) continue;
        bool whip = p->wram[0x1828 + sl] == 5;
        if (!whip && sl < 4) continue;
        int ox = (int16_t)(p->wram[W_PX_HI + sl] << 8 | p->wram[W_PX + sl]);
        int oy = (int16_t)(p->wram[W_PY_HI + sl] << 8 | p->wram[W_PY + sl]);
        int ahead = (int8_t)p->wram[0x1930 + sl], below = (int8_t)p->wram[0x19E0 + sl];
        float cx = (float)(ox + (whip && p->wram[0x0AC0] ? -ahead : ahead) + mf.org - mf.fb0), cy = (float)(oy + below);
        if (overlap(cx - hw, cy - hh, cx + hw, cy + hh, x0, y0, x1, y1)) return true;
    }
    return false;
}

/* the sub-weapons' moves (before a frame) */
static void subs_step(Pce *p, float rx, float ry)
{
    if (mf.sub_wait) mf.sub_wait--;
    if (mf.cat.on) {
        float was = mf.cat.y;
        mf.cat.t++;
        if (mf.cat.hit) {   /* (struck: it flashes and is gone) */
            if (++mf.cat.hit > 14) mf.cat.on = false;
        } else {
            if (mf.cat.mode == 1) {   /* (running: at him, springing once close) */
                float d = rx - mf.cat.x;
                if ((d > 0) == (mf.cat.vx > 0) && (d < 0 ? -d : d) < 44) mf.cat.mode = 2, mf.cat.vy = -3.4f, mf.cat.vx *= 1.4f;
            }
            mf.cat.x += mf.cat.vx;
            mf.cat.y += mf.cat.vy;
            if (mf.cat.mode != 1) mf.cat.vy += 0.2f;
            else {
                float g = ground_at(mf.cat.x, was);
                if (g > mf.cat.y + 0.5f) mf.cat.mode = 0;   /* (off a ledge's edge) */
            }
            float g = mf.cat.vy >= 0 ? ground_at(mf.cat.x, was) : (float)ARENA_FLOOR;
            if (mf.cat.mode != 1 && mf.cat.vy >= 0 && mf.cat.y >= g) {
                mf.cat.y = g, mf.cat.vy = 0, mf.cat.mode = 1;
                mf.cat.vx = mf.cat.vx > 0 ? 2.2f : -2.2f;
            }
            if (mf.cat.x < -24 || mf.cat.x > ARENA_W + 24 || mf.cat.t > 400) mf.cat.on = false;
            float h = mf.cat.mode == 2 ? 28.0f : 14.0f;
            if (mf.cat.on && overlap(rx - 7, ry - 44, rx + 7, ry, mf.cat.x - 12, mf.cat.y - h, mf.cat.x + 12, mf.cat.y))
                hurt_richter(p, 6, mf.cat.x);
            if (mf.cat.on && richter_strikes(p, mf.cat.x - 12, mf.cat.y - h, mf.cat.x + 12, mf.cat.y)) mf.cat.hit = 1;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (!mf.red[i].on) continue;
        mf.red[i].t++;
        if (mf.red[i].hit) {
            if (++mf.red[i].hit > 14) mf.red[i].on = false;
            continue;
        }
        if (mf.red[i].t > 28) {   /* (then turning on him, a little at a time) */
            float dx = rx - mf.red[i].x, dy = (ry - 26) - mf.red[i].y, d = sqrtf(dx * dx + dy * dy);
            if (d < 1) d = 1;
            mf.red[i].vx += (dx / d * 2.6f - mf.red[i].vx) * 0.06f;
            mf.red[i].vy += (dy / d * 2.6f - mf.red[i].vy) * 0.06f;
        } else {
            mf.red[i].vy += 0.05f;
        }
        mf.red[i].x += mf.red[i].vx;
        mf.red[i].y += mf.red[i].vy;
        if (mf.red[i].x < -24 || mf.red[i].x > ARENA_W + 24 || mf.red[i].y > ARENA_FLOOR + 8 || mf.red[i].t > 360) {
            mf.red[i].on = false;
            continue;
        }
        if (overlap(rx - 7, ry - 44, rx + 7, ry, mf.red[i].x - 6, mf.red[i].y - 6, mf.red[i].x + 6, mf.red[i].y + 6)) {
            hurt_richter(p, 4, mf.red[i].x);
            mf.red[i].on = false;
        } else if (richter_strikes(p, mf.red[i].x - 7, mf.red[i].y - 7, mf.red[i].x + 7, mf.red[i].y + 7)) {
            mf.red[i].hit = 1;
        }
    }
}

/* ...drawn (after a frame) */
static void subs_draw(const Canvas *cv, int base)
{
    if (mf.cat.on && !(mf.cat.hit && (mf.cat.hit & 2))) {
        static const uint16_t RUN[4] = {0x1B0, 0x1B2, 0x1B4, 0x1B6};
        Piece pc;
        int h = 16;
        if (mf.cat.mode == 1) pc = (Piece){0, 0, RUN[(mf.cat.t / 5) & 3], 0x0100};
        else if (mf.cat.mode == 2) pc = (Piece){0, 0, 0x1B8, 0x1100}, h = 32;
        else pc = (Piece){0, 0, 0x1BC, 0x0100};
        draw_frame(cv, &pc, 1, mf.cat_cells, 0x1B0, 0xFFFF, NULL, mf.cat_pal, base + (int)mf.cat.x - 16, (int)mf.cat.y - h, 32,
                   mf.cat.vx < 0, mf.cat.hit > 0);
    }
    for (int i = 0; i < 2; i++)
        if (mf.red[i].on && !(mf.red[i].hit && (mf.red[i].hit & 2))) {
            Piece dv = {0, 0, (uint16_t)(0x1A0 + (mf.red[i].t / 4) % 2), (uint16_t)(mf.red[i].vx < 0 ? 0x0800 : 0x0000)};
            draw_frame(cv, &dv, 1, mf.dove, 0x1A0, 0xFFFF, NULL, mf.red_pal, base + (int)mf.red[i].x - 8, (int)mf.red[i].y - 8, 16,
                       false, mf.red[i].hit > 0);
        }
}

/* before a frame: her move, her doves, the hits */
static void maria_step(Pce *p)
{
    float rx, ry;
    richter_at(p, &rx, &ry);
    if (mf.inv) mf.inv--;
    if (mf.dove_wait) mf.dove_wait--;
    if (mf.flinch_wait) mf.flinch_wait--;
    if (mf.flash) mf.flash--;
    maria_hits(p);
    mf.t++;
    switch (mf.state) {
    case M_INTRO:
        mf.pose = MP_01;
        mf.right = rx > mf.x;
        if (mf.t > 20) maria_choose(p);   /* (straight into the fight) */
        break;
    case M_IDLE:
        mf.pose = (mf.t / 40) % 3 == 2 ? MP_02 : MP_01;
        mf.right = rx > mf.x;
        if (mf.t > ((rx - mf.x) * (rx - mf.x) < 80 * 80 ? 6 : 24 + rnd(30))) maria_choose(p);
        break;
    case M_WALK:
        mf.pose = WALK_CYCLE[(mf.t / 5) & 7];
        mf.right = mf.dir > 0;
        mf.x += 1.3f * (float)mf.dir;
        if (mf.t > mf.walk_for || (mf.dir > 0 ? rx - mf.x : mf.x - rx) < 70) mf.state = M_IDLE, mf.t = 0;
        break;
    case M_THROW:
        /* (t below 0: standing between two throws, facing him) */
        if (mf.t < 0) {
            mf.pose = MP_01;
            mf.right = rx > mf.x;
            break;
        }
        mf.pose = mf.t < 6 ? MP_29 : mf.t < 12 ? MP_2A : MP_2B;
        if (mf.t == 8) {
            if (mf.subw) throw_sub(p);
            else throw_dove(p), mf.dove_wait = 150;
        }
        if (mf.t >= 24) {
            if (--mf.throws > 0) mf.t = -(DOVE_GAP - 24);   /* (the next dove DOVE_GAP after) */
            else mf.state = M_IDLE, mf.t = 0;
        }
        break;
    case M_JUMP: {
        float was = mf.y;
        mf.x += mf.vx;
        mf.y += mf.vy;
        mf.vy += 0.25f;
        if (!mf.second && !mf.fall && mf.vy > -1.0f && rnd(3)) mf.second = true, mf.vy = -4.2f, mf.t = 0;   /* (the somersault) */
        if (mf.second) mf.pose = FLIP_CYCLE[mf.t / 4 < 5 ? mf.t / 4 : 5];
        else mf.pose = mf.vy < 0 ? MP_17 : MP_11;
        /* (landing: on the floor, or falling onto a ledge) */
        float g = mf.vy > 0 ? ground_at(mf.x, was) : (float)ARENA_FLOOR;
        if (mf.y >= g) {
            mf.y = g;
            mf.vx = mf.vy = 0;
            mf.fall = false;
            mf.state = M_CROUCH, mf.t = 100;   /* (a moment crouched on landing) */
        }
        if (mf.second && overlap(rx - 7, ry - 44, rx + 7, ry, mf.x - 10, mf.y - 40, mf.x + 10, mf.y)) hurt_richter(p, 8, mf.x);
        break;
    }
    case M_CROUCH:
        if (mf.t >= 100) {   /* (landing) */
            mf.pose = MP_1B;
            if (mf.t >= 108) mf.state = M_IDLE, mf.t = 0;
            break;
        }
        mf.pose = mf.t < 4 ? MP_19 : mf.t < 8 ? MP_1A : MP_1B;
        mf.right = mf.dir > 0;
        if (mf.t >= 14) mf.state = M_SLIDE, mf.t = 0, mf.vx = 3.4f * (float)mf.dir;
        break;
    case M_SLIDE:
        mf.pose = mf.t < 3 ? MP_1F : mf.t < 6 ? MP_20 : mf.t < 10 ? MP_25 : mf.t < 30 ? MP_26 : MP_27;
        mf.x += mf.vx;
        mf.vx *= 0.97f;
        if (overlap(rx - 7, ry - 44, rx + 7, ry, mf.x - 14, mf.y - 16, mf.x + 14, mf.y)) hurt_richter(p, 8, mf.x);
        if (mf.t >= 40) mf.state = M_IDLE, mf.t = 0;
        break;
    case M_HURT:
        mf.pose = mf.t < 6 ? MP_33 : mf.t < 12 ? MP_34 : MP_35;
        mf.x += mf.vx;
        mf.vx *= 0.9f;
        if (mf.t >= 20) maria_escape(rx);   /* (straight away from more hits) */
        break;
    case M_DOWN:
        mf.pose = MP_35;
        if (mf.phase == 1) {
            /* (her gauge empty: lightning strikes her twice, each with her yell, a white burst
             * from her and the thunder after it, the music changes; the wall and ground go
             * moonlit, the ledges come in; her gauge fills again, and the second phase begins) */
            if (mf.t == 1) music_track(p, P2_TRACK);
            if (mf.t == 1 || mf.t == STRIKE2_T) {
                fs.strike_at = fs.t;
                fs.strike_maria = true;
                fs.next_strike = fs.t + 600;
                if (fs.have_yell) sound_extra_play_at(fs.yell, MARIA_YELL_LEN, MARIA_YELL_RATE, 256, 0);
                int x = (int)mf.x + rnd(30) - 15, y = 0, n = 0;
                fs.bolt_pts[n][0] = (int16_t)x, fs.bolt_pts[n][1] = 0, n++;
                while (y < (int)mf.y - 40 && n < 15) {
                    y += 10 + rnd(10);
                    if (y > (int)mf.y - 40) y = (int)mf.y - 40;
                    x += rnd(21) - 10;
                    if (y == (int)mf.y - 40) x = (int)mf.x;
                    fs.bolt_pts[n][0] = (int16_t)x, fs.bolt_pts[n][1] = (int16_t)y, n++;
                }
                fs.n_bolt_pts = n;
            }
            /* (struck: white and shaking; after, flashing as her gauge fills) */
            int lt = fs.t - fs.strike_at;
            if (lt >= 0 && lt < STRIKE_HOLD) {
                mf.flash = 2;
                mf.shake = (lt & 2) ? 2 : -2;
            } else {
                mf.shake = 0;
                mf.flash = (mf.t / 4) & 1 ? 2 : 0;
            }
            int t2 = mf.t - STRIKE2_T;   /* (all the rest from the second strike on) */
            fs.dim = t2 < 6 ? 0 : t2 >= 66 ? 256 : (t2 - 6) * 256 / 60;
            fs.ledge_a = t2 < 20 ? 0 : t2 >= 60 ? 256 : (t2 - 20) * 256 / 40;
            mf.hp = t2 <= 0 ? 0 : t2 * MARIA_HP2 / REFILL_T;
            if (mf.hp >= MARIA_HP2) {
                mf.hp = MARIA_HP2;
                mf.phase = 2;
                mf.flash = 0;
                mf.shake = 0;
                mf.inv = 30;
                mf.state = M_IDLE, mf.t = 0;
                mf.trail_at = -1;   /* (the echoes start from here) */
            }
        }
        else if (mf.t == 1 && fs.finale_t < 0) {   /* (the second phase's end: the finale) */
            fs.finale_t = 0;
            fs.fade_at = -1;
            if (fs.have_death_yell) sound_extra_play_at(fs.death_yell, MARIA_DEATH_LEN, MARIA_DEATH_RATE, 256, 0);
            memset(fs.orb, 0, sizeof fs.orb);
        }
        if (mf.phase == 2) {
            mf.flash = mf.t < 60 && ((mf.t >> 2) & 1) ? 2 : 0;
            mf.shake = mf.t < 70 ? ((mf.t >> 1) & 1 ? 2 : -2) : 0;
        }
        break;
    }
    /* off a ledge's edge (walking, sliding, knocked back): she falls */
    if (mf.state != M_JUMP && mf.state != M_INTRO && ground_at(mf.x, mf.y) > mf.y + 0.5f) {
        mf.state = M_JUMP, mf.vy = 0, mf.fall = true, mf.second = false, mf.t = 0;
        if (mf.vx == 0) mf.vx = 0.6f * (float)mf.dir;
    }
    /* the whip coming: now and then she jumps it */
    if ((mf.state == M_IDLE || mf.state == M_WALK) && p->wram[0x0EE0] == 0x03 && (rx - mf.x) * (rx - mf.x) < 90 * 90 && rnd(100) < 4)
        maria_escape(rx);
    if (mf.x < 14) mf.x = 14, mf.vx = 0;
    if (mf.x > ARENA_W - 14) mf.x = ARENA_W - 14, mf.vx = 0;
    /* touching her hurts, as an enemy does */
    if (mf.state != M_DOWN && mf.state != M_INTRO && mf.state != M_HURT &&
        overlap(rx - 7, ry - 44, rx + 7, ry, mf.x - 6, mf.y - 40, mf.x + 6, mf.y))
        hurt_richter(p, 4, mf.x);
    /* the trail (second phase) */
    if (mf.phase == 2) {
        int n = (mf.trail_at + 1) % TRAIL;
        if (mf.trail_at < 0)
            for (int i = 0; i < TRAIL; i++) mf.trail[i].x = mf.x, mf.trail[i].y = mf.y, mf.trail[i].pose = (uint8_t)mf.pose, mf.trail[i].right = mf.right;
        mf.trail[n].x = mf.x, mf.trail[n].y = mf.y, mf.trail[n].pose = (uint8_t)mf.pose, mf.trail[n].right = mf.right;
        mf.trail_at = n;
    }
    subs_step(p, rx, ry);
    /* her doves: flying straight on */
    for (int i = 0; i < 4; i++) {
        if (!mf.dove_[i].on) continue;
        mf.dove_[i].x += mf.dove_[i].vx;
        mf.dove_[i].y += mf.dove_[i].vy;
        mf.dove_[i].t++;
        if (mf.dove_[i].x < -16 || mf.dove_[i].x > ARENA_W + 16) mf.dove_[i].on = false;
        else if (overlap(rx - 7, ry - 44, rx + 7, ry, mf.dove_[i].x - 6, mf.dove_[i].y - 6, mf.dove_[i].x + 6, mf.dove_[i].y + 6)) {
            hurt_richter(p, 6, mf.dove_[i].x);
            mf.dove_[i].on = false;
        }
    }
}

/* after a frame: her doves, her, her gauge's fill */
static void maria_draw(const Canvas *cv, int base)
{
    subs_draw(cv, base);
    for (int i = 0; i < 4; i++)
        if (mf.dove_[i].on) {
            Piece dv = {0, 0, (uint16_t)(0x1A0 + (mf.dove_[i].t / 4) % 3), (uint16_t)(mf.dove_[i].vx < 0 ? 0x0800 : 0x0000)};
            draw_frame(cv, &dv, 1, mf.dove, 0x1A0, 0xFFFF, NULL, mf.dove_pal, base + (int)mf.dove_[i].x - 8, (int)mf.dove_[i].y - 8, 16, false, false);
        }
    /* the second phase's afterimages: where she was 18, 12 and 6 frames before, fading */
    if (mf.phase == 2 && mf.trail_at >= 0 && mf.state != M_DOWN)
        for (int k = 0; k < 3; k++) {
            int back = 18 - 6 * k, i = ((mf.trail_at - back) % TRAIL + TRAIL) % TRAIL;
            float dx = mf.trail[i].x - mf.x, dy = mf.trail[i].y - mf.y;
            if (dx * dx + dy * dy < 4) continue;   /* (standing still: none) */
            draw_mpose_as(cv, mf.trail[i].pose, base + (int)mf.trail[i].x, (int)mf.trail[i].y - MARIA_FEET,
                          mf.trail[i].right, false, 60 + 40 * k);
        }
    bool dying = mf.phase == 2 && mf.state == M_DOWN;
    if (dying) g_dissolve = mf.t < 60 ? 0 : mf.t >= FIN_GONE ? 17 : (mf.t - 60) * 17 / (FIN_GONE - 60);
    if (dying || !(mf.inv && (mf.inv & 2) && !mf.flash))   /* (blinking while she cannot be hurt) */
        draw_mpose(cv, mf.pose, base + (int)mf.x + mf.shake, (int)mf.y - MARIA_FEET, mf.right, mf.flash > 0);
    g_dissolve = 0;
}

static void gauge_fill(const Canvas *cv, int gx, int gy)
{
    static const uint32_t TUBE[4] = {0xFFB6B6DAu, 0xFFFFFFFFu, 0xFFDADAFFu, 0xFF9191B6u};
    int full = mf.phase == 2 || mf.state == M_DOWN ? MARIA_HP2 : MARIA_HP;   /* (from the refill on: the second gauge) */
    int h = (mf.hp > full ? full : mf.hp) * 86 / full;
    for (int ly = 94 - h; ly <= 94; ly++)
        for (int k = 0; k < 4; k++) plot(cv, gx + 24 + k, gy + ly, TUBE[k]);
}

static bool fight_load(Pce *p)
{
    if (fs.loaded) return fs.loaded > 0;
    fs.loaded = -1;
    static uint16_t vram[0x8000];
    static uint8_t buf[TITLE_STREAM_MAX];
    memset(vram, 0, sizeof vram);
    for (size_t i = 0; i < sizeof TITLE_STREAMS / sizeof TITLE_STREAMS[0]; i++) {
        if (!disc_span(p, TITLE_STREAMS[i].off, TITLE_STREAM_MAX, buf)) return false;
        unpack(buf, TITLE_STREAM_MAX, TITLE_STREAMS[i].vram, TITLE_STREAMS[i].flag, vram);
    }
    uint8_t vb[0x10000];
    for (int i = 0; i < 0x8000; i++) vb[i * 2] = (uint8_t)vram[i], vb[i * 2 + 1] = (uint8_t)(vram[i] >> 8);
    if (crc32_of(vb, sizeof vb) != TITLE_VRAM_CRC) return false;
    uint8_t tp[12 * 18];
    if (!disc_span(p, TITLE_PALS, sizeof tp, tp) || crc32_of(tp, sizeof tp) != TITLE_PALS_CRC) return false;
    uint32_t pal[2][128];
    for (int k = 0; k < 8; k++) {
        unpack_pal(tp + 18 * k, pal[0] + 16 * k);
        unpack_pal(tp + 18 * (k >= 4 ? k + 4 : k), pal[1] + 16 * k);
    }
    static uint8_t wall[0x2000 + 0x100 * 32 + 7 * 32 + 8 * 18];
    if (!disc_span(p, WALL_BLOCKS_AT, 0x2000, wall) || !disc_span(p, WALL_TILES + 0x110 * 32, 0x100 * 32, wall + 0x2000) ||
        !disc_span(p, WALL_TILES2 + 0x210 * 32, 7 * 32, wall + 0x2000 + 0x100 * 32) ||
        !disc_span(p, WALL_PALS, 8 * 18, wall + 0x2000 + 0x100 * 32 + 7 * 32) || crc32_of(wall, sizeof wall) != WALL_CRC)
        return false;
    uint8_t sp[8 * 128 + 18];
    if (!disc_span(p, BOLT_CELLS + 0x199 * 128, 7 * 128, sp) || !disc_span(p, BOLT_PAL, 18, sp + 7 * 128) ||
        crc32_of(sp, 7 * 128 + 18) != BOLT_CRC)
        return false;
    memcpy(fs.bolt, sp, 7 * 128);
    unpack_pal(sp + 7 * 128, fs.bolt_pal[0]);
    if (!disc_span(p, BLOOD_CELLS + 0x178 * 128, 8 * 128, sp) || !disc_span(p, BLOOD_PAL, 18, sp + 8 * 128) ||
        crc32_of(sp, sizeof sp) != BLOOD_CRC)
        return false;
    memcpy(fs.blood, sp, 8 * 128);
    unpack_pal(sp + 8 * 128, fs.blood_pal[0]);
    if (!disc_span(p, GAUGE_CELLS + 0x1F8 * 128, 8 * 128, sp) || !disc_span(p, GAUGE_PAL, 18, sp + 8 * 128) ||
        crc32_of(sp, sizeof sp) != GAUGE_CRC)
        return false;
    memcpy(fs.gauge, sp, 8 * 128);
    unpack_pal(sp + 8 * 128, fs.gauge_pal[0]);
    uint8_t tb[12 * 128 + 36];
    if (!disc_span(p, TORCH_CELLS, 12 * 128, tb) || !disc_span(p, TORCH_PAL_FLAME, 18, tb + 12 * 128) ||
        !disc_span(p, TORCH_PAL_STAND, 18, tb + 12 * 128 + 18) || crc32_of(tb, sizeof tb) != TORCH_CRC)
        return false;
    memcpy(fs.torch, tb, 12 * 128);
    fs.have_thunder = disc_span(p, THUNDER_AT, THUNDER_LEN, fs.thunder) && crc32_of(fs.thunder, THUNDER_LEN) == THUNDER_CRC;
    fs.have_yell = disc_span(p, MARIA_YELL_AT, MARIA_YELL_LEN, fs.yell) && crc32_of(fs.yell, MARIA_YELL_LEN) == MARIA_YELL_CRC;
    {
        uint8_t b[8 * 128 + ORB_FADES * ORB_FADE_STEP + 18 + 4 * 128], *f = b + 8 * 128, *o = f + ORB_FADES * ORB_FADE_STEP;
        uint8_t dp[18];
        fs.have_orb_art = disc_span(p, ORB_CELLS, 8 * 128, b) && disc_span(p, ORB_FADE, ORB_FADES * ORB_FADE_STEP, f) &&
                          disc_span(p, ORB_PAL, 18, o) && disc_span(p, SPARK_CELLS, 4 * 128, o + 18) &&
                          crc32_of(b, sizeof b) == ORB_ART_CRC && disc_span(p, DOVE_PAL, 18, dp);
        if (fs.have_orb_art) {
            memcpy(fs.orb_cells, b, sizeof fs.orb_cells);
            memcpy(fs.spark_cells, o + 18, sizeof fs.spark_cells);
            for (int k = 0; k < ORB_FADES; k++) unpack_pal(f + k * ORB_FADE_STEP, fs.orb_pal[k]);
            unpack_pal(o, fs.orb_pal[ORB_FADES]);
            unpack_pal(dp, fs.spark_pal[0]);
        }
    }
    fs.have_death_cry = disc_span(p, DEATH_CRY_AT, DEATH_CRY_LEN, fs.death_cry) &&
                        crc32_of(fs.death_cry, DEATH_CRY_LEN) == DEATH_CRY_CRC;
    fs.have_death_yell = disc_span(p, MARIA_DEATH_AT, MARIA_DEATH_LEN, fs.death_yell) &&
                         crc32_of(fs.death_yell, MARIA_DEATH_LEN) == MARIA_DEATH_CRC;
    {
        uint8_t st[128 + 18];
        if (!disc_span(p, STONE_TILES, 64, st) || !disc_span(p, STONE_TILES + 0x200, 64, st + 64) ||
            !disc_span(p, STONE_PAL, 18, st + 128) || crc32_of(st, sizeof st) != STONE_CRC)
            return false;
        memcpy(fs.stone, st, 128);
        unpack_pal(st + 128, fs.stone_pal[0]);
    }
    unpack_pal(tb + 12 * 128, fs.torch_pal[0]);
    unpack_pal(tb + 12 * 128 + 18, fs.torch_pal[1]);
    /* the title art that is the title's (its layout: the title's map) */
    const TitleArt *art = NULL;
    for (int i = 0; i < fs.n_arts && !art; i++) {
        int same = 0;
        for (int r = 0; r < TITLEART_ROWS; r++)
            for (int c = 0; c < TITLEART_COLS; c++) same += fs.arts[i].layout[r][c] == vram[r * 64 + c];
        if (fs.arts[i].loaded && same > TITLEART_ROWS * TITLEART_COLS * 3 / 4) art = &fs.arts[i];
    }
    /* the sky: the title (its map, palette entries), its sides from the art (else its edges
     * carried on, mirrored), raised */
    for (int y = 0; y < ARENA_H; y++) {
        int ty = y + SKY_LIFT > 223 ? 223 : y + SKY_LIFT;
        for (int x = 0; x < ARENA_W; x++) {
            int e = 0;
            int mx = x - TITLEART_SIDE;
            if (mx >= 0 && mx < 256) {
                uint16_t m = vram[(ty >> 3) * 64 + (mx >> 3)];
                int c = tile_px(&vram[(m & 0x7FF) * 16], mx & 7, ty & 7);
                e = c ? (m >> 12) * 16 + c : 0;
            } else if (art) {
                uint16_t a = art->side[ty][mx < 0 ? x : TITLEART_SIDE + (mx - 256)];
                e = a == TITLEART_NONE ? 0 : a & 0x7F;
            } else {
                int sx = mx < 0 ? -mx - 1 : 511 - mx;
                uint16_t m = vram[(ty >> 3) * 64 + (sx >> 3)];
                int c = tile_px(&vram[(m & 0x7FF) * 16], sx & 7, ty & 7);
                e = c ? (m >> 12) * 16 + c : 0;
            }
            fs.sky[0][y * ARENA_W + x] = pal[0][e & 0x7F];
            fs.sky[1][y * ARENA_W + x] = pal[1][e & 0x7F];
        }
    }
    /* the wall over it: map rows from y 380 (above its top course, only its brown merlons) */
    uint32_t wp[8][16];
    for (int k = 0; k < 8; k++) unpack_pal(wall + 0x2000 + 0x100 * 32 + 7 * 32 + 18 * k, wp[k]);
    memset(fs.solid, 0, sizeof fs.solid);
    for (int y = WALL_TOP; y < ARENA_H; y++) {
        int my = 380 + y - WALL_TOP, br = (my - 352) / 32;
        if (br > 4) break;
        for (int x = 0; x < ARENA_W; x++) {
            int mxx = 100 + x, bc = (mxx - 96) / 32;
            int blk = WALL_BLOCK[br][bc], ti = ((my >> 3) & 3) * 4 + ((mxx >> 3) & 3);
            uint16_t m = (uint16_t)(wall[blk * 32 + ti * 2] | wall[blk * 32 + ti * 2 + 1] << 8);
            int t = m & 0x7FF;
            if (t < 0x110 || t > 0x216) continue;
            const uint8_t *tb = wall + 0x2000 + (t - 0x110) * 32;
            uint16_t tw[16];
            for (int i = 0; i < 16; i++) tw[i] = (uint16_t)(tb[i * 2] | tb[i * 2 + 1] << 8);
            int c = tile_px(tw, mxx & 7, my & 7);
            if (!c) continue;
            uint32_t col = wp[(m >> 12) & 7][c];
            int r = col >> 16 & 255, g = col >> 8 & 255, b = col & 255;
            bool brown = r >= g && g >= b && r > 40;
            if (my < 393 && !brown && !fs.solid[(y - 1) * ARENA_W + x]) continue;   /* (above a merlon) */
            fs.sky[0][y * ARENA_W + x] = fs.sky[1][y * ARENA_W + x] = col;
            fs.solid[y * ARENA_W + x] = 1;
        }
    }
    /* black behind the wall (its gaps), from its top course down */
    for (int y = WALL_TOP + 13; y < ARENA_H; y++)
        for (int x = 0; x < ARENA_W; x++)
            if (!fs.solid[y * ARENA_W + x]) fs.sky[0][y * ARENA_W + x] = fs.sky[1][y * ARENA_W + x] = 0xFF000000u;
    if (!maria_load(p)) return false;
    fs.loaded = 1;
    return true;
}


/* the camera to the room's right end and held there (the arena: the whole picture) */
static void lock_camera(Pce *p)
{
    int cam = cam_page(p) * 256 + p->wram[W_CAM_X], d = 768 - cam;
    p->wram[W_CAM_PAGE] = 3;
    p->wram[0x1E10] = 1;
    p->wram[W_CAM_X] = 0;
    p->wram[CAM_LO] = 0;
    for (int s = 0; s < 16; s++) {   /* (the player's objects: in screen coordinates) */
        if (!p->wram[0x0A10 + s] && s) continue;
        int x = (int16_t)(p->wram[W_PX_HI + s] << 8 | p->wram[W_PX + s]) - d;
        p->wram[W_PX_HI + s] = (uint8_t)(x >> 8);
        p->wram[W_PX + s] = (uint8_t)x;
    }
    p->wram[LIMIT_LEFT_PAGE] = 3;
    p->wram[0x1E8E] = 0;                 /* (Y: page 1 less DROP) */
    p->wram[0x1E28] = 0;
    p->wram[0x1E20] = (uint8_t)(256 - DROP);
}

/* the camera's Y back to the room's (and the player's objects with it) */
static void unlock_camera(Pce *p)
{
    if (p->wram[0x1E8E] != 0) return;
    p->wram[0x1E8E] = 1;
    p->wram[0x1E28] = 1;
    p->wram[0x1E20] = 0;
    for (int s = 0; s < 16; s++) {
        if (!p->wram[0x0A10 + s] && s) continue;
        int y = (int16_t)(p->wram[W_PY_HI + s] << 8 | p->wram[W_PY + s]) - DROP;
        p->wram[W_PY_HI + s] = (uint8_t)(y >> 8);
        p->wram[W_PY + s] = (uint8_t)y;
    }
}

static void music_fight(Pce *p);
static void music_track(Pce *p, int track);
static void music_back(Pce *p);

/* The ledges solid: the floor's map entry (the one under the arena's floor) written into the
 * map's cells under them every frame (the game's columns, streamed as the camera moves, can
 * draw over them), the cells' own put back when they go. */
static void ledge_floor(Pce *p, bool on)
{
    Vdc *d = &p->vdc;
    static const int map_w[4] = {32, 64, 128, 128};
    int mw = map_w[(d->reg[9] >> 4) & 3], mh = (d->reg[9] & 0x40) ? 64 : 32;
    if (!on) {
        for (int i = 0; i < fs.n_bat; i++) d->vram[fs.bat_at[i]] = fs.bat_saved[i];
        fs.n_bat = 0;
        return;
    }
    const uint16_t *my = vdc_row_map_y();
    int row_of[2];
    int fr = (my[ARENA_FLOOR + 2] & (mh * 8 - 1)) >> 3, fc = ((ARENA_X0 + ARENA_W / 2) >> 3) & (mw - 1);
    uint16_t floor_e = d->vram[fr * mw + fc];
    bool first = fs.n_bat == 0;
    if (!first) floor_e = d->vram[fs.bat_at[0]];   /* (as written the first time) */
    int n = 0;
    for (int l = 0; l < LEDGES; l++) {
        for (int r = 0; r < 2; r++) row_of[r] = (my[ledge_top(l) + 8 * r] & (mh * 8 - 1)) >> 3;
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 2 * LEDGE_BLOCKS; c++) {
                int at = row_of[r] * mw + (((ARENA_X0 + LEDGE_X[l] + 8 * c) >> 3) & (mw - 1));
                if (first) fs.bat_at[n] = at, fs.bat_saved[n] = d->vram[at];
                d->vram[at] = floor_e;
                n++;
            }
    }
    fs.n_bat = n;
}

/* The fight in save states: the host plays it, outside the game's memory, so a state saved
 * in it came back to the card game (with the camera where the arena had it: the room's own
 * wall stood mid-picture, the floor drawn on past it). Each frame of the arena a snapshot of
 * it is kept in free space of the area's room bank ($FF there, $9D25 on), which a save state
 * holds; a state loaded with one goes straight back into the arena. */
#define FIGHT_SAVE 0x9D80
#define FIGHT_SAVE_END 0x9E00
typedef struct {
    char magic[4];             /* "RFGT" */
    uint8_t version, phase, mstate, pose, right, second, fall, throws;
    int16_t t, mt, hp, inv, flash, dove_wait, flinch_wait, walk_for, dir;
    float x, y, vx, vy;
    int16_t strike_in, dim, ledge_a;
    uint8_t music_ours, music_repeat, music_playing, pad;
    int32_t music_start, music_end;
    int64_t music_pos;
} FightSave;
_Static_assert(sizeof(FightSave) <= FIGHT_SAVE_END - FIGHT_SAVE, "the fight's snapshot fits its space");

static void fight_save(Pce *p)
{
    uint8_t *m = at(p, B_ROOMS, FIGHT_SAVE);
    if (!m) return;
    if (fs.state == FIGHT_WON) {   /* (won: kept so, the tombstones stay; "RFGW") */
        memset(m, 0xFF, FIGHT_SAVE_END - FIGHT_SAVE);
        memcpy(m, "RFGW", 4);
        return;
    }
    if (fs.state != FIGHT_ARENA) {
        if (!memcmp(m, "RFGT", 4)) memset(m, 0xFF, FIGHT_SAVE_END - FIGHT_SAVE);
        return;
    }
    FightSave f;
    memset(&f, 0, sizeof f);
    memcpy(f.magic, "RFGT", 4);
    f.version = 1;
    f.phase = (uint8_t)mf.phase, f.mstate = (uint8_t)mf.state, f.pose = (uint8_t)mf.pose;
    f.right = mf.right, f.second = mf.second, f.fall = mf.fall, f.throws = (uint8_t)mf.throws;
    f.t = (int16_t)fs.t, f.mt = (int16_t)mf.t, f.hp = (int16_t)mf.hp, f.inv = (int16_t)mf.inv, f.flash = (int16_t)mf.flash;
    f.dove_wait = (int16_t)mf.dove_wait, f.flinch_wait = (int16_t)mf.flinch_wait, f.walk_for = (int16_t)mf.walk_for;
    f.dir = (int16_t)mf.dir;
    f.x = mf.x, f.y = mf.y, f.vx = mf.vx, f.vy = mf.vy;
    f.strike_in = (int16_t)(fs.next_strike - fs.t);
    f.dim = (int16_t)fs.dim, f.ledge_a = (int16_t)fs.ledge_a;
    f.music_ours = fs.music_ours, f.music_repeat = fs.music_repeat, f.music_playing = fs.music_playing;
    f.music_start = fs.music_start, f.music_end = fs.music_end, f.music_pos = fs.music_pos;
    memcpy(m, &f, sizeof f);
}

/* (a state loaded in the arena: the fight as it was) */
static bool fight_resume(Pce *p)
{
    const uint8_t *m = at(p, B_ROOMS, FIGHT_SAVE);
    FightSave f;
    if (!m || memcmp(m, "RFGT", 4)) return false;
    memcpy(&f, m, sizeof f);
    if (f.version != 1 || !fight_load(p)) return false;
    fs.state = FIGHT_ARENA;
    fs.t = f.t;
    maria_reset();
    mf.phase = f.phase, mf.state = f.mstate, mf.pose = f.pose;
    mf.right = f.right, mf.second = f.second, mf.fall = f.fall, mf.throws = f.throws;
    mf.t = f.mt, mf.hp = f.hp, mf.inv = f.inv, mf.flash = f.flash;
    mf.dove_wait = f.dove_wait, mf.flinch_wait = f.flinch_wait, mf.walk_for = f.walk_for, mf.dir = f.dir;
    mf.x = f.x, mf.y = f.y, mf.vx = f.vx, mf.vy = f.vy;
    fs.strike_at = -1000;
    fs.strike_maria = false;
    fs.next_strike = fs.t + (f.strike_in > 0 ? f.strike_in : 120);
    fs.dim = f.dim, fs.ledge_a = f.ledge_a;
    fs.n_bat = 0;
    fs.finale_t = -1;
    fs.music_ours = f.music_ours, fs.music_repeat = f.music_repeat, fs.music_playing = f.music_playing;
    fs.music_start = f.music_start, fs.music_end = f.music_end, fs.music_pos = f.music_pos;
    return true;
}

/* before a frame */
static void fight_frame(Pce *p)
{
    if (!in_hidden(p)) {
        if (fs.state != FIGHT_NONE) music_back(p);
        fs.state = FIGHT_NONE;
        grave_reset();
        fight_save(p);
        return;
    }
    if (fs.state == FIGHT_NONE) {   /* (won before: the tombstones, no card game) */
        const uint8_t *m = at(p, B_ROOMS, FIGHT_SAVE);
        if (m && !memcmp(m, "RFGW", 4)) fs.state = FIGHT_WON, fs.won_t = WON_FADE;
    }
    if (fs.state == FIGHT_NONE && p->wram[LIMIT_LEFT_PAGE] == 3 && fight_resume(p)) return;
    if (fs.state == FIGHT_NONE) {
        if (p->wram[LIMIT_LEFT_PAGE] == 3) {   /* (a state loaded mid-fight): the room's own limits */
            p->wram[LIMIT_LEFT_PAGE] = 2;
            p->wram[0x1EC6] = 3;
            p->wram[0x1EC7] = 0;
            unlock_camera(p);
        }
        return;
    }
    if (!playing(p)) return;
    fs.t++;
    if (fs.state == FIGHT_DYING && fs.t >= DYING_T) {
        fs.state = FIGHT_FADE, fs.t = 0;
    } else if (fs.state == FIGHT_FADE && fs.t == FADE_T / 2) {
        lock_camera(p);
    } else if (fs.state == FIGHT_FADE && fs.t >= FADE_T) {
        fs.state = FIGHT_ARENA, fs.t = 0;
        maria_reset();
        fs.dim = fs.ledge_a = 0;
        fs.strike_maria = false;
        fs.finale_t = -1;
        music_fight(p);
        fs.strike_at = -1000;
        fs.next_strike = 120 + rnd(120);   /* (the first soon after the arena comes in) */
    }
    if (fs.state == FIGHT_WON) {   /* (back in the room: it fades in; Death's grave) */
        if (fs.won_t < WON_FADE) fs.won_t++;
        grave_step(p);
        return;
    }
    if (fs.state >= FIGHT_FADE && cam_page(p) == 3) p->wram[LIMIT_LEFT_PAGE] = 3;
    if (fs.state == FIGHT_ARENA) {
        maria_step(p);
        if (fs.t >= fs.next_strike) {
            fs.strike_at = fs.t;
            fs.next_strike = fs.t + 420 + rnd(481);   /* (7-15 s on) */
            fs.strike_dx = rnd(200) - 120;
            fs.strike_flip = rnd(2);
            fs.strike_maria = false;
        }
        if (fs.t == fs.strike_at + THUNDER_DELAY && fs.have_thunder)
            sound_extra_play_at(fs.thunder, THUNDER_LEN, 8, THUNDER_VOLUME, THUNDER_SOFTEN);
    }
    if (fs.state == FIGHT_ARENA && fs.finale_t >= 0) {
        fs.finale_t++;
        if (fs.finale_t == FIN_ECHO && fs.have_death_yell)
            sound_extra_play_at(fs.death_yell, MARIA_DEATH_LEN, MARIA_DEATH_RATE, 130, 90);   /* (its echo) */
        orbs_step(p);
        fs.next_strike = fs.t + 600;   /* (no more lightning) */
        /* (the rain over and every orb gathered: a moment, then the fade out) */
        if (fs.fade_at < 0 && fs.finale_t >= FIN_RAIN1) {
            bool left = false;
            for (int i = 0; i < ORBS; i++) left |= fs.orb[i].on;
            static int none_for;
            none_for = left ? 0 : none_for + 1;
            if (none_for >= FIN_AFTER_LAST || fs.finale_t >= FIN_RAIN1 + FIN_GATHER_MAX) fs.fade_at = fs.finale_t, none_for = 0;
        }
        if (fs.fade_at >= 0 && fs.finale_t >= fs.fade_at + FIN_FADE_LEN) {
            /* back to the room: its own limits and camera, the stage's music; won */
            ledge_floor(p, false);
            fs.state = FIGHT_WON;
            fs.won_t = 0;
            fs.dim = fs.ledge_a = 0;
            fs.finale_t = -1;
            p->wram[LIMIT_LEFT_PAGE] = 2;
            p->wram[0x1EC6] = 3;
            p->wram[0x1EC7] = 0;
            unlock_camera(p);
            music_back(p);
            fight_save(p);
            return;
        }
    }
    ledge_floor(p, fs.state == FIGHT_ARENA && fs.ledge_a >= 256);
    fight_save(p);
}

/* the arena's pixel at its x, y */
/* moonlit: darker and cooled to the night sky's blue (the second phase's wall and ground) */
static uint32_t moonlit(uint32_t c)
{
    int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
    int mr = r * 35 / 100, mg = g * 42 / 100, mb = b * 60 / 100 + 30;
    if (mb > 255) mb = 255;
    return 0xFF000000u | (uint32_t)mr << 16 | (uint32_t)mg << 8 | (uint32_t)mb;
}
static uint32_t mix(uint32_t a, uint32_t b, int t)   /* (t 0-256: a to b) */
{
    uint32_t o = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) o |= (uint32_t)((int)(a >> sh & 255) + (((int)(b >> sh & 255) - (int)(a >> sh & 255)) * t >> 8)) << sh;
    return o;
}

static uint32_t arena_px(int ax, int y, bool flash)
{
    if (ax < 0) ax = 0;
    if (ax >= ARENA_W) ax = ARENA_W - 1;
    uint32_t c = fs.sky[flash][y * ARENA_W + ax];
    if (fs.dim && (fs.solid[y * ARENA_W + ax] || y >= WALL_TOP + 13)) c = mix(c, moonlit(c), fs.dim);
    return c;
}

/* the ledges: their blocks (from the second phase's start, dithered in as `ledge_a` grows) */
static void draw_ledges(const Canvas *cv, int base)
{
    if (!fs.ledge_a) return;
    int dis = 16 - fs.ledge_a * 16 / 256;   /* (how much of each block is left out) */
    for (int l = 0; l < LEDGES; l++)
        for (int b = 0; b < LEDGE_BLOCKS; b++)
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    int q = (y >> 3) * 2 + (x >> 3);
                    uint16_t tw[16];
                    for (int i = 0; i < 16; i++) tw[i] = (uint16_t)(fs.stone[q * 32 + i * 2] | fs.stone[q * 32 + i * 2 + 1] << 8);
                    int c = tile_px(tw, x & 7, y & 7);
                    int px = base + LEDGE_X[l] + 16 * b + x, py = ledge_top(l) + y;
                    if (DITHER[py & 3][px & 3] < dis) continue;
                    uint32_t col = fs.stone_pal[0][c];   /* (0 too: the mortar, dark) */
                    if (fs.ledge_a < 256 && ((fs.t >> 1) & 1)) col = mix(col, 0xFFFFFFFFu, 140);   /* (flickering in) */
                    plot(cv, px, py, col);
                }
}

/* a red orb (a beaten boss's, drawn by the host): a shiny sphere, a glint now and then */
static void draw_orb(const Canvas *cv, int cx, int cy, int age)
{
    for (int y = -5; y <= 5; y++)
        for (int x = -5; x <= 5; x++) {
            int d2 = x * x + y * y;
            if (d2 > 26) continue;
            int lx = x + 2, ly = y + 2, l2 = lx * lx + ly * ly;   /* (lit from the top left) */
            uint32_t c = d2 > 18 ? 0xFF6E000Au : l2 < 3 ? 0xFFFFD2D2u : l2 < 8 ? 0xFFFF5A50u : 0xFFC8141Eu;
            plot(cv, cx + x, cy + y, c);
        }
    if ((age / 6) % 5 == 0) {   /* (a glint) */
        plot(cv, cx + 4, cy - 6, 0xFFFFFFC8u);
        plot(cv, cx + 3, cy - 6, 0xFFFFE680u), plot(cv, cx + 5, cy - 6, 0xFFFFE680u);
        plot(cv, cx + 4, cy - 7, 0xFFFFE680u), plot(cv, cx + 4, cy - 5, 0xFFFFE680u);
    }
}

/* the game's orb at its middle cx, cy (age: frames since it appeared) */
static void draw_game_orb(const Canvas *cv, int cx, int cy, int age, int spark)
{
    if (age < ORB_WHITE_T) {   /* (white, shimmering, fading to red) */
        static const uint16_t FL[4] = {0x0000, 0x0800, 0x8000, 0x8800};
        Piece pc = {0, 0, 0x140, FL[(age / 2) & 3]};
        uint32_t (*pal)[16] = &fs.orb_pal[age * ORB_FADES / ORB_WHITE_T];
        draw_frame(cv, &pc, 1, fs.orb_cells, 0x140, 0xFFFF, NULL, pal, cx - 8, cy - 8, 16, false, false);
        return;
    }
    /* (red, the fireball turning in it: behind the orb) */
    int f = ((age - ORB_WHITE_T) / 4) % 6;
    Piece pc[2] = {{0, 0, 0x141, 0x0000}, {0, 0, (uint16_t)(0x142 + f), (uint16_t)(((age / 24) & 1) ? 0x0800 : 0)}};
    draw_frame(cv, pc, 2, fs.orb_cells, 0x140, 0xFFFF, NULL, &fs.orb_pal[ORB_FADES], cx - 8, cy - 8, 16, false, false);
    if (spark >= 0 && spark < 16) {   /* (a sparkle by it) */
        Piece sp = {0, 0, (uint16_t)(0x1F3 + spark / 4), 0x0000};
        int sx = cx - 8 + ((age * 7) % 11) - 5, sy = cy - 14 + ((age * 5) % 7);
        draw_frame(cv, &sp, 1, fs.spark_cells, 0x1F3, 0xFFFF, NULL, fs.spark_pal, sx, sy, 16, false, false);
    }
}

/* the orbs: falling, bouncing off the floor, the ledges and the arena's sides; Richter gathers
 * them (rx, ry his feet) */
static void orbs_step(Pce *p)
{
    float rx, ry;
    richter_at(p, &rx, &ry);
    int t = fs.finale_t;
    if (t >= FIN_RAIN0 && t < FIN_RAIN1 && t % 3 == 0)
        for (int i = 0; i < ORBS; i++)
            if (!fs.orb[i].on) {
                fs.orb[i].on = true;
                fs.orb[i].age = 0;
                fs.orb[i].x = (float)(12 + rnd(ARENA_W - 24));
                fs.orb[i].y = -8.0f - (float)rnd(30);
                fs.orb[i].vx = (float)(rnd(41) - 20) / 20.0f;
                fs.orb[i].vy = 1.0f + (float)rnd(10) / 10.0f;
                fs.orb[i].spark = -1;
                break;
            }
    for (int i = 0; i < ORBS; i++) {
        if (!fs.orb[i].on) continue;
        float was = fs.orb[i].y;
        fs.orb[i].age++;
        fs.orb[i].x += fs.orb[i].vx;
        fs.orb[i].y += fs.orb[i].vy;
        fs.orb[i].vy += 0.18f;
        float g = ground_at(fs.orb[i].x, was + 5) - 5;
        if (fs.orb[i].vy > 0 && fs.orb[i].y >= g) {
            fs.orb[i].y = g;
            fs.orb[i].vy = -fs.orb[i].vy * 0.62f;
            fs.orb[i].vx *= 0.85f;
            if (fs.orb[i].vy > -0.9f) fs.orb[i].vy = 0;
        }
        if (fs.orb[i].x < 6) fs.orb[i].x = 6, fs.orb[i].vx = -fs.orb[i].vx;
        if (fs.orb[i].x > ARENA_W - 6) fs.orb[i].x = ARENA_W - 6, fs.orb[i].vx = -fs.orb[i].vx;
        /* (a sparkle now and then) */
        if (fs.orb[i].spark >= 0 && ++fs.orb[i].spark >= 16) fs.orb[i].spark = -1;
        if (fs.orb[i].spark < 0 && fs.orb[i].age > ORB_WHITE_T && !rnd(40)) fs.orb[i].spark = 0;
        /* (gathered: Richter touching it) */
        if (fs.fade_at < 0 && overlap(rx - 9, ry - 46, rx + 9, ry, fs.orb[i].x - 6, fs.orb[i].y - 6, fs.orb[i].x + 6, fs.orb[i].y + 6)) {
            fs.orb[i].on = false;
            fs.gathered++;
            sfx_request(PICKUP_SFX);
        }
    }
}

/* black over the whole picture, `a` 0-256 */
static void darken_all(const Canvas *cv, int a)
{
    if (a <= 0) return;
    for (int y = 0; y < cv->h; y++)
        for (int x = 0; x < cv->w; x++) {
            uint32_t *px = &cv->fb[y * PCE_FB_MAX_W + x];
            *px = a >= 256 ? 0xFF000000u : mix(*px, 0xFF000000u, a);
        }
}

/* the tombstone where Death was struck down (the host's own drawing): a rounded grey stone,
 * a cross cut in it, on a step; x the middle, `bottom` the floor */
static void draw_tombstone(const Canvas *cv, int x, int bottom)
{
    static const uint32_t L = 0xFFB4B4BEu, M = 0xFF8C8C96u, D = 0xFF5A5A64u, K = 0xFF323238u;
    for (int y = 0; y < 26; y++)
        for (int dx = -9; dx <= 9; dx++) {
            int yy = bottom - 26 + y;
            if (y >= 22) {   /* (the step) */
                plot(cv, x + dx, yy, y == 22 ? L : y == 25 ? K : M);
                continue;
            }
            if (dx < -7 || dx > 7) continue;
            if (y < 6) {   /* (the rounded top) */
                int ry = 6 - y;
                if (dx * dx + ry * ry * 2 > 56) continue;
            }
            uint32_t c = dx <= -6 ? L : dx >= 6 ? D : M;
            if (y < 2 && dx > -5 && dx < 3) c = L;
            /* (the cross) */
            if ((dx >= -1 && dx <= 0 && y >= 4 && y <= 17) || (y >= 7 && y <= 8 && dx >= -4 && dx <= 3)) c = K;
            plot(cv, x + dx, yy, c);
        }
}

/* Death's grave, once the fight is won: the cemetery's tombstone that comes for you (Stage 3),
 * the game's own sprite from the disc (its cells $120-$12F and palette, stage data at $5EF800
 * and $5F92F6). It stands where he fell until Richter comes near, then shakes, rises, leans
 * over and dives at him, three times, and flies back to its place. Its touch hurts; four
 * strikes of the whip break it, and after a while it stands there again. Maria's stands still. */
#define GRAVE_CELLS 0x5EF800
#define GRAVE_PAL 0x5F92F6
#define GRAVE_CRC 0xFBF38593u
#define GRAVE_HP 4
#define GRAVE_W 32
#define GRAVE_H 48
enum { G_OFF, G_REST, G_WAKE, G_RISE, G_AIM, G_DIVE, G_HOME, G_BREAK, G_GONE };
/* its poses (cells from $120; the frame's top left; facing right: leaning to the right) */
static const Piece GRAVE_POSE[3][2] = {
    {{8, 0, 0x12C, 0x1080}, {0, 32, 0x120, 0x0180}},   /* upright */
    {{8, 0, 0x122, 0x0080}, {0, 16, 0x124, 0x1180}},   /* leaning */
    {{16, 0, 0x123, 0x0080}, {0, 16, 0x128, 0x1180}},  /* leaning far */
};
static struct {
    int loaded;                /* 0 not yet, 1 read from the disc, -1 not there */
    uint8_t cells[16 * 128];
    uint32_t pal[1][16];
    int state, t, hp, inv, flash, dives, pose, rest_for;
    float x, y, vx, vy;        /* room X of its middle, Y of its bottom */
    bool right;
} gv;

static float grave_home(void) { return (float)(DEATH_X + DEATH_W / 2); }

static bool grave_load(Pce *p)
{
    if (gv.loaded) return gv.loaded > 0;
    uint8_t b[16 * 128 + 18];
    gv.loaded = disc_span(p, GRAVE_CELLS, 16 * 128, b) && disc_span(p, GRAVE_PAL, 18, b + 16 * 128) &&
                        crc32_of(b, sizeof b) == GRAVE_CRC ? 1 : -1;
    if (gv.loaded > 0) {
        memcpy(gv.cells, b, sizeof gv.cells);
        unpack_pal(b + 16 * 128, gv.pal[0]);
    }
    return gv.loaded > 0;
}

static void grave_reset(void) { gv.state = G_OFF; }

/* a grave's frame: x its middle, bottom on screen */
static void grave_draw_at(const Canvas *cv, int pose, int x, int bottom, bool right, bool white)
{
    draw_frame(cv, GRAVE_POSE[pose], 2, gv.cells, 0x120, 0xFFFF, NULL, gv.pal, x - GRAVE_W / 2, bottom - GRAVE_H, GRAVE_W,
               !right, white);
}

/* before a frame, in the room once the fight is won */
static void grave_step(Pce *p)
{
    if (!grave_load(p)) return;
    float floor = (float)ROOM_FLOOR;
    if (gv.state == G_OFF) {
        gv.t = gv.hp = gv.inv = gv.flash = gv.dives = gv.pose = 0, gv.vx = gv.vy = 0;
        gv.state = G_REST;
        gv.x = grave_home(), gv.y = floor;
        gv.rest_for = 90;
        gv.right = false;
    }
    int cam = scene_cam(p);
    int sx = (int16_t)(p->wram[W_PX_HI] << 8 | p->wram[W_PX]), sy = (int16_t)(p->wram[W_PY_HI] << 8 | p->wram[W_PY]);
    float rx = (float)(sx + cam), ry = (float)(sy + MARIA_FEET);   /* (Richter in the room: his feet) */
    gv.t++;
    if (gv.inv) gv.inv--;
    if (gv.flash) gv.flash--;
    bool live = gv.state >= G_WAKE && gv.state <= G_HOME;
    switch (gv.state) {
    case G_REST:
        gv.pose = 0;
        if (gv.rest_for) gv.rest_for--;
        else if (fs.won_t >= WON_FADE && fabsf(rx - gv.x) < 160) gv.state = G_WAKE, gv.t = 0, gv.dives = 0, gv.hp = GRAVE_HP;
        break;
    case G_WAKE:   /* (shaking where it stands) */
        if (gv.t >= 36) gv.state = G_RISE, gv.t = 0, gv.vy = -3.0f;
        break;
    case G_RISE:   /* (up out of the ground, drifting toward him) */
        gv.right = rx > gv.x;
        gv.pose = 0;
        gv.vx = (rx > gv.x ? 0.6f : -0.6f);
        gv.vy += 0.1f;
        if (gv.vy >= 0 || gv.y < floor - 100) gv.vy = 0, gv.state = G_AIM, gv.t = 0;
        break;
    case G_AIM:    /* (leaning over toward him, hovering) */
        gv.right = rx > gv.x;
        gv.vx *= 0.9f;
        gv.vy = sinf(gv.t * 0.3f) * 0.4f;
        gv.pose = gv.t < 10 ? 1 : 2;
        if (gv.t >= 24) {
            float dx = rx - gv.x, dy = (ry - 16) - (gv.y - GRAVE_H / 2), d = sqrtf(dx * dx + dy * dy);
            if (d < 1) d = 1;
            gv.vx = dx / d * 3.4f, gv.vy = dy / d * 3.4f;
            if (gv.vy < 0.6f) gv.vy = 0.6f;
            gv.state = G_DIVE, gv.t = 0;
        }
        break;
    case G_DIVE:
        gv.pose = 1;
        if (gv.y >= floor || gv.t > 70) {   /* (into the ground; up again, or home) */
            gv.y = floor < gv.y ? floor : gv.y;
            gv.dives++;
            gv.state = gv.dives >= 3 ? G_HOME : G_RISE;
            gv.t = 0, gv.vx = 0, gv.vy = -3.0f;
        }
        break;
    case G_HOME: { /* (back over its place, and down onto it) */
        gv.pose = 0;
        float hx = grave_home(), dx = hx - gv.x;
        gv.right = dx > 0;
        gv.vx = fabsf(dx) < 2 ? dx : (dx > 0 ? 1.6f : -1.6f);
        if (gv.y > floor - 60 && fabsf(dx) > 4) gv.vy = -1.2f;
        else if (fabsf(dx) <= 4) gv.vy = 1.4f;
        else gv.vy = 0;
        if (fabsf(dx) <= 4 && gv.y >= floor) {
            gv.x = hx, gv.y = floor, gv.vx = gv.vy = 0;
            gv.state = G_REST, gv.rest_for = 180, gv.right = false;
        }
        break;
    }
    case G_BREAK:  /* (struck down: it shakes and comes apart) */
        if (gv.t >= 40) gv.state = G_GONE, gv.t = 0;
        break;
    case G_GONE:   /* (and after a while it stands in its place again) */
        if (gv.t >= 300) {
            gv.state = G_REST, gv.t = 0, gv.rest_for = 180;
            gv.x = grave_home(), gv.y = floor, gv.vx = gv.vy = 0, gv.right = false, gv.pose = 0;
        }
        break;
    }
    if (gv.state == G_RISE || gv.state == G_AIM || gv.state == G_DIVE || gv.state == G_HOME) {
        gv.x += gv.vx, gv.y += gv.vy;
        if (gv.x < 24) gv.x = 24;
        if (gv.x > 488) gv.x = 488;
        if (gv.y > floor) gv.y = floor;
    }
    if (!live) return;
    /* its touch: Richter hurt */
    if (overlap(rx - 7, ry - 44, rx + 7, ry, gv.x - 13, gv.y - 44, gv.x + 13, gv.y - 2)) hurt_richter(p, 6, gv.x);
    /* the whip on it (as on Death: its box while it strikes) */
    if (gv.inv) return;
    for (int sl = 1; sl < 16; sl++) {
        if (!p->wram[0x0A10 + sl] || p->wram[0x1828 + sl] != 5) continue;
        int fr = p->wram[0x0A68 + sl];
        if (fr < 5 || fr > 7) continue;
        int ox = (int16_t)(p->wram[W_PX_HI + sl] << 8 | p->wram[W_PX + sl]);
        int oy = (int16_t)(p->wram[W_PY_HI + sl] << 8 | p->wram[W_PY + sl]);
        int ahead = (int8_t)p->wram[0x1930 + sl], hw = p->wram[0x1988 + sl];
        int below = (int8_t)p->wram[0x19E0 + sl], hh = p->wram[0x1A38 + sl];
        float cx = (float)(ox + (p->wram[0x0AC0] ? -ahead : ahead) + cam), cy = (float)(oy + below);
        if (overlap(cx - hw, cy - hh, cx + hw, cy + hh, gv.x - 14, gv.y - 46, gv.x + 14, gv.y)) {
            gv.flash = 8, gv.inv = 20;
            if (--gv.hp <= 0) {
                gv.state = G_BREAK, gv.t = 0;
            } else if (gv.state == G_DIVE || gv.state == G_AIM) {   /* (knocked back up) */
                gv.state = G_RISE, gv.t = 0, gv.vy = -2.5f;
                gv.x += rx < gv.x ? 8 : -8;
            }
            return;
        }
    }
}

/* the graves, drawn in the room (screen: cam the room's camera); host-drawn if the disc's are not there */
static void draw_graves(const Canvas *cv, int cam, int maria_x)
{
    if (gv.loaded <= 0) {
        draw_tombstone(cv, (int)grave_home() - cam, ROOM_FLOOR);
        draw_tombstone(cv, maria_x, ROOM_FLOOR);
        return;
    }
    grave_draw_at(cv, 0, maria_x, ROOM_FLOOR, false, false);
    if (gv.state == G_GONE) return;
    if (gv.state == G_OFF) {
        grave_draw_at(cv, 0, (int)grave_home() - cam, ROOM_FLOOR, false, false);
        return;
    }
    int x = (int)lroundf(gv.x) - cam, y = (int)lroundf(gv.y);
    if (gv.state == G_WAKE || gv.state == G_BREAK) x += (gv.t >> 1) & 1 ? 1 : -1;
    if (gv.state == G_BREAK) g_dissolve = gv.t < 10 ? 0 : (gv.t - 10) * 17 / 30;
    if (gv.state == G_REST && gv.rest_for > 120 && gv.t < 60) g_dissolve = 16 - gv.t * 16 / 60;   /* (back in its place) */
    grave_draw_at(cv, gv.pose, x, y, gv.right, gv.flash > 0 && (gv.flash & 2));
    g_dissolve = 0;
}

/* the strike's burst: a white glow swelling out from her and fading (lt: frames since) */
static void draw_burst(const Canvas *cv, int base, int lt)
{
    int cx = base + (int)mf.x, cy = (int)mf.y - 22;
    int r = 12 + lt * 4, a = 235 * (STRIKE_HOLD - lt) / STRIKE_HOLD;
    for (int y = cy - r; y <= cy + r; y++)
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            /* (bright at the middle, and a brighter rim as it swells) */
            int in = 256 - d2 * 256 / (r * r), rim = d2 * 256 / (r * r) > 200 ? 120 : 0;
            int w = a * (in + rim) / 256;
            if (w > 0) blend(cv, x, y, 0xFFFFFFu, w > 255 ? 255 : w);
        }
}

/* the bolt that strikes Maria: a jagged line from the top of the sky down to her */
static void draw_maria_bolt(const Canvas *cv, int base)
{
    static const uint32_t C[3] = {0xFF7850FFu, 0xFFC8C8FFu, 0xFFFFFFFFu};
    for (int w = 2; w >= 0; w--)
        for (int i = 0; i + 1 < fs.n_bolt_pts; i++) {
            int x0 = fs.bolt_pts[i][0], y0 = fs.bolt_pts[i][1], x1 = fs.bolt_pts[i + 1][0], y1 = fs.bolt_pts[i + 1][1];
            int n = (y1 - y0) > 0 ? y1 - y0 : 1;
            for (int k = 0; k <= n; k++) {
                int x = x0 + (x1 - x0) * k / n, y = y0 + (y1 - y0) * k / n;
                for (int d = -w; d <= w; d++) plot(cv, base + x + d, y, C[2 - w]);
            }
        }
}

/* the arena over the picture, behind the game's sprites; `a` 0-256: how much of it */
static void draw_arena(const Canvas *cv, int a, bool flash, int fb0)
{
    for (int y = 0; y < cv->h; y++)   /* (the picture's last row too: the arena's carried on) */
        for (int fx = 0; fx < cv->w; fx++) {
            if (cv->spr[y * PCE_FB_MAX_W + fx]) continue;
            uint32_t c = arena_px(fx - fb0, y < ARENA_H ? y : ARENA_H - 1, flash), *px = &cv->fb[y * PCE_FB_MAX_W + fx];
            if (a >= 256) {
                *px = c;
                continue;
            }
            uint32_t o = *px;
            int r = ((int)(o >> 16 & 255) * (256 - a) + (int)(c >> 16 & 255) * a) >> 8;
            int g = ((int)(o >> 8 & 255) * (256 - a) + (int)(c >> 8 & 255) * a) >> 8;
            int b = ((int)(o & 255) * (256 - a) + (int)(c & 255) * a) >> 8;
            *px = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
}


/* the torches, at their places in the arena (its x 0 at screen x `base`) */
static void draw_torches(const Canvas *cv, int base, uint64_t frame)
{
    /* (the stand moonlit with the wall in the second phase; the flame, a light, as it is) */
    for (int c = 0; c < 16; c++) {
        fs.torch_dim[0][c] = fs.torch_pal[0][c];
        fs.torch_dim[1][c] = fs.dim ? mix(fs.torch_pal[1][c], moonlit(fs.torch_pal[1][c]), fs.dim) : fs.torch_pal[1][c];
    }
    for (int i = 0; i < 2; i++) {
        Piece stand = {0, 16, 0xB0, 0x1001}, flame = {0, 0, FLAME_CELLS[(frame / 5) % 4], 0x0000};
        int x = base + TORCH_ARENA_X[i], y = ARENA_FLOOR - 48;
        draw_frame(cv, &stand, 1, fs.torch, 0xB0, 0xFFFF, NULL, fs.torch_dim, x, y, 16, false, false);
        draw_frame(cv, &flame, 1, fs.torch, 0xB0, 0xFFFF, NULL, fs.torch_pal, x, y, 16, false, false);
    }
}

/* the game's sprites (Richter, his weapons) faded toward the arena behind them, `shown` 0-256;
 * the HUD (top left) left as it is */
static void fade_sprites(const Canvas *cv, int shown, int fb0)
{
    for (int y = 0; y < cv->h; y++)
        for (int fx = 0; fx < cv->w; fx++) {
            if (!cv->spr[y * PCE_FB_MAX_W + fx]) continue;
            int sx = fx - cv->org;
            if (y < 112 && sx < 112) continue;
            uint32_t c = arena_px(fx - fb0, y < ARENA_H ? y : ARENA_H - 1, false), *px = &cv->fb[y * PCE_FB_MAX_W + fx], o = *px;
            int r = ((int)(c >> 16 & 255) * (256 - shown) + (int)(o >> 16 & 255) * shown) >> 8;
            int g = ((int)(c >> 8 & 255) * (256 - shown) + (int)(o >> 8 & 255) * shown) >> 8;
            int b = ((int)(c & 255) * (256 - shown) + (int)(o & 255) * shown) >> 8;
            *px = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
}

/* the blood: drops thrown from Death as he dies, each through the splash's 8 frames */
static void blood_step(int cx, int cy)
{
    if (fs.state == FIGHT_DYING && fs.t < 110 && fs.t % 3 == 0)
        for (int i = 0; i < 24; i++)
            if (!fs.drop[i].on) {
                fs.drop[i].on = true;
                fs.drop[i].age = 0;
                fs.drop[i].x = (float)(cx - 8 + rnd(20) - 10);
                fs.drop[i].y = (float)(cy - 8 + rnd(30) - 15);
                fs.drop[i].vx = (float)(rnd(50) - 15) / 20.0f;
                fs.drop[i].vy = (float)(rnd(30) - 25) / 20.0f;
                break;
            }
    for (int i = 0; i < 24; i++) {
        if (!fs.drop[i].on) continue;
        fs.drop[i].x += fs.drop[i].vx;
        fs.drop[i].y += fs.drop[i].vy;
        fs.drop[i].vy += 0.06f;
        if (++fs.drop[i].age >= 48) fs.drop[i].on = false;
    }
}

static bool fight_busy(void) { return fs.state != FIGHT_NONE; }

/* The fight's music: the disc's own Bloody Tears (audio track 11), looping, played as the game
 * plays its music; the stage's put back (where it was) once the room is left. */
#define FIGHT_TRACK 11
static void music_fade_out(Pce *p)
{
    if (!fs.music_ours) {
        fs.music_start = p->cdda.start_lba, fs.music_end = p->cdda.end_lba, fs.music_pos = p->cdda.pos;
        fs.music_repeat = p->cdda.repeat, fs.music_playing = p->cdda.playing;
        fs.music_ours = true;
    }
    p->cdda.fade_step = -3;   /* (as the game's CD_FADE $0C: about 2.5 s) */
}
static void music_track(Pce *p, int track)
{
    const DiscTrack *t = disc_track(&p->disc, track);
    if (!t || t->data) return;
    p->cdda.start_lba = t->lba;
    p->cdda.end_lba = t->lba + t->sectors;
    p->cdda.pos = (int64_t)t->lba * 588;
    p->cdda.repeat = true;
    p->cdda.playing = true;
    p->cdda.volume = 256;
    p->cdda.fade_step = 0;
}
static void music_fight(Pce *p) { music_track(p, FIGHT_TRACK); }
static void music_back(Pce *p)
{
    if (!fs.music_ours) return;
    p->cdda.start_lba = fs.music_start, p->cdda.end_lba = fs.music_end, p->cdda.pos = fs.music_pos;
    p->cdda.repeat = fs.music_repeat, p->cdda.playing = fs.music_playing;
    p->cdda.volume = 256;
    p->cdda.fade_step = 0;
    fs.music_ours = false;
}
static void fight_reset(void)
{
    fs.state = FIGHT_NONE;
    fs.music_ours = false;
    fs.dim = fs.ledge_a = 0;
    fs.finale_t = -1;
    fs.n_bat = 0;   /* (a state loaded, or the room left: its map is drawn afresh) */
}
static void fight_start(Pce *p)
{
    if (fs.state == FIGHT_NONE && fight_load(p)) {
        fs.state = FIGHT_DYING, fs.t = 0;
        music_fade_out(p);
        if (fs.have_death_cry) sound_extra_play_at(fs.death_cry, DEATH_CRY_LEN, DEATH_CRY_RATE, 256, 0);
        memset(fs.drop, 0, sizeof fs.drop);
    }
}

void secret_after_frame(Pce *p)
{
    if (p->wram[W_AREA] != AREA || !in_hidden(p) || !scene_load(p)) return;
    Vdc *d = &p->vdc;
    Canvas cv = {d->fb, vdc_sprite_pixels(), d->fb_wide - vdc_fb_shift(), d->fb_w, d->fb_h};
    int cam = scene_cam(p);
    /* (the room's scene by the camera its picture was drawn with: the game draws a frame
     * with the scroll it set the frame before, so the camera in RAM is a step ahead while it
     * moves and the table shook as Richter walked; the background's X scroll, low 9 bits,
     * tells which) */
    cam += ((((int)vdc_row_scroll()[ROOM_FLOOR - 40] - (cam + 512)) & 0x1FF) ^ 0x100) - 0x100;
    /* (the arena in the level, by the camera its picture was drawn with: with Level ends the
     * walkable arena's picture is moved back as its camera follows Richter, so it stands still
     * on the screen; without, it scrolls as a room does) */
    int fb0 = ARENA_X0 - (cam + 512) + cv.org;   /* (the picture's column of the arena's x 0) */
    int lt = fs.t - fs.strike_at;   /* (a strike flashes as the title's: 3 frames, 3 dark, 3) */
    bool flash = fs.state == FIGHT_ARENA && ((lt >= 0 && lt <= 2) || (lt >= 6 && lt <= 8));
    if (fs.state == FIGHT_ARENA) {
        int base = fb0 - cv.org;   /* (the arena's x 0 on the screen) */
        draw_arena(&cv, 256, flash, fb0);
        if (lt >= 0 && lt <= 2 && !fs.strike_maria)   /* (the bolt, its place and side each strike's own) */
            draw_frame(&cv, BOLT, 6, fs.bolt, 0x199, 0xFFFF, NULL, fs.bolt_pal, base + TITLEART_SIDE + fs.strike_dx,
                       -SKY_LIFT, 224, fs.strike_flip, false);
        draw_torches(&cv, base, p->frame);
        draw_ledges(&cv, base);
        mf.org = cv.org, mf.fb0 = fb0;
        maria_draw(&cv, base);
        if (fs.finale_t >= 0) {
            for (int i = 0; i < ORBS; i++)
                if (fs.orb[i].on) {
                    if (fs.have_orb_art) draw_game_orb(&cv, base + (int)fs.orb[i].x, (int)fs.orb[i].y, fs.orb[i].age, fs.orb[i].spark);
                    else draw_orb(&cv, base + (int)fs.orb[i].x, (int)fs.orb[i].y, fs.orb[i].age);
                }
            if (fs.fade_at >= 0) darken_all(&cv, (fs.finale_t - fs.fade_at) * 256 / FIN_FADE_LEN);
        }
        if (fs.strike_maria && lt >= 0 && lt < STRIKE_HOLD) {
            draw_burst(&cv, base, lt);
            if (!(lt & 2)) draw_maria_bolt(&cv, base);   /* (flickering) */
        }
        draw_frame(&cv, GAUGE, 3, fs.gauge, 0x1F8, 0xFFFF, NULL, fs.gauge_pal, base + GAUGE_ARENA_X, 16, 32, false, false);
        gauge_fill(&cv, base + GAUGE_ARENA_X, 16);
        return;
    }
    /* the table, its candle (flickering) and the cards played */
    int tx0 = TABLE_X0 - cam, tx1 = TABLE_X1 - cam, ty = TABLE_Y;
    for (int k = 0; k < 2; k++) {
        int lx = k ? tx1 - 7 : tx0 + 4;
        rect(&cv, lx, ty + 8, lx + 3, ROOM_FLOOR - 1, WOOD);
        rect(&cv, lx, ty + 8, lx, ROOM_FLOOR - 1, WOOD_L);
        rect(&cv, lx + 3, ty + 8, lx + 3, ROOM_FLOOR - 1, WOOD_D);
    }
    rect(&cv, tx0, ty, tx1, ty + 1, WOOD_L);
    rect(&cv, tx0, ty + 2, tx1, ty + 4, WOOD);
    rect(&cv, tx0 + 2, ty + 5, tx1 - 2, ty + 7, WOOD_D);
    int cx = (tx0 + tx1) / 2;
    rect(&cv, cx - 1, ty - 7, cx + 1, ty - 1, 0xFFDADAB6u);
    bool flick = (p->frame / 5) % 3 == 0;
    plot(&cv, cx, ty - 8, 0xFFFFB600u);
    plot(&cv, cx, ty - 9, 0xFFFFB600u);
    plot(&cv, cx + (flick ? 1 : 0), ty - 10, 0xFFFFFF91u);
    plot(&cv, cx - 1, ty - 9, 0xFFFF9100u);
    plot(&cv, cx + 1, ty - 9, 0xFFFF9100u);
    /* where the hands are (relative to the table's middle, at its top) */
    int dx, dy, mx = MARIA_X - cam, my = ROOM_FLOOR - MARIA_H;
    death_at(p, cam, &dx, &dy);
    int hand_x[2] = {dx + 26 - cx, mx - cx}, hand_y[2] = {dy + 46 - ty, my + 20 - ty};
    if (p->frame != sc.last_frame) {
        sc.last_frame = p->frame;
        if (fs.state == FIGHT_NONE) cards_step(hand_x, hand_y);
        else if (fs.state != FIGHT_WON) blood_step(dx + DEATH_W / 2, dy + DEATH_H / 2);
    }
    /* the deck and the cards played */
    for (int i = 0; i < (sc.deck > 4 ? 3 : sc.deck > 0 ? sc.deck < 3 ? sc.deck : 2 : 0); i++)
        card(&cv, cx - 22, ty - 2 - i, 5, 2, 0);
    for (int i = 0; i < sc.n_table; i++) card(&cv, cx + sc.table_x[i], ty - 2, 5, 2, sc.table_kind[i]);
    /* Death, hovering, his cards held toward the table */
    if (fs.state == FIGHT_NONE) {
        draw_frame(&cv, DEATH, 8, sc.death, 0x140, 0x1F4, sc.death + 16 * 128, sc.death_pal, dx, dy, DEATH_W, false, sc.flash > 0);
        for (int i = 0; i < sc.hand[0] && i < 4; i++) card(&cv, dx + 24 + i * 2, dy + 46 - (i & 1), 3, 5, 0);
    } else if (fs.state == FIGHT_DYING) {   /* shaking, flashing, coming apart */
        int t = fs.t;
        g_dissolve = t < 60 ? 0 : (t - 60) * 17 / (DYING_T - 60);
        draw_frame(&cv, DEATH, 8, sc.death, 0x140, 0x1F4, sc.death + 16 * 128, sc.death_pal,
                   dx + (t < 70 ? ((t >> 1) & 1 ? 2 : -2) : 0), dy, DEATH_W, false, t < 48 && !((t >> 2) & 1));
        g_dissolve = 0;
    }
    /* Maria, facing him, her hand of cards up (the fight won: a tombstone where each stood) */
    if (fs.state == FIGHT_WON) {
        draw_graves(&cv, cam, mx + MARIA_W / 2);
    } else {
        draw_frame(&cv, MARIA, 2, sc.maria, 0x184, 0xFFFF, NULL, sc.maria_pal, mx, my, MARIA_W, true, false);
        for (int i = 0; i < sc.hand[1] && i < 4; i++) card(&cv, mx - 2 + i * 2, my + 20 - (i & 1), 3, 5, 0);
    }
    /* a card (or the swept trick) on its way */
    if (sc.moving) {
        double t = (double)sc.mv_t / sc.mv_len;
        int x = cx + sc.mv_from_x + (int)lround((sc.mv_to_x - sc.mv_from_x) * t);
        int y = ty + sc.mv_from_y + (int)lround((sc.mv_to_y - sc.mv_from_y) * t - (sc.mv_sweep ? 0 : 8 * sin(3.14159265 * t)));
        if (sc.mv_sweep)
            for (int i = 0; i < sc.mv_sweep && i < 3; i++) card(&cv, x + i * 2, y, 5, 2, 0);
        else card(&cv, x, y, sc.mv_to == 1 ? 5 : 3, sc.mv_to == 1 ? 2 : 5, sc.mv_to == 1 ? sc.mv_kind : 0);
    }
    candle_glow(&cv, cx, ty - 9, p->frame);
    if (fs.state == FIGHT_WON && fs.won_t < WON_FADE) darken_all(&cv, 256 - fs.won_t * 256 / WON_FADE);
    for (int i = 0; i < 24; i++)   /* the blood */
        if (fs.drop[i].on) {
            Piece d1 = {0, 0, 0x178, 0x0000};
            d1.cell = (uint16_t)(0x178 + fs.drop[i].age / 6);
            draw_frame(&cv, &d1, 1, fs.blood, 0x178, 0xFFFF, NULL, fs.blood_pal, (int)fs.drop[i].x, (int)fs.drop[i].y, 16, false, false);
        }
    if (fs.state == FIGHT_FADE) {
        mf.org = cv.org, mf.fb0 = fb0;
        draw_arena(&cv, fs.t * 256 / FADE_T, false, fb0);
        int half = FADE_T / 2, shown = fs.t < half ? 256 - fs.t * 256 / half : (fs.t - half) * 256 / half;
        fade_sprites(&cv, shown, fb0);
        if (fs.t >= half) {   /* (the arena's torches and Maria coming in) */
            int base = fb0 - cv.org;
            g_dissolve = 16 - shown * 16 / 256;
            draw_torches(&cv, base, p->frame);
            draw_frame(&cv, MARIA, 2, sc.maria, 0x184, 0xFFFF, NULL, sc.maria_pal, base + MARIA_ARENA_X, ARENA_FLOOR - MARIA_H, MARIA_W, true, false);
            g_dissolve = 0;
        }
    }
}
