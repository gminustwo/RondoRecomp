#!/bin/bash
# Builds the macOS release (Apple silicon, macOS 15 and newer: Zig 0.17's own minimum, Setup
# runs it; SDL3 and the game are for macOS 11 and newer), on a Mac with Xcode's command
# line tools, CMake and Ninja: dist/RondoRecomp-<version>-macos-arm64.zip, which holds
# RondoRecomp.app (the name the updater looks for: tools/frontend/update.h).
#
# The app holds Setup's programs (Contents/MacOS), libSDL3.0.dylib (Contents/Frameworks), and
# the SDK, the pinned Zig for macOS, version.txt and the player's README (Contents/Resources).
# No game material: Setup builds the game from the player's own disc in a folder of theirs,
# ~/Library/Application Support/RondoRecomp/install (tools/frontend/programs.h).
# Ad-hoc signed, not notarized: on the first start macOS asks the player to allow it
# (System Settings > Privacy & Security > Open Anyway).
#
# Work files in build/mac (SDL3 and Zig are kept between runs; delete them to redo).
# Written for the bash 3.2 macOS ships.
set -euo pipefail
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define RONDO_VERSION "\(.*\)"/\1/p' tools/frontend/programs.h)
ZIGV=$(sed -n 's/^#define RONDO_ZIG_VERSION "\(.*\)"/\1/p' tools/frontend/programs.h)
L=$PWD/build/mac
S=$L/src
DEPS=$L/deps
JOBS=$(sysctl -n hw.ncpu)
mkdir -p "$S" "$DEPS"
for t in cmake ninja codesign ditto iconutil sips; do
    command -v $t >/dev/null || { echo "needs $t"; exit 1; }
done

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL --proto '=https' -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | shasum -a 256 -c --quiet
}

# the same SDL3 and Zig versions as the Windows and Linux releases
SDL=SDL3-3.4.18
ZIG=zig-aarch64-macos-$ZIGV
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/$SDL.tar.gz \
      9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 $SDL.tar.gz
fetch https://ziglang.org/download/$ZIGV/$ZIG.tar.xz \
      b607e9b9234790a008116ae5bdb71c6243b84b9fb42a53a9e70fde41c06c536a $ZIG.tar.xz

# SDL3: shared, for macOS 11 and newer (Homebrew's is for the Mac that installed it)
if [ ! -f "$DEPS/lib/libSDL3.0.dylib" ]; then
    W=$(mktemp -d)
    tar -xf "$S/$SDL.tar.gz" -C "$W"
    cmake -S "$W/$SDL" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_FRAMEWORK=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$W/build" -j"$JOBS"
    cmake --install "$W/build"
    cp "$W/$SDL/LICENSE.txt" "$DEPS/SDL3-LICENSE.txt"
    rm -rf "$W"
fi

# Setup, the SDK (and the developer tools, unused here)
cmake -S . -B "$L/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$DEPS" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build "$L/cmake" -j"$JOBS"
for t in rondo_setup "RondoRecomp Setup" sdk/lib/libSDL3.0.dylib; do
    [ -f "$L/cmake/$t" ] || { echo "mac: $t was not built"; exit 1; }
done

P=$(mktemp -d)
APP=$P/RondoRecomp.app
C=$APP/Contents
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources"
install -m 755 "$L/cmake/rondo_setup" "$L/cmake/RondoRecomp Setup" "$C/MacOS/"
strip -x "$C/MacOS/rondo_setup" "$C/MacOS/RondoRecomp Setup"
install -m 644 "$DEPS/lib/libSDL3.0.dylib" "$C/Frameworks/"
cp -R "$L/cmake/sdk" "$C/Resources/sdk"
find "$C/Resources/sdk" -type d -exec chmod 755 {} + && find "$C/Resources/sdk" -type f -exec chmod 644 {} +
tar -xf "$S/$ZIG.tar.xz" -C "$P" && mv "$P/$ZIG" "$C/Resources/zig"
[ "$("$C/Resources/zig/zig" version)" = "$ZIGV" ] || { echo "zig/ is not $ZIGV (RONDO_ZIG_VERSION in programs.h)"; exit 1; }
install -m 644 docs/PLAYER_README.txt "$C/Resources/README.txt"
printf 'APPL????' > "$C/PkgInfo"

# the icon: tools/linux/rondorecomp.png (drawn from shapes, tools/linux/make_icon.py)
I=$P/AppIcon.iconset
mkdir -p "$I"
for s in 16 32 128 256; do
    sips -z $s $s tools/linux/rondorecomp.png --out "$I/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    [ $d -le 256 ] && sips -z $d $d tools/linux/rondorecomp.png --out "$I/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$I" -o "$C/Resources/AppIcon.icns"
rm -rf "$I"

{
    echo "Third-party software included with RondoRecomp"
    echo "==============================================="
    echo
    echo "SDL 3 (Contents/Frameworks/libSDL3.0.dylib, sdk/include/SDL3, sdk/lib): zlib license"
    echo "    https://libsdl.org"
    echo "Zig $("$C/Resources/zig/zig" version) (zig/): MIT; its LLVM, Clang and LLD: Apache 2.0 with LLVM exceptions"
    echo "    https://ziglang.org - see zig/LICENSE"
    echo "stb_truetype (sdk/src/frontend/stb_truetype.h, draws the menus' text): public domain"
    echo "    (or MIT), Sean Barrett - https://github.com/nothings/stb"
    echo "Cinzel, Cinzel Decorative and UnifrakturMaguntia fonts (sdk/src/frontend/ui_fonts.c, built"
    echo "into rondo for the menus): SIL Open Font License 1.1 - the Cinzel Project Authors,"
    echo "Natanael Gama; j. 'mach' wust, Peter Wiegel. https://github.com/google/fonts"
    echo "Shinonome Gothic 12 and 16 dot bitmap fonts (sdk/src/runtime/pce/kanji_font.c, the"
    echo "Japanese text of the original disc): public domain, The Electronic Font Open Laboratory"
    echo "    http://openlab.jp/efont/shinonome/"
    echo "rcheevos 12.5.0 (sdk/src/rcheevos, built into rondo for RetroAchievements): MIT,"
    echo "    RetroAchievements - https://github.com/RetroAchievements/rcheevos"
    echo "LZMA SDK 26.05 (7z and LZMA decoder: built into the Setup programs for .7z files, and its LZMA"
    echo "    decoder, sdk/src/lzma, into the game for .chd files): public domain, Igor Pavlov -"
    echo "    https://www.7-zip.org/sdk.html"
    echo "stb_vorbis 1.22 (built into the Setup programs for Virtual Console audio): public domain"
    echo "    (or MIT), Sean Barrett - https://github.com/nothings/stb"
    echo "(The Setup programs use macOS's own zlib.)"
    echo
    echo "The license texts follow."
    for f in "$DEPS/SDL3-LICENSE.txt" tools/fonts/OFL-*.txt third_party/rcheevos/LICENSE; do
        [ -f "$f" ] || continue
        echo
        echo "---------------------------------------------------------------------------"
        case $f in
            */SDL3-LICENSE.txt) echo "SDL3/LICENSE.txt" ;;
            */rcheevos/LICENSE) echo "rcheevos/LICENSE" ;;
            *) basename "$f" ;;
        esac
        echo "---------------------------------------------------------------------------"
        cat "$f"
    done
} > "$C/Resources/THIRD_PARTY.txt"
chmod 644 "$C/Resources/THIRD_PARTY.txt"

# every Mach-O inside: the app's minimum macOS is the newest they need (Zig's; no newer than
# MACOS_MIN), and they use nothing from outside macOS but SDL3
MACOS_MIN=15.0
newer() { [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n | tail -1)" = "$1" ]; }
MIN=11.0
while IFS= read -r f; do
    file -b "$f" | grep -q Mach-O || continue
    m=$(otool -l "$f" | awk '/LC_BUILD_VERSION/ {b=1} b && /minos/ {print $2; exit}')
    [ -n "$m" ] || continue
    if newer "$m" "$MACOS_MIN"; then echo "mac: ${f#$APP/} is for macOS $m (newer than $MACOS_MIN)"; exit 1; fi
    if newer "$m" "$MIN"; then MIN=$m; fi
    if otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/|@rpath/libSDL3\.0\.dylib$)' | grep .; then
        echo "mac: ${f#$APP/} uses a library from outside macOS"
        exit 1
    fi
done < <(find "$APP" -type f -perm -u+x -o -type f -name '*.dylib')
sed -e "s/@VERSION@/$VER/g" -e "s/@MINOS@/$MIN/g" tools/mac/Info.plist > "$C/Info.plist"

# version.txt: the version and a fingerprint of everything else in the app, as on Linux. Setup
# copies it into the game it builds (build/version.txt), and the app rebuilds the game when
# they differ: a new build of the same version too (tools/setup/gui.c: mac_app_start).
ID=$(cd "$C" && find . -type f ! -name version.txt -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256 | shasum -a 256 | cut -c1-12)
echo "$VER $ID" > "$C/Resources/version.txt" && chmod 644 "$C/Resources/version.txt"

# ad-hoc signatures (Apple silicon runs only signed code; strip undid the programs'): the
# code inside first, then the app, which seals the rest
codesign -f -s - "$C/Frameworks/libSDL3.0.dylib" "$C/Resources/sdk/lib/libSDL3.0.dylib" \
    "$C/Resources/zig/zig" "$C/MacOS/rondo_setup"
codesign -f -s - "$APP"
codesign --verify --deep --strict "$APP"

mkdir -p dist
ZIP=dist/RondoRecomp-$VER-macos-arm64.zip
rm -f dist/RondoRecomp-*-macos-arm64.zip
ditto -c -k --keepParent "$APP" "$ZIP"
echo "release: $ZIP ($(du -sh "$ZIP" | cut -f1); the app $(du -sh "$APP" | cut -f1))"
rm -rf "$P"
