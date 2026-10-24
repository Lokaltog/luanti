#!/usr/bin/env python3
"""Build the afx_ambience sound set from real recordings.

    import_sounds.py <assets_dir> <out_sounds_dir>

<assets_dir> holds three shallow clones (all on GitHub):
    moodist/        github.com/remvze/moodist          (public/sounds, CC0 / Pixabay)
    vl/             github.com/VoxeLibre/VoxeLibre      (mods/**/sounds)
    ambient_sounds/ github.com/n0RE5/ambient_sounds     (sounds, CC0 / CC-BY)

Loops are cut to a length, made seamless with an equal-power crossfade of
the tail into the head, and loudness-normalised per role. Positional sounds
are folded to mono (OpenAL only spatialises mono). Needs ffmpeg and numpy.

Naming follows the afx buses: afx_out_* are outdoor beds (muffled indoors,
steered toward openings); everything else is played positionally.
"""
import os
import subprocess
import sys

import numpy as np

RATE = 44100


def decode(path, mono=False):
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ar", str(RATE),
           "-ac", "1" if mono else "2", "-"]
    raw = subprocess.run(cmd, check=True, capture_output=True).stdout
    x = np.frombuffer(raw, dtype=np.float32).astype(np.float64)
    return x.reshape(-1, 1 if mono else 2)


def encode(x, path, lowpass=None, highpass=None, rate_scale=None):
    filters = []
    if rate_scale:
        # slow it down (pitch and tempo together), like a tape
        filters.append(f"asetrate={int(RATE * rate_scale)},aresample={RATE}")
    if highpass:
        filters.append(f"highpass=f={highpass}")
    if lowpass:
        filters.append(f"lowpass=f={lowpass}:poles=2")
    ch = x.shape[1]
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", str(RATE), "-ac", str(ch),
           "-i", "-"]
    if filters:
        cmd += ["-af", ",".join(filters)]
    cmd += ["-c:a", "libvorbis", "-q:a", "5", path]
    subprocess.run(cmd, input=x.astype(np.float32).tobytes(), check=True)


def rms_db(x):
    return 10 * np.log10(np.mean(x ** 2) + 1e-12)


def normalise(x, target_db):
    """RMS normalisation (a loudness proxy that behaves on noise beds), with a
    peak ceiling so transients never clip."""
    g = 10 ** ((target_db - rms_db(x)) / 20)
    peak = np.max(np.abs(x)) * g
    if peak > 0.95:
        g *= 0.95 / peak
    return x * g


def seamless(x, seconds, start=0.0, xfade=1.5):
    """Cut [start, start+seconds+xfade) and fold the tail onto the head."""
    a = int(start * RATE)
    n = int(seconds * RATE)
    f = int(xfade * RATE)
    seg = x[a:a + n + f]
    if len(seg) < n + f:
        raise ValueError("source too short")
    out = seg[:n].copy()
    t = np.linspace(0, np.pi / 2, f)[:, None]
    out[:f] = seg[:f] * np.sin(t) + seg[n:n + f] * np.cos(t)
    return out


def onsets(x, n, length, min_gap=0.6, pre=0.01):
    """Pick the n loudest separated events (cave drips) as one-shots."""
    m = np.abs(x[:, 0])
    win = int(0.005 * RATE)
    env = np.convolve(m, np.ones(win) / win, mode="same")
    order = np.argsort(env)[::-1]
    picks = []
    for i in order:
        if all(abs(i - p) > min_gap * RATE for p in picks):
            picks.append(i)
            if len(picks) == n:
                break
    out = []
    for p in sorted(picks):
        a = max(0, p - int(pre * RATE))
        seg = x[a:a + int(length * RATE)].copy()
        fade = np.linspace(1, 0, len(seg))[:, None] ** 2
        out.append(seg * fade)
    return out


def main(assets, out):
    os.makedirs(out, exist_ok=True)
    md = os.path.join(assets, "moodist/public/sounds")
    vl = os.path.join(assets, "vl/mods")
    am = os.path.join(assets, "ambient_sounds/sounds")
    made = []

    def bed(name, src, seconds, start=0.0, db=-24.0, **kw):
        x = seamless(decode(src), seconds, start)
        encode(normalise(x, db), os.path.join(out, name + ".ogg"), **kw)
        made.append(name)

    def emitter(name, src, seconds, start=0.0, db=-20.0, xfade=1.5, **kw):
        x = seamless(decode(src, mono=True), seconds, start, xfade)
        encode(normalise(x, db), os.path.join(out, name + ".ogg"), **kw)
        made.append(name)

    def oneshot(name, src, db=-20.0, trim=None, start=0.0, stereo=False, **kw):
        x = decode(src, mono=not stereo)
        x = x[int(start * RATE):]
        if trim:
            x = x[:int(trim * RATE)].copy()
            x[:int(0.3 * RATE)] *= np.linspace(0, 1, int(0.3 * RATE))[:, None]
            x[-int(0.05 * RATE):] *= np.linspace(1, 0, int(0.05 * RATE))[:, None]
        encode(normalise(x, db), os.path.join(out, name + ".ogg"), **kw)
        made.append(name)

    # Outdoor beds (stereo, non-positional)
    bed("afx_out_rain", f"{md}/rain/light-rain.mp3", 40, start=10, db=-24)
    bed("afx_out_rain_heavy", f"{md}/rain/heavy-rain.mp3", 18, start=1, db=-22)
    bed("afx_out_wind", f"{md}/nature/wind.mp3", 40, start=5, db=-26)
    bed("afx_out_wind_trees", f"{md}/nature/wind-in-trees.mp3", 40, start=5, db=-26)
    bed("afx_out_wind_howl", f"{md}/nature/howling-wind.mp3", 40, start=5, db=-27)
    bed("afx_out_crickets", f"{md}/animals/crickets.mp3", 40, start=20, db=-28)
    bed("afx_out_birds_far", f"{md}/animals/birds.mp3", 40, start=10, db=-30)

    # Rain hitting surfaces (mono, positional loops at the surface)
    emitter("afx_rain_glass", f"{md}/rain/rain-on-window.mp3", 24, start=2, db=-22)
    emitter("afx_rain_wood", f"{md}/rain/rain-on-tent.mp3", 30, start=10, db=-22, lowpass=4000)
    emitter("afx_rain_leaves", f"{md}/rain/rain-on-leaves.mp3", 30, start=5, db=-24)
    emitter("afx_rain_metal", f"{md}/rain/rain-on-car-roof.mp3", 8, start=0.5, db=-24)

    # Lava, fire, water (mono, positional)
    emitter("afx_lava", f"{vl}/CORE/vl_env_sounds/sounds/env_sounds_lava.1.ogg", 6, db=-22)
    emitter("afx_lava_bubble", f"{md}/things/boiling-water.mp3", 14, start=2, db=-24, lowpass=2500)
    emitter("afx_fire", f"{md}/nature/campfire.mp3", 30, start=10, db=-24)

    # Cave drips: single drops cut from a drip recording
    drops = onsets(decode(f"{md}/nature/droplets.mp3", mono=True), 6, 0.6)
    for i, d in enumerate(drops):
        name = f"afx_drip.{i + 1}"
        encode(normalise(d, -24), os.path.join(out, name + ".ogg"))
        made.append(name)

    # Birds: short single calls (mono, positional one-shots), as sound groups
    birds = [
        ("afx_bird_robin.1", "ambient_sounds_robin.ogg"),
        ("afx_bird_cardinal.1", "ambient_sounds_cardinal.ogg"),
        ("afx_bird_lark.1", "ambient_sounds_crestedlark.ogg"),
        ("afx_bird_crow.1", "ambient_sounds_crow_1.ogg"),
        ("afx_bird_crow.2", "ambient_sounds_crow_2.ogg"),
        ("afx_bird_owl.1", "ambient_sounds_owl_1.ogg"),
        ("afx_bird_owl.2", "ambient_sounds_owl_2.ogg"),
    ]
    for name, f in birds:
        oneshot(name, f"{am}/{f}", db=-22)

    # Thunder and a cave bed for later milestones
    oneshot("afx_thunder.1", f"{vl}/ENVIRONMENT/lightning/sounds/lightning_thunder.2.ogg", db=-16)
    bed("afx_in_cave", f"{am}/ambient_sounds_cave_large_2.ogg", 20, start=2, db=-34)

    # Biome beds
    bed("afx_out_waves", f"{md}/nature/waves.mp3", 40, start=10, db=-26)
    bed("afx_out_jungle", f"{md}/nature/jungle.mp3", 40, start=30, db=-28)
    bed("afx_out_frogs", f"{md}/animals/frog.mp3", 40, start=10, db=-30)

    # Proximity loops (mono, positional)
    emitter("afx_river", f"{md}/nature/river.mp3", 30, start=10, db=-24)
    emitter("afx_waterfall", f"{md}/nature/waterfall.mp3", 18, start=2, db=-22)
    emitter("afx_bees", f"{md}/animals/beehive.mp3", 30, start=5, db=-26)
    emitter("afx_insects", f"{md}/animals/beehive.mp3", 30, start=12, db=-30, highpass=300)

    # Under water (non-positional, indoor bus: never muffled further)
    bed("afx_in_underwater", f"{md}/places/underwater.mp3", 30, start=5, db=-26)

    # Mood layers, played by the engine from its tension/cozy scalars
    bed("afx_mood_dread", f"{am}/ambient_sounds_cave_3.ogg", 40, start=5, db=-30,
        rate_scale=0.8, lowpass=1500)
    bed("afx_mood_cozy", f"{md}/nature/campfire.mp3", 30, start=40, db=-32, lowpass=1800)
    oneshot("afx_mood_relief", f"{md}/things/wind-chimes.mp3", start=12, trim=7, db=-28,
            stereo=True)
    for i in range(1, 6):
        oneshot(f"afx_mood_stinger.{i}", f"{vl}/PLAYER/vl_cavesounds/sounds/cave{i}.ogg", db=-24)

    # ---- Round 4 ----
    an = os.path.join(assets, "animalia/sounds")
    mtg = os.path.join(assets, "minetest_game/mods")

    def segment(name, src, start, length, db, mono=True, attack=0.0, **kw):
        x = decode(src, mono=mono)[int(start * RATE):int((start + length) * RATE)].copy()
        if attack > 0:
            n = int(attack * RATE)
            x[:n] *= np.linspace(0, 1, n)[:, None] ** 2
        tail = int(min(1.5, length / 4) * RATE)
        x[-tail:] *= np.linspace(1, 0, tail)[:, None] ** 2
        encode(normalise(x, db), os.path.join(out, name + ".ogg"), **kw)
        made.append(name)

    # Thunder: three claps in one recording. Near = the crack as recorded;
    # far = the rumble, low-passed and with a rounded attack.
    th = f"{md}/rain/thunder.mp3"
    for i, st in enumerate([0.3, 20.7, 41.8]):
        segment(f"afx_thunder_near.{i + 1}", th, st, 12, -16)
    for i, st in enumerate([21.4, 42.6]):
        segment(f"afx_thunder_far.{i + 1}", th, st, 16, -20, attack=0.8, lowpass=500)

    # Water's edge, after the rain
    emitter("afx_shore", f"{md}/nature/waves.mp3", 30, start=40, db=-24, lowpass=3000)
    emitter("afx_drip_after", f"{md}/nature/droplets.mp3", 30, start=12, db=-28)
    emitter("afx_gutter", f"{md}/nature/river.mp3", 20, start=50, db=-30, highpass=600)
    bed("afx_out_gulls", f"{md}/animals/seagulls.mp3", 35, start=3, db=-32)

    # Reactions: leaf rustle bursts, bird alarm calls, frogs, a plop
    for i, st in enumerate([1.0, 6.5, 12.0]):
        segment(f"afx_leaf_rustle.{i + 1}", f"{md}/nature/walk-on-leaves.mp3", st, 1.2, -22)
    for sp in ["bluebird", "goldfinch", "cardinal"]:
        for i in (1, 2, 3):
            oneshot(f"afx_bird_{sp}.{i}", f"{an}/{sp}/animalia_{sp}.{i}.ogg", db=-22)
    for i in (1, 2, 3):
        oneshot(f"afx_frog.{i}", f"{an}/frog/animalia_frog_{i}.ogg", db=-24)
    oneshot("afx_plop", f"{vl}/ITEMS/mcl_fishing/sounds/watersplash.ogg", db=-26,
            lowpass=2200)

    # Home: furnace, a clock, a cat
    emitter("afx_furnace", f"{mtg}/default/sounds/default_furnace_active.ogg", 8, db=-28,
            lowpass=3000)
    # A tick must not double up in the loop crossfade: cut on a whole number
    # of tick periods with a very short splice
    clk = decode(f"{md}/things/clock.mp3", mono=True)[:, 0]
    env = np.abs(clk)
    ac = np.correlate(env[:RATE * 6] - env[:RATE * 6].mean(),
                      env[:RATE * 6] - env[:RATE * 6].mean(), "full")[RATE * 6 - 1:]
    period = (np.argmax(ac[int(0.3 * RATE):int(1.5 * RATE)]) + int(0.3 * RATE)) / RATE
    emitter("afx_clock", f"{md}/things/clock.mp3", period * round(10 / period), start=1,
            db=-30, xfade=0.03)
    emitter("afx_purr", f"{md}/animals/cat-purring.mp3", 24, start=4, db=-26)

    # Villages
    bed("afx_out_village_night", f"{md}/places/night-village.mp3", 40, start=20, db=-32)
    bed("afx_out_village_day", f"{md}/animals/chickens.mp3", 40, start=30, db=-34,
        lowpass=4000)

    # Structures: a door's creak slowed into old timbers; mail rings as chains
    door = f"{vl}/ITEMS/mcl_doors/sounds/doors_door_open.ogg"
    for i, r in enumerate([0.45, 0.55, 0.38]):
        oneshot(f"afx_creak.{i + 1}", door, db=-24, rate_scale=r, lowpass=2500)
    chain = f"{vl}/ITEMS/mcl_armor/sounds/mcl_armor_equip_chainmail.ogg"
    for i, r in enumerate([0.8, 0.65]):
        oneshot(f"afx_chains.{i + 1}", chain, db=-26, rate_scale=r)

    # Other worlds
    bed("afx_in_nether", f"{am}/ambient_sounds_ethereal_fiery_main.ogg", 24, start=1, db=-26)
    bed("afx_in_end", f"{md}/nature/howling-wind.mp3", 40, start=10, db=-32,
        rate_scale=0.7, lowpass=900)
    for i, r in enumerate([0.5, 0.42]):
        oneshot(f"afx_end_chime.{i + 1}", f"{am}/ambient_sounds_ethereal_crystal_2.ogg",
                db=-28, rate_scale=r)

    # Drafts: the whistling band of a howling wind, for gaps in walls
    emitter("afx_draft", f"{md}/nature/howling-wind.mp3", 30, start=15, db=-26,
            highpass=650, lowpass=1700)

    print(f"wrote {len(made)} sounds to {out}")
    for m in made:
        print("  " + m)


if __name__ == "__main__":
    main(*sys.argv[1:3])
