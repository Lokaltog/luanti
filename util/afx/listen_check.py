#!/usr/bin/env python3
"""Listening regression tests: record scenarios in the real client and check
that each segment's loudness sits in its expected band.

    listen_check.py [--only NAME] [--keep DIR] [bands.json]

Each check in bands.json (default: listen_bands.json next to this script):

    {"name": "muffle", "scenario": "voxelibre",
     "args": ["afxplay_mode=muffle", "afx_mood=0"],
     "env": {"AFX_XDO": "..."},            # optional, see tour.sh
     "segments": {
        "forest floor": {"lufs": [-36, -24]},          # played loudness
        "clearing": {"bed": [-40, -10]},               # dB of the peak-normalised mix
        "sealed plank house": {"bed_rel": ["forest floor", -60, -25]},
        "pocket 8": {"peak": [null, -40]}}}

Segments are the timeline labels the scenario logs ("afxscene: <label>" or
"afxplay: <label>", matched by prefix), cut from the recording aligned to its
end (the wave writer starts late). Per segment, over 100 ms windows after a
3 s settle: "bed" is the median level, "peak" the 98th percentile, both of
the recording normalised to its peak (so they say how the mix balances).
"lufs" (integrated) and "loudest" (the loudest 3 s, short-term LUFS) are
measured at the level the client actually played, and say how loud it is.
"_rel" bands compare with another segment of the same run.

Every run also has to stay under the gain-staging ceiling (the check's
"ceiling" overrides the defaults in CEILING): no sample over full scale
(the output limiter holds it), and no 3 s louder than the short-term
ceiling. Lua errors fail the run.
"""
import json
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from analyze_tour import read_wav  # noqa: E402

TS = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")
LABEL = re.compile(r"(?:afxplay|afxscene): (?!where)(.*)$")
SILENT = -120.0
# Nothing should ever get painfully loud: the whole run, at its played level
CEILING = {"sample_peak": 0.0, "short_term": -12.0}


def loudness(x, rate):
    """Integrated LUFS and loudest short-term (3 s) LUFS of a stereo cut."""
    import pyloudnorm
    meter = pyloudnorm.Meter(rate)
    lufs = meter.integrated_loudness(x) if len(x) >= 0.4 * rate else SILENT
    loudest = SILENT
    win, hop = 3 * rate, rate // 2
    for i in range(0, max(1, len(x) - win), hop):
        a = x[i:i + win]
        if len(a) < win:
            break
        loudest = max(loudest, meter.integrated_loudness(a))
    clean = lambda v: max(SILENT, v) if np.isfinite(v) else SILENT  # noqa: E731
    return clean(lufs), clean(loudest)


def load_raw(wav):
    path = wav.rsplit(".", 1)[0] + ".raw.npz"
    if not os.path.exists(path):
        return None, None
    z = np.load(path)
    x = z["x"].astype(np.float64)
    return (x if x.ndim > 1 else np.stack([x, x], 1)), int(z["rate"])


def run(check, out_dir, display):
    env = dict(os.environ, USE_XVFB="1", XVFB_DISPLAY=str(display),
               TOUR_TIMEOUT=str(check.get("timeout", 240)))
    env.update(check.get("env", {}))
    cmd = [os.path.join(HERE, "tour.sh"), out_dir, check["scenario"]] + check.get("args", [])
    subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    base = os.path.join(out_dir, check["scenario"])
    return base + ".log", base + ".wav"


def segments(log, wav):
    t_audio, segs = None, []
    errors = []
    for line in open(log, errors="replace"):
        if "ERROR[" in line or "Lua error" in line or "runtime error" in line:
            errors.append(line.strip()[:200])
        m = TS.match(line)
        if not m:
            continue
        t = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
        if "Audio: Global Initialized" in line and t_audio is None:
            t_audio = t
        m2 = LABEL.search(line)
        if m2 and t_audio:
            segs.append((m2.group(1).strip(), (t - t_audio).total_seconds()))
    x, sr = read_wav(wav)
    if x.ndim > 1:
        x = x.mean(axis=1)
    raw, raw_sr = load_raw(wav)
    # align to the end: "done" is logged when the recording is about to stop
    if segs and segs[-1][0] == "done":
        shift = len(x) / sr - segs[-1][1]
        segs = [(n, t + shift) for n, t in segs]
    stats = {}
    for i, (name, t) in enumerate(segs):
        end = segs[i + 1][1] if i + 1 < len(segs) else len(x) / sr
        a = x[max(0, int((t + 3) * sr)):max(0, int(end * sr))]
        w = int(0.1 * sr)
        k = len(a) // w
        if k < 5:
            continue
        r = np.sqrt((a[:k * w].reshape(k, w) ** 2).mean(1))
        db = 20 * np.log10(np.maximum(r, 1e-6))
        stats[name] = {"bed": max(SILENT, float(np.median(db))),
                       "peak": max(SILENT, float(np.percentile(db, 98)))}
        if raw is not None:
            seg = raw[max(0, int((t + 3) * raw_sr)):max(0, int(end * raw_sr))]
            stats[name]["lufs"], stats[name]["loudest"] = loudness(seg, raw_sr)
    if raw is not None:
        whole = {"sample_peak": 20 * np.log10(max(1e-9, float(np.abs(raw).max())))}
        whole["lufs"], whole["short_term"] = loudness(raw, raw_sr)
        stats["(whole run)"] = whole
    return stats, errors


def find(stats, prefix):
    for name, s in stats.items():
        if name.startswith(prefix):
            return name, s
    return None, None


def within(v, lo, hi):
    return (lo is None or v >= lo) and (hi is None or v <= hi)


def evaluate(check, stats, errors):
    rows, ok = [], not errors
    whole = stats.get("(whole run)")
    if whole is None:
        rows.append(("(whole run)", "ceiling", "no raw-level recording", False))
        ok = False
    else:
        ceiling = dict(CEILING, **check.get("ceiling", {}))
        for key, top in ceiling.items():
            good = whole[key] <= top
            rows.append(("(whole run)", key, f"{whole[key]:.1f} dB, ceiling {top}", good))
            ok = ok and good
        rows.append(("(whole run)", "lufs", f"{whole['lufs']:.1f} LUFS", True))
    for prefix, bands in check["segments"].items():
        name, s = find(stats, prefix)
        if s is None:
            rows.append((prefix, "missing", "", False))
            ok = False
            continue
        for key, band in bands.items():
            if key.endswith("_rel"):
                metric = key[:-4]
                ref_name, ref = find(stats, band[0])
                if ref is None:
                    rows.append((prefix, key, f"no segment {band[0]!r}", False))
                    ok = False
                    continue
                v = s[metric] - ref[metric]
                good = within(v, band[1], band[2])
                rows.append((prefix, key, f"{v:+.1f} dB vs {band[0]!r} in {band[1:]}", good))
            else:
                v = s[key]
                good = within(v, band[0], band[1])
                rows.append((prefix, key, f"{v:.1f} dB in {band}", good))
            ok = ok and good
        if "lufs" in s:
            rows.append((prefix, "played", f"{s['lufs']:.1f} LUFS, loudest 3 s {s['loudest']:.1f}", True))
    return ok, rows


def main(argv):
    only, keep, path = None, None, os.path.join(HERE, "listen_bands.json")
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--only":
            only = args.pop(0)
        elif a == "--keep":
            keep = args.pop(0)
        else:
            path = a
    checks = json.load(open(path))
    failed = 0
    for i, check in enumerate(checks):
        if only and check["name"] != only:
            continue
        out = os.path.join(keep, check["name"]) if keep else tempfile.mkdtemp(prefix="afxlisten-")
        os.makedirs(out, exist_ok=True)
        log, wav = run(check, out, 80 + i)
        if not os.path.exists(wav):
            print(f"FAIL {check['name']}: no recording ({log})")
            failed += 1
            continue
        stats, errors = segments(log, wav)
        ok, rows = evaluate(check, stats, errors)
        print(f"{'PASS' if ok else 'FAIL'} {check['name']}")
        for seg, key, what, good in rows:
            print(f"   {'ok ' if good else 'BAD'} {seg[:34]:34} {key:9} {what}")
        for e in errors[:5]:
            print(f"   ERR {e}")
        failed += 0 if ok else 1
    print(f"\n{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
