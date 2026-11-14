/* See moves.h. Richter's and Maria's objects, as docs/findings.md ("The whip", "Objects")
 * describes them. */
#include "moves.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "pce/video.h"

/* object arrays: work RAM offsets, 88 slots each; slot 0 is the player, 1-15 the player's
 * weapons */
#define OBJ_ACTIVE 0x0A10
#define OBJ_FRAME 0x0A68      /* animation frame */
#define OBJ_FACING 0x0AC0     /* 0 right, 1 left */
#define OBJ_X_HI 0x0B70       /* position in the picture: high, low, eighth bits below */
#define OBJ_X 0x0BC8
#define OBJ_X_SUB 0x0C20
#define OBJ_Y_HI 0x0C78
#define OBJ_Y 0x0CD0
#define OBJ_Y_SUB 0x0D28
#define OBJ_VX_HI 0x0D80      /* velocity, 8.8 pixels a frame */
#define OBJ_VX_LO 0x0DD8
#define OBJ_VY_HI 0x0E30
#define OBJ_VY_LO 0x0E88
#define OBJ_STATE 0x0EE0
#define OBJ_TIMER 0x0F90      /* animation: frames to the next */
#define OBJ_KIND 0x1828       /* 5: the player's whip or a dove */
#define OBJ_BOX_X 0x1930      /* hitbox: centre ahead of X (mirrored facing left), half width, */
#define OBJ_BOX_HW 0x1988     /* centre below Y, half height */
#define OBJ_BOX_Y 0x19E0
#define OBJ_BOX_HH 0x1A38

/* Richter's states ($0EE0 of the player) */
enum { RICHTER_ATTACK = 0x03, RICHTER_JUMP_ATTACK = 0x0B };

/* Richter's code, bank $6E at $C000: the routine that gives an object its hitbox from its
 * animation frame ($DD1F: frame -> entry at $DD56, entry -> X ahead, Y, half width, half
 * height at $DDE8), checked before its tables are touched */
#define RICHTER_BANK 0x6E
#define BOX_ENTRY 0x1D56      /* offsets in the bank */
#define BOX_TABLE 0x1DE8
static const uint8_t BOX_CODE[] = {0xBC, 0x68, 0x2A, 0xB9, 0x56, 0xDD, 0xF0, 0xEF, 0x0A,
                                   0x0A, 0xA8, 0xB9, 0xE8, 0xDD, 0x9D, 0x30, 0x39};
#define BOX_CODE_AT 0x1D24
#define STRIKE_FIRST 5        /* the whip's strike: frames 05-07 (standing and jumping) */
#define STRIKE_LAST 7
#define HAND 20               /* Richter's hand, pixels ahead of his X, as the boxes start */
#define WHIP_PATTERNS 0x1A0   /* the whip's sprites: these 16 patterns (in palette 1, or 3 and 4 as it flashes) */

static const float PI_4 = 0.78539816f;

static uint8_t *richter_bank(Pce *p) { return p->superram + (RICHTER_BANK - 0x68) * 0x2000; }

static bool richter(Pce *p)
{
    return p->wram[RONDO_CHARACTER] == 0 && !memcmp(richter_bank(p) + BOX_CODE_AT, BOX_CODE, sizeof BOX_CODE);
}

static int pos16(const Pce *p, int hi, int lo, int slot)
{
    return (int16_t)(p->wram[hi + slot] << 8 | p->wram[lo + slot]);
}

/* the whip: the player's weapon object at Richter's own place (in a jump it follows him a
 * frame late, a few pixels behind) */
static int find_whip(const Pce *p)
{
    int px = pos16(p, OBJ_X_HI, OBJ_X, 0), py = pos16(p, OBJ_Y_HI, OBJ_Y, 0);
    for (int s = 1; s < 16; s++)
        if (p->wram[OBJ_ACTIVE + s] && p->wram[OBJ_KIND + s] == 5 && abs(pos16(p, OBJ_X_HI, OBJ_X, s) - px) <= 4 &&
            abs(pos16(p, OBJ_Y_HI, OBJ_Y, s) - py) <= 8)
            return s;
    return -1;
}

static bool in_air(const Pce *p)
{
    return p->wram[OBJ_STATE] == RICHTER_JUMP_ATTACK || p->wram[OBJ_VY_HI] || p->wram[OBJ_VY_LO];
}

/* the direction held, as moves.h numbers it; down only in the air */
static int aim_dir(uint8_t aim, bool air)
{
    bool ahead = aim & (PAD_LEFT | PAD_RIGHT);
    if (aim & PAD_UP) return ahead ? 1 : 2;
    if (air && (aim & PAD_DOWN)) return ahead ? -1 : -2;
    return 0;
}

void moves_reset(Moves *m)
{
    memset(m, 0, sizeof *m);
}

uint8_t moves_pad(Moves *m, const Pce *p, const MoveOptions *o, uint8_t pad, bool attack_held)
{
    m->aim = pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
    m->attack = attack_held;
    if (o->eight_way && attack_held && game_playing(p)) pad &= (uint8_t)~PAD_UP;
    /* while the whip hangs the d-pad swings it: the game, with Richter in his strike's last
     * frame, would take a direction as the end of the attack */
    if (m->limp && attack_held) pad &= (uint8_t)~(PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
    return pad;
}

/* ---- the whip's hitbox -------------------------------------------------------------- */

/* A strike frame's box (X ahead, Y, half width, half height) turned `dir` eighths of a turn
 * around Richter's hand: the box of the whip turned, cut down on the diagonals (a box round
 * a diagonal whip covers more than the whip). */
static void box_turned(const uint8_t in[4], int dir, uint8_t out[4])
{
    int xo = (int8_t)in[0], yo = (int8_t)in[1], hw = in[2], hh = in[3];
    int end = xo + hw, hand = HAND;
    if (end <= hand + 4) hand = xo - hw;       /* the short first frame */
    float half = (float)(end - hand) / 2, a = (float)dir * PI_4, c = cosf(a), s = sinf(a);
    float cx = (float)hand + half * c, cy = (float)yo - half * s;
    float ex = fabsf(c) * half + fabsf(s) * (float)hh, ey = fabsf(s) * half + fabsf(c) * (float)hh;
    if (dir == 1 || dir == -1) { ex *= 0.8f; ey *= 0.8f; }
    out[0] = (uint8_t)(int8_t)lroundf(cx);
    out[1] = (uint8_t)(int8_t)lroundf(cy);
    out[2] = (uint8_t)lroundf(ex);
    out[3] = (uint8_t)lroundf(ey);
}

/* The strike frames' boxes are ours for one frame at a time: written before it, the game's
 * own put back after it, so no save state or rewind step ever holds them. */
static void box_write(Moves *m, Pce *p, int k, const uint8_t box[4])
{
    uint8_t *bank = richter_bank(p);
    uint16_t at = (uint16_t)(BOX_TABLE + 4 * bank[BOX_ENTRY + STRIKE_FIRST + k]);
    if (at + 4 > 0x2000) return;
    if (!m->patch[k].on) {
        m->patch[k].at = at;
        memcpy(m->patch[k].orig, bank + at, 4);
        m->patch[k].on = true;
    }
    memcpy(m->patch[k].ours, box, 4);
    memcpy(bank + at, box, 4);
}

static void boxes_restore(Moves *m, Pce *p)
{
    uint8_t *bank = richter_bank(p);
    for (int k = 0; k < 3; k++) {
        if (!m->patch[k].on) continue;
        /* (unless the frame loaded a stage, with its own) */
        if (!memcmp(bank + m->patch[k].at, m->patch[k].ours, 4)) memcpy(bank + m->patch[k].at, m->patch[k].orig, 4);
        m->patch[k].on = false;
    }
}

static void boxes_turn(Moves *m, Pce *p, int dir)
{
    uint8_t *bank = richter_bank(p);
    for (int k = 0; k < 3; k++) {
        uint16_t at = (uint16_t)(BOX_TABLE + 4 * bank[BOX_ENTRY + STRIKE_FIRST + k]);
        if (at + 4 > 0x2000) continue;
        uint8_t box[4];
        box_turned(m->patch[k].on ? m->patch[k].orig : bank + at, dir, box);
        box_write(m, p, k, box);
    }
}

/* ---- the whip's picture --------------------------------------------------------------- */

/* a sprite pattern's pixel (16 x 16 cell, 4 planes of 16 words) */
static int pattern_pixel(const Vdc *d, int cell, int x, int y)
{
    uint32_t a = ((uint32_t)cell * 64 + (uint32_t)y) & 0x7FFF;
    int bit = 15 - x;
    return ((d->vram[a] >> bit) & 1) | ((d->vram[(a + 16) & 0x7FFF] >> bit) & 1) << 1 |
           ((d->vram[(a + 32) & 0x7FFF] >> bit) & 1) << 2 | ((d->vram[(a + 48) & 0x7FFF] >> bit) & 1) << 3;
}

/* The strike's sprites (around Richter) into the window, and left out of the
 * picture; Richter's hand is where the whip starts. False if there are none. */
static bool gather(Moves *m, Pce *p)
{
    const Vdc *d = &p->vdc;
    int px = pos16(p, OBJ_X_HI, OBJ_X, 0), py = pos16(p, OBJ_Y_HI, OBJ_Y, 0);
    m->win_x = px - MOVES_WIN_W / 2;
    m->win_y = py - 112;
    memset(m->win, 0, sizeof m->win);
    uint64_t mask = 0;
    for (int i = 0; i < 64; i++) {
        const uint16_t *s = &d->satb[i * 4];
        uint16_t attr = s[3];
        int pattern = (s[2] >> 1) & 0x3FF;
        if (pattern < WHIP_PATTERNS || pattern >= WHIP_PATTERNS + 16) continue;
        int sx = (s[1] & 0x3FF) - 32, sy = (s[0] & 0x3FF) - 64;
        int w = attr & 0x0100 ? 32 : 16, cgy = (attr >> 12) & 3, h = cgy == 0 ? 16 : cgy == 1 ? 32 : 64;
        if (sx + w <= px - 96 || sx >= px + 96 || sy + h <= py - 80 || sy >= py + 16) continue;
        int base = pattern & ~(w == 32 ? 1 : 0) & ~(h == 32 ? 2 : h == 64 ? 6 : 0);
        bool fx = attr & 0x0800, fy = attr & 0x8000;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int xx = fx ? w - 1 - x : x, yy = fy ? h - 1 - y : y;
                int c = pattern_pixel(d, base + (xx >> 4) + (yy >> 4) * 2, xx & 15, yy & 15);
                int wx = sx + x - m->win_x, wy = sy + y - m->win_y;
                if (c && wx >= 0 && wx < MOVES_WIN_W && wy >= 0 && wy < MOVES_WIN_H)
                    m->win[wy][wx] = (uint8_t)((attr & 0xF) << 4 | c);
            }
        mask |= 1ull << i;
    }
    if (!mask) return false;
    /* the hand: the whip's nearest column to Richter, the middle of what is in it */
    int near = -1;
    for (int k = 0; k < MOVES_WIN_W && near < 0; k++) {
        int x = m->facing > 0 ? k : MOVES_WIN_W - 1 - k;
        if ((x + m->win_x - px) * m->facing < 4) continue;   /* (behind the hand: Richter's) */
        for (int y = 0; y < MOVES_WIN_H; y++)
            if (m->win[y][x]) { near = x; break; }
    }
    if (near < 0) return false;
    int sum = 0, n = 0;
    for (int k = 0; k < 4; k++) {
        int x = near + k * m->facing;
        if (x < 0 || x >= MOVES_WIN_W) break;
        for (int y = 0; y < MOVES_WIN_H; y++)
            if (m->win[y][x]) { sum += y; n++; }
    }
    m->pivot_x = near + m->win_x;
    m->pivot_y = sum / n + m->win_y;
    m->hand_f = (m->pivot_x - px) * m->facing;
    m->hand_y = m->pivot_y - py;
    vdc_set_sprite_hide(mask);
    return true;
}

/* the window's pixels turned `dir` eighths of a turn around the hand, into the picture */
static void draw_turned(const Moves *m, Pce *p)
{
    Vdc *d = &p->vdc;
    const uint32_t *argb = &p->vce.argb[0x100];
    float a = (float)m->dir * PI_4, c = cosf(a), s = sinf(a);
    for (int y = 0; y < MOVES_WIN_H; y++) {
        int py = m->win_y + y;
        if (py < 0 || py >= d->fb_h) continue;
        for (int x = 0; x < MOVES_WIN_W; x++) {
            int px = m->win_x + x;
            if (px < 0 || px >= d->fb_w) continue;
            float f = (float)((px - m->pivot_x) * m->facing), u = (float)(m->pivot_y - py);
            float f0 = f * c + u * s, u0 = u * c - f * s;
            if (f0 < -1.5f) continue;                       /* (only what is ahead of the hand) */
            int sx = m->pivot_x + (int)lroundf(f0) * m->facing - m->win_x, sy = m->pivot_y - (int)lroundf(u0) - m->win_y;
            if (sx < 0 || sx >= MOVES_WIN_W || sy < 0 || sy >= MOVES_WIN_H || !m->win[sy][sx]) continue;
            d->fb[py * PCE_FB_MAX_W + px] = argb[m->win[sy][sx]];
        }
    }
}

/* ---- doves -------------------------------------------------------------------------------- */

/* A dove flying out moves as the game moves it, turned: its position gets what its velocity
 * turned adds to it. */
static void dove_turn(Pce *p, int s, int dir, int facing)
{
    int vx = (int16_t)(p->wram[OBJ_VX_HI + s] << 8 | p->wram[OBJ_VX_LO + s]);
    int vy = (int16_t)(p->wram[OBJ_VY_HI + s] << 8 | p->wram[OBJ_VY_LO + s]);
    float a = (float)dir * PI_4, c = cosf(a), sn = sinf(a);
    float f = (float)(vx * facing), u = (float)-vy;
    float f2 = f * c - u * sn, u2 = f * sn + u * c;
    int dx = (int)lroundf(f2) * facing - vx, dy = (int)lroundf(-u2) - vy;
    int32_t x = (int32_t)pos16(p, OBJ_X_HI, OBJ_X, s) * 256 + p->wram[OBJ_X_SUB + s] + dx;
    int32_t y = (int32_t)pos16(p, OBJ_Y_HI, OBJ_Y, s) * 256 + p->wram[OBJ_Y_SUB + s] + dy;
    p->wram[OBJ_X_HI + s] = (uint8_t)(x >> 16);
    p->wram[OBJ_X + s] = (uint8_t)(x >> 8);
    p->wram[OBJ_X_SUB + s] = (uint8_t)x;
    p->wram[OBJ_Y_HI + s] = (uint8_t)(y >> 16);
    p->wram[OBJ_Y + s] = (uint8_t)(y >> 8);
    p->wram[OBJ_Y_SUB + s] = (uint8_t)y;
}

static void doves(Moves *m, Pce *p, const MoveOptions *o)
{
    bool maria = p->wram[RONDO_CHARACTER] == 1 && o->eight_way && game_playing(p);
    for (int s = 1; s < 16; s++) {
        bool act = p->wram[OBJ_ACTIVE + s] && p->wram[OBJ_KIND + s] == 5;
        if (!act || !maria) {
            m->dove[s].on = false;
        } else if (!m->active[s]) {                   /* thrown this frame */
            m->dove[s].on = true;
            m->dove[s].dir = aim_dir(m->aim, in_air(p));
            m->dove[s].facing = p->wram[OBJ_FACING] ? -1 : 1;
        }
        if (m->dove[s].on && m->dove[s].dir && p->wram[OBJ_STATE + s] == 0)   /* flying out */
            dove_turn(p, s, m->dove[s].dir, m->dove[s].facing);
        m->active[s] = act;
    }
}

/* ---- the limp whip ------------------------------------------------------------------------ */

#define LINKS 8               /* the hanging whip: 8 links of 6 pixels, the last its tip */
#define LINK 6
#define TIP 10
static const float PI_F = 3.14159265f;

static float wrap(float a)
{
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}

/* where the d-pad points (0 ahead, up positive); nothing held: hanging straight down */
static float limp_target(uint8_t aim, int facing)
{
    int f = aim & (facing > 0 ? PAD_RIGHT : PAD_LEFT) ? 1 : aim & (facing > 0 ? PAD_LEFT : PAD_RIGHT) ? -1 : 0;
    int u = aim & PAD_UP ? 1 : aim & PAD_DOWN ? -1 : 0;
    return f || u ? atan2f((float)u, (float)f) : -PI_F / 2;
}

/* The first link turns towards the d-pad, a step at a time; each link after it follows the
 * one before, a little late, so the whip curves as it swings. */
static void limp_swing(Moves *m)
{
    float d = wrap(limp_target(m->aim, m->facing) - m->link_angle[0]), step = 0.22f;
    m->link_angle[0] += d > step ? step : d < -step ? -step : d;
    for (int i = 1; i < LINKS; i++) m->link_angle[i] += wrap(m->link_angle[i - 1] - m->link_angle[i]) * 0.45f;
}

static void limp_start(Moves *m)
{
    m->limp = true;
    m->limp_frames = 0;
    for (int i = 0; i < LINKS; i++) m->link_angle[i] = (float)m->dir * PI_4;
    if (!m->hand_f) { m->hand_f = 22; m->hand_y = -11; }
}

/* Richter and the whip held in the strike's last frame; the whip's box on its tip */
static void limp_hold(Moves *m, Pce *p, int s)
{
    p->wram[OBJ_TIMER] = 4;
    p->wram[OBJ_TIMER + s] = 4;
    limp_swing(m);
    float tf = (float)m->hand_f, ty = (float)m->hand_y;
    for (int i = 0; i < LINKS; i++) {
        float len = i == LINKS - 1 ? TIP : LINK;
        tf += len * cosf(m->link_angle[i]);
        ty -= len * sinf(m->link_angle[i]);
    }
    uint8_t box[4] = {(uint8_t)(int8_t)lroundf(tf), (uint8_t)(int8_t)lroundf(ty), 8, 8};
    box_write(m, p, STRIKE_LAST - STRIKE_FIRST, box);
    p->wram[OBJ_BOX_X + s] = box[0];
    p->wram[OBJ_BOX_Y + s] = box[1];
    p->wram[OBJ_BOX_HW + s] = box[2];
    p->wram[OBJ_BOX_HH + s] = box[3];
    m->limp_frames++;
}

/* let go: the strike's last frame and Richter's end on the next frame, as they would have */
static void limp_end(Moves *m, Pce *p, int s)
{
    m->limp = false;
    if (s >= 0) {
        p->wram[OBJ_TIMER] = 1;
        p->wram[OBJ_TIMER + s] = 1;
    }
}

/* how far ahead of the hand the straight whip in the window reaches */
static int whip_length(const Moves *m)
{
    int len = 0;
    for (int f = 0; f < MOVES_WIN_W; f++) {
        int x = m->pivot_x + f * m->facing - m->win_x;
        if (x < 0 || x >= MOVES_WIN_W) break;
        for (int u = -6; u <= 6; u++) {
            int y = m->pivot_y - u - m->win_y;
            if (y >= 0 && y < MOVES_WIN_H && m->win[y][x]) { len = f + 1; break; }
        }
    }
    return len;
}

/* the links, each a piece of the straight whip turned to its angle, end to end from the hand */
static void draw_limp(const Moves *m, Pce *p)
{
    Vdc *d = &p->vdc;
    const uint32_t *argb = &p->vce.argb[0x100];
    int len = whip_length(m);
    if (len < TIP + LINK) return;
    float x = (float)m->pivot_x, y = (float)m->pivot_y;
    for (int i = 0; i < LINKS; i++) {
        bool tip = i == LINKS - 1;
        float l = tip ? TIP : LINK, a = m->link_angle[i], c = cosf(a), sn = sinf(a), w = tip ? 5.5f : 3.5f;
        int src = tip ? len - TIP : 8 + (i & 1) * LINK;
        float ex = x + l * c * (float)m->facing, ey = y - l * sn;
        int x0 = (int)fminf(x, ex) - 6, x1 = (int)fmaxf(x, ex) + 6, y0 = (int)fminf(y, ey) - 6, y1 = (int)fmaxf(y, ey) + 6;
        for (int py = y0; py <= y1; py++) {
            if (py < 0 || py >= d->fb_h) continue;
            for (int px = x0; px <= x1; px++) {
                if (px < 0 || px >= d->fb_w) continue;
                float df = ((float)px - x) * (float)m->facing, du = y - (float)py;
                float fl = df * c + du * sn, ul = du * c - df * sn;
                if (fl < -0.5f || fl >= l - 0.5f || fabsf(ul) > w) continue;
                int sx = m->pivot_x + (src + (int)lroundf(fl)) * m->facing - m->win_x;
                int sy = m->pivot_y - (int)lroundf(ul) - m->win_y;
                if (sx < 0 || sx >= MOVES_WIN_W || sy < 0 || sy >= MOVES_WIN_H || !m->win[sy][sx]) continue;
                d->fb[py * PCE_FB_MAX_W + px] = argb[0x10 | (m->win[sy][sx] & 0xF)];   /* (not flashing) */
            }
        }
        x = ex;
        y = ey;
    }
}

/* ---- each frame ------------------------------------------------------------------------- */

void moves_before_frame(Moves *m, Pce *p, const MoveOptions *o, bool live)
{
    m->draw = m->draw_limp = false;
    if (!live) return;
    bool on = (o->eight_way || o->limp_whip) && game_playing(p) && richter(p);
    int s = on ? find_whip(p) : -1;
    m->whip = s;
    if (s < 0) {
        m->dir = 0;
        m->limp = false;
        return;
    }
    uint8_t f = p->wram[OBJ_FRAME + s], st = p->wram[OBJ_STATE];
    bool strike = f >= STRIKE_FIRST && f <= STRIKE_LAST;
    bool attacking = st == RICHTER_ATTACK || st == RICHTER_JUMP_ATTACK;
    if (!strike) {                                    /* winding up: the direction held */
        m->dir = o->eight_way && attacking ? aim_dir(m->aim, st == RICHTER_JUMP_ATTACK) : 0;
        m->facing = p->wram[OBJ_FACING] ? -1 : 1;
        m->limp = false;
    }
    if (m->dir) boxes_turn(m, p, m->dir);             /* the strike's boxes, for this frame */
    bool hold = o->limp_whip && f == STRIKE_LAST && m->attack && attacking;
    if (hold && !m->limp) limp_start(m);
    if (!hold && m->limp) limp_end(m, p, s);
    if (m->limp) limp_hold(m, p, s);
    if (m->limp) m->draw_limp = gather(m, p);
    else if (strike && m->dir) m->draw = gather(m, p);
}

void moves_after_frame(Moves *m, Pce *p, const MoveOptions *o, bool live)
{
    if (live) boxes_restore(m, p);
    if (m->draw || m->draw_limp) vdc_set_sprite_hide(0);
    if (m->draw) draw_turned(m, p);
    if (m->draw_limp) draw_limp(m, p);
    if (live) doves(m, p, o);
}
