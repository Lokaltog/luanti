#!/usr/bin/env bash
# Records the afx test tour: what the player hears, plus the client's
# acoustic state, at six stations (open field, cave, tunnel, hut, cliff,
# under water). Then prints a per-station summary.
#
#   util/afx/tour.sh [outdir] [key=value ...]
#   util/afx/tour.sh /tmp/tour afx_enable=false     # baseline
#
# Audio goes through OpenAL Soft's wave backend into <outdir>/tour.wav, so no
# sound card is needed. USE_XVFB=1 renders off-screen on Mesa's software GL.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/afx-tour}; shift || true
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -r "$ROOT/util/afx/testworld" "$WORK/world"

cat > "$WORK/alsoft.conf" <<CONF
[general]
drivers = wave
channels = stereo
frequency = 48000
hrtf = false
[wave]
file = $OUT/tour.wav
CONF

CONF=$WORK/client.conf
cat > "$CONF" <<CONF
screen_w = 640
screen_h = 360
viewing_range = 40
enable_sound = true
sound_volume = 1.0
mute_sound = false
afx_debug = true
enable_ipv6 = false
ipv6_server = false
CONF
for kv in "$@"; do echo "${kv%%=*} = ${kv#*=}" >> "$CONF"; done

if [ "${USE_XVFB:-0}" = 1 ]; then
	DISP=:${XVFB_DISPLAY:-98}
	Xvfb "$DISP" -screen 0 640x360x24 >/dev/null 2>&1 &
	XPID=$!
	trap 'kill $XPID 2>/dev/null; rm -rf "$WORK"' EXIT
	export DISPLAY=$DISP LIBGL_ALWAYS_SOFTWARE=1
	sleep 1
fi

ALSOFT_CONF="$WORK/alsoft.conf" timeout 240 "$ROOT/bin/luanti" --config "$CONF" \
	--go --info --world "$WORK/world" --name tester --password "" >"$OUT/log.txt" 2>&1 || true

python3 "$ROOT/util/afx/analyze_tour.py" "$OUT/log.txt" "$OUT/tour.wav"
