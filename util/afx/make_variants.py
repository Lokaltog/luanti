#!/usr/bin/env python3
"""Extra takes of a game's footstep, dig and place sounds.

    make_variants.py <game_dir> <out_sounds_dir> [--takes 3]

Luanti groups "name.N.ogg" files into one sound group across all mods, so a
mod can add members to a game's groups without touching the game. For each
group matching PATTERNS this writes new takes into the free slots (.9, .8,
... down), each made from one of the game's own samples with:

  - pitch shifted (asetrate) and speed shifted (atempo) independently,
    a few percent each way, so steps differ in both timbre and length
  - a gentle tilt of the tone (high shelf +/- 2 dB) and level (+/- 1 dB)

Run it against the game you play (Mineclonia, VoxeLibre, MTG) so the takes
match that game's own sounds. Needs ffmpeg.
"""
import argparse
import os
import random
import re
import subprocess
import sys
from collections import defaultdict

PATTERNS = [
    r"_footstep$", r"^default_dig_", r"^default_dug_", r"^default_place_node",
    r"_dig$", r"_dug$", r"^mcl_sounds_place", r"^mcl_sounds_dug",
]

NAME = re.compile(r"^(?P<group>.+?)(?:\.(?P<n>\d))?\.ogg$")


def groups_in(game_dir):
    groups = defaultdict(dict)  # group -> {index or None: path}
    for root, _, files in os.walk(game_dir):
        if os.sep + "sounds" not in root + os.sep:
            continue
        for f in files:
            m = NAME.match(f)
            if not m:
                continue
            g = m.group("group")
            n = m.group("n")
            groups[g][int(n) if n is not None else None] = os.path.join(root, f)
    return groups


def make_take(src, dst, rng):
    pitch = 2 ** (rng.uniform(-90, 90) / 1200)    # up to ~0.9 semitone
    tempo = rng.uniform(0.92, 1.08)                # speed, independent of pitch
    tilt = rng.uniform(-2.0, 2.0)                  # dB above ~3 kHz
    gain = rng.uniform(-1.0, 1.0)                  # dB
    probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=sample_rate",
                            "-of", "csv=p=0", src], capture_output=True, text=True)
    sr = int((probe.stdout.strip().splitlines() or ["44100"])[0])
    af = (f"asetrate={int(sr * pitch)},aresample={sr},atempo={tempo:.4f},"
          f"highshelf=f=3000:g={tilt:.2f},volume={gain:.2f}dB")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-af", af,
                    "-c:a", "libvorbis", "-q:a", "4", dst], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--takes", type=int, default=3)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    os.makedirs(a.out_dir, exist_ok=True)

    made = 0
    for g, members in sorted(groups_in(a.game_dir).items()):
        if not any(re.search(p, g) for p in PATTERNS):
            continue
        used = {n for n in members if n is not None}
        free = [n for n in range(9, -1, -1) if n not in used]
        # a bare "name.ogg" can't share a group with numbered extras safely
        # in every engine version; give it numbered companions anyway
        sources = list(members.values())
        for slot in free[:a.takes]:
            dst = os.path.join(a.out_dir, f"{g}.{slot}.ogg")
            make_take(rng.choice(sources), dst, rng)
            made += 1
    print(f"wrote {made} takes to {a.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
