#!/usr/bin/env python3
"""Per-station summary of an afx tour (see tour.sh).

    analyze_tour.py <log.txt> <tour.wav>

For each station: the client's last logged acoustic state, and from the
recording the level, left/right balance and brightness of what the player
heard (the outdoor bed dominates; clicks add room reverb).
"""
import re
import sys
from datetime import datetime

import numpy as np

TS = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")


def stamp(line):
    m = TS.match(line)
    return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S") if m else None


def read_wav(path):
    """PCM or float WAV, including WAVE_FORMAT_EXTENSIBLE (OpenAL Soft writes it)."""
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    pos, fmt, body = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], int.from_bytes(data[pos + 4:pos + 8], "little")
        chunk = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = chunk
        elif cid == b"data":
            body = chunk if size else data[pos + 8:]
        pos += 8 + size + (size & 1)
    if fmt is None or body is None:
        raise ValueError("missing fmt or data chunk")
    tag = int.from_bytes(fmt[0:2], "little")
    channels = int.from_bytes(fmt[2:4], "little")
    rate = int.from_bytes(fmt[4:8], "little")
    bits = int.from_bytes(fmt[14:16], "little")
    if tag == 0xFFFE:
        tag = int.from_bytes(fmt[24:26], "little")
    if tag == 3:
        x = np.frombuffer(body[:len(body) // (bits // 8) * (bits // 8)],
                          dtype=np.float32 if bits == 32 else np.float64).astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(body[:len(body) // 2 * 2], dtype=np.int16) / 32768.0
    elif tag == 1 and bits == 8:
        x = (np.frombuffer(body, dtype=np.uint8).astype(np.float64) - 128) / 128.0
    else:
        raise ValueError(f"unsupported WAV format {tag}/{bits}")
    x = x[:len(x) // channels * channels]
    return x.reshape(-1, channels), rate


def main(log_path, wav_path):
    stations, afx_lines, audio_t0 = [], [], None
    for line in open(log_path, errors="replace"):
        t = stamp(line)
        if t is None:
            continue
        if "Audio: Global Initialized" in line and audio_t0 is None:
            audio_t0 = t
        m = re.search(r"afxscene: station (\w+)", line)
        if m:
            stations.append((m.group(1), t))
        m = re.search(r"(afx: encl .*)", line)
        if m:
            afx_lines.append((t, m.group(1)))
        if "afx: EFX" in line or "ALC_EXT_EFX" in line:
            print(line.strip().split(": ", 2)[-1])

    if not stations:
        print("no stations in log; did the tour run?")
        return 1

    samples, rate = None, 48000
    try:
        samples, rate = read_wav(wav_path)
    except (OSError, ValueError) as e:
        print(f"no audio: {e}")

    print(f"\n{'station':8} {'level dB':>9} {'L/R dB':>7} {'bright':>7}  client state")
    for i, (name, t) in enumerate(stations):
        end = stations[i + 1][1] if i + 1 < len(stations) else None
        state = [s for (ts, s) in afx_lines if ts >= t and (end is None or ts < end)]
        state = state[-1] if state else "(no afx line)"
        level = lr = bright = float("nan")
        if samples is not None and audio_t0 is not None:
            a = (t - audio_t0).total_seconds() + 3
            b = ((end - audio_t0).total_seconds() if end else a + 6) - 0.5
            seg = samples[int(a * rate):int(b * rate)]
            if len(seg) > rate // 2:
                e = (seg ** 2).mean(axis=0) + 1e-12
                level = 10 * np.log10(e.sum())
                lr = 10 * np.log10(e[0] / e[1])
                d = np.diff(seg, axis=0)
                bright = float((d ** 2).sum() / (seg ** 2).sum())
        print(f"{name:8} {level:9.1f} {lr:7.1f} {bright:7.3f}  {state}")
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:3]))
