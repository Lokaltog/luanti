#!/usr/bin/env bash
# Records a scripted listening scenario in the real client: what the player
# hears (OpenAL Soft's wave backend, no sound card needed) plus the client's
# acoustic state, with the afx_ambience content mod running.
#
#   util/afx/tour.sh <outdir> <scenario> [key=value ...]
#   scenarios: hut_rain tunnel_wind forest cavern cliff descent meadow beach sky_island
#   voxelibre: a real-game tour (needs games/voxelibre, e.g. a VoxeLibre checkout)
#   util/afx/tour.sh /tmp/afx cavern afx_enable=false     # A/B baseline
#
# Writes <outdir>/<scenario>.wav (from the scenario start), .mp3 and .log.
# USE_XVFB=1 renders off-screen on Mesa's software GL.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; SCEN=$2; shift 2
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
if [ "$SCEN" = voxelibre ]; then
	cp -r "$ROOT/util/afx/vltest" "$WORK/world"
	mkdir -p "$WORK/world/worldmods"
else
	cp -r "$ROOT/util/afx/testworld" "$WORK/world"
fi
cp -r "$ROOT/util/afx/mods/afx_ambience" "$WORK/world/worldmods/"
# AFX_NO_TAKES=1: without the extra footstep/dig takes (for A/B)
if [ "${AFX_NO_TAKES:-0}" = 1 ]; then
	find "$WORK/world/worldmods/afx_ambience/sounds" -regex '.*\(footstep\|dig\|dug\|place\).*\.[789]\.ogg' -delete
fi

cat > "$WORK/alsoft.conf" <<CONF
[general]
drivers = wave
channels = stereo
frequency = 48000
hrtf = false
[wave]
file = $WORK/raw.wav
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
afxscene_scenario = $SCEN
afx_ambience_use_biomes = false
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

LOG=$OUT/$SCEN.log
ALSOFT_CONF="$WORK/alsoft.conf" timeout ${TOUR_TIMEOUT:-300} "$ROOT/bin/luanti" --config "$CONF" \
	--go --info --world "$WORK/world" --name tester --password "" >"$LOG" 2>&1 || true

python3 "$ROOT/util/afx/cut_scenario.py" "$LOG" "$WORK/raw.wav" "$OUT/$SCEN.wav"
ffmpeg -v error -y -i "$OUT/$SCEN.wav" -codec:a libmp3lame -q:a 2 "$OUT/$SCEN.mp3"
grep -E "afxscene: |afx: encl" "$LOG" | sed -E 's/^[0-9-]+ //' | cut -c1-170
