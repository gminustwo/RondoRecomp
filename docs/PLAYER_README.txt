RondoRecomp
===========

Turns your own copy of Castlevania: Rondo of Blood (PC Engine Super CD-ROM2) into a
native Windows or Linux program (the Steam Deck too). No game files are included: you
provide your own disc image.

What you need
-------------
Your disc image: the .cue file together with the track files it lists (.bin, .img, .iso
and/or .wav), either zipped (the files may be inside a folder in the zip; a .7z works
too) or unzipped in a folder of their own. A .chd (made with chdman) works as well, and
the game plays it as it is: it takes about half the space of the unpacked tracks.
Either disc works: the original Japanese release (Akumajou Dracula X: Chi no Rondo) or
the English fan translation. With both, you can switch languages in the game.
The Virtual Console releases work too (see Virtual Console below): the Wii's .wad (the
Japanese game) and the Wii U's title (WUP, the English translation).

How to use it
-------------
1. Unpack this folder anywhere you can write to (not Program Files). Unpack all of it: Setup
   needs every file in the zig folder (about 21,000 of them) to build the game.
2. Run "RondoRecomp Setup.exe". For a zipped disc image click "Disc zip..." and pick the
   zip (or the .cue or .chd file); for an unzipped one click "Disc folder..." and pick the
   folder the .cue file is in (subfolders are searched too).
3. Setup checks the disc, extracts it to the game folder, recompiles the game's code
   into native code and builds rondo.exe in this folder. This takes about a minute
   (the first build prepares the bundled C compiler's runtime once).
4. Click Play. From then on, start rondo.exe directly.
To add the other language, run Setup again and choose that disc: it is installed
next to the first (game\en and game\ja) instead of replacing it.

Linux and Steam Deck
--------------------
The Linux download is a single file, RondoRecomp-<version>-x86_64.AppImage. It needs a
64-bit PC with glibc 2.35 or newer: SteamOS 3 on the Steam Deck, Ubuntu 22.04, Debian 12,
Fedora 36, Arch, or later. Run it: the first time, Setup opens; once the game is built, the
AppImage starts the game. Setup keeps the game it builds, and your disc's files, in
~/.local/share/RondoRecomp/install (the folder RONDO_HOME names, if you set it). To open
Setup again, run the AppImage with --setup.

On the Steam Deck:
1. Switch to Desktop Mode (Steam button, Power, Switch to Desktop).
2. Put the AppImage and your disc zip on the Deck (download them, or copy them over), for
   example in your home folder.
3. In Dolphin, the file manager, right-click the AppImage, choose Properties, then
   Permissions, tick "Is executable" and click OK. Double-click the AppImage: Setup opens.
   Click "Disc zip..." and pick your zip (or "Disc folder..." for an unzipped image), and
   wait until it says Done.
4. Add the game to Steam: open Steam, Games, Add a Non-Steam Game to My Library, Browse...,
   choose the AppImage, then Add Selected Programs.
5. Return to Gaming Mode (the "Return to Gaming Mode" icon on the desktop). The game is in
   your library under Non-Steam.
Keep the AppImage where it is once it is in Steam: Steam starts it from there, and updates
replace it there.
To add the other disc later, choose Setup on the game's start screen: the game closes and
Setup's window opens in its place. In Gaming Mode there may be no file dialog for "Disc
zip..." (Setup then says so): add discs in Desktop Mode.

Virtual Console (Wii and Wii U)
-------------------------------
Setup also takes Rondo of Blood as Nintendo sold it for the Wii and the Wii U:
- the Wii's .wad (the USA or the European release). This is the original Japanese game,
  untranslated, as the Wii sold it everywhere: it is installed as the Japanese disc.
- the Wii U's title, as a WUP: a folder with title.tmd, title.tik and its .app files, or a
  zip or 7z with that folder in it. This is Konami's English translation: it is installed
  as the English disc.
Choose the .wad, zip or 7z with "Disc zip...", or the WUP folder with "Disc folder...".
Both are encrypted with their console's key, which RondoRecomp does not include. Setup
looks next to the file you chose (and in the folder above it), and next to Setup, for:
- keys.txt: keys written as 32 hex digits, one per line, as in Cemu's keys.txt (the Wii
  U common key for a WUP; the Wii common key for a .wad);
- otp.bin, dumped from your Wii U: it holds both keys;
- keys.bin, from a NAND backup of your Wii (BootMii).
If none is there, Setup's window asks you to choose the file. On the command line:
rondo_setup <game.wad | WUP folder | .zip | .7z> --keys <file>.
A .wad needs no key if you have installed it in Dolphin (Tools, Install WAD...): Setup
finds it there, decrypted, using Dolphin's own settings for where its Wii storage is. You
can also choose that installed title's folder with "Disc folder..." (in Dolphin's Wii
folder: title/00010001/5141504e/content for the USA release).
The Virtual Console's CD audio is compressed (Ogg Vorbis: 48 kHz on the Wii, 32 kHz on the
Wii U); Setup converts it to the 44.1 kHz the game plays, so it sounds as on the Wii or Wii
U, not quite as the disc. The Wii release leaves out the disc's first track (the warning a
CD player would play), which the game never plays.
The Deck's controls are an Xbox controller to the game: View is Back, Menu is Start, L1 and
R1 are LB and RB, L2 and R2 are LT and RT. Save states as Back + RB work the same way (View +
R1). Options, saves and states are kept in ~/.local/share/RondoRecomp/RondoRecomp/.
To log in to RetroAchievements in Gaming Mode, choose User name or Password: Steam's
keyboard opens (Steam + X opens it if it does not). Achievements need the system's libcurl,
which SteamOS and the usual desktop distributions have.

RetroAchievements
-----------------
Rondo of Blood has achievements and leaderboards at RetroAchievements
(https://retroachievements.org/game/4855). Both discs count: the English patch and the
Japanese original are the same game there. To earn them:
1. Make a free account at retroachievements.org.
2. In the game's menu, RetroAchievements: choose User name and type it, then Password, then
   Log in. Only the login token RetroAchievements sends back is kept (in settings.ini), never
   the password. Log out forgets it.
3. Play. When the game starts, a notice shows how many achievements you have; each one you
   earn pops up as it unlocks, and leaderboard attempts show their progress at the top right.
   The page shows your account and the game's progress.
Hardcore mode (off by default) is RetroAchievements' stricter way to play: while it is on,
loading save states, rewind, the cheats, Richter's run (hold LB) and the 8-way and limp
whip are off, and unlocks
count as hardcore. Switching it on restarts the game. Without a connection the game plays
as usual; unlocks made offline are sent when it is back.

Updates
-------
When the game starts it asks GitHub (github.com/gminustwo/RondoRecomp) whether there is a
newer version. If there is, the start screen says so under the title and beside Updates.
The Updates page shows what is new; "Download and install" does the rest:
1. Setup downloads the new version for your system. On Windows that is usually a few MB:
   the compiler is only downloaded again when a new version needs a different one. On
   Linux it is the new AppImage. The download is checked against the SHA-256 checksum
   GitHub publishes for it.
2. On Windows, its files take the place of the old ones next to the game. Your disc
   images, saves, save states and options are not touched.
3. Setup rebuilds the game from the new version (about a minute; longer on a Steam Deck),
   and the game restarts as the new version. On Linux, the new AppImage then takes the
   old one's place, under the same name, so Steam keeps finding it.
If anything goes wrong, the old version is put back and the page says what happened.
"Check at launch" (on by default) turns the question to GitHub off; "Check now" asks
anyway. RondoRecomp Setup's window also offers "Update to ..." when there is a new version,
and on the command line: rondo_setup --update. Versions before 1.0 have no updater: replace
one of those by hand once.

Controls
--------
The defaults (change them on the menu's Controls page, see below):

              Controller (Xbox)     Keyboard
Move          d-pad / left stick    arrow keys      (fixed; extra keys: see below)
Jump          A                     X
Attack        X                     Z
Sub-weapon    B                     C
Item Crash    Y                     V
Backflip      RB                    A
Run           hold LB               hold S
Pause         Start                 Enter
Menu          Back / View           F1              (Esc always opens it too)
Rewind        hold LT               hold R
Save state    -                     F5
Load state    -                     F8
State slot                          F6 / F7 (slots 1-9, fixed)
Fullscreen                          F11 (fixed)

Changing a control: on the Controls page choose its row (A / Enter or a click). It counts
down from 5, and whatever you hold down when the count ends becomes the control: a
controller button (the triggers count), two controller buttons, or a key. A button or key
you press and let go during the count is taken too, if nothing is held at the end. If
another action had it, the two swap. Esc, a click, nothing pressed, or the control it
already has leaves it as it was. On a selected row, X on the controller clears its
controller button and Delete clears its key, where the game can do without it (Sub-weapon,
Backflip, Run, Rewind, Save state, Load state). Save state and load state have no controller
button until you give them one. "Reset controls" puts every control back. The
menus themselves always use the d-pad / stick / arrows, A / Enter and B / Esc.

Moving with other keys (WASD, say): the Move up / down / left / right rows take a key each,
which moves as well as the arrow does (the arrows, the d-pad and the stick always move).
They have none until you give them one. A and S are Backflip and Run at first, so giving
them to Move left and Move down leaves those two without a key: give them others (Shift,
Space, ...) afterwards.

Two buttons make a combination, such as Back + RB for Save state and Back + LB for Load
state: during the count hold Back, then press RB, and keep both down until it ends. In play,
hold the first button and press the second. The second button then does only the
combination, so RB and LB on their own still backflip and run. A button that starts a
combination does its own job when you let go of it without pressing a second one: with the
example above, Back opens the menu when released instead of when pressed.

Sub-weapon presses Up + Attack for you, Item Crash is the original SELECT button, and
Backflip presses Jump twice quickly (as Maria it gives her double jump). The original
inputs (Up + Attack, Jump twice) work as well.

The PC Engine's start button was called RUN. The game's title screen says PUSH START
BUTTON! when you are playing with a controller and PUSH ENTER KEY! on the keyboard (in
place of the original PUSH RUN BUTTON!), going by what you pressed last; PUSH PAUSE
BUTTON! / KEY! if you gave Pause another control.

Run is Richter's run from Symphony of the Night: hold LB while walking to go about 1.8
times as fast (his run there is 2.3 pixels a frame against a 1.25 walk; Rondo's walk is the
same 1.25). Jumps started while running keep the speed. Maria runs too.

Start screen
------------
rondo.exe opens on a start screen; the game itself starts only when you choose Play
(START, Enter, A, or a click; it goes on to the game's title screen, past the logo and
intro, unless Skip cinematics is Never). The menu also has the language, loading a save
state, the option pages below, Updates (see Updates above), Setup (to add the other
language's disc or rebuild; rondo.exe closes so Setup can rebuild it) and Quit. Controller, keyboard and mouse all
work; the line at the bottom of the menu says what the selected item does. Menu, Start
screen comes back to it. Gameplay, Start screen at launch: Off goes straight into the
game (rondo.exe --play does that once).

Menu
----
F1, Esc or the controller's Back button pauses the game and opens the menu: save and load
states, choose the slot, reset, back to the start screen, quit, and:

Video      Fullscreen (uses the screen's own resolution, e.g. 4K)
           VSync: on (the default), each frame is shown when the screen refreshes, so the
           picture never tears; off shows it at once
           Aspect ratio: 4:3 (as on a TV), square pixels, or stretched to fill the screen
           Size: Fit screen (as big as the screen allows), or Whole multiples (every game
           pixel the same size; leaves a border)
           Filter: Sharp (crisp, even pixels), Nearest, or Smooth
           Screen effect: Off, Scanlines, CRT (scanlines, phosphor stripes and a glow),
           Curved CRT (the same on curved glass, darker at the corners), or Composite (a
           soft TV picture); Effect strength: Light, Medium or Strong
           Beside the picture: when the picture is narrower than the screen, a blurred
           copy of it, or black
Gameplay   Skip cinematics: Press START, Automatically, or Never. During the logo and
           the intro (the story narration and opening before the title screen), START, A
           or X goes straight to the title screen, the way the game itself allows once it
           has shown them; Automatically goes there at once. Story scenes in the game have
           no skip of their own: START (or Automatically) runs them through fast instead
           Rewind on/off, volume, Start screen at launch on/off
           8-way whip and doves (off by default): hold a direction as you attack and
           Richter whips that way, as in Super Castlevania IV: up, up and ahead, and in
           the air down and down and ahead; Maria throws her doves the same way. Up and
           Attack then no longer throws the sub-weapon: use the Sub-weapon control
           Limp whip (off by default): keep Attack held after Richter's whip and it hangs
           from his hand; the d-pad swings it round (it still hurts what it touches) and
           letting go of Attack ends it. Ideas from Pizzano's Super Rondo of Blood hack,
           made anew for RondoRecomp (none of the hack is used)
RetroAchievements
           On / off, hardcore mode, your account (see RetroAchievements above)
Cheats     Infinite health, infinite hearts (always 99), infinite lives. At a stage's end
           the game still counts your health and hearts down into the score, as it
           always does; the next stage starts with them full again
           Maria unlocked: the game offers Maria as it does after you rescue her in Stage 2
           (and your save keeps it)
           Play as Maria: every stage is played as Maria, the Prologue too (starts from the
           next stage you enter)
Language   English patch or Japanese original (each is its own disc; see below)

Options are kept in %APPDATA%\RondoRecomp\RondoRecomp\settings.ini (on Linux in
~/.local/share/RondoRecomp/RondoRecomp/settings.ini), or in the user folder in portable
mode (see Portable mode below).

Your own icon (Windows)
-----------------------
RondoRecomp includes no game artwork. To give rondo.exe an icon, put an image named
icon.png (up to 256 x 256 pixels) or icon.ico next to "RondoRecomp Setup.exe" and run
Setup (or Rebuild); it is built into rondo.exe and shows on its window and taskbar button.
On Linux and the Steam Deck, Steam's Properties for the game set its pictures instead.

Rewind plays the game backwards while held (faster after 3 seconds) and continues from
where you let go; up to 30 minutes are kept. Rewinding never changes your saved game.

In-game saves are kept in %APPDATA%\RondoRecomp\RondoRecomp\bram.bin (bram-ja.bin for
Japanese), save states in %APPDATA%\RondoRecomp\RondoRecomp\states\ (on Linux the same
files in ~/.local/share/RondoRecomp/RondoRecomp/).
To keep a separate set (options, saves, states), set the environment variable
RONDORECOMP_PROFILE to a folder before starting Setup or rondo.exe; your usual ones are
left alone.

Portable mode
-------------
To keep options, saves and save states with the game instead (on a USB stick, say), tick
"Portable" at the bottom of Setup's window. Setup makes a folder named "user" next to
rondo.exe and offers to copy the options and saves you have so far into it (they stay in
%APPDATA% too); from then on the game uses that folder and nothing in %APPDATA%, so the
whole folder can be moved anywhere. Untick it to go back: Setup copies what is in user back
to %APPDATA%\RondoRecomp\RondoRecomp (replacing what is there) and keeps the folder,
renamed user.old. Making the user folder yourself does the same as ticking the box (copy
settings.ini, bram.bin, bram-ja.bin and the states folder into it to bring yours along).
Updates leave the user folder alone. On Linux the user folder is next to the AppImage; it
holds your options, saves and states, while the game Setup builds stays in
~/.local/share/RondoRecomp/install. RONDORECOMP_PROFILE, if set, still comes first.

Japanese and English
--------------------
The game's text and subtitles come from the disc image: Japanese is the original disc
(Akumajou Dracula X: Chi no Rondo), English is the fan translation of it. rondo.exe starts
with whichever disc Setup installed (the language chosen in the menu, if both are). Menu,
Language switches and restarts the game; if that language's disc is not installed, it asks
for its .cue (or .chd) file once and remembers it. The Japanese story text uses the System Card's
kanji font, which RondoRecomp supplies (Shinonome, a public-domain bitmap font), so no
System Card image is needed. Both discs run the same recompiled code.

Rebuild
-------
rondo.exe notes any game code it had to run without recompiled code in misses.txt.
"Rebuild" in Setup recompiles again including that code. rondo.log shows how much ran
natively in the last session.

Command line
------------
rondo_setup.exe <disc.zip | disc.7z | disc.cue | disc.chd | folder | game.wad> [--keys <file>]
                                          the same steps without a window (--keys: the
                                          key file for a Virtual Console release)
rondo_setup.exe --rebuild                 recompile again with misses.txt
rondo_setup.exe --update                  install the latest version from GitHub (--check:
                                          only say whether there is one)
rondo_setup.exe --portable on|off         portable mode on or off, as Setup's checkbox
                                          (on copies your options and saves in; off copies
                                          them back and keeps the folder as user.old)
(on Linux: the AppImage with --setup-cli, then the same options)

Status
------
Work in progress. Music, sound effects and voices all play.

Third-party software: see THIRD_PARTY.txt.
