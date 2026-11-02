#!/usr/bin/env python3
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
"""Measure how the colour of bowed strings changes with dynamics, from the University of Iowa
Musical Instrument Samples (https://theremin.music.uiowa.edu/MIS.html, "may be downloaded and
used for any projects, without restrictions"): violin, viola, cello, double bass, arco, recorded
in an anechoic chamber at pp, mf and ff.

Every file is a chromatic run on one string (e.g. Cello.arco.mf.sulG.G2Gb3). The notes are found
by their silences, their pitch is checked, and for every note that exists at pp/mf/ff on the same
string the differences of the harmonic envelopes are taken. Output (npz): per learned key the
median difference curves pianissimo-mf and fortissimo-mf – no audio.

usage: iowa.py <folder with the *.arco.mono.2496 folders (searched recursively)> out.npz
"""
import os, re, subprocess, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from learn import SR, BANDS, KEYS, harmonic_envelope
from learn_dynamics import shape

INSTR = [("Bass", 0, 42), ("Cello", 43, 54), ("Viola", 55, 66), ("Violin", 67, 127)]
PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
NOTE = r"([A-G])(b?)(\d)"


def midi(letter, flat, octave):
    return 12 * (int(octave) + 1) + PC[letter] - (1 if flat else 0)


def parse(name):
    """Cello.arco.mf.sulG.G2Gb3.flac / Bass.arco.sulA.ff.A1B1.mono.flac -> (instr, dyn, string, lo, hi)"""
    t = name.split(".")
    if len(t) < 5 or t[1] != "arco": return None
    dyn = next((x for x in t if x in ("pp", "mf", "ff")), None)
    string = next((x for x in t if x.startswith("sul")), None)
    rng = next((x for x in t[2:] if re.fullmatch(NOTE + "(" + NOTE + ")?", x)), None)
    if not (dyn and string and rng): return None
    m = re.fullmatch(NOTE + "(?:" + NOTE + ")?", rng)
    lo = midi(*m.group(1, 2, 3))
    hi = midi(*m.group(4, 5, 6)) if m.group(4) else lo
    return t[0], dyn, string, lo, hi


def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "1", "-ar", str(SR), "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32)


def pitch(x):
    """cepstrum: robust when the fundamental of low strings is weak"""
    X = np.log(np.abs(np.fft.rfft(x * np.hanning(len(x)), 1 << 18)) + 1e-9)
    c = np.fft.irfft(X)
    q = np.arange(len(c)) / SR
    m = (q > 1 / 2400) & (q < 1 / 25)
    return 69 + 12 * np.log2((1 / q[m][np.argmax(c[m])]) / 440)


def notes(x, lo, hi):
    """(midi, sustained part) of every note of a chromatic run: a pitch track every 50 ms,
    split where the pitch changes or the sound stops"""
    hop, win = int(0.05 * SR), int(0.1 * SR)
    n = (len(x) - win) // hop
    if n < 10: return []
    db = np.array([10 * np.log10(np.mean(x[i * hop:i * hop + win] ** 2) + 1e-14) for i in range(n)])
    lab = np.full(n, -1)
    for i in np.where(db > db.max() - 35)[0]:
        det = pitch_frame(x[i * hop:i * hop + win])
        cand = [det + 12 * o for o in (-2, -1, 0, 1, 2) if lo - 0.5 <= det + 12 * o <= hi + 0.5]
        if not cand: continue
        c = min(cand, key=lambda v: abs(v - det))
        if abs(c - round(c)) < 0.4: lab[i] = int(round(c))
    out, i = [], 0
    while i < n:
        if lab[i] < 0: i += 1; continue
        j = i
        while j < n and (lab[j] == lab[i] or (lab[j] == -1 and j + 1 < n and lab[j + 1] == lab[i])): j += 1
        if j - i >= 14:                                           # at least 0.7 s of one pitch
            a2 = i + 4                                            # skip the bow attack
            top = db[a2:j].max()
            keep = np.where(db[a2:j] > top - 6)[0]
            if len(keep) >= 8:
                s0, e0 = a2 + keep[0], min(a2 + keep[-1], a2 + keep[0] + 60)
                out.append((int(lab[i]), x[s0 * hop: e0 * hop + win]))
        i = j
    return out


def pitch_frame(x):
    X = np.log(np.abs(np.fft.rfft(x * np.hanning(len(x)), 1 << 15)) + 1e-9)
    c = np.fft.irfft(X)
    q = np.arange(len(c)) / SR
    m = (q > 1 / 2400) & (q < 1 / 25)
    return 69 + 12 * np.log2((1 / q[m][np.argmax(c[m])]) / 440)


def main():
    root, out = sys.argv[1], sys.argv[2]
    env = {}                                   # (instr, string, midi, dyn) -> [harmonic envelopes]
    ok = bad = 0
    for dp, _, fs in os.walk(root):
        if "mono.2496" not in dp: continue
        for f in sorted(fs):
            p = parse(f)
            if not p: continue
            instr, dyn, string, lo, hi = p
            found = notes(load(os.path.join(dp, f)), lo, hi)
            ok += len(found); bad += max(0, hi - lo + 1 - len(found))
            for k, s in found:
                env.setdefault((instr, string, k, dyn), []).append(harmonic_envelope(s, 440 * 2 ** ((k - 69) / 12)))
    print(f"{ok} Töne erkannt, {bad} nicht gefunden")
    res = {}
    for dyn in ("pp", "ff"):
        curves, counts = [], []
        for k in KEYS:
            instr = next(i for i, lo, hi in INSTR if lo <= k <= hi)
            f0k = 440 * 2 ** ((k - 69) / 12)
            ds = []
            for (ins, string, m, d), es in env.items():
                if ins != instr or d != dyn or abs(m - k) > 3: continue
                ref = env.get((ins, string, m, "mf"))
                if not ref: continue
                f0 = 440 * 2 ** ((m - 69) / 12)
                dd = shape(np.mean(es, axis=0) - np.mean(ref, axis=0), f0)
                ds.append(np.interp(np.log2(BANDS), np.log2(BANDS) + np.log2(f0 / f0k), dd))
            counts.append(len(ds))
            curves.append(np.median(ds, axis=0) if ds else np.zeros(len(BANDS)))
        res[dyn] = np.array(curves); res[dyn + "_n"] = np.array(counts)
        print(f"{dyn} gegen mf: {min(counts)}…{max(counts)} Tonpaare je Taste")
    np.savez(out, keys=np.array(KEYS), **res)


if __name__ == "__main__":
    main()
