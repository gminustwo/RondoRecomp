#!/bin/bash
# The macOS app (tools/package_release_mac.sh): a fresh install into a RONDO_HOME of its own,
# the game started through the app (as the Finder does: open), the start screen's Setup and
# Setup's Play (one process throughout), then an update by the game itself (--autotest-update)
# from a local server offering the same release zip as a newer release: the app is replaced
# and the new game's --autotest runs. Work files in $T (default ~/apptest-mac).
#   DISC=<.chd or .cue of your disc> tools/updtest/macos.sh
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
VER=$(sed -n 's/^#define RONDO_VERSION "\(.*\)"/\1/p' "$REPO/tools/frontend/programs.h")
ZIP=RondoRecomp-$VER-macos-arm64.zip
T=${T:-$HOME/apptest-mac}
DISC=${DISC:?DISC=<your disc .chd or .cue>}
rm -rf "$T" && mkdir -p "$T/srv" "$T/shots" "$T/home/user" && cd "$T" || exit 1
cp "$REPO/dist/$ZIP" srv/ && ditto -x -k "srv/$ZIP" . || exit 1
APP=$T/RondoRecomp.app
export RONDO_HOME=$T/home/
waitgone() { for _ in $(seq 1 450); do pgrep -f "$T/home/rondo|$APP/" >/dev/null || return 0; sleep 2; done; echo "STILL RUNNING"; }

START=$(date +%s)
"$APP/Contents/MacOS/rondo_setup" "$DISC" > setup.log 2>&1
echo "== setup through the app: exit $? after $(( $(date +%s) - START ))s"
tr '\r' '\n' < setup.log | grep -v '^\s*$' | tail -2
echo "version stamp: $(cat home/build/version.txt 2>/dev/null)"

rm -f home/setup_autotest.log home/rondo.log
open --env RONDO_HOME="$RONDO_HOME" "$APP" --args --autotest-setup && sleep 3 && waitgone
echo "== app -> game -> Setup -> game:"
cat home/setup_autotest.log 2>/dev/null || echo "NO setup_autotest.log"
grep -E "^process|^build|native" home/rondo.log 2>/dev/null || echo "THE GAME DID NOT COME BACK"
pids=$( (grep -o "process [0-9]*" home/setup_autotest.log; grep -o "^process: [0-9]*" home/rondo.log) 2>/dev/null | grep -o "[0-9]*" | sort -u | wc -l | tr -d ' ')
[ "$pids" = 1 ] && echo "one process throughout" || echo "PROCESSES DIFFER ($pids)"

# the update: this release, offered as a newer one
SUM=$(shasum -a 256 "srv/$ZIP" | cut -d' ' -f1)
SIZE=$(stat -f %z "srv/$ZIP")
cat > srv/latest.json <<EOF
{"tag_name": "v$VER", "html_url": "https://github.com/gminustwo/RondoRecomp/releases/tag/v$VER",
 "draft": false, "prerelease": false,
 "body": "## New in $VER\n\n- **The macOS updater test.** Nothing else.\n",
 "assets": [{"name": "$ZIP", "size": $SIZE, "digest": "sha256:$SUM",
             "browser_download_url": "http://127.0.0.1:8767/$ZIP"}]}
EOF
python3 -m http.server 8767 --bind 127.0.0.1 --directory srv > server.log 2>&1 &
SERVER=$!
sleep 1
INODE=$(stat -f %i "$APP")
rm -f home/rondo.log
START=$(date +%s)
open --env RONDO_HOME="$RONDO_HOME" --env RONDO_UPDATE_URL=http://127.0.0.1:8767/latest.json \
    --env RONDO_UPDATE_FROM=0.1 "$APP" --args --autotest-update "$T/shots" && sleep 3 && waitgone
echo "== update through the game: after $(( $(date +%s) - START ))s"
kill $SERVER
grep autotest-update home/rondo_update.log
echo "-- the restarted game's --autotest:"
grep -E "^build|native|autotest" home/rondo.log
[ "$(stat -f %i "$APP")" != "$INODE" ] && echo "the app was replaced" || echo "THE APP WAS NOT REPLACED"
codesign --verify --deep --strict "$APP" && echo "its signature checks out" || echo "ITS SIGNATURE DOES NOT CHECK OUT"
ls -d "$APP.new" "$APP.old" 2>/dev/null && echo "LEFT BEHIND"
ls home/update 2>/dev/null && echo "home/update LEFT BEHIND"
true
