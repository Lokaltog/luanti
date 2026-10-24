#!/usr/bin/env bash
# Smoke test for the fx renderer.
#
#   util/fx/smoketest.sh            # headless: Xvfb + Mesa llvmpipe, no GPU needed
#   USE_XVFB=0 util/fx/smoketest.sh # on your own display and GPU
#
# Renders the test scene in several configurations (fx off, night, day,
# underwater, each debug view). Fails if a shader fails to compile, a GL error
# is logged, or the client never gets to take its screenshot.
# Screenshots land in ${OUT:-shots/smoke}/.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "${OUT:-$ROOT/shots/smoke}")
USE_XVFB=${USE_XVFB:-1}
WAIT=${WAIT:-$([ "$USE_XVFB" = 1 ] && echo 45 || echo 10)}
mkdir -p "$OUT"

if [ "$USE_XVFB" = 1 ]; then
	command -v Xvfb >/dev/null || { echo "Xvfb not found (it is in the devenv shell)" >&2; exit 2; }
	DISP=:${XVFB_DISPLAY:-97}
	Xvfb "$DISP" -screen 0 960x540x24 >/dev/null 2>&1 &
	XPID=$!
	trap 'kill $XPID 2>/dev/null' EXIT
	export DISPLAY=$DISP LIBGL_ALWAYS_SOFTWARE=1
	sleep 1
fi

# name | extra settings (space separated key=value)
CASES=(
	"upstream|fx_enable=false"
	"night|"
	"day|fxscene_time=0.45"
	"underwater|fxscene_time=0.45 fxscene_view=underwater"
	"no_taa_ao|fx_taa=false fx_ao=false"
	"debug1_chroma|fx_debug_view=1"
	"debug2_emissive|fx_debug_view=2"
	"debug3_luminance|fx_debug_view=3"
	"debug5_scenecopy|fx_debug_view=5"
	"debug7_ao|fx_debug_view=7"
	"tuner|fx_open_tuner_on_start=true"
	"mood_vibrant|fx_mood=vibrant fxscene_time=0.45"
	"mood_dark|fx_mood=dark"
)
if [ -n "${ONLY:-}" ]; then
	mapfile -t CASES < <(printf '%s\n' "${CASES[@]}" | grep -E "^($ONLY)\|")
fi

fail=0
for case in "${CASES[@]}"; do
	name=${case%%|*}
	extra=${case#*|}
	WORK=$(mktemp -d)
	cp -r "$ROOT/util/fx/testworld" "$WORK/world"
	CONF=$WORK/client.conf
	cat > "$CONF" <<CONF
video_driver = opengl3
screen_w = 960
screen_h = 540
enable_post_processing = true
enable_bloom = true
enable_auto_exposure = false
enable_sound = false
enable_ipv6 = false
ipv6_server = false
opengl_debug = true
debug_log_level = info
fx_auto_screenshot = $OUT/$name.png
fx_auto_screenshot_delay = $WAIT
CONF
	for kv in $extra; do echo "${kv%%=*} = ${kv#*=}" >> "$CONF"; done
	LOG=$WORK/debug.txt
	rm -f "$OUT/$name.png"
	timeout $((WAIT + 60)) "$ROOT/bin/luanti" --config "$CONF" --logfile "$LOG" \
		--go --world "$WORK/world" --name tester --password "" >"$WORK/stdout.txt" 2>&1
	code=$?
	errors=$(grep -hE "failed to compile|Failed to compile|GL_INVALID|FBO incomplete|ShaderException|Segmentation|terminate called" \
		"$LOG" "$WORK/stdout.txt" 2>/dev/null | sort -u | head -5)
	[ -f "$OUT/$name.png" ] || errors+=$'\n(no screenshot, exit code '"$code"')'
	if [ -n "$errors" ]; then
		echo "FAIL $name"
		echo "$errors" | sed 's/^/    /'
		cp "$LOG" "$OUT/$name.log" 2>/dev/null
		fail=1
	else
		echo "ok   $name"
	fi
	rm -rf "$WORK"
done
echo "screenshots: $OUT"
exit $fail
