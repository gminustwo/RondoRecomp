#!/bin/bash
# Builds the Linux release inside the Ubuntu 22.04 container (tools/package_release_linux.sh
# runs it there): SDL3 from pinned sources, Setup and the SDK against it, the pinned Zig for
# Linux, then dist/RondoRecomp-<version>-x86_64.AppImage: one file to run (and to add to Steam
# on a Steam Deck), which also updates itself (tools/linux/AppRun, tools/frontend/update.h).
# No game material: players provide their own disc image to Setup, which builds `rondo` on
# their machine, in a folder outside the AppImage.
#
# Work files in build/linux (SDL3 and Zig are kept between runs; delete them to redo).
set -euo pipefail
cd "$(dirname "$0")/../.."
VER=$(sed -n 's/^#define RONDO_VERSION "\(.*\)"/\1/p' tools/frontend/programs.h)
ZIGV=$(sed -n 's/^#define RONDO_ZIG_VERSION "\(.*\)"/\1/p' tools/frontend/programs.h)
L=$PWD/build/linux
S=$L/src
DEPS=$L/deps
JOBS=$(nproc)
mkdir -p "$S" "$DEPS"

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL --proto '=https' -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | sha256sum -c --quiet
}

# the same SDL3 and Zig versions as the Windows release (MSYS2's SDL3, tools/fetch_zig.sh)
SDL=SDL3-3.4.18
ZIG=zig-x86_64-linux-0.17.0
fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/$SDL.tar.gz \
      9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 $SDL.tar.gz
fetch https://ziglang.org/download/0.17.0/$ZIG.tar.xz \
      1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026 $ZIG.tar.xz
# the AppImage's runtime: mounts the image (FUSE, linked in: no libfuse needed) and starts AppRun
fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
      2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d appimage-runtime-20251108-x86_64

# SDL3: shared, its backends loaded at run time (dlopen), so it needs nothing the system
# may lack
if [ ! -f "$DEPS/lib/libSDL3.so.0" ]; then
    W=$(mktemp -d)
    tar -xf "$S/$SDL.tar.gz" -C "$W"
    cmake -S "$W/$SDL" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_INSTALL_LIBDIR=lib -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_RPATH=OFF
    cmake --build "$W/build" -j"$JOBS"
    cmake --install "$W/build"
    cp "$W/$SDL/LICENSE.txt" "$DEPS/SDL3-LICENSE.txt"
    rm -rf "$W"
fi

# Setup, the SDK (and the developer tools, unused here)
cmake -S . -B "$L/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$DEPS"
cmake --build "$L/cmake" -j"$JOBS"
for t in rondo_setup "RondoRecomp Setup" sdk/lib/libSDL3.so.0; do
    [ -f "$L/cmake/$t" ] || { echo "linux: $t was not built"; exit 1; }
done

# the AppImage's folder, put together on the container's own file system so that the image
# keeps exact permissions (programs 755, everything else 644)
P=$(mktemp -d)
OUT=$P/RondoRecomp.AppDir
mkdir -p "$OUT"
install -m 755 "$L/cmake/rondo_setup" "$L/cmake/RondoRecomp Setup" tools/linux/AppRun "$OUT/"
install -m 644 tools/linux/RondoRecomp.desktop tools/linux/rondorecomp.png "$OUT/"
ln -s rondorecomp.png "$OUT/.DirIcon"
echo "$VER" > "$OUT/version.txt" && chmod 644 "$OUT/version.txt"
install -m 644 "$L/cmake/sdk/lib/libSDL3.so.0" "$OUT/"
cp -r "$L/cmake/sdk" "$OUT/sdk"
find "$OUT/sdk" -type d -exec chmod 755 {} + && find "$OUT/sdk" -type f -exec chmod 644 {} +
tar -xf "$S/$ZIG.tar.xz" -C "$P" && mv "$P/$ZIG" "$OUT/zig"
[ "$("$OUT/zig/zig" version)" = "$ZIGV" ] || { echo "zig/ is not $ZIGV (RONDO_ZIG_VERSION in programs.h)"; exit 1; }
install -m 644 docs/PLAYER_README.txt "$OUT/README.txt"
strip "$OUT/rondo_setup" "$OUT/RondoRecomp Setup"

{
    echo "Third-party software included with RondoRecomp"
    echo "==============================================="
    echo
    echo "SDL 3 (libSDL3.so.0, sdk/include/SDL3, sdk/lib): zlib license"
    echo "    https://libsdl.org"
    echo "Zig $("$OUT/zig/zig" version) (zig/): MIT; its LLVM, Clang and LLD: Apache 2.0 with LLVM exceptions"
    echo "    https://ziglang.org - see zig/LICENSE"
    echo "zlib (statically linked into the Setup programs): zlib license"
    echo "    https://zlib.net"
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
    echo "LZMA SDK 26.05 (7z decoder, built into the Setup programs for .7z files): public domain,"
    echo "    Igor Pavlov - https://www.7-zip.org/sdk.html"
    echo "stb_vorbis 1.22 (built into the Setup programs for Virtual Console audio): public domain"
    echo "    (or MIT), Sean Barrett - https://github.com/nothings/stb"
    echo "AppImage type2-runtime 20251108 (the start of the AppImage file, which mounts it): MIT,"
    echo "    the AppImage project - https://github.com/AppImage/type2-runtime; it includes"
    echo "    libfuse 3 (LGPL 2.1) and squashfuse (BSD), see that page"
    echo
    echo "The license texts follow."
    for f in "$DEPS/SDL3-LICENSE.txt" /usr/share/doc/zlib1g/copyright tools/fonts/OFL-*.txt third_party/rcheevos/LICENSE; do
        [ -f "$f" ] || continue
        echo
        echo "---------------------------------------------------------------------------"
        case $f in
            */SDL3-LICENSE.txt) echo "SDL3/LICENSE.txt" ;;
            */zlib1g/copyright) echo "zlib/copyright" ;;
            */rcheevos/LICENSE) echo "rcheevos/LICENSE" ;;
            *) basename "$f" ;;
        esac
        echo "---------------------------------------------------------------------------"
        cat "$f"
    done
} > "$OUT/THIRD_PARTY.txt"
chmod 644 "$OUT/THIRD_PARTY.txt"

# the AppImage's folder as a tar, which keeps those permissions on the way out of the
# container; tools/package_release_linux.sh makes the AppImage from it
tar -cf "$L/appdir.tar" -C "$OUT" --owner=0 --group=0 --numeric-owner .
echo "appdir: $L/appdir.tar ($(du -sh "$OUT" | cut -f1))"
rm -rf "$P"
