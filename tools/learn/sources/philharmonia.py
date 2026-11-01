#!/usr/bin/env python3
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
"""Measure how the colour of bowed strings changes with dynamics, from the Philharmonia
Orchestra sound samples (https://philharmonia.co.uk/resources/sound-samples/, CC BY-SA 3.0).

Solo players, one note per file, many dynamics. For every note that exists in two
dynamics (same player, same pitch) the difference of the harmonic envelopes is taken; the
median over neighbouring notes gives one difference curve per learned key and dynamic.
Only these curves are written (npz) – no audio.

usage: philharmonia.py <Strings folder of the sample download> out.npz
"""
import os, re, subprocess, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from learn import SR, BANDS, KEYS, harmonic_envelope
from learn_dynamics import shape

INSTR = [("double bass", 0, 42), ("cello", 43, 54), ("viola", 55, 66), ("violin", 67, 127)]
PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
NAME = re.compile(r"^[a-z -]+_([A-G])(s?)(-?\d)_(1|15)_(pianissimo|forte|fortissimo)_arco-normal\.mp3$")


def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "1", "-ar", str(SR), "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32)


def steady(x):
    """the sustained part of a short note: level within 6 dB of its maximum, after 0.1 s"""
    hop = int(0.02 * SR)
    r = np.array([np.sqrt(np.mean(x[i:i + hop] ** 2)) for i in range(0, len(x) - hop, hop)]) + 1e-9
    db = 20 * np.log10(r / r.max())
    on = np.where(db > -6)[0]
    a, b = max(on[0] * hop, int(0.1 * SR)), (on[-1] + 1) * hop
    return x[a:b] if b - a > int(0.25 * SR) else None


def main():
    root, out = sys.argv[1], sys.argv[2]
    notes = {}                        # (instrument, midi, length, dyn) -> envelope
    for instr, lo, hi in INSTR:
        for f in sorted(os.listdir(os.path.join(root, instr))):
            m = NAME.match(f)
            if not m: continue
            midi = 12 * (int(m.group(3)) + 1) + PC[m.group(1)] + (1 if m.group(2) else 0)
            y = steady(load(os.path.join(root, instr, f)))
            if y is None: continue
            notes[(instr, midi, m.group(4), m.group(5))] = harmonic_envelope(y, 440 * 2 ** ((midi - 69) / 12))
    print(f"{len(notes)} Töne ausgewertet")
    res = {}
    for dyn in ("pianissimo", "fortissimo"):
        curves, counts = [], []
        for k in KEYS:
            instr = next(i for i, lo, hi in INSTR if lo <= k <= hi)
            f0k = 440 * 2 ** ((k - 69) / 12)
            ds = []
            for (ins, midi, ln, d), e in notes.items():
                if ins != instr or d != dyn or abs(midi - k) > 3: continue
                ref = notes.get((ins, midi, ln, "forte"))
                if ref is None: continue
                f0 = 440 * 2 ** ((midi - 69) / 12)
                dd = shape(e - ref, f0)
                # move to the learned key's harmonic grid (shift by the pitch difference)
                ds.append(np.interp(np.log2(BANDS), np.log2(BANDS) + np.log2(f0 / f0k), dd))
            counts.append(len(ds))
            curves.append(np.median(ds, axis=0) if ds else np.zeros(len(BANDS)))
        res[dyn] = np.array(curves); res[dyn + "_n"] = np.array(counts)
        print(f"{dyn} gegen forte: {min(counts)}…{max(counts)} Tonpaare je Taste")
    np.savez(out, keys=np.array(KEYS), **res)


if __name__ == "__main__":
    main()
