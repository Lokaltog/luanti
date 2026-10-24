#!/usr/bin/env python3
"""fx renderer test harness.

    util/fx/test/run.py                    # every case, headless (Xvfb + llvmpipe)
    util/fx/test/run.py --gpu              # on your display and GPU (4090 goldens)
    util/fx/test/run.py --only 'night|day' # a subset (regex on case names)
    util/fx/test/run.py --update-goldens   # accept the current images as references
    util/fx/test/run.py dump [game]        # write <game>'s node dump to util/fx/test/data/

Each case starts the client in fx test mode (src/client/fx/fx_test.h): fixed
1/60 s steps, capture once the scene mod reports ready and meshing has gone
idle. A case fails on a shader/GL/Lua error, a missing capture, a failed probe
(NaN/Inf pixels, black frames, region brightness or hue), a golden mismatch, or
a failed comparison with another case. Results: <out>/report.html.
"""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
# what the cases run from: a snapshot of ROOT, so editing shaders or
# rebuilding while a run is in progress cannot change the results
RUN_ROOT = ROOT
sys.path.insert(0, str(HERE))
from cases import CASES, WORLDS  # noqa: E402

ERROR_PATTERNS = re.compile(
    r"failed to compile|Failed to compile|failed to link|GL_INVALID|FBO incomplete|ShaderException|"
    r"Segmentation|terminate called|Runtime error|ModError|AddressSanitizer|"
    r"ERROR\[Main\]: fx|ERROR\[Main\]: Irrlicht"
)

BASE_CONF = {
    "video_driver": "opengl3",
    "screen_w": "960",
    "screen_h": "540",
    "enable_post_processing": "true",
    "enable_bloom": "true",
    "enable_auto_exposure": "false",
    "enable_sound": "false",
    "enable_ipv6": "false",
    "ipv6_server": "false",
    "opengl_debug": "true",
    "debug_log_level": "info",
    "server_announce": "false",
    "mute_sound": "true",
    "show_welcome_message": "false",
    "enable_damage": "false",  # a long wait under water must not drown the player
    # the scenes fit in +-24 nodes; a short send range lets meshing go idle fast
    "viewing_range": "80",
    "max_block_send_distance": "4",
    "max_block_generate_distance": "4",
    "block_send_optimize_distance": "2",
}


# ----------------------------------------------------------------- images

def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float32) / 255.0


def luminance(rgb: np.ndarray) -> np.ndarray:
    return rgb @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def crop(img: np.ndarray, region) -> np.ndarray:
    if region is None:
        return img
    h, w = img.shape[:2]
    x0, y0, x1, y1 = region
    return img[int(y0 * h):max(int(y1 * h), int(y0 * h) + 1), int(x0 * w):max(int(x1 * w), int(x0 * w) + 1)]


def hue_deg(rgb: np.ndarray) -> float:
    """Hue of the mean colour, in degrees."""
    r, g, b = rgb.reshape(-1, 3).mean(0)
    mx, mn = max(r, g, b), min(r, g, b)
    if mx - mn < 1e-4:
        return float("nan")
    if mx == r:
        h = (g - b) / (mx - mn) % 6
    elif mx == g:
        h = (b - r) / (mx - mn) + 2
    else:
        h = (r - g) / (mx - mn) + 4
    return float(h * 60)


def saturation(rgb: np.ndarray) -> float:
    r, g, b = rgb.reshape(-1, 3).mean(0)
    mx = max(r, g, b)
    return float((mx - min(r, g, b)) / mx) if mx > 1e-4 else 0.0


def image_stats(img: np.ndarray) -> dict:
    lum = luminance(img)
    raw = (img * 255).round().astype(np.int32)
    magenta = int(((raw[..., 0] == 255) & (raw[..., 1] == 0) & (raw[..., 2] == 255)).sum())
    return {
        "mean_lum": float(lum.mean()),
        "black_frac": float((lum < 0.02).mean()),
        "white_frac": float((lum > 0.98).mean()),
        "nan_pixels": magenta,
    }


GOLDEN_SIZE = (480, 270)  # goldens are stored and compared at this size


def downscale(path: Path) -> np.ndarray:
    im = Image.open(path).convert("RGB")
    if im.size != GOLDEN_SIZE:
        im = im.resize(GOLDEN_SIZE, Image.BOX)
    return np.asarray(im).astype(np.float32) / 255.0


def compare(a: np.ndarray, b: np.ndarray, region=None) -> dict:
    a, b = crop(a, region), crop(b, region)
    if a.shape != b.shape:
        return {"mean": 255.0, "frac": 1.0, "shape": f"{a.shape} vs {b.shape}"}
    d = np.abs(a - b)
    return {"mean": float(d.mean() * 255), "frac": float((d.max(-1) > 40 / 255).mean())}


# ----------------------------------------------------------------- probes

def run_probe(probe: dict, img: np.ndarray) -> tuple[bool, str]:
    """A probe is a dict: optional region [x0,y0,x1,y1] (fractions), and checks."""
    name = probe.get("name", "probe")
    part = crop(img, probe.get("region"))
    msgs, ok = [], True
    lum = float(luminance(part).mean())
    if "lum" in probe:
        lo, hi = probe["lum"]
        good = lo <= lum <= hi
        ok &= good
        msgs.append(f"lum {lum:.3f} {'in' if good else 'NOT in'} [{lo}, {hi}]")
    if "hue" in probe:
        target, tol = probe["hue"]
        h = hue_deg(part)
        dist = abs((h - target + 180) % 360 - 180) if h == h else 999
        good = dist <= tol
        ok &= good
        msgs.append(f"hue {h:.0f}° {'within' if good else 'NOT within'} {tol}° of {target}°")
    if "sat_min" in probe:
        s = saturation(part)
        good = s >= probe["sat_min"]
        ok &= good
        msgs.append(f"saturation {s:.2f} {'>=' if good else '<'} {probe['sat_min']}")
    if "sat_max" in probe:
        s = saturation(part)
        good = s <= probe["sat_max"]
        ok &= good
        msgs.append(f"saturation {s:.2f} {'<=' if good else '>'} {probe['sat_max']}")
    return ok, f"{name}: " + ", ".join(msgs)


# ----------------------------------------------------------------- running

@dataclass
class Result:
    name: str
    ok: bool = True
    problems: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    image: Path | None = None
    golden: Path | None = None
    diff: Path | None = None
    stats: dict = field(default_factory=dict)
    seconds: float = 0.0

    def fail(self, msg: str):
        self.ok = False
        self.problems.append(msg)

    def check(self, ok: bool, msg: str, xfail: str | None = None):
        """xfail = the phase/finding that will fix a known failure."""
        if xfail and not ok:
            self.notes.append(f"known failure ({xfail}): {msg}")
        elif xfail and ok:
            self.fail(f"passes now, remove xfail ({xfail}): {msg}")
        elif ok:
            self.notes.append(msg)
        else:
            self.fail(msg)


def detect_backend(log_text: str, gpu: bool) -> str:
    if not gpu or "llvmpipe" in log_text:
        return "llvmpipe"
    return "gpu"


def make_snapshot() -> Path:
    """Copy what a run reads (binary, shaders, builtin, data) to a temp root."""
    snap = Path(tempfile.mkdtemp(prefix="fxsnap-"))
    (snap / "bin").mkdir()
    shutil.copy2(ROOT / "bin/luanti", snap / "bin/luanti")
    for d in ("builtin", "client", "fonts", "textures", "util/fx"):
        if (ROOT / d).exists():
            shutil.copytree(ROOT / d, snap / d, symlinks=True,
                            ignore=shutil.ignore_patterns("golden", "__pycache__"))
    (snap / "games").mkdir()
    for g in (ROOT / "games").iterdir():
        if g.name == "devtest":
            shutil.copytree(g, snap / "games" / g.name, symlinks=True)
        elif g.is_dir():
            (snap / "games" / g.name).symlink_to(g.resolve())
    return snap


def prepare_world(case: dict, work: Path) -> Path:
    world_def = WORLDS[case.get("world", "fxscene")]
    world = work / "world"
    shutil.copytree(RUN_ROOT / world_def["path"], world, symlinks=True)
    wm = world / "worldmods"
    wm.mkdir(exist_ok=True)
    for mod in world_def.get("mods", []) + case.get("mods", []):
        shutil.copytree(RUN_ROOT / "util/fx/mods" / mod, wm / mod)
    return world


def run_case(case: dict, out: Path, args, env: dict) -> Result:
    res = Result(case["name"])
    t0 = time.time()
    with tempfile.TemporaryDirectory(prefix="fxtest-") as td:
        work = Path(td)
        world = prepare_world(case, work)
        shot = out / f"{case['name']}.png"
        for p in (shot, Path(str(shot) + ".json")):
            p.unlink(missing_ok=True)
        conf = dict(BASE_CONF)
        conf.update(WORLDS[case.get("world", "fxscene")].get("settings", {}))
        conf.update({k: str(v).replace("@ROOT@", str(RUN_ROOT)) for k, v in case.get("settings", {}).items()})
        for kv in args.set or []:  # --set overrides, for one-off experiments
            k, _, v = kv.partition("=")
            conf[k.strip()] = v.strip()
        conf["fx_test_capture"] = str(shot)
        conf_path = work / "client.conf"
        conf_path.write_text("".join(f"{k} = {v}\n" for k, v in conf.items()))
        log = work / "debug.txt"
        cmd = [str(RUN_ROOT / "bin/luanti"), "--config", str(conf_path), "--logfile", str(log),
               "--go", "--world", str(world), "--name", "tester", "--password", "",
               # parallel runs (another --display) need their own server port
               "--port", str(30000 + int(args.display))]
        try:
            proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=case.get("timeout", args.timeout))
            code, stdout = proc.returncode, proc.stdout.decode(errors="replace")
        except subprocess.TimeoutExpired as e:
            code, stdout = "timeout", (e.stdout or b"").decode(errors="replace")
        log_text = log.read_text(errors="replace") if log.exists() else ""
        shutil.copy(log, out / f"{case['name']}.log") if log.exists() else None
        res.seconds = time.time() - t0

        errors = sorted({m.group(0) + ": " + line.strip()[:160]
                         for line in (log_text + stdout).splitlines()
                         for m in [ERROR_PATTERNS.search(line)] if m})
        for e in errors[:6]:
            res.fail(e)
        for pat in case.get("log_must", []):
            if not re.search(pat, log_text):
                res.fail(f"log is missing /{pat}/")
        if code not in (0,):
            res.fail(f"exit code {code}")
        if not shot.exists():
            res.fail("no capture")
            return res
        res.image = shot
        stats_path = Path(str(shot) + ".json")
        if stats_path.exists():
            res.stats.update(json.loads(stats_path.read_text()))

        img = load_rgb(shot)
        st = image_stats(img)
        res.stats.update(st)
        if st["nan_pixels"] > case.get("max_nan_pixels", 0):
            res.fail(f"{st['nan_pixels']} NaN/Inf pixels (magenta)")
        if st["black_frac"] > case.get("max_black", 0.6):
            res.fail(f"{st['black_frac']:.0%} of the frame is black")
        if st["white_frac"] > case.get("max_white", 0.3):
            res.fail(f"{st['white_frac']:.0%} of the frame is white")
        for probe in case.get("probes", []):
            ok, msg = run_probe(probe, img)
            res.check(ok, msg, probe.get("xfail"))

        backend = detect_backend(log_text, args.gpu)
        if case.get("golden", True):
            golden = HERE / "golden" / backend / f"{case['name']}.png"
            res.golden = golden
            small = downscale(shot)
            if args.update_goldens:
                golden.parent.mkdir(parents=True, exist_ok=True)
                Image.fromarray((small * 255).round().astype(np.uint8)).save(golden, optimize=True)
                res.notes.append(f"golden updated ({backend})")
            elif golden.exists():
                tol = {"mean": 3.0, "frac": 0.03}
                tol.update(case.get("golden_tolerance", {}).get(backend, {}))
                ref = load_rgb(golden)
                c = compare(small, ref)
                res.stats["golden"] = c
                diff = np.abs(small - ref) if "shape" not in c else None
                if diff is not None:
                    res.diff = out / f"{case['name']}.diff.png"
                    Image.fromarray(np.clip(diff * 4 * 255, 0, 255).astype(np.uint8)).save(res.diff)
                if c["mean"] > tol["mean"] or c["frac"] > tol["frac"]:
                    res.fail(f"golden mismatch ({backend}): mean {c['mean']:.2f} > {tol['mean']} "
                             f"or {c['frac']:.1%} pixels > {tol['frac']:.0%}")
                else:
                    res.notes.append(f"golden ok: mean {c['mean']:.2f}, {c['frac']:.1%} pixels")
            else:
                res.notes.append(f"no {backend} golden yet (run with --update-goldens)")
    return res


def check_pairs(results: dict[str, Result]):
    for case in CASES:
        res = results.get(case["name"])
        if not res or not res.image:
            continue
        img = load_rgb(res.image)
        for pair in case.get("compare", []):
            other = results.get(pair["with"])
            if not other or not other.image:
                res.notes.append(f"compare with {pair['with']}: skipped (not run)")
                continue
            c = compare(img, load_rgb(other.image), pair.get("region"))
            label = pair.get("name", f"vs {pair['with']}")
            if "min_mean" in pair and c["mean"] < pair["min_mean"]:
                res.check(False, f"{label}: mean diff {c['mean']:.2f} < {pair['min_mean']} "
                          "(expected a visible change)", pair.get("xfail"))
            elif "max_mean" in pair and c["mean"] > pair["max_mean"]:
                res.check(False, f"{label}: mean diff {c['mean']:.2f} > {pair['max_mean']} "
                          "(expected no change)", pair.get("xfail"))
            else:
                res.check(True, f"{label}: mean diff {c['mean']:.2f}", pair.get("xfail"))


def write_report(results: list[Result], out: Path):
    rows = []
    for r in results:
        def img(p):
            return f"<a href='{html.escape(os.path.relpath(p, out))}'><img src='{html.escape(os.path.relpath(p, out))}'></a>" if p and Path(p).exists() else ""
        stats = ", ".join(f"{k} {v:.3g}" if isinstance(v, float) else f"{k} {v}"
                          for k, v in r.stats.items() if not isinstance(v, dict))
        rows.append(
            f"<tr class='{'ok' if r.ok else 'fail'}'><td><b>{html.escape(r.name)}</b><br>{'ok' if r.ok else 'FAIL'}"
            f"<br>{r.seconds:.0f} s</td><td>{img(r.image)}</td><td>{img(r.golden)}</td><td>{img(r.diff)}</td>"
            f"<td><ul>{''.join(f'<li class=p>{html.escape(p)}</li>' for p in r.problems)}"
            f"{''.join(f'<li>{html.escape(n)}</li>' for n in r.notes)}</ul><small>{html.escape(stats)}</small></td></tr>")
    (out / "report.html").write_text(
        "<!doctype html><meta charset=utf-8><title>fx test report</title><style>"
        "body{font:13px system-ui;margin:16px}img{width:320px}td{vertical-align:top;padding:6px;border-bottom:1px solid #ccc}"
        "tr.fail td:first-child{color:#b00}li.p{color:#b00}</style>"
        "<table><tr><th>case</th><th>capture</th><th>golden</th><th>diff ×4</th><th>checks</th></tr>"
        + "".join(rows) + "</table>")


def start_xvfb(display: str):
    if not shutil.which("Xvfb"):
        sys.exit("Xvfb not found (it is in the devenv shell); or run with --gpu")
    proc = subprocess.Popen(["Xvfb", display, "-screen", "0", "960x540x24", "-nolisten", "tcp"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        if Path(f"/tmp/.X11-unix/X{display[1:]}").exists():
            return proc
        time.sleep(0.1)
    proc.kill()
    sys.exit(f"Xvfb did not start on {display}")


def make_env(args) -> tuple[dict, subprocess.Popen | None]:
    env = dict(os.environ)
    xvfb = None
    if not args.gpu:
        display = f":{args.display}"
        xvfb = start_xvfb(display)
        env.update(DISPLAY=display, LIBGL_ALWAYS_SOFTWARE="1")
    return env, xvfb


def cmd_dump(args):
    env, xvfb = make_env(args)
    try:
        game = args.game
        world_def = {"path": None}
        with tempfile.TemporaryDirectory(prefix="fxdump-") as td:
            world = Path(td) / "world"
            (world / "worldmods").mkdir(parents=True)
            (world / "world.mt").write_text(f"gameid = {game}\nbackend = sqlite3\nmg_name = singlenode\n")
            shutil.copytree(ROOT / "util/fx/mods/fx_nodedump", world / "worldmods/fx_nodedump")
            conf = Path(td) / "client.conf"
            conf.write_text("".join(f"{k} = {v}\n" for k, v in BASE_CONF.items()) + "fx_nodedump_quit = true\n")
            log = Path(td) / "debug.txt"
            subprocess.run([str(ROOT / "bin/luanti"), "--config", str(conf), "--logfile", str(log), "--go",
                            "--world", str(world), "--name", "tester", "--password", ""],
                           env=env, timeout=600, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            src = world / "fx_nodes.json"
            if not src.exists():
                print(log.read_text(errors="replace")[-3000:])
                sys.exit("no dump written")
            dst = HERE / "data" / f"{game}_nodes.json"
            dst.parent.mkdir(exist_ok=True)
            shutil.copy(src, dst)
            n = len(json.loads(src.read_text())["nodes"])
            print(f"{n} nodes -> {dst.relative_to(ROOT)}")
    finally:
        if xvfb:
            xvfb.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="?", default="run", choices=["run", "dump", "list"])
    ap.add_argument("game", nargs="?", default="mineclonia")
    ap.add_argument("--only", help="regex on case names")
    ap.add_argument("--gpu", action="store_true", help="use the real display and GPU (no Xvfb)")
    ap.add_argument("--display", default=os.environ.get("XVFB_DISPLAY", "97"))
    ap.add_argument("--update-goldens", action="store_true")
    ap.add_argument("--set", action="append", metavar="KEY=VALUE",
                    help="extra client setting for every case (experiments; use with --out)")
    ap.add_argument("--out", default=str(ROOT / "shots/test"))
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--no-snapshot", action="store_true",
                    help="run straight from the checkout (edits during the run affect it)")
    args = ap.parse_args()

    if args.command == "list":
        for c in CASES:
            print(f"{c['name']:24s} {c.get('world', 'fxscene'):10s} {c.get('doc', '')}")
        return
    if args.command == "dump":
        return cmd_dump(args)

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cases = [c for c in CASES if not args.only or re.search(args.only, c["name"])]
    # cases another case compares against run too
    wanted = {c["name"] for c in cases}
    for c in list(cases):
        for p in c.get("compare", []):
            if p["with"] not in wanted:
                wanted.add(p["with"])
                cases.append(next(x for x in CASES if x["name"] == p["with"]))
    global RUN_ROOT
    if not args.no_snapshot:
        RUN_ROOT = make_snapshot()
    env, xvfb = make_env(args)
    results: dict[str, Result] = {}
    try:
        for case in cases:
            r = run_case(case, out, args, env)
            results[case["name"]] = r
            print(f"{'ok  ' if r.ok else 'FAIL'} {r.name:24s} {r.seconds:5.0f} s  "
                  + ("; ".join(r.problems[:3]) if not r.ok else ""), flush=True)
    finally:
        if xvfb:
            xvfb.kill()
        if RUN_ROOT != ROOT:
            shutil.rmtree(RUN_ROOT, ignore_errors=True)
    check_pairs(results)
    ordered = [results[c["name"]] for c in cases]
    write_report(ordered, out)
    failed = [r for r in ordered if not r.ok]
    for r in failed:
        if r.problems and all("golden" not in p for p in r.problems[:1]):
            pass
    print(f"\n{len(ordered) - len(failed)}/{len(ordered)} passed. Report: {out / 'report.html'}")
    for r in failed:
        print(f"  FAIL {r.name}: " + "; ".join(r.problems))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
