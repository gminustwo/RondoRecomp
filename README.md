# RondoRecomp

Work in progress: static recompilation of *Castlevania: Rondo of Blood* (PC Engine Super CD-ROM²)
into a native PC application.

**No game data is included or committed.** Tools read your own disc image (CUE sheet with
ISO/WAV/BIN tracks); everything derived from it goes to `work/`, which is git-ignored.

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
  blurred sides, square pixels, stretch, integer scaling, sharp filtering), cinematic
  skipping, cheats (infinite health, hearts, lives), modern controller layout with
  sub-weapon / Item Crash / backflip buttons, and a Japanese option (with your Japanese disc).
- The original Japanese disc and the English translation, installed side by side (the
  Japanese story text uses a bundled public-domain kanji font in place of the System Card's).
- Richter's run from Symphony of the Night (hold LB), and Maria cheats (unlocked; play as her
  from the Prologue on). See [docs/runtime.md](docs/runtime.md).
- [RetroAchievements](https://retroachievements.org/game/4855): achievements, leaderboards and
  hardcore mode through RetroAchievements' own client library (rcheevos), for both discs.
- Windows and Linux (x86-64, glibc 2.35 or newer: the Steam Deck, Ubuntu 22.04, Debian 12,
  Fedora 36, Arch and later).
- Updates from the game itself: the start screen says when a new release is out, and its
  Updates page downloads it (a few MB on Windows, the new AppImage on Linux), checks it,
  installs it, rebuilds the game and restarts it. See [docs/distribution.md](docs/distribution.md#updates).

## For players

Download the release (`RondoRecomp-<version>-windows-x64.zip`, or
`RondoRecomp-<version>-x86_64.AppImage` for Linux and the Steam Deck), run
`RondoRecomp Setup` (on Linux, the AppImage), choose your own disc image (zipped, or the
folder it is unzipped in), then click Play. On the Steam Deck, add the AppImage to Steam as a
non-Steam game. See [docs/PLAYER_README.txt](docs/PLAYER_README.txt) and
[docs/distribution.md](docs/distribution.md).

## Layout

- `runtime/cpu/` HuC6280 interpreter and shared instruction semantics
- `runtime/pce/` PC Engine system, video, disc, backup RAM, System Card replacement
- `runtime/recomp/` native-code dispatch and verification
- `tests/` conformance test runners
- `tools/pce/` platform code: disc access, HuC6280 decoder, System Card table, banked code explorer
- `tools/rondo/` game-specific formats (overlay loader)
- `tools/setup/` Setup (command line and window): zip import, disc check, recompile, build with zig cc
- `tools/recomp/` code generator and capture script
- `tools/frontend/` windowed frontend (SDL3)
- `docs/` notes
- `work/` local output and test data (ignored)
