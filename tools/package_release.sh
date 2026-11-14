#!/bin/bash
# Assembles the release folder dist/RondoRecomp and dist/RondoRecomp-<version>-windows-x64.zip,
# and the update package (the same without zig/, which installations already have):
# dist/RondoRecomp-<version>-windows-x64-update-zig<zig version>.zip (tools/frontend/update.h).
# The release contains no game material: players provide their own disc image to Setup.
#
# usage: tools/package_release.sh [version]   (default: RONDO_VERSION, tools/frontend/programs.h)
set -euo pipefail
cd "$(dirname "$0")/.."
VER=${1:-$(sed -n 's/^#define RONDO_VERSION "\(.*\)"/\1/p' tools/frontend/programs.h)}
ZIGV=$(sed -n 's/^#define RONDO_ZIG_VERSION "\(.*\)"/\1/p' tools/frontend/programs.h)
B=build
LIC=/c/msys64/ucrt64/share/licenses

[ -x build/zig/zig.exe ] || tools/fetch_zig.sh
cmake --build $B

OUT=dist/RondoRecomp
rm -rf "$OUT" dist/RondoRecomp-*-windows-x64*.zip   # (the Linux release, also in dist/, is kept)
mkdir -p "$OUT"
cp "$B/RondoRecomp Setup.exe" "$B/rondo_setup.exe" "$B/SDL3.dll" "$B/libiconv-2.dll" "$OUT/"
cp -r "$B/sdk" "$OUT/sdk"
cp -r build/zig "$OUT/zig"
[ "$("$OUT/zig/zig.exe" version)" = "$ZIGV" ] || { echo "zig/ is not $ZIGV (RONDO_ZIG_VERSION in programs.h)"; exit 1; }
cp docs/PLAYER_README.txt "$OUT/README.txt"

{
    echo "Third-party software included with RondoRecomp"
    echo "==============================================="
    echo
    echo "SDL 3 (SDL3.dll, sdk/include/SDL3, sdk/lib): zlib license"
    echo "    https://libsdl.org"
    echo "Zig $("$OUT/zig/zig.exe" version) (zig/): MIT; its LLVM, Clang and LLD: Apache 2.0 with LLVM exceptions"
    echo "    https://ziglang.org - see zig/LICENSE"
    echo "zlib (statically linked into the Setup programs): zlib license"
    echo "    https://zlib.net"
    echo "GNU libiconv (libiconv-2.dll, used by SDL3.dll): LGPL 2.1 or later"
    echo "    https://www.gnu.org/software/libiconv - source: https://packages.msys2.org"
    echo "stb_truetype (sdk/src/frontend/stb_truetype.h, draws the menus' text): public domain"
    echo "    (or MIT), Sean Barrett - https://github.com/nothings/stb"
    echo "Cinzel, Cinzel Decorative and UnifrakturMaguntia fonts (sdk/src/frontend/ui_fonts.c, built"
    echo "into rondo.exe for the menus): SIL Open Font License 1.1 - the Cinzel Project Authors,"
    echo "Natanael Gama; j. 'mach' wust, Peter Wiegel. https://github.com/google/fonts"
    echo "Shinonome Gothic 12 and 16 dot bitmap fonts (sdk/src/runtime/pce/kanji_font.c, the"
    echo "Japanese text of the original disc): public domain, The Electronic Font Open Laboratory"
    echo "    http://openlab.jp/efont/shinonome/"
    echo "rcheevos 12.5.0 (sdk/src/rcheevos, built into rondo.exe for RetroAchievements): MIT,"
    echo "    RetroAchievements - https://github.com/RetroAchievements/rcheevos"
    echo "LZMA SDK 26.05 (7z and LZMA decoder: built into the Setup programs for .7z files, and its LZMA"
    echo "    decoder, sdk/src/lzma, into the game for .chd files): public domain, Igor Pavlov -"
    echo "    https://www.7-zip.org/sdk.html"
    echo "stb_vorbis 1.22 (built into the Setup programs for Virtual Console audio): public domain"
    echo "    (or MIT), Sean Barrett - https://github.com/nothings/stb"
    echo
    echo "The license texts follow."
    for f in "$LIC/SDL3/LICENSE.txt" "$LIC/zlib/LICENSE" "$LIC/libiconv/COPYING.LIB" tools/fonts/OFL-*.txt third_party/rcheevos/LICENSE; do
        [ -f "$f" ] || continue
        echo
        echo "---------------------------------------------------------------------------"
        echo "$f" | sed 's|.*/licenses/||; s|tools/fonts/||; s|third_party/||'
        echo "---------------------------------------------------------------------------"
        cat "$f"
    done
} > "$OUT/THIRD_PARTY.txt"

(cd dist && python -m zipfile -c "RondoRecomp-$VER-windows-x64.zip" RondoRecomp)
UPD=RondoRecomp-$VER-windows-x64-update-zig$ZIGV.zip
(cd dist && python -c '
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk("RondoRecomp"):
        if root == "RondoRecomp":
            dirs.remove("zig")
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, p.replace(os.sep, "/"))
' "$UPD")
echo "release: $OUT ($(du -sh "$OUT" | cut -f1)), dist/RondoRecomp-$VER-windows-x64.zip ($(du -sh "dist/RondoRecomp-$VER-windows-x64.zip" | cut -f1)), dist/$UPD ($(du -sh "dist/$UPD" | cut -f1))"
