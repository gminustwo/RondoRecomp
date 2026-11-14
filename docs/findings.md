# Rondo of Blood: reverse-engineering notes

Target image: "Castlevania Rondo of Blood [English]" (CUE + 2 Mode1/2048 ISO + 20 WAV).
Everything below was derived with the tools in `tools/`; addresses are logical
(CPU) addresses unless written `bank:addr`.

## Disc layout

| Track | Type | LBA | Records | Notes |
|---|---|---|---|---|
| 1 | audio | 0 | 3665 | warning track |
| 2 | data | 3890 (`$0F32`) | 10149 | **the track the game reads** |
| 3-21 | audio | 14189… | | CD-DA music |
| 22 | data | 211262 | 13300 | second base (via `CD_BASE`); same layout as track 2 for most records, differs in ~1,650 records; tail is zero padding |

Record numbers are relative to track 2's INDEX 01. Loader tables store absolute
LBAs and subtract `$0F32`, which confirms the 3:00 pregap layout.

## Boot sequence

1. IPL header (track 2, record 1): load 1 record from record 2 to `$3000` (work RAM), jump `$3000`.
   MPR2-6 header bytes are `$00-$04` (assumed relative to `$80`; unverified, and unused since the
   boot code lives in work RAM).
2. Boot program (`$3000`):
   - clears VRAM through the VDC ports
   - `CD_BASE` ×2: base 1 = track `$02`, base 2 = track `$22` (BCD)
   - `EX_GETVER`: System Card < 3 → loads 20 records from record 35 to `$4000` and runs it
     (presumably the "Super System Card required" screen)
   - `EX_MEMOPEN` (`$E0DE`) → A = first Super RAM bank (`$68`), stored at zp `$DB`
   - runs boot loader group 0, maps MPR2/3 = `$69`/`$6A`, jumps `$4000`
3. Main init (`$40F9`): clears work RAM (overwrites the boot sector), sets screen mode,
   loads main-table groups 4 and 6 and an ADPCM clip, installs IRQ vectors with `EX_SETVEC`
   (1 → `$4177`, 2 → `$4297`), then idles in `JMP *`. **All game logic runs from interrupts.**

## Overlay loader format

Group table: 5-byte entries `[lba lo, lba mid, lba hi, cmdlist lo, cmdlist hi]`.

Command list:

| Bytes | Meaning |
|---|---|
| `00 b4 b5 b6` | MPR4/5/6 = `$DB` + b4/b5/b6 |
| `nn off cnt` (nn `$01-$7F`) | CD_READ `cnt` records from group record + `off` to dest[nn] |
| `C0 lo hi` | continue at command list `hi:lo` (main loader only) |
| `80`, `82`, `83` | post-process via routine at `$8006` in bank `$DB+4` (main loader only) |
| `81` | copy `$C000-$D3FF` → `$A000` with MPR5 = `$DB+4` (main loader only) |
| other `$8x-$FF` | end |

- Boot loader: `$309C`, table `$312F` (only group 0 valid), dest always `$8000`.
- Main loader: `6A:6FD8`, table `$74D1`, dest table `$70F8` (1→`$8000`, 2→`$9000`, 3→`$9400`).
- ADPCM loader: `6A:6E43`, table `$7135` (4-byte entries: lba24, record count), uses `AD_TRANS`.

## Overlay inventory (`tools/list_overlays.py`)

- 95 main-table groups:
  - 24 stage loads (groups 16-87): two 8-record loads into `$7A/$7B` plus one 73-89 record
    load across ~16 banks (post-ops `$80 $81`)
  - 9 × 96-record loads into `$78-$87` (post-op `$82`): scenes / cutscenes, to be confirmed
  - engine/menu groups 0-15, 88-94
- Banks `$69 $6A $75 $76 $77` are loaded once at boot and never reloaded (40 KiB resident).
- Groups cover 3,672 records; ADPCM table 55 clips / 1,791 records; ~4,500 records remain
  (expected: graphics streamed by overlay code directly).

## Resident code (`tools/explore_main.py`)

Bank-aware recursive descent from `$4000` plus both IRQ vectors, after groups 0/4/6:

- ~6,600 instructions / ~14.4 KB of code in banks `$69 $6A $6B $6C $72 $75 $76`
- 96 TAM sites (35 with runtime-computed banks), 12 indirect jumps
  (10 tables found heuristically, 2 through RAM vectors at `$2000`: dynamic dispatch)
- BIOS surface: 24 entries (CD read/seek/play/pause/stat/fade/search, ADPCM
  trans/play/cplay/stop/stat, BM_* saves, EX_* screen/vector/joypad/font, MA_MUL8U)

## System Card 3.0 (checked against a Japanese v3.0 dump, `tools/explore_bios.py`)

- The jump table at `$E000` has 81 entries. Our 77 names line up, including `EX_MEMOPEN` at `$E0DE`.
- Interrupt dispatch: each hardware vector handler tests one bit of `irq_m` (zp `$F5`).
  If the bit is set, it jumps straight through a work-RAM vector and does nothing else:
  bit 0 → IRQ2 `($2200)`, bit 1 → IRQ1 `($2202)`, bit 2 → timer `($2204)`, bit 3 → NMI `($2206)`.
  When the bit is clear, the BIOS IRQ1 handler does its own frame work: reads VDC status,
  updates the scroll registers from RAM, keeps a frame counter, runs the PSG driver, reads
  the joypads, and optionally calls the vsync (`$2208`, bit 4) and hsync (`$220A`, bit 6) hooks.
- **Rondo sets `irq_m = $06`.** Its IRQ1 (`$4177`) and timer (`$4297`) handlers therefore get the
  hardware interrupts directly, and the BIOS does no per-frame or per-tick work for this game.
  Only IRQ2 (CD/ADPCM) stays with the BIOS, and it is only used inside BIOS CD/ADPCM calls.
- Runtime consequence: the BIOS is needed only as a library of 24 functions. Interrupts can be
  sent straight to the game's handlers without emulating BIOS interrupt code.

## Frame structure (observed at runtime)

- The vblank handler (`$4177`) saves registers and MPR4-6 and reads VDC status. If the raster
  flag is set, it runs the raster dispatcher (table `$44E9`); if the vblank flag is not set, it
  exits. Otherwise it counts the frame (`$47`), re-enables interrupts and runs the frame logic,
  guarded against re-entry by a busy flag (`$33`).
- The second dispatcher (`69:44E6`) selects per-scene raster effects. Entry 0 means "none".
- System Card calls seen from boot to the save menu: CD_READ, CD_PLAY, CD_FADE, CD_STAT,
  AD_TRANS, AD_PLAY, AD_STOP, AD_STAT (every frame), EX_JOYSNS (every frame), EX_GETFNT,
  BM_READ/WRITE/DELETE/FREE (save setup), EX_SCRSIZ.
- EX_GETFNT: the Japanese disc's attract-mode story (frames ~577-6276 of the capture script,
  252 glyphs, 12x12) is drawn from the System Card's kanji font; the English translation uses
  its own font except for "ＤＲＡＣＵＬＡ" (full-width, ~frame 5976).

## Disc releases

| Release | Layout | Data track 2 | Data track 22 |
|---|---|---|---|
| Akumajou Dracula X: Chi no Rondo (JP), e.g. a CloneCD .img/.cue | 22 tracks | original | original |
| English translation (BIN/ISO/WAV per track, 2012 cue) | 22 tracks | translated (1660 sectors differ) | original (+3300 sectors) |
| English translation (redump-style BIN per track, 2024) | 22 tracks | the same as above | translated too |

All share the track layout (CD audio is found by absolute position, so it must match) and the
boot sector (fingerprint `DB4D9415EC501573`); the code differs little (an English-built
`rondo.exe` runs the Japanese disc 99% native). Game RAM, run and Maria behave the same on
both.

## Game state (work RAM, found by watching the game; `tools/frontend/game.h`)

Addresses are offsets in work RAM (bank $F8; $2000+ in the game's address space).

| Offset | Meaning |
|---|---|
| $30 | main state: 00 boot, 02 opening, 03 title, 04 save menu, 05 playing, 06 area change, 08 reset, 0A story cinematic |
| $31 | sub-state of $30 |
| $8D | lives, BCD (3 at the start) |
| $94 | hearts, BCD (shown on the HUD; 10 at the start) |
| $98 | health; $5C (92) is a full bar |
| $A2 | current area (00 before Stage 0, 0A, 10, ...) |

- RUN skips the opening (state 02) but not the Stage 0 cinematic (state 0A, frames 12531-16098
  of the capture run).
- Undoing health losses after each frame keeps Richter alive (verified through the hit at
  frame 17865 and the death at 21924 of the capture run); undoing the lives loss at that death
  leaves the game restarting the area normally.
- $1460 is not an invincibility switch in this version: writing 01 every frame made Richter
  take more hits (it is likely a per-object timer).
- Input: presses shorter than 3 frames are ignored; a second I press 2-8 frames after the
  first one ends makes Richter backflip.

## Characters, save record, player movement

| Where | Meaning |
|---|---|
| zp $9F | character: 0 Richter, 1 Maria. Each stage loads that character's code; the area number has it in bit 0 (Stage 0 is $0A / $0B) |
| $1F40-$1F56 | the current game's save record (copied to backup RAM on saving): $1F40 bit 0 lets the player change to Maria (not offered in the Prologue); $1F41-$1F42 a BCD counter; $1F43-$1F4C areas visited, $1F4D-$1F56 areas cleared (bit per area, one half per character) |
| $0D80 / $0DD8 | player (object slot 0) X velocity, high / low, 8.8 pixels a frame |
| $0AC0 | player facing (0 right, 1 left) |
| $0F90 | player animation frame timer (counts down to the next frame) |

Richter walks at $0140 (1.25 px a frame), jumps at $0150, Maria walks and jumps at $0160. The
walking speed is set once as a walk starts (a two-entry table by facing in Richter's code).
Forcing $9F = 1 before Stage 0 loads plays the Prologue as Maria.

## The whip

Found for the Super Castlevania IV moves (`tools/frontend/moves.c`).

Objects 0-15 are the player and the player's weapons; more of their arrays (work RAM
offsets, 88 slots each): `$0A10` in use, `$0A68` animation frame, `$0AC0` facing, X
`$0B70` / `$0BC8` / `$0C20` (high, low, below the pixel) and Y `$0C78` / `$0CD0` / `$0D28`
(picture coordinates: the sprite table's, and the picture's pixels), velocity `$0D80` /
`$0DD8` and `$0E30` / `$0E88` (8.8), `$0EE0` state, `$0F90` animation timer, `$1828` kind
(5: the whip or a dove), `$1250` can be hit, and the hitbox: `$1930` centre ahead of X
(negated when facing left), `$19E0` centre below Y, `$1988` half width, `$1A38` half
height. The collision code (`$6B:9413` puts the player's objects' box centres in `$2710` /
`$2720`; `$6B:9466` checks each enemy against them) counts a hit when both distances are
under the sums of the half sizes, and stores the enemy's slot in the weapon's `$1880`.

Richter's states (`$0EE0` of object 0): 01 standing, 03 attacking, 06 crouching attack, 0A
jumping, 0B jumping attack. His whip is object 3 at his own X and Y (a frame late in a
jump), frames 02-04 winding up, 05-07 the strike (09-0E crouching; 1E, 1F winding up in a
jump, then 04-07). Its box comes from Richter's code (bank `$6E` at `$C000`, the same in
the stages tried, Stage 1 and area `$14`): `$DD1F` looks the frame up in `$DD56` and copies
the entry at `$DDE8` + 4 x entry (X ahead, Y, half width, half height) when the frame
changes; the strike's entries
are `0F F5 0F 05`, `20 F5 18 03` and `28 F5 20 03` (40 pixels ahead, 11 up, 64 wide).
Its sprites are patterns `$1A0`-`$1AF` (16 x 16, some 32 wide) in palette 1, the last frame
flashing through palettes 3 and 4; the sprite table after a frame matches the RAM after it.
Holding the attack's animation timers (`$0F90` of objects 0 and 3) at 4 keeps Richter and
the whip in the strike's last frame for as long as they are held; let go, both end as usual.
A direction pressed then ends the attack early (into a walk).

Maria's dove (object kind 5) flies out at X speed `$0450` (4.3 pixels a frame) slowing by
`$0030` a frame, state 0, then (state 1, 2) turns back and homes in on her.

## End of a stage

Each stage's data block on the disc (13 of them, 352 sectors apart in the data track, from
sector 1696 of the English disc's track 2) starts with a program loaded at `$9000` in bank 6B.
Its part at `$933F` is the stage's end, a state machine in zp `$80`, run each frame through
`JSR $8000` by main state `$05` (the `$69:510E` table: state 5 is `69:5265`). Its steps
(`$934E` table): 0 `935E` sound, 1 `938B` a text window with the health and hearts, saves the
hearts in `$350D`, and sets `$2ABB` when the health bar is full; 2 `9403` reveals the window;
3 `9513`; 4 `951E` the tally: each frame, health `$98` down 1 while above 0, then hearts `$94`
down 1 (BCD) while above 0, each step adding to the score (`JSR $408A`; an extra life when it
passes a mark); 5 `9573` and 6 `9587` an extra life for a full bar (`$2ABB`); 7 `95AA` puts the
hearts back from `$350D` and sets `$37`, which main state 5 turns into the area change (state
`$06`, `69:54B7`). `$3510` holds each step back for that many frames (`95B5`).

So the end needs health and hearts to reach 0 in the playing state: a cheat that undoes every
loss of health there stops the stage from ending (issue 6). `game.c` lets either be counted
down once it drops more than 12 times in a second, which damage and sub-weapons never do.

## Camera, scrolling and objects

Found for the widescreen option (0.6 to 0.12; taken out in 0.13, see git history for
`tools/frontend/wide.c`).

- Scroll layers at $1E00+ (8 layers): X sub/low/high $1E00/$1E08/$1E10, Y $1E18/$1E20/$1E28,
  velocities $1E6E-$1E86, map page counters $1E8E/$1E96, per-frame copies at $1E39+ for the
  raster split. Layer 0 is the playfield camera; the camera's limits are $1EBE (left page) and
  $1EC6/$1EC7 (right X). The camera follows the player to keep it at screen X 128.
- The level is a grid of 256-pixel screens ($1D00 table, 16 x 16 pages); the background map
  is 512 x 512 (MWR $50). The scroll code streams 32-pixel columns 64 px past the edge it moves
  towards, reading that distance from a small table also used for vertical streaming.
- Objects: 88 slots in parallel arrays at $2A10-$3A38 (+$2000 in CPU space); positions are in
  screen coordinates and are shifted by the camera every frame. Objects spawn from a per-room
  list sorted by world X when within 64 px of the screen, are removed beyond 128 px, and (for
  some) only move within 64 px. Slot $57 is the HUD. Some enemy generators sit fixed to the
  screen at X 320 (Stage 1's skeletons).
- The sprite builder writes up to 64 sprites straight to the VDC and drops any piece whose X is
  outside the 256-pixel screen.
- The Stage 0 carriage chase does not scroll the camera; its scenery layers loop round the
  whole 512-pixel map, part of which is shared between layers. Its raster bands (rows of the
  picture, X scroll): sky 0-62 (+1 every few frames), far trees 63-78 (+1), trees 79-126
  (+4), carriage 127-182, grass 183-224 (+8). For the first ~400 frames the carriage
  (background) is parked in the band's map at scroll 256, just off both edges of the 256
  visible pixels, and its horses (sprite objects 3B, 4B-4E, 51-53 at screen X -224..-72,
  static) just off the left, while the trees (sprite objects, 16x64 pieces) go by; then the
  band scrolls and the carriage drives in from the left.
- The sprite builder (72:8000) keeps the object slot it is drawing in zp `$D9`; pieces with
  X + 32 negative are dropped at 72:8127, those with X + 32 below a size-dependent bound
  ($81A1,X) at 72:813A.

## Open questions

- [ ] Role of track 22 / second `CD_BASE` (error-retry mirror?) and which track carries the translation
- [ ] Post-process routine at `$DB+4:$8006` (decompressor?) — ops `$80/$82/$83`
- [ ] Validate heuristic jump tables and resolve RAM-vector dispatch (needs traces)
- [ ] Code inside stage/scene overlays: entry points and size per overlay
- [x] Confirm System Card entry names against a 3.0 dump (done; 4 trailing entries unnamed, unused)
