#!/usr/bin/env python3
"""The afx sound pipeline: every sample at a known loudness, every level in
one table.

    sound_pipeline.py report                 loudness of every mod sound
    sound_pipeline.py normalize [--dry-run]  bring every sound to its category's
                                             reference loudness; write levels.lua
    sound_pipeline.py ir <in.wav> <name>     an impulse response for the
                                             engine's convolution reverb
                                             ("left.wav+right.wav" for two
                                             mono takes; name it afx_ir_*)
    sound_pipeline.py import <pack_dir> <rules.json> [--dry-run]
                                             cut, loop, pick takes and normalize
                                             sounds from a sound library

Categories and their file reference loudness (integrated LUFS; short sounds
use their loudest 100 ms instead):

    bed      long loops heard as a place (afx_out_*, afx_in_*, rain on things,
             fire, rivers...)                                     -30
    accent   one-shots that happen in the world (birds, drips,
             creaks, knocks, ice...)                              -22
    foley    your own body (cloth, breath, rustle, swim)          -26
    dry      non-positional cues (score, heart, ring, mood)       -24

Each group's entry in levels.lua (dB) is the gain on top of its files: the
table is the mix. `normalize` keeps the mix as it was (each
group's entry is the difference it made to the files); from then on, levels
are tuned in the table, and a new or replaced sample needs no gain hunting.

Engine-played groups (room tone, cave moan, whoomp, ring, score, mood,
whizz, knock, impulse responses) only get their variants matched to each
other: the engine has its own gains.

Rules for `import` (JSON list):
    {"glob": "**/*rain*heavy*.wav", "name": "afx_out_rain_heavy", "kind": "bed",
     "seconds": 20}                         best steady stretch, seamless loop
    {"glob": "**/*twig*snap*.wav", "name": "afx_twig", "kind": "accent",
     "takes": 4, "max_seconds": 1.2}        distinct hits, trimmed and faded
"""
import collections
import glob
import json
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
# the engine's own ambience sounds and its mix (builtin/afx)
SOUNDS = os.path.join(ROOT, "sounds", "afx")
LEVELS = os.path.join(ROOT, "builtin", "afx", "levels.lua")
SR = 48000

TARGET = {"bed": -30.0, "accent": -22.0, "foley": -26.0, "dry": -24.0}

# group name -> category, first match wins
CATEGORY_RULES = [
    (r"^afx_(dry_breath|cloth|leaf_rustle|swim|splash_out|bubbles|dry_gasp)$", "foley"),
    (r"^afx_dry_|^afx_mood_", "dry"),
    (r"^afx_(out|in)_|^afx_rain_|^afx_(fire|furnace|lava|lava_bubble|river|waterfall|shore|"
     r"gutter|drip_after|insects|bees|draft|purr|clock|sizzle|cricket|roomtone|cave_moan|"
     r"fireball_fly)$", "bed"),
    (r"^afx_", "accent"),
]

# played by the engine itself: variants matched, group level untouched
ENGINE_GROUPS = {"afx_roomtone", "afx_cave_moan", "afx_dry_whoomp", "afx_dry_ring",
                 "afx_dry_score_piano", "afx_dry_score_bell", "afx_dry_score_bowl",
                 "afx_mood_dread", "afx_mood_cozy", "afx_mood_relief", "afx_mood_stinger",
                 "afx_whizz", "afx_knock_hollow", "afx_dry_heart"}


def category(group):
    for pat, cat in CATEGORY_RULES:
        if re.search(pat, group):
            return cat
    return None  # not ours (the game's own footstep takes)


def load(path, channels=1):
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", path, "-ac", str(channels),
                          "-ar", str(SR), "-f", "f32le", "-"], capture_output=True).stdout
    x = np.frombuffer(raw, dtype=np.float32).copy()
    return x.reshape(-1, channels) if channels > 1 else x


def save(path, x, quality=4):
    ch = 1 if x.ndim == 1 else x.shape[1]
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "f32le", "-ar", str(SR),
                    "-ac", str(ch), "-i", "-", "-c:a", "libvorbis", "-q:a", str(quality), path],
                   input=np.ascontiguousarray(x, dtype=np.float32).tobytes(), check=True)


def loudness(x):
    """Integrated LUFS; for short sounds, the loudest 100 ms (momentary)."""
    import pyloudnorm as pyln
    mono = x if x.ndim == 1 else x.mean(axis=1)
    if len(mono) >= int(0.5 * SR):
        try:
            v = pyln.Meter(SR).integrated_loudness(mono)
            if np.isfinite(v):
                return float(v)
        except ValueError:
            pass
    w = int(0.1 * SR)
    if len(mono) < w:
        mono = np.pad(mono, (0, w - len(mono)))
    k = len(mono) // w
    r = np.sqrt((mono[:k * w].reshape(k, w) ** 2).mean(1)).max()
    return float(20 * np.log10(r + 1e-9) - 0.7)  # ~ LUFS for a broadband hit


def groups():
    g = collections.defaultdict(list)
    for f in sorted(glob.glob(os.path.join(SOUNDS, "*.ogg"))):
        n = os.path.basename(f)
        g[re.sub(r"\.\d+\.ogg$", "", n).replace(".ogg", "")].append(f)
    return g


def cmd_report(_args):
    for grp, files in groups().items():
        cat = category(grp)
        if not cat:
            continue
        ls = [loudness(load(f)) for f in files]
        print(f"{grp:30} {cat:7} n={len(files):2} {min(ls):6.1f}..{max(ls):6.1f} LUFS  "
              f"(ref {TARGET[cat]:.0f})")


def cmd_normalize(args):
    dry = "--dry-run" in args
    levels = {}
    for grp, files in groups().items():
        cat = category(grp)
        if not cat:
            continue
        xs = [load(f, 2 if "_ir_" in grp else 1) for f in files]
        ls = [loudness(x) for x in xs]
        med = float(np.median(ls))
        engine = grp in ENGINE_GROUPS or grp.startswith("afx_ir_")
        target = med if engine else TARGET[cat]
        after = []
        for f, x, l in zip(files, xs, ls):
            gain_db = target - l
            if abs(gain_db) < 0.5:
                after.append(l)
                continue
            y = x * 10 ** (gain_db / 20)
            peak = np.abs(y).max()
            if peak > 0.97:  # keep headroom: the level table makes it up
                y *= 0.97 / peak
                gain_db += 20 * np.log10(0.97 / peak)
            after.append(l + gain_db)
            print(f"  {os.path.basename(f):34} {l:6.1f} -> {l + gain_db:6.1f} LUFS ({gain_db:+.1f} dB)")
            if not dry:
                save(f, y)
        if not engine:
            # the mix stays where it was: the group sounded `med`, its files now
            # sit at `after` (the reference, or under it where the peak
            # wouldn't allow it); the table holds the gain that makes up the rest
            levels[grp] = round(med - float(np.median(after)), 1)
    out = LEVELS
    lines = ["-- Generated by util/afx/sound_pipeline.py normalize, then tuned by hand:",
             "-- every sound group's gain in dB on top of its files, which sit at their",
             "-- category's reference loudness (bed -30, accent -22, foley -26, dry -24",
             "-- LUFS). This table is the mix: tune levels here, not in the code.",
             "return {"]
    for grp in sorted(levels):
        lines.append(f'\t["{grp}"] = {levels[grp]},')
    lines.append("}")
    if not dry:
        # keep hand-tuned entries from an earlier table
        old = {}
        if os.path.exists(out):
            for m in re.finditer(r'\["([^"]+)"\]\s*=\s*(-?[\d.]+)', open(out).read()):
                old[m.group(1)] = float(m.group(2))
        if old:
            print("levels.lua exists: entries kept, new groups added")
            for grp in levels:
                if grp in old:
                    levels[grp] = old[grp]
            lines = lines[:4] + [f'\t["{g}"] = {levels[g]},' for g in sorted(levels)] + ["}"]
        open(out, "w").write("\n".join(lines) + "\n")
        print("wrote", out)


def cmd_ir(args):
    """An impulse response: from the direct sound on, at most 2.9 s (the
    engine needs it in one buffer), the tail faded, the energy normalised so
    every space sends the same level and the slot gain decides."""
    src, name = args[0], args[1]
    n = int(2.9 * SR)

    def from_onset(c):
        a = np.abs(c)
        start = max(0, int(np.argmax(a > 0.1 * a.max())) - int(0.002 * SR))
        c = c[start:start + n]
        return np.pad(c, (0, n - len(c)))
    if "+" in src:
        # "left.wav+right.wav": two mono takes (a pop recorded at two mics),
        # each from its own onset
        x = np.stack([from_onset(load(p)) for p in src.split("+")[:2]], axis=1)
    else:
        x = load(src, 2)
        mono = np.abs(x).max(axis=1)
        start = max(0, int(np.argmax(mono > 0.1 * mono.max())) - int(0.002 * SR))
        x = x[start:start + n]
    # drop silence (or noise floor) past the decay: where it's 60 dB down
    e = (x ** 2).mean(axis=1)
    tail = np.cumsum(e[::-1])[::-1]
    end = int(np.argmax(tail < tail[0] * 1e-6)) or len(x)
    x = x[:max(end, int(0.3 * SR))]
    fade = int(0.4 * SR)
    if len(x) > fade:
        x[-fade:] *= np.linspace(1, 0, fade)[:, None] ** 2
    # drop the direct sound: the dry path already has it
    d = int(0.004 * SR)
    x[:d] *= np.linspace(0, 1, d)[:, None]
    e = np.sqrt((x ** 2).sum() / x.shape[1])
    x = x / (e + 1e-9) * 0.25
    peak = np.abs(x).max()
    if peak > 0.97:
        x *= 0.97 / peak
    save(os.path.join(SOUNDS, name + ".ogg"), x, quality=6)
    print(f"{name}: {len(x) / SR:.2f} s from {os.path.basename(src)}")


def best_loop(x, seconds, xfade=1.0):
    """The steadiest stretch of `seconds`, made seamless by crossfading its
    end into its start at the point where they match best."""
    n, xf = int(seconds * SR), int(xfade * SR)
    if len(x) < n + xf:
        n = len(x) - xf
    w = int(0.5 * SR)
    k = len(x) // w
    mono = x if x.ndim == 1 else x.mean(axis=1)
    r = 20 * np.log10(np.sqrt((mono[:k * w].reshape(k, w) ** 2).mean(1)) + 1e-9)
    span = (n + xf) // w
    best, best_s = 0, 1e9
    for i in range(0, max(1, k - span)):
        s = r[i:i + span].std()
        if s < best_s:
            best, best_s = i, s
    seg = x[best * w:best * w + n + xf].copy()
    t = np.linspace(0, 1, xf)
    if seg.ndim > 1:
        t = t[:, None]
    body = seg[:n].copy()
    body[:xf] = seg[:xf] * np.sqrt(t) + seg[n:n + xf] * np.sqrt(1 - t)
    return body


def takes(x, count, max_seconds):
    """Distinct hits: onsets where the level jumps, cut to the next onset or
    max_seconds, faded; then the ones that differ most from each other."""
    # library one-shots often start right on the hit: give it a lead-in
    x = np.concatenate([np.zeros(int(0.1 * SR), dtype=x.dtype), x])
    w = int(0.005 * SR)
    k = len(x) // w
    e = 20 * np.log10(np.sqrt((x[:k * w].reshape(k, w) ** 2).mean(1)) + 1e-9)
    on = [i for i in range(10, k - 1) if e[i] - np.median(e[max(0, i - 40):i]) > 12 and
          e[i] == e[max(0, i - 3):i + 4].max()]
    # one peak per hit: the first of onsets closer than 50 ms
    on = [o for j, o in enumerate(on) if j == 0 or o - on[j - 1] > 10]
    segs = []
    for i, o in enumerate(on):
        a = max(0, o * w - int(0.005 * SR))
        b = min(len(x), a + int(max_seconds * SR))
        if i + 1 < len(on):
            b = min(b, on[i + 1] * w - int(0.01 * SR))
        if b - a < int(0.08 * SR):
            continue
        s = x[a:b].copy()
        f = min(len(s) // 3, int(0.05 * SR))
        s[-f:] *= np.linspace(1, 0, f)
        segs.append(s)
    if not segs:
        return []

    def feat(s):
        spec = np.abs(np.fft.rfft(s, 4096))
        c = (spec * np.arange(len(spec))).sum() / (spec.sum() + 1e-9)
        return np.array([c / 100, len(s) / SR * 10, 20 * np.log10(np.abs(s).max() + 1e-9) / 6])
    fs = [feat(s) for s in segs]
    pick = [int(np.argmax([np.abs(s).max() for s in segs]))]
    while len(pick) < min(count, len(segs)):
        d = [min(np.linalg.norm(fs[i] - fs[j]) for j in pick) if i not in pick else -1
             for i in range(len(segs))]
        pick.append(int(np.argmax(d)))
    return [segs[i] for i in pick]


def cmd_import(args):
    pack, rules = args[0], json.load(open(args[1]))
    dry = "--dry-run" in args
    for r in rules:
        files = sorted(glob.glob(os.path.join(pack, r["glob"]), recursive=True))
        if not files:
            print(f"{r['name']}: nothing matches {r['glob']}")
            continue
        kind = r.get("kind", "accent")
        target = TARGET[kind]
        outs, srcs = [], []
        if kind == "bed":
            # beds stay stereo: the engine steers them toward the opening
            x = load(files[0], r.get("channels", 2))
            outs, srcs = [best_loop(x, r.get("seconds", 20))], [files[0]]
        else:
            for f in files:
                got = takes(load(f), r.get("takes", 4) - len(outs), r.get("max_seconds", 1.5))
                outs += got
                srcs += [f] * len(got)
                if len(outs) >= r.get("takes", 4):
                    break
        for i, (y, src) in enumerate(zip(outs, srcs), 1):
            y = y * 10 ** ((target - loudness(y)) / 20)
            y *= min(1.0, 0.97 / (np.abs(y).max() + 1e-9))
            name = f"{r['name']}.ogg" if len(outs) == 1 else f"{r['name']}.{i}.ogg"
            print(f"{name:34} from {os.path.basename(src)}")
            if not dry:
                save(os.path.join(SOUNDS, name), y)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    {"report": cmd_report, "normalize": cmd_normalize, "ir": cmd_ir,
     "import": cmd_import}[sys.argv[1]](sys.argv[2:])
