# Distribution: player-supplied game, recompiled on the player's PC

Goal: the release contains no game code or data. The player provides their own copy of the
game as a zip, and a setup program turns it into a native `rondo.exe` on their machine.
This is the same model as LASRecomp: tools are shipped, the game-specific code is generated locally.

## What the player does

1. Download the RondoRecomp release and unpack it anywhere.
2. Run **RondoRecomp Setup** and choose the zip of their *Castlevania: Rondo of Blood* disc image
   ("Disc zip...", which also takes a .cue file), or the folder it is unzipped in ("Disc
   folder...": every .cue under it is checked where it is, and the first Rondo of Blood disc is
   copied in). A .7z of the image works too, and so do the Virtual Console releases (see
   Virtual Console below).
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
                                   # -> dist/RondoRecomp-<version>-x86_64.AppImage
```

Built in an Ubuntu 22.04 container (`tools/linux/Containerfile`, `tools/linux/build.sh`), so it
runs on glibc 2.35 or newer (SteamOS 3, Ubuntu 22.04, Debian 12, Fedora 36, Arch). The
container builds:
- SDL3 3.4.18, the version the Windows release uses. It is a shared library whose X11,
  Wayland, PipeWire, PulseAudio, ALSA, D-Bus and udev backends load at run time, so it needs
  only libc.
- The Setup programs, linked against SDL3 with zlib static.
- The SDK, with Zig 0.17.0 for Linux.

The release is an AppImage: one file, which is what a Steam Deck player adds to Steam. Its
folder is put together inside the container, so the image keeps exact permissions: the
programs are 755 and everything else 644. The container hands it back as a tar, and
`tools/package_release_linux.sh` makes the squashfs image (zstd) from that tar with the
host's `mksquashfs` (squashfs-tools 4.6 or newer). The image goes after the AppImage runtime
(type2-runtime 20251108, pinned by SHA-256; it carries its own FUSE, so the system needs no
libfuse).

Inside the AppImage:

| File | |
|---|---|
| `AppRun` | what the AppImage starts (`tools/linux/AppRun`), see below |
| `RondoRecomp Setup`, `rondo_setup` | Setup with a window, and from the command line |
| `libSDL3.so.0` | used by Setup and the game; both find it next to themselves (`$ORIGIN`) |
| `sdk/`, `zig/` | as on Windows (`sdk/lib/libSDL3.so.0` to link against) |
| `version.txt`, `RondoRecomp.desktop`, `rondorecomp.png` | the version; the desktop entry and its icon (`tools/linux/make_icon.py`, drawn from shapes) |

The AppImage is read-only, so Setup builds the game outside it, in a folder of its own:
`~/.local/share/RondoRecomp/install` (`RONDO_HOME` sets another). `AppRun` passes three
folders on (`tools/frontend/programs.h`): `RONDO_APPDIR`, where the image is mounted (Setup's
tools); `RONDO_HOME`, where the game is built; and `RONDO_APPIMAGE`, the AppImage file.
Started without options, it starts the built game. If there is no game yet, it opens Setup's
window. If the game was built by another version (`build/version.txt`, which Setup writes),
it opens Setup's window, which rebuilds and then plays. `--setup` opens Setup's window, and
`--setup-cli` runs `rondo_setup`.

On Linux the game and Setup's window hand over in one process: the start screen's Setup
quits the game (saving as usual), then `exec`s Setup, and Setup's Play `exec`s the game (or
the AppImage). A new process would not work. Steam closes whatever a game leaves running
when its own process ends, and the AppImage stays mounted only while a process holds the
pipe its runtime opened. `SDL_CreateProcess` closes the files a child would inherit, so a
child loses that hold. `rondo --autotest-setup` checks this: the game opens Setup, whose
`--autotest-play` plays the game's `--autotest`. Each notes its process id in
`setup_autotest.log`, and `tools/updtest/appimage.sh` checks they are the same. On Windows
Setup starts as a process of its own, and the same test runs there too.

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

## Virtual Console

Setup also imports Rondo of Blood from the Wii's and the Wii U's Virtual Console
(`tools/setup/vc.c`): a `.wad`, or a Wii U title as a WUP (`title.tmd`, `title.tik`,
`NNNNNNNN.app`), each on its own or in a zip or 7z. A 7z is read with the LZMA SDK's decoder
(`tools/setup/sevenzip.c`, `third_party/lzma`). The Virtual Console disc becomes an ordinary
cue sheet in `game/en/`, and from then on nothing tells it from a disc.

**Keys.** Both are encrypted, and the console's common key is the player's to supply.
RondoRecomp ships no keys. Setup looks for `keys.txt`, `otp.bin`, `keys.bin` and
`common-key.bin`:
- next to the input and in the folder above it;
- next to Setup, and in the AppImage's game folder;
- in the file `--keys` or `RONDO_KEYS` names, or the one Setup's window asks for.

It tries every 32-hex-digit key in a text file (Cemu's `keys.txt` has hundreds), and every
16 bytes at a multiple of 4 in a small binary one, such as a Wii U's `otp.bin` or a Wii's
BootMii `keys.bin`. Each is tried as the common key and as the title key itself. The right
one decrypts the start of content 0 to the file system's `FST` (Wii U) or the banner's `IMET`
(Wii). With no key, `rondo_setup` exits with 4, and the window then asks for the file.

**Wii U.** Content 0 is the title's file system. Hashed contents are in 0x10000-byte blocks:
0x400 bytes of hashes, decrypted with a zero IV, then 0xFC00 bytes of data, whose IV is the
block's entry in those hashes. The game is `/content/pceemu/pce.pkg`: a u32 length, then
files, each a u32 size (little-endian), a NUL-terminated name and the data.

**Wii.** The ticket and TMD come after the header and certificates, and then the contents
(64-byte aligned; IV: the content's index). Each content is checked against the TMD's SHA-1.
The game is content 5, a U8 archive (an LZ77-compressed one, types 0x10 and 0x11, would do
too). It holds `KMCD3005.hcd` and the tracks `KMCD3005_NN.bin` and `.ogg`, plus the
emulator's files: `config.ini`, the manual, and `syscard3P.pce`, the System Card, which
RondoRecomp doesn't use. For the key test, the largest content's first block must decrypt
to the U8 or LZ77 header. Content 0 starts with the title's name (`DRACULAX.1002031`), not
the usual banner header.

**Without a key**, a `.wad` that Dolphin has installed is read from Dolphin's Wii storage.
Dolphin keeps installed titles decrypted, in `title/<id high>/<id low>/content/`. Setup
looks for the storage:
- where `NANDRootPath` in Dolphin's `Dolphin.ini` puts it;
- otherwise in the usual places: `%APPDATA%\Dolphin Emulator\Wii`,
  `Documents\Dolphin Emulator\Wii`, `~/.local/share/dolphin-emu/Wii`, and Flatpak's
  `~/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu/Wii`.

The contents are checked against the WAD's TMD. The player can also choose such a
`content` folder (`title.tmd` with a Wii title id) with "Disc folder...".

**The disc.** Both have a track list, `CDROM.hcd`, with one line per track:
`track02,code ,track02.bin,004040,010299` gives the number, `audio` or `code`, the file, the
start as LBA + 150, and the length in sectors. The tracks themselves are:
- data tracks: zlib blocks of 128 2048-byte sectors, with a u32 count, then count × (u32
  offset, u32 size);
- audio tracks: Ogg Vorbis at 32 kHz (stb_vorbis, `third_party/stb_vorbis`).

Setup writes:
- `Track NN.iso` for each data track (`MODE1/2048`);
- `Track NN.wav` for each audio track, resampled to 44.1 kHz with a windowed sinc
  interpolation (441 phases from 32 kHz);
- a cue sheet whose `PREGAP`s and `INDEX`es put every track where it is on the disc.

Lengths are cut at the next track's start, because the lists let some tracks overlap.

The audio track after a data track has a 2-second pregap, which the two lists show
differently:
- **Wii U:** the track starts where the data ends, so its first 150 sectors are the pregap
  (`INDEX 00`, then `INDEX 01` at 00:02:00).
- **Wii:** a gap comes before the track, so the gap is the `PREGAP`.

Either way the track starts where it does on the disc.

On both discs the second data track (22) is a copy of track 2's first 10,000 sectors. The
Wii leaves it out, and the Wii U has the untranslated one, so Setup makes track 22 from
track 2. An audio track whose file is missing becomes silence. The Wii lacks track 1, the
warning a CD player plays, which the game never plays.

What the releases hold (checked against the discs):
- **Wii U:** the English translation. Its track 2 is the English disc's to the sector: the
  "English translation" disc image is Konami's Virtual Console translation, and the import
  matches that disc in all its data. So the disc fingerprint, the recompiled code and
  RetroAchievements' hash (game 4855) are the English disc's.
- **Wii (the USA and the European `.wad`):** the original Japanese game, untranslated. No
  translation is anywhere in the title, not in the emulator either. Track 2 is the
  Japanese disc's to the sector, and the import matches that disc in all its data. The two
  releases differ only in the manual, `TITLE.TXT` and `config.ini`.
- **The audio:** Ogg Vorbis, 32 kHz on the Wii U and 48 kHz on the Wii. The Wii U's matches
  the disc within 3 frames (40 ms) for most tracks, with a correlation of 0.98 to 0.99.
  Track 3 starts about 0.7 s early, and tracks 20 and 21 are slightly re-edited.

The full set of tests (`--test-achievements`, `--autotest`, `--autotest-start`,
`--autotest-features`) gives the same results as with the discs.

The WAD's decryption was tested on a WAD re-encrypted under a made-up key, since there's no
Wii key at hand. Its own SHA-1s check the result. The Wii releases themselves were tested
through Dolphin's installed copies.

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

From the Linux AppImage, steps 3 to 5 are different:
1. The new AppImage is downloaded next to the old one (`<name>.new`) and checked.
2. It rebuilds the game (`--setup-cli --rebuild`). If that fails, it is deleted, and the
   game is left as it was.
3. Only then is it renamed over the old AppImage, so the name Steam starts stays the same.
   The running game keeps its old, now unnamed, image until it exits.
4. The game restarts through the new AppImage, with `execv` again.

Each release has, for Windows, the full package and an update package without `zig/`:
`RondoRecomp-<v>-windows-x64-update-zig<zig>.zip` (about 7 MB against 104 MB). The Zig
version in the name is `RONDO_ZIG_VERSION`. An installation downloads the update package
only when that name matches its own Zig; otherwise it downloads the full package, so a Zig
upgrade needs nothing special. For Linux it has the AppImage,
`RondoRecomp-<v>-x86_64.AppImage`, which updates whole. (A Linux installation from a folder
looks for `-linux-x86_64.tar.gz` packages, which releases no longer have.)

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

`tools/updtest/linux.sh` tests a Linux folder installation, from the tar.gz packages releases had
before the AppImage. `tools/updtest/appimage.sh` tests the AppImage, in WSL too:
- Setup builds the game through it.
- The game starts through it.
- The game updates itself from a local server offering the same AppImage, and restarts
  through the new one.

## Work plan

1. ~~Setup core: zip import, identification, extraction.~~ Done.
2. ~~Build orchestration: runtime SDK, `zig cc`, `rondo.exe`.~~ Done.
3. ~~Recompiler~~ Done (see docs/recompiler.md).
4. ~~Setup GUI.~~ Done (`RondoRecomp Setup.exe`).
5. ~~Release packaging.~~ Done (`tools/package_release.sh`).
