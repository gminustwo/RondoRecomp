# Distribution: player-supplied game, recompiled on the player's PC

Goal: the release contains no game code or data. The player provides their own copy of the
game as a zip, and a setup program turns it into a native `rondo.exe` on their machine.
This is the same model as LASRecomp: tools are shipped, the game-specific code is generated locally.

## What the player does

1. Download the RondoRecomp release and unpack it anywhere.
2. Run **RondoRecomp Setup** and choose the zip of their *Castlevania: Rondo of Blood* disc image
   ("Disc zip...", which also takes a .cue file), or the folder it is unzipped in ("Disc
   folder...": every .cue under it is checked where it is, and the first Rondo of Blood disc is
   copied in).
3. Setup checks the disc, extracts what it needs, recompiles the game code and produces `rondo.exe`.
4. From then on: double-click `rondo.exe`.

## Release contents (no game material)

| Item | Purpose | License |
|---|---|---|
| `RondoRecomp Setup.exe` | import, check, extract, recompile, build | ours |
| `sdk/` | prebuilt runtime static library + headers (CPU fallback, video, audio, System Card replacement, frontend) | ours |
| `SDL3.dll` (+ deps) | window, input, audio | zlib |
| `zig/` | C compiler and linker used during setup (`zig cc`), Zig 0.17.0, trimmed | MIT; LLVM/Clang/LLD: Apache 2.0 with LLVM exceptions |

## Setup pipeline

1. **Import**: read the zip (CUE with BIN, IMG, ISO and/or WAV tracks). Nothing is written until the disc checks out.
2. **Identify**: PC Engine CD signature on the first data track, then a fingerprint of the boot
   program sector to recognise the release (unknown releases are refused or need explicit confirmation).
   The original Japanese disc and the English translation share the boot sector; the language
   comes from sectors 5-10 of the data track, which the translation changes (`discid_language`).
3. **Extract** to `game/en/` or `game/ja/` by language, keeping a disc already installed in the
   other language (a single disc from Setup up to 0.6, straight in `game/`, is moved into its
   language's folder first).
4. **Recompile**: the generator (built into Setup, no Python needed) walks the boot loader and the
   95 overlay groups from the main loader table, discovers code in every overlay, and emits C into
   `build/gen/`. Code that cannot be proven statically keeps an interpreter fallback.
5. **Build**: `zig cc` compiles `build/gen/*.c` and links it with `sdk/librondo.a` and SDL3 into
   `rondo.exe`. Builds are resumable: generated files that are already compiled are skipped.
6. **Done**: `rondo.exe` next to `game/`; Setup offers to launch it.

An `icon.ico` or `icon.png` (up to 256 x 256) the player puts next to Setup is built into
`rondo.exe` as its icon (resource 1, compiled by `zig cc` from `build/rondo.rc`; a PNG goes
into an .ico as it is). None is shipped: no game artwork is included.

The generated C and the extracted data stay on the player's PC and are never redistributed.

## Current state

All five steps are done.

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang && cmake --build build   # also assembles build/sdk
tools/fetch_zig.sh                                                             # pinned Zig 0.17.0 -> build/zig
tools/package_release.sh 0.2       # dist/RondoRecomp + dist/RondoRecomp-0.2-windows-x64.zip
```

Release folder (412 MB unpacked, 103 MB zipped; Zig is most of it):

| File | |
|---|---|
| `RondoRecomp Setup.exe` | Setup with a window: choose the zip, watch progress, Play / Rebuild |
| `rondo_setup.exe` | the same steps from the command line (`--rebuild` too) |
| `sdk/` | runtime and frontend sources, SDL3 headers and import library, DLLs for rondo.exe |
| `zig/` | C compiler used by Setup |
| `README.txt`, `THIRD_PARTY.txt` | player instructions, third-party licenses |

`tools/package_release.sh` also writes the update package (the same without `zig/`), see
Updates below.

What Setup does (about 40 s; the first build also prepares Zig's C library):

1. import the zip (CUE + tracks, subfolders allowed, CRC-checked, Zip64 aware)
2. check the disc (PC Engine signature, boot-sector fingerprint, language); stage in `game.tmp/`,
   then `game/en/` or `game/ja/`
3. recompile: run the game headless to find its code, emit C into `build/gen/` (docs/recompiler.md);
   from the English disc when it is installed (its capture run sees the most code), else the
   Japanese one. The other disc runs the same code, checked as it goes (99% native)
4. build: `zig cc` compiles the runtime + generated C into `rondo.exe` (GUI subsystem), copies DLLs

`rondo.exe` uses `game/en/` and `game/ja/` next to itself (the menu's Language switches), keeps
saves in `%APPDATA%\RondoRecomp` (`-ja` files for Japanese), records code it
had to interpret in `misses.txt`, and writes `rondo.log` (share of CPU cycles run natively).
`Rebuild` / `--rebuild` recompiles with those misses.

Tested from the packaged zip unpacked into an empty folder: Setup built rondo.exe from the
zipped disc with no options, and the game ran at 99.9% native CPU cycles.

Possible follow-ups: trim the Zig toolchain like LASRecomp's `tools/zig_slim.sh` does (most of
the download), and an icon and version resource for the executables.

## Linux (and the Steam Deck)

```sh
tools/package_release_linux.sh     # on Linux or in WSL, with podman
                                   # -> dist/RondoRecomp-<version>-linux-x86_64.tar.gz
```

Built in an Ubuntu 22.04 container (`tools/linux/Containerfile`, `tools/linux/build.sh`), so it
runs on glibc 2.35 or newer (SteamOS 3, Ubuntu 22.04, Debian 12, Fedora 36, Arch). The
container builds:
- SDL3 3.4.18, the version the Windows release uses. It is a shared library whose X11,
  Wayland, PipeWire, PulseAudio, ALSA, D-Bus and udev backends load at run time, so it needs
  only libc.
- The Setup programs, linked against SDL3 with zlib static.
- The SDK, with Zig 0.17.0 for Linux.

The release folder is put together inside the container, so the archive keeps exact
permissions: the programs are 755 and everything else 644.

The release folder:

| File | |
|---|---|
| `RondoRecomp Setup`, `rondo_setup` | Setup with a window, and from the command line |
| `libSDL3.so.0` | used by Setup and the game; both find it next to themselves (`$ORIGIN`) |
| `sdk/`, `zig/` | as on Windows (`sdk/lib/libSDL3.so.0` to link against) |

Setup builds `rondo` with `zig cc -target x86_64-linux-gnu.2.35`. It links against glibc
2.35's symbols whatever the machine has, plus `-D_DEFAULT_SOURCE`, because rcheevos uses
`strdup` and `strcasecmp` and glibc hides them under `-std=c11`. The tests run the same on
Linux (in WSL):
- the package unpacked into an empty folder
- Setup from a zip and from unzipped folders (both discs)
- `--test-achievements` through the system's libcurl
- `--autotest`, `--autotest-start` and `--autotest-features`
  (`tools/updtest/linux_regression.sh`, on the install `tools/updtest/linux.sh` makes)

RetroAchievements: the SDK carries rcheevos (`sdk/src/rcheevos`). `sdk/sources.txt` lists its
sources, plus `-I` (relative to the SDK) and `-D` lines, which Setup passes to the compiler.
On Windows the game also links `winhttp`.

## Updates

From 1.0, an installation updates itself from the GitHub releases
(`tools/frontend/update.h`, `tools/setup/install.c`):

1. **Check.** The game asks GitHub's API for the latest release when it starts
   (`check_updates` in settings.ini, on by default), on a thread of its own (`http.c`).
   Setup's window asks too. A release counts when its tag (`v1.1`) is newer than
   `RONDO_VERSION` and it has a package for this system. The start screen then shows a
   notice, and the Updates page shows the first points of the release notes' "New in"
   section.
2. **Download.** "Download and install" starts `rondo_setup --update` with its output
   piped back to the game, which shows each step and a progress bar. On Windows it runs
   without a console window (`tools/frontend/child.c`), and so does the compiler under it.
   Setup asks GitHub again and downloads the package into `update/`. Its size and SHA-256
   are checked against the asset's `digest` in GitHub's answer.
3. **Unpack** into `update/new/`: zip on Windows; tar.gz on Linux, with the file modes
   kept. Paths that would land outside it are refused.
4. **Install.** Each top-level file and folder of the package is renamed into place. What
   was there is first renamed into `update/old/`. A rename works even for a program or DLL
   that is running, like the game and Setup itself. If anything fails, everything is put
   back.
5. **Rebuild.** The new `rondo_setup --rebuild` builds the game from the new SDK. If that
   fails, the old files are put back.
   - Setup always links the game into `build/` first, then swaps it in the same way, so a
     rebuild works while the game is running and a failed one leaves the old game.
6. **Restart.**
   - The game restarts as the new version: `execv` on Linux, keeping the same process, so
     Steam keeps track of it; a new process on Windows.
   - The new game removes what is left of `update/` (the old files that were in use).

Each release has, for each system, the full package and an update package without `zig/`:
`RondoRecomp-<v>-windows-x64-update-zig<zig>.zip` (about 7 MB against 104 MB) and
`RondoRecomp-<v>-linux-x86_64-update-zig<zig>.tar.gz` (about 5 MB). The packaging scripts
make both. The Zig version in the name is `RONDO_ZIG_VERSION`. An installation downloads
the update package only when that name matches its own Zig; otherwise it downloads the
full package, so a Zig upgrade needs nothing special.

To test without publishing:
- `RONDO_UPDATE_URL` points the check at another release JSON, for example one served from
  `http://127.0.0.1`.
- `RONDO_UPDATE_FROM` makes the installation pretend to be an older version.
- `rondo --autotest-update DIR` goes through the start screen and the Updates page,
  installs the update, then restarts into `--autotest`.

`tools/updtest/windows.sh` and `tools/updtest/linux.sh` run this against a local server for
each case:
- a normal update
- the full package
- a wrong SHA-256, which is refused
- a package that cannot build, which is put back
- the real latest release on GitHub

## Work plan

1. ~~Setup core: zip import, identification, extraction.~~ Done.
2. ~~Build orchestration: runtime SDK, `zig cc`, `rondo.exe`.~~ Done.
3. ~~Recompiler~~ Done (see docs/recompiler.md).
4. ~~Setup GUI.~~ Done (`RondoRecomp Setup.exe`).
5. ~~Release packaging.~~ Done (`tools/package_release.sh`).
