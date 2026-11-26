/* An easter egg made for RondoRecomp: in Stage 1's house (the room with the stairs and the
 * arched door on the upper floor), the Konami code entered standing by the door (Up Up Down
 * Down Left Right Left Right II I) opens it; Up at the open door takes Richter or Maria into a
 * hidden room the game never had, and Up at that room's own door brings them back to the
 * house, by the door. No SDL here, so rondo_headless can test it (--secret).
 *
 * The room is made the game's own way, from its own pieces (docs/findings.md, "Rooms"): a
 * level grid naming two screens, the screens laid out from Stage 1's 32-pixel blocks, and a
 * room header (camera and limits, where the player starts), entered by the game's own room
 * change (event 3, as walking off a room's edge). Collision comes from the tiles drawn, as it
 * does everywhere in the game. The room loads as the house itself (room 1, route step 1:
 * the stage goes on as before), with the house's grid and header pointers pointed at ours for
 * the change only. Its two screens borrow the slots of the street's first two (screens 1 and
 * 2, the room before the house, not seen from here); the street's own are kept in the stage's
 * RAM while borrowed (so a save state holds them) and put back as soon as the hidden room is
 * left. The door opens by its tiles' graphics in VRAM, the door's pixels lifting into the
 * arch.
 *
 * Off in RetroAchievements hardcore (the caller's choice). Nothing is done on a disc or stage
 * data that does not match what it was made for (the USA disc's Stage 1). */
#pragma once

#include <stdbool.h>

#include "pce/pce.h"
#include "titleart.h"

/* before a frame runs, with p->pad as the game will read it; on: allowed (not in
 * RetroAchievements hardcore mode, and Video > Widescreen 16:9 on: the hidden room's fight is
 * made for the wider picture) */
void secret_frame(Pce *p, bool on);
/* the title art loaded (the arena's sky takes the title's sides from it) */
void secret_set_title_arts(const TitleArt *arts, int n);
/* after a frame: what the host draws over the picture (the hidden room's scene) */
void secret_after_frame(Pce *p);
/* forget the host-side state (after a state load, a rewind, a restart); the stage's RAM is
 * looked at again next frame */
void secret_reset(void);
