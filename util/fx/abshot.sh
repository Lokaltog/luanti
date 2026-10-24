#!/usr/bin/env bash
# Repeatable A/B screenshots of the fx test scene.
#
#   util/fx/abshot.sh <out.png> [key=value ...]
#   util/fx/abshot.sh shots/upstream.png fx_enable=false
#   util/fx/abshot.sh shots/fx.png fx_debug_view=1
#
# Runs the client on the fixed night scene in util/fx/testworld (three colored
# lamps, a lava pool, a white light), waits for meshes to settle and grabs
# the window. With USE_XVFB=1 it renders off-screen on Mesa's software GL,
# which is slow but needs no GPU: handy in CI or over SSH.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; shift
WAIT=${WAIT:-20}
[ "${USE_XVFB:-0}" = 1 ] && WAIT=${WAIT_XVFB:-55}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -r "$ROOT/util/fx/testworld" "$WORK/world"

CONF=$WORK/client.conf
cat > "$CONF" <<CONF
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
CONF
for kv in "$@"; do echo "${kv%%=*} = ${kv#*=}" >> "$CONF"; done

if [ "${USE_XVFB:-0}" = 1 ]; then
	DISP=:${XVFB_DISPLAY:-99}
	Xvfb "$DISP" -screen 0 960x540x24 >/dev/null 2>&1 &
	XPID=$!
	trap 'kill $XPID 2>/dev/null; rm -rf "$WORK"' EXIT
	export DISPLAY=$DISP LIBGL_ALWAYS_SOFTWARE=1
	sleep 1
fi

"$ROOT/bin/luanti" --config "$CONF" --go --world "$WORK/world" \
	--name tester --password "" >"$WORK/log.txt" 2>&1 &
PID=$!
sleep "$WAIT"
mkdir -p "$(dirname "$OUT")"
if command -v import >/dev/null; then
	import -window root "$OUT"
else
	grim "$OUT" 2>/dev/null || { echo "need imagemagick (import) or grim" >&2; exit 1; }
fi
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
echo "wrote $OUT"
