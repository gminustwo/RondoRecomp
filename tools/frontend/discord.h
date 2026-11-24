/* Discord status ("Rich Presence"): the player's Discord profile says they are playing, where
 * (the stage, the title screen, the start screen...) and for how long. Only the Discord app on
 * this computer is talked to (discord.c); nothing goes out to the network from here. */
#pragma once

#include <stdbool.h>

/* Once, after the settings are loaded: the thread that keeps the status. Not in test runs. */
void discord_start(void);
/* What the status says now, and whether it is shown at all (Gameplay: Discord status). Cheap:
 * call it every frame; the thread sends a change at most every 5 s (Discord's limit). Off
 * clears the status at once. */
void discord_set(bool on, const char *details, const char *state);
/* How it is going, for rondo.log: "shown on Discord", "Discord isn't running"... */
const char *discord_status_text(void);
