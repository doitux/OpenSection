#!/usr/bin/env python3
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
"""Learn the body resonances of violin, viola, cello and double bass from the dry (anechoic)
University of Iowa recordings.

Idea: the string produces a smooth spectrum; the wooden body adds a fixed pattern of peaks and
dips that does not move with the note. Every harmonic of every note (and, with vibrato, every
moment of it) samples that pattern at a slightly different frequency. Per note the smooth trend
is removed; what all notes have in common at a given frequency is the body. Output: one
fine-structure curve per section (dB on a 1/48-octave grid, smooth trend = 0) – no audio.

usage: iowa_body.py <folder with the *.arco.mono.2496 folders> out.npz
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from iowa import parse, load, notes, SR

SECTIONS = ["Bass", "Cello", "Viola", "Violin"]
GRID = 60 * 2 ** (np.arange(0, 48 * np.log2(12000 / 60)) / 48)       # 1/48 octave, 60 Hz .. 12 kHz
N, HOP = 8192, 2048


def harmonic_track(x, k):
    """per frame and harmonic: (frequency, level dB) of the spectral peak near h * f0"""
    f0n = 440 * 2 ** ((k - 69) / 12)
    win = np.hanning(N)
    out = []
    for i in range(0, len(x) - N, HOP):
        X = np.abs(np.fft.rfft(x[i:i + N] * win))
        f0 = f0n
        lv = 20 * np.log10(X + 1e-12)
        fr = []
        for h in range(1, 80):
            fc = h * f0
            if fc > 11000: break
            b = int(round(fc * N / SR)); w = max(2, int(0.03 * fc * N / SR))
            seg = X[b - w:b + w + 1]
            if len(seg) < 3: break
            j = b - w + int(np.argmax(seg))
            if 0 < j < len(X) - 1:            # parabolic interpolation of the peak
                a, c, d = lv[j - 1], lv[j], lv[j + 1]
                p = 0.5 * (a - d) / (a - 2 * c + d + 1e-12)
                fpk, lpk = (j + p) * SR / N, c - 0.25 * (a - d) * p
                if h == 1 or abs(fpk / h - f0) < 0.03 * f0:
                    if h == 1: f0 = fpk            # follow the vibrato
                    fr.append((fpk, lpk, h))
        if fr: out.append(fr)
    return out


def smooth_oct(fs, ls, frac=1 / 3):
    """trend of the levels over log frequency (1/3 octave)"""
    lf = np.log2(fs)
    return np.array([np.median(ls[np.abs(lf - x) < frac / 2]) for x in lf])


def main():
    root, out = sys.argv[1], sys.argv[2]
    pts = {s: [] for s in SECTIONS}          # (frequency, residual dB, note id)
    nid = 0
    for dp, _, fs in os.walk(root):
        if "mono.2496" not in dp: continue
        for f in sorted(fs):
            p = parse(f)
            if not p: continue
            instr, dyn, string, lo, hi = p
            for k, seg in notes(load(os.path.join(dp, f)), lo, hi):
                tr = harmonic_track(seg, k)
                if len(tr) < 4: continue
                # the note's own smooth spectrum (mean over frames per harmonic, 1/3-octave trend)
                hs = sorted({h for fr in tr for _, _, h in fr})
                mean = {h: np.mean([l for fr in tr for (ff, l, hh) in fr if hh == h]) for h in hs}
                mf = np.array([440 * 2 ** ((k - 69) / 12) * h for h in hs]); ml = np.array([mean[h] for h in hs])
                if len(hs) < 4: continue
                trend = dict(zip(hs, smooth_oct(mf, ml)))
                for fr in tr:
                    off = np.mean([l - mean[h] for _, l, h in fr])      # frame loudness (bow changes)
                    for ff, l, h in fr:
                        if l - mean[h] < -20: continue                    # dropouts
                        pts[instr].append((ff, l - off - trend[h], nid))
                nid += 1
    res, rep = {}, []
    for s in SECTIONS:
        a = np.array(pts[s])
        if len(a) == 0: res[s] = np.zeros(len(GRID)); continue
        lg = np.log2(a[:, 0]); g = np.log2(GRID)
        curve, half = np.zeros(len(GRID)), np.zeros((2, len(GRID)))
        for i, x in enumerate(g):
            m = np.abs(lg - x) < 1 / 96
            if m.sum() >= 6: curve[i] = np.median(a[m, 1])
            for e in (0, 1):                                          # split-half check (odd / even notes)
                mm = m & (a[:, 2] % 2 == e)
                if mm.sum() >= 3: half[e, i] = np.median(a[mm, 1])
        # remove what is left of a trend (the colour is learned elsewhere)
        tr = np.array([np.median(curve[np.abs(g - x) < 1 / 6]) for x in g])
        curve -= tr
        ok = (GRID > 150) & (GRID < 8000)
        rel = np.corrcoef(half[0][ok], half[1][ok])[0, 1]
        res[s] = curve
        rep.append(f"{s}: {len(a)} Messpunkte, Struktur ±{np.std(curve[ok]):.1f} dB, Übereinstimmung zweier Hälften r = {rel:.2f}")
    print("\n".join(rep))
    np.savez(out, grid=GRID, **res)


if __name__ == "__main__":
    main()
