#!/usr/bin/env python3
"""Cut a tour.sh recording to the scenario: from "afxscene: start" to the
end, normalised to -1 dBFS peak, 16-bit.

    cut_scenario.py <log> <raw.wav> <out.wav>
"""
import re
import sys
import wave
from datetime import datetime

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from analyze_tour import read_wav  # noqa: E402

TS = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")


def main(log, raw, out):
    t_audio = t_start = None
    for line in open(log, errors="replace"):
        m = TS.match(line)
        if not m:
            continue
        t = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
        if "Audio: Global Initialized" in line and t_audio is None:
            t_audio = t
        if "afxscene: start" in line:
            t_start = t
    x, rate = read_wav(raw)
    off = (t_start - t_audio).total_seconds() if t_audio and t_start else 0
    x = x[int(max(0, off - 0.5) * rate):]
    x = x * (0.89 / max(1e-6, np.abs(x).max()))
    with wave.open(out, "wb") as w:
        w.setnchannels(x.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())
    print(f"{out}: {len(x) / rate:.1f}s")


if __name__ == "__main__":
    main(*sys.argv[1:4])
