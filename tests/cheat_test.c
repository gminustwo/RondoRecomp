/* Cheat tests (tools/frontend/game.c), on work RAM alone (no disc): cheat_test
 *
 * 1. the end of a stage, as the game counts it (docs/findings.md, "End of a stage"): health,
 *    then hearts, down a step a frame to 0 in the playing state; it must finish with and
 *    without the cheats
 * 2. hits while playing are undone, before and after a stage's end
 * 3. hearts spent on sub-weapons come back
 */
#include <stdio.h>
#include <string.h>

#include "game.h"

static Pce pce;
static GameWatch gw;
static int failures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
            failures++;                                      \
        }                                                    \
    } while (0)

static uint8_t bcd_dec(uint8_t v) { return (v & 0x0F) ? (uint8_t)(v - 1) : (uint8_t)(v - 0x10 + 9); }

static void frame(const GameCheats *c)
{
    pce.frame++;
    game_apply_cheats(&pce, c, &gw);
}

/* a new game in Stage 1: full health, hearts at 10 */
static void start(const GameCheats *c)
{
    memset(&pce, 0, sizeof pce);
    pce.wram[RONDO_STATE] = RONDO_PLAYING;
    pce.wram[RONDO_HEALTH] = 0x5C;
    pce.wram[RONDO_HEARTS] = 0x10;
    pce.wram[RONDO_LIVES] = 0x03;
    game_watch_reset(&gw, &pce);
    for (int i = 0; i < 120; i++) frame(c);
}

/* The stage's end (its program's step 4, 951E in the boss overlay): a step of health, else a
 * step of hearts, each frame; done when both are 0. Returns the frames it took, or -1. */
static int stage_end(const GameCheats *c, int limit)
{
    for (int n = 0; n < limit; n++) {
        uint8_t *ram = pce.wram;
        if (ram[RONDO_HEALTH]) ram[RONDO_HEALTH]--;
        else if (ram[RONDO_HEARTS]) ram[RONDO_HEARTS] = bcd_dec(ram[RONDO_HEARTS]);
        else return n;
        frame(c);
    }
    return -1;
}

/* the next stage: the area change, then the game's own refill */
static void next_stage(const GameCheats *c)
{
    for (int i = 0; i < 300; i++) frame(c);   /* the rest of the stage's end: lives, hearts back */
    pce.wram[RONDO_STATE] = RONDO_AREA_CHANGE;
    for (int i = 0; i < 12; i++) frame(c);
    pce.wram[RONDO_STATE] = RONDO_PLAYING;
    pce.wram[RONDO_HEALTH] = 0x5C;
    for (int i = 0; i < 60; i++) frame(c);
}

/* an enemy's hit every second for `secs` seconds; returns the lowest health seen */
static int hits(const GameCheats *c, int secs, int damage)
{
    int low = pce.wram[RONDO_HEALTH];
    for (int s = 0; s < secs; s++) {
        pce.wram[RONDO_HEALTH] = pce.wram[RONDO_HEALTH] > damage ? (uint8_t)(pce.wram[RONDO_HEALTH] - damage) : 0;
        for (int i = 0; i < 60; i++) {
            frame(c);
            if (pce.wram[RONDO_HEALTH] < low) low = pce.wram[RONDO_HEALTH];
        }
    }
    return low;
}

int main(void)
{
    const GameCheats none = {0};
    const GameCheats all = {.health = true, .hearts = true, .lives = true};

    /* 1. the stage's end */
    start(&none);
    int n = stage_end(&none, 2000);
    CHECK(n == 92 + 10, "without cheats the stage's end took %d frames, not %d", n, 92 + 10);

    start(&all);
    CHECK(pce.wram[RONDO_HEARTS] == 0x99, "hearts are %02X with the cheat, not 99", pce.wram[RONDO_HEARTS]);
    n = stage_end(&all, 2000);
    CHECK(n >= 0, "with the cheats the stage's end never finished (health %02X, hearts %02X)",
          pce.wram[RONDO_HEALTH], pce.wram[RONDO_HEARTS]);
    CHECK(n >= 0 && n < 92 + 99 + 60, "with the cheats the stage's end took %d frames", n);
    printf("stage's end: %d frames without cheats, %d with health, hearts and lives\n", 92 + 10, n);

    /* 2. hits are undone, in the next stage too */
    next_stage(&all);
    CHECK(pce.wram[RONDO_HEARTS] == 0x99, "hearts are %02X in the next stage, not 99", pce.wram[RONDO_HEARTS]);
    int low = hits(&all, 20, 8);
    CHECK(low == 0x5C, "after a stage's end, hits took health down to %d", low);

    start(&all);
    low = hits(&all, 20, 8);
    CHECK(low == 0x5C, "hits took health down to %d", low);
    low = hits(&all, 3, 0x5C);
    CHECK(low == 0x5C, "a hit for the whole bar took health down to %d", low);
    start(&none);
    low = hits(&none, 3, 8);
    CHECK(low == 0x5C - 24, "without the cheat 3 hits of 8 left %d, not %d", low, 0x5C - 24);

    /* 3. sub-weapons: a throw for 5 hearts twice a second */
    start(&all);
    int fewest = 0x99;
    for (int i = 0; i < 1200; i++) {
        if (i % 30 == 0) for (int k = 0; k < 5; k++) pce.wram[RONDO_HEARTS] = bcd_dec(pce.wram[RONDO_HEARTS]);
        frame(&all);
        if (pce.wram[RONDO_HEARTS] < fewest) fewest = pce.wram[RONDO_HEARTS];
    }
    CHECK(fewest == 0x99, "sub-weapons left hearts at %02X", fewest);

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("cheats: all checks passed\n");
    return 0;
}
