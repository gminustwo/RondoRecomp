#!/bin/bash
# Builds the Linux release (x86-64; the Steam Deck and desktop Linux):
# dist/RondoRecomp-<version>-x86_64.AppImage. Run on Linux or in WSL, with podman and
# squashfs-tools 4.6 or newer: the build runs in an Ubuntu 22.04 container
# (tools/linux/Containerfile, built once and cached) so that the result runs on glibc 2.35 or
# newer; the AppImage is put together here from the folder it hands back. See
# tools/linux/build.sh and docs/distribution.md.
#
# usage: tools/package_release_linux.sh
set -euo pipefail
cd "$(dirname "$0")/.."
command -v podman >/dev/null || { echo "needs podman (sudo apt install podman)"; exit 1; }
command -v mksquashfs >/dev/null || { echo "needs mksquashfs (sudo apt install squashfs-tools)"; exit 1; }
IMG=localhost/rondorecomp-build:22.04
podman build -q -t "$IMG" -f tools/linux/Containerfile tools/linux >/dev/null
podman run --rm -v "$PWD:/src" -w /src "$IMG" tools/linux/build.sh

# the AppImage: the runtime (fetched by build.sh), then the folder as a squashfs image (zstd,
# which the runtime reads), made from the tar so that it keeps the container's permissions
VER=$(sed -n 's/^#define RONDO_VERSION "\(.*\)"/\1/p' tools/frontend/programs.h)
APP=dist/RondoRecomp-$VER-x86_64.AppImage
W=$(mktemp -d)
mksquashfs - "$W/image.squashfs" -tar -noappend -no-progress -quiet -comp zstd -Xcompression-level 19 \
    < build/linux/appdir.tar
mkdir -p dist
rm -f dist/RondoRecomp-*-linux-x86_64*.tar.gz dist/RondoRecomp-*-x86_64.AppImage
cat build/linux/src/appimage-runtime-20251108-x86_64 "$W/image.squashfs" > "$APP"
chmod 755 "$APP"
rm -rf "$W" build/linux/appdir.tar
echo "release: $APP ($(du -sh "$APP" | cut -f1))"
