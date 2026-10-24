#!/usr/bin/env bash
# Screenshots of the mood grade at scenario milestones, with and without it.
#   util/afx/moodshots.sh <outdir> <scenario> "<log marker>:<delay s>" ...
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; SCEN=$2; shift 2
mkdir -p "$OUT"
for g in 0 1; do
	DISP=9$g
	( USE_XVFB=1 XVFB_DISPLAY=$DISP "$ROOT/util/afx/tour.sh" "$OUT/run$g" "$SCEN" \
		fx_mood_grade=$g enable_post_processing=true >/dev/null 2>&1 ) &
	for spec in "$@"; do
		marker=${spec%%:*}; delay=${spec##*:}
		until grep -q "afxscene: $marker" "$OUT/run$g/$SCEN.log" 2>/dev/null; do sleep 1; done
		sleep "$delay"
		DISPLAY=:$DISP import -window root "$OUT/${SCEN}_${marker// /_}_grade$g.png"
		grep "afx: encl" "$OUT/run$g/$SCEN.log" | tail -1 | grep -o "tension.*home [0-9]*"
	done
	wait
done
