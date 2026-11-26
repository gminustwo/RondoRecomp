# RondoRecomp

[![release](https://img.shields.io/github/v/release/gminustwo/RondoRecomp?filter=v%2A&sort=semver&display_name=tag&label=release&color=orange)](https://github.com/gminustwo/RondoRecomp/releases/latest)
[![downloads](https://img.shields.io/github/downloads/gminustwo/RondoRecomp/total?label=downloads&color=brightgreen)](https://github.com/gminustwo/RondoRecomp/releases)
[![Patreon](https://img.shields.io/badge/Patreon-support-F96854?logo=patreon&logoColor=white)](https://www.patreon.com/rengar1)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-support-FF5E5B?logo=kofi&logoColor=white)](https://ko-fi.com/rengar12707)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-support-FFDD00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/rengar)

Static recompilation of *Castlevania: Rondo of Blood* (PC Engine Super CD-ROM²) into a native PC application.

## Widescreen 16:9 (experimental)

Video > Widescreen (experimental) holds the widescreen options, each with a help line at the
bottom of the menu.

- **Widescreen** (off by default): shows more of the level on each side, 42 game pixels a
  side with the TV's 4:3 pixels and 64 with square pixels (stretched the last 4% or so to fill
  16:9). Background, enemies, items and Stage 2's rain fill the wider picture; the HUD and a
  boss's health bar stay at the screen's edges; scenery the game drew only as wide as the 4:3
  screen is carried on (Stage 2's bridge). Off in RetroAchievements hardcore mode.
- **Level ends** (on by default): at the start or end of a level, the extra width goes to the
  side the level continues on, so there's no black past the level's wall.
- **Spawn distance** (Far by default): Far brings enemies and items to life further out, so
  they enter the wider picture from beyond it; Near makes them where the 4:3 game does.
- **Room sides** (on by default): rooms that keep the 4:3 picture (boss rooms, shafts) show the
  room mirrored at each side, fading to black, instead of black bars.
- **Open arenas** (on by default): boss rooms that lock the camera to one screen but have no
  wall at their sides are played across the whole 16:9 picture, Richter free to walk to its
  edges. So far: Stage 3's giant three-eyed skull.
- **Wide menus** (on by default): the file select menus and the continue screen go
  widescreen too, their backgrounds carried on to the sides.
- **Title art** (on by default): the title screen and the stage map widened with clouds of
  their own, built in (the art holds only the sides, as colour numbers: none of the game's
  picture). A `title_sides.png` or `map_sides.png` of your own in the profile folder is used
  instead.

What widescreen also takes care of:

- A boss that locks the camera mid-level keeps the wide picture.
- Doors: the next area shows beside the door as you go through; the room you leave stays
  whole on the other side; the door you came through stays in place in the next room (at its
  height, up stairs too); a next room with graphics of its own shows black past its wall
  instead of the old room's leftovers.
- Enemies sent in from just past the screen's edges come from beyond the wider picture's
  edges, never popping in inside it.
- Background objects (the figure on Stage 2's boss tower) stay where the game puts them.
- GAME OVER: the frozen level and the continue screen are both 16:9.
- Weapons hit enemies anywhere in the wider picture.

## Supported Platforms

Currently Windows, Linux x86 (Steam Deck).

> If you want support for a different platform and want to contribute, please feel free to put in a pull request to add that support in.

## Issues
If you have any bugs to report or issues, please use the issues tab to request what's happening. Include as much info as you can. "Game won't work for me" with no info will be closed.

## Game Versions Supported
RondoRecomp works with four releases of *Rondo of Blood*:

PC Engine disc images (a .cue file plus its track files: .bin, .img, .iso and/or .wav; zipped, 7z'd or in a folder), or a .chd made with chdman
- The original Japanese release, Akumajou Dracula X: Chi no Rondo, for the PC Engine Super CD-ROM².
- The English fan translation of that disc. Both the 2012 version and the 2024 redump-style version work.

Virtual Console releases (you supply the console key, which the game doesn't include)
- The Wii .wad, either the USA or the European release. It's the untranslated Japanese game, so it installs as the Japanese disc. If you've already installed it in Dolphin, no key is needed.
- The Wii U title (WUP). This is Konami's English translation, so it installs as the English disc.

If you install one Japanese source and one English source, the Language option in the game switches between them. RetroAchievements counts both as the same game.

All the discs share the same track layout and boot sector. The CD audio is found by its exact position on the disc, so a disc image with a different track layout won't work. **Other versions, such as the PSP's Dracula X Chronicles or the PS4's Castlevania Requiem, aren't supported.**

> [!IMPORTANT]
> **No game data is included or committed.** Tools read your own disc image (CUE sheet with
ISO/WAV/BIN tracks); everything derived from it goes to `work/`, which is git-ignored.

## Features
- Rewinding up to 30 min that works like the Konami Castlevania collections
- Compatible with the original PC Engine version, English patched, Wii, and Wii U versions
- Full suite of video options including filters and screen effects
- Torch glow (Video > Graphic enhancements, off by default): the wall torches light the bricks round them, their light flickering (Stage 3's dungeon torches; only the picture changes)
- Candle glow (Video > Graphic enhancements, off by default): the blue-flamed candles get a soft, flickering blue halo (only the picture changes)
- Alcove glow (Video > Graphic enhancements, off by default): torches in the castle halls' painted alcoves, even half behind the wall, make the alcove's red bricks flicker with their light (only the picture changes)
- Moon glow (Video > Graphic enhancements, off by default): a faint rim of light round the moon on Stage 5's docks (only the picture changes)
- Shield shine (Video > Graphic enhancements, off by default): now and then (3 to 7 s apart) a glint of light runs round the rim of one of the heraldic shields on Stage 5's docks, picked at random, sometimes two (only the picture changes)
- Sail sway (Video > Graphic enhancements, off by default): the purple sails on Stage 5's docks move in the wind, the long ones swaying, the short ones fluttering; on Stage 5's ship the grey sails move too, in a style picked at random each visit (only the picture changes)
- All effects (at the top of Video > Graphic enhancements): turns every graphic enhancement on or off at once
- Night sky (Video > Graphic enhancements, off by default): the stars over Stage 5's docks twinkle faintly, and about once a minute a small shooting star crosses the sky (only the picture changes)
- 16:9 widescreen, with level ends and menus filled in rather than black, and your own art beside the title and stage map
- Save stating
- Cheats
- A pause menu on the game's own pause screen (RUN / START): CONTINUE, SAVE & END (saves your progress without using a continue) and, with Maria unlocked, PLAYER CHANGE, in the game's own letters and heart cursor read from your disc
- Super Castlevania IV moves (Gameplay page): 8-way whipping and dove throwing, and a limp whip, after Pizzano's Super Rondo of Blood hack (RondoRecomp's own version)
- **Reduce slowdown** (Gameplay page, off by default): busy scenes such as Death's fight, scythes flying and blood splashing, keep full speed: the game's CPU gets half a frame of extra time each frame, the picture and sound waiting meanwhile. Not in RetroAchievements hardcore mode
- Sticky stairs (Gameplay page, off by default): a jump comes down onto stairs without Up held
- RetroAchievements
- Discord status: your Discord profile shows the stage you're in (Stage 0 to 7, the other routes' 2' to 5', the final stage), the title screen or the start screen, and for how long; Gameplay page turns it off. Only the Discord app on the same computer is talked to
- Portable mode: tick Portable in Setup (or make a `user` folder next to `rondo.exe` or the AppImage) and options, saves and states are kept there
- 4:3 screens (arcade cabinets): Setup's window always fits the screen and has a 4:3 layout (picked on a 4:3 screen, or with its "4:3 screen" checkbox or `--4:3`); the game's start screen and menus are laid out for 4:3 too

## Analysis tools

Requires Python 3.10+.

```sh
python tools/survey_disc.py   <game.cue>   # track layout, IPL header, data-track comparison
python tools/explore_boot.py  <game.cue>   # boot program: BIOS calls, loader → work/boot.asm
python tools/list_overlays.py <game.cue>   # every overlay group the main loader can load
python tools/explore_main.py  <game.cue>   # resident code map (bank-aware) → work/main.asm
```

Findings so far: [docs/findings.md](docs/findings.md).

## Status

- **Static recompiler:** Setup recompiles the game's code from the player's disc into C and
  builds a native `rondo.exe` with a bundled Zig compiler. On a 20,000-frame run through
  Stage 0, 99.3% of CPU cycles run natively; 402,593 native chunk executions were checked
  against the interpreter with no differences. See [docs/recompiler.md](docs/recompiler.md).
- **Runtime:** HuC6280 interpreter (253/256 opcodes pass every SingleStepTests case), PC Engine
  video, sound (PSG effects, ADPCM voices, CD audio), backup RAM saves, save states, rewind,
  and the System Card functions the game uses, reimplemented in C (no BIOS needed).
- A gothic start screen (a candlelit town at night under a pale moon) and menus in arched
  gold-and-umber frames with blackletter titles, with controller, keyboard and mouse.
- An in-game menu (F1 / Back): video modes for 4K, 16:9 and ultrawide screens (4:3 with
  blurred sides, square pixels, stretch, integer scaling, sharp filtering), a 16:9 widescreen
  option (off by default) that shows more of the level on each side (with the picture moving
  over at level ends and the file select menus wide too),
  cinematic
  skipping, cheats (infinite health, hearts, lives), modern controller layout with
  sub-weapon / Item Crash / backflip buttons, rebindable controls (keys of your own to move, such
  as WASD, alongside the arrows), and a Japanese option (with your Japanese disc).
- The original Japanese disc and the English translation, installed side by side (the
  Japanese story text uses a bundled public-domain kanji font in place of the System Card's).
- Richter's run from Symphony of the Night (hold LB; on by default, Gameplay page turns it off), and Maria cheats (unlocked; play as her
  from the Prologue on). See [docs/runtime.md](docs/runtime.md).
- [RetroAchievements](https://retroachievements.org/game/4855): achievements, leaderboards and
  hardcore mode through RetroAchievements' own client library (rcheevos), for both discs.
  RetroAchievements hasn't recognised Rondo Recomp for hardcore yet. Until it does, its server
  records unlocks as softcore, doesn't keep leaderboard entries, and shows a "Warning: Unknown
  Emulator" notice when the game starts. The game then says "softcore only".
- Windows and Linux (x86-64, glibc 2.35 or newer: the Steam Deck, Ubuntu 22.04, Debian 12,
  Fedora 36, Arch and later).
- Setup takes the disc image zipped, as a 7z, unzipped or as a `.chd` (MAME's compressed CD
  format, which the game reads as it is: about half the space; see
  [docs/distribution.md](docs/distribution.md#chd)), or the Virtual Console releases:
  the Wii's `.wad` (the Japanese game) and the Wii U's title (WUP, the English translation),
  with the console key the player supplies, or a `.wad` installed in Dolphin. See
  [docs/distribution.md](docs/distribution.md#virtual-console).
- Updates from the game itself: the start screen says when a new release is out, and its
  Updates page downloads it (a few MB on Windows, the new AppImage on Linux), checks it,
  installs it, rebuilds the game and restarts it. See [docs/distribution.md](docs/distribution.md#updates).

## For players

Download the release (`RondoRecomp-<version>-windows-x64.zip`, or
`RondoRecomp-<version>-x86_64.AppImage` for Linux and the Steam Deck), run
`RondoRecomp Setup` (on Linux, the AppImage), choose your own disc image (zipped, or the
folder it is unzipped in) or Virtual Console release, then click Play. On the Steam Deck, add the AppImage to Steam as a
non-Steam game. See [docs/PLAYER_README.txt](docs/PLAYER_README.txt) and
[docs/distribution.md](docs/distribution.md).

## Layout

- `runtime/cpu/` HuC6280 interpreter and shared instruction semantics
- `runtime/pce/` PC Engine system, video, disc (cue sheets, and .chd images read in place), backup RAM, System Card replacement
- `runtime/recomp/` native-code dispatch and verification
- `tests/` conformance test runners
- `tools/pce/` platform code: disc access, HuC6280 decoder, System Card table, banked code explorer
- `tools/rondo/` game-specific formats (overlay loader)
- `tools/setup/` Setup (command line and window): zip, 7z and .chd import, disc check, recompile, build with zig cc
- `tools/recomp/` code generator and capture script
- `tools/frontend/` windowed frontend (SDL3)
- `docs/` notes
- `work/` local output and test data (ignored)
