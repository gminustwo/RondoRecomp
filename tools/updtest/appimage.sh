#!/bin/bash
# The Linux AppImage (in WSL too, which has FUSE): a fresh install into a RONDO_HOME of its
# own, the game started through the AppImage, then an update by the game itself
# (--autotest-update) from a local server offering the same AppImage as a newer release. On
# Linux the game restarts in the same process, through the new AppImage, so this returns
# after the new game's --autotest. Work files in ~/apptest.
#   DISC=<folder with your disc's .cue> tools/updtest/appimage.sh
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
DISC=${DISC:-$REPO/work/release_test13/RondoRecomp/game/en}
VER=$(sed -n 's/^#define RONDO_VERSION "\(.*\)"/\1/p' "$REPO/tools/frontend/programs.h")
APP=RondoRecomp-$VER-x86_64.AppImage
T=~/apptest
rm -rf $T && mkdir -p $T/srv $T/shots && cd $T || exit 1
cp "$REPO/dist/$APP" . && cp "$REPO/dist/$APP" srv/ || exit 1
export RONDO_HOME=$T/home

START=$(date +%s)
./$APP --setup-cli "$DISC" > setup.log 2>&1
echo "== setup through the AppImage: exit $? after $(( $(date +%s) - START ))s"
tr '\r' '\n' < setup.log | grep -v '^\s*$' | tail -2
echo "version stamp: $(cat home/build/version.txt 2>/dev/null)"
ls -l home/rondo home/libSDL3.so.0 2>&1 | cut -c1-10,40-

./$APP --autotest > /dev/null 2>&1
echo "== --autotest through the AppImage: exit $?"
grep -E "^build|native|autotest" home/rondo.log

# the start screen's Setup and Setup's Play: the game becomes Setup, which becomes the game
# again, all in one process (Steam keeps track of it; the AppImage stays mounted)
rm -f home/setup_autotest.log home/rondo.log
timeout 300 ./$APP --autotest-setup > /dev/null 2>&1
echo "== game -> Setup -> game: exit $?"
cat home/setup_autotest.log 2>/dev/null || echo "NO setup_autotest.log"
grep -E "^process|^build|native" home/rondo.log 2>/dev/null || echo "THE GAME DID NOT COME BACK"
pids=$( (grep -o "process [0-9]*" home/setup_autotest.log; grep -o "^process: [0-9]*" home/rondo.log) 2>/dev/null | grep -o "[0-9]*" | sort -u | wc -l)
[ "$pids" = 1 ] && echo "one process throughout" || echo "PROCESSES DIFFER ($pids)"

# the update: this AppImage, offered as a newer release
SUM=$(sha256sum srv/$APP | cut -d' ' -f1)
SIZE=$(stat -c %s srv/$APP)
cat > srv/latest.json <<EOF
{"tag_name": "v$VER", "html_url": "https://github.com/gminustwo/RondoRecomp/releases/tag/v$VER",
 "draft": false, "prerelease": false,
 "body": "## New in $VER\n\n- **The AppImage updater test.** Nothing else.\n",
 "assets": [{"name": "$APP", "size": $SIZE, "digest": "sha256:$SUM",
             "browser_download_url": "http://127.0.0.1:8767/$APP"}]}
EOF
python3 -m http.server 8767 --bind 127.0.0.1 --directory srv > server.log 2>&1 &
SERVER=$!
sleep 1
INODE=$(stat -c %i $APP)
rm -f home/rondo.log
START=$(date +%s)
RONDO_UPDATE_URL=http://127.0.0.1:8767/latest.json RONDO_UPDATE_FROM=0.1 timeout 900 ./$APP \
    --autotest-update "$T/shots" > game.out 2>&1
echo "== update through the game: exit $? after $(( $(date +%s) - START ))s"
kill $SERVER
grep autotest-update home/rondo_update.log
echo "-- the restarted game's --autotest:"
grep -E "^build|native|autotest" home/rondo.log
[ "$(stat -c %i $APP)" != "$INODE" ] && echo "the AppImage was replaced" || echo "THE APPIMAGE WAS NOT REPLACED"
cmp -s $APP srv/$APP && echo "it is the release's AppImage" || echo "IT IS NOT THE RELEASE'S APPIMAGE"
[ -x $APP ] && echo "executable" || echo "NOT EXECUTABLE"
ls $APP.new 2>/dev/null && echo ".new LEFT BEHIND"
mount | grep -c "\.mount_Rondo" | sed 's/^/AppImage mounts left: /'
