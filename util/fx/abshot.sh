#!/usr/bin/env bash
# Repeatable screenshot of the fx test scene.
#
#   util/fx/abshot.sh <out.png> [key=value ...]
#   util/fx/abshot.sh shots/upstream.png fx_enable=false
#   util/fx/abshot.sh shots/fx-day.png fxscene_time=0.45
#   util/fx/abshot.sh shots/under.png fxscene_time=0.45 fxscene_view=underwater
#
# Runs the client on the fixed scene in util/fx/testworld (colored lamps, lava,
# glass wall, water pool). The client saves the frame itself
# (fx_auto_screenshot) after WAIT seconds and quits, so this works on X11,
# Wayland or headless. USE_XVFB=1 renders off-screen on Mesa's software GL.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "$1"); shift
USE_XVFB=${USE_XVFB:-0}
WAIT=${WAIT:-$([ "$USE_XVFB" = 1 ] && echo 45 || echo 12)}

WORK=$(mktemp -d)
cleanup() { [ -n "${XPID:-}" ] && kill "$XPID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT
cp -r "$ROOT/util/fx/testworld" "$WORK/world"

CONF=$WORK/client.conf
cat > "$CONF" <<CONF
video_driver = opengl3
screen_w = 960
screen_h = 540
enable_post_processing = true
enable_bloom = true
enable_auto_exposure = false
smooth_lighting = true
viewing_range = 60
enable_sound = false
enable_ipv6 = false
ipv6_server = false
fx_auto_screenshot = $OUT
fx_auto_screenshot_delay = $WAIT
CONF
for kv in "$@"; do echo "${kv%%=*} = ${kv#*=}" >> "$CONF"; done

if [ "$USE_XVFB" = 1 ]; then
	DISP=:${XVFB_DISPLAY:-99}
	Xvfb "$DISP" -screen 0 960x540x24 >/dev/null 2>&1 &
	XPID=$!
	export DISPLAY=$DISP LIBGL_ALWAYS_SOFTWARE=1
	sleep 1
fi

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
timeout $((WAIT + 60)) "$ROOT/bin/luanti" --config "$CONF" --logfile "$WORK/debug.txt" \
	--go --world "$WORK/world" --name tester --password "" >"$WORK/stdout.txt" 2>&1 || true
if [ -f "$OUT" ]; then
	echo "wrote $OUT"
else
	echo "no screenshot; last log lines:" >&2
	tail -20 "$WORK/debug.txt" >&2
	exit 1
fi
