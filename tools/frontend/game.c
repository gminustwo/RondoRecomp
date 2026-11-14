/* See game.h. */
#include "game.h"

#include <string.h>

void game_watch_reset(GameWatch *w, const Pce *p)
{
    memset(w, 0, sizeof *w);
    w->last_state = game_state(p);
    w->last_health = p->wram[RONDO_HEALTH];
    w->last_lives = p->wram[RONDO_LIVES];
    w->last_hearts = p->wram[RONDO_HEARTS];
}

static bool menu_state(uint8_t s) { return s <= RONDO_SAVE_MENU || s == 0x08; }

/* Whether the game is counting a value down (game.h, GameTally): it went down more than 12
 * times in the last second; over once it has been quiet for 3 seconds, or the state changes. */
static bool tally_running(GameTally *t, bool state_changed, bool dropped, uint64_t frame)
{
    if (state_changed) {
        t->running = false;
        memset(t->drops, 0, sizeof t->drops);
    }
    t->drops[frame % 60] = dropped;
    t->quiet_frames = dropped ? 0 : t->quiet_frames + 1;
    int drops = 0;
    for (int i = 0; i < 60; i++) drops += t->drops[i];
    if (drops > 12) t->running = true;
    else if (t->running && t->quiet_frames > 180) t->running = false;
    return t->running;
}

void game_apply_cheats(Pce *p, const GameCheats *c, GameWatch *w)
{
    uint8_t s = game_state(p);
    uint8_t *ram = p->wram;
    bool state_changed = s != w->last_state;

    /* Health: undo any loss while playing (the game's own refills still work, and a new
     * area or character keeps the maximum the game sets), except while the game counts it
     * down at a stage's end, which would otherwise never finish. */
    if (c->health) {
        bool dropped = s == RONDO_PLAYING && w->last_state == RONDO_PLAYING && ram[RONDO_HEALTH] < w->last_health;
        if (!tally_running(&w->health_tally, state_changed, dropped, p->frame) && dropped)
            ram[RONDO_HEALTH] = w->last_health;
    }

    /* Lives: undo any loss outside the title and menus. */
    if (c->lives && !menu_state(s) && !menu_state(w->last_state) && ram[RONDO_LIVES] < w->last_lives)
        ram[RONDO_LIVES] = w->last_lives;

    /* Hearts: 99 while playing, except while the game counts them down at a stage's end. */
    if (c->hearts) {
        bool dropped = s == RONDO_PLAYING && ram[RONDO_HEARTS] < w->last_hearts;
        if (!tally_running(&w->hearts_tally, state_changed, dropped, p->frame) && s == RONDO_PLAYING &&
            ram[RONDO_HEARTS] != 0x99)
            ram[RONDO_HEARTS] = 0x99;
    }

    /* Maria: unlocked in the save record while playing (so the game's own player change
     * offers her, and a save keeps it); played from every stage load, the Prologue too,
     * by setting the character whenever a stage may be loading */
    bool loading = s != RONDO_PLAYING || ram[RONDO_SUB] == 0x00;
    if (c->maria_unlocked && s == RONDO_PLAYING) ram[RONDO_UNLOCKS] |= 0x01;
    if (c->play_maria) {
        if (loading) ram[RONDO_CHARACTER] = 1;
        w->maria_forced = true;
    } else if (w->maria_forced && loading) {
        ram[RONDO_CHARACTER] = 0;
        w->maria_forced = false;
    }

    w->last_state = s;
    w->last_health = ram[RONDO_HEALTH];
    w->last_lives = ram[RONDO_LIVES];
    w->last_hearts = ram[RONDO_HEARTS];
}

/* SotN: Richter walks at 1.25 and runs at 2.3 pixels a frame. */
static int run_speed(int walk) { return walk * 46 / 25; }

void game_apply_run(Pce *p, bool held, GameWatch *w)
{
    uint8_t *ram = p->wram;
    if (game_state(p) != RONDO_PLAYING) { w->run_from = 0; return; }
    int v = (int16_t)(ram[RONDO_PLAYER_VX_LO] | ram[RONDO_PLAYER_VX_HI] << 8);
    int mag = v < 0 ? -v : v;
    bool forward = v != 0 && (v < 0) == (ram[RONDO_PLAYER_FACING] != 0);
    bool maria = ram[RONDO_CHARACTER] == 1;
    int walk = maria ? 0x160 : 0x140, jump = maria ? 0x160 : 0x150;   /* 8.8 px a frame */
    int to = -1;
    if (held && forward && (mag == walk || mag == jump)) {
        w->run_from = mag;
        to = run_speed(mag);
    } else if (!held && w->run_from && mag == run_speed(w->run_from)) {
        to = w->run_from;                  /* let go: back to the game's own speed */
        w->run_from = 0;
    } else if (mag == 0) {
        w->run_from = 0;
    }
    if (to >= 0) {
        int nv = v < 0 ? -to : to;
        ram[RONDO_PLAYER_VX_LO] = (uint8_t)nv;
        ram[RONDO_PLAYER_VX_HI] = (uint8_t)(nv >> 8);
        mag = to;
    }
    /* legs to match: the walk cycle's frame timer counts down 6..1; take an extra step
     * off it on 5 frames out of 6 (never past 1, where the game moves to the next frame) */
    if (w->run_from == walk && mag == run_speed(walk) && ram[RONDO_PLAYER_ANIM_TIMER] > 1 && p->frame % 6)
        ram[RONDO_PLAYER_ANIM_TIMER]--;
}
