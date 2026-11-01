#!/usr/bin/env python3
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
"""Teach an existing model how the colour changes between soft and loud playing.

Levels: soft (velocity 0.39) from a section with recorded layers (VSCO2-CE), optionally
pianissimo (0.2) and fortissimo (1.0) from the Philharmonia solo samples (measured with
sources/philharmonia.py as differences to forte).

Source: a sampled section with recorded dynamic layers (e.g. VSCO2-CE, see
sources/vsco2_sfz.py). For every learned key the soft and the loud layer are measured
(harmonic envelope); their difference is the target. The engine then plays the same key
softly and loudly, its own difference is measured and the soft curves are corrected
(closed loop). The loud colour of the model is not touched: the engine keeps it exactly at
the learning velocity.

Only the difference curves end up in the model (a few KB). No audio is stored.

usage: learn_dynamics.py --model models/opensection.osm --sfz vsco2-strings.sfz
                         --sfizz-render <path> --engine <opensection-render> --out <model>
"""
import argparse, os, struct, subprocess, tempfile
import numpy as np
from learn import SR, BANDS, KEYS, read_wav, harmonic_envelope, register

VEL_LOUD, VEL_SOFT = 0.71, 0.39          # engine velocities (MIDI 90 and 50)
VEL_PP, VEL_FF = 0.2, 1.0                # Philharmonia pianissimo / fortissimo (against forte = loud)
MIDI_LOUD, MIDI_SOFT = 110, 50           # inside the loud / soft layer of the source


def source_notes(sfz, sfizz_render, cache):
    if os.path.exists(cache):
        d = np.load(cache)
        return {(int(k.split("_")[0]), k.split("_")[1]): d[k] for k in d.files}
    out = {}
    with tempfile.TemporaryDirectory() as t:
        for k in KEYS:
            for name, vel in (("soft", MIDI_SOFT), ("loud", MIDI_LOUD)):
                trk = b"\x00\xff\x51\x03" + struct.pack(">I", 500000)[1:] + b"\x00" + bytes([0x90, k, vel]) \
                    + b"\x9e\x60" + bytes([0x80, k, 0]) + b"\x00\xff\x2f\x00"
                mid, wav = os.path.join(t, "n.mid"), os.path.join(t, "n.wav")
                open(mid, "wb").write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(trk)) + trk)
                subprocess.run([sfizz_render, "--sfz", sfz, "--midi", mid, "--wav", wav, "-s", str(SR)], capture_output=True, check=True)
                out[(k, name)] = read_wav(wav).mean(0)[int(0.6 * SR): int(3.0 * SR)]
    np.savez(cache, **{f"{k}_{n}": v for (k, n), v in out.items()})
    return out


def shape(e, f0):
    """difference curve with the fundamental region at 0 dB, nothing below the fundamental"""
    ok = BANDS >= f0 * 0.85
    e = e.copy(); e[~ok] = e[ok][0]
    ref = (BANDS >= f0 * 0.85) & (BANDS <= f0 * 2.2)
    return e - e[ref].mean()


def smooth_keys(arr, wide=False):
    """across neighbouring keys (and, for the targets, across frequency): one sample per
    layer is a single take – we want the general change, not that take's resonances"""
    k = np.array([1, 2, 3, 2, 1] if wide else [1, 2, 1], float); k /= k.sum()
    pad = len(k) // 2
    p = np.pad(arr, ((pad, pad), (0, 0)), mode="edge")
    sm = sum(k[i] * p[i:i + len(arr)] for i in range(len(k)))
    if wide:
        q = np.pad(sm, ((0, 0), (1, 1)), mode="edge")
        sm = 0.25 * q[:, :-2] + 0.5 * q[:, 1:-1] + 0.25 * q[:, 2:]
    return sm


NOISE_GRID = [500, 800, 1300, 2000, 3200, 4800, 6760]
NOISE_LO = 5000          # above this the high band is mostly rosin noise, below mostly the strings' harmonics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True); ap.add_argument("--sfz", required=True)
    ap.add_argument("--sfizz-render", required=True); ap.add_argument("--engine", required=True)
    ap.add_argument("--out", required=True); ap.add_argument("--cache", default="dyn-targets.npz")
    ap.add_argument("--philharmonia", help="npz from sources/philharmonia.py (pianissimo / fortissimo)")
    ap.add_argument("--rounds", type=int, default=2)
    a = ap.parse_args()

    base = [l for l in open(a.model) if not l.split()[:1] or l.split()[0] not in ("soft", "dyn", "noisesoft", "dynnode", "dk")]
    base = [l for l in base if not l.startswith("# dynamics")]
    f0 = lambda k: 440 * 2 ** ((k - 69) / 12)
    recs = source_notes(a.sfz, a.sfizz_render, a.cache)
    soft = smooth_keys(np.array([shape(harmonic_envelope(recs[(k, "soft")], f0(k)) - harmonic_envelope(recs[(k, "loud")], f0(k)), f0(k))
                                 for k in KEYS]), wide=True)
    targets = {VEL_SOFT: np.clip(soft, -40, 6)}
    sources = ["VSCO2-CE (CC0, soft vs loud layer)"]
    if a.philharmonia:
        P = np.load(a.philharmonia)
        pp = smooth_keys(P["pianissimo"], wide=True)
        targets[VEL_PP] = np.clip(np.minimum(pp, targets[VEL_SOFT]), -40, 6)   # never brighter than soft playing
        targets[VEL_FF] = np.clip(smooth_keys(P["fortissimo"], wide=True), -3, 8)
        sources.append("Philharmonia Orchestra sound samples (CC BY-SA 3.0, pianissimo/fortissimo vs forte)")
    vels = sorted(targets)
    hi = lambda k: (BANDS > 2000) & (BANDS < 8000) & (BANDS > 2 * f0(k))
    for v in vels:
        print(f"Ziel Anschlag {v:.2f} gegen {VEL_LOUD} (2–8 kHz): " + "  ".join(
            f"{k}:{targets[v][i][hi(k)].mean():+.0f}" for i, k in enumerate(KEYS) if i % 6 == 0) + " dB")

    tmp = tempfile.mkdtemp()
    noise = [0.0] * 4
    def model_text(cur):
        lines = base + [f"dyn {VEL_LOUD} {VEL_SOFT}\n", "noisesoft " + " ".join("%.0f" % f for f in noise) + "\n"]
        for v in vels:
            lines.append(f"dynnode {v}\n")
            lines += ["dk %d %s\n" % (k, " ".join("%.2f" % x for x in cur[v][i])) for i, k in enumerate(KEYS)]
        return "".join(lines)

    def engine_env(text, k, vel):
        mp, npth, wp = (os.path.join(tmp, n) for n in ("m.osm", "n.txt", "o.wav"))
        open(mp, "w").write(text)
        open(npth, "w").write(f"0.0 3.0 {k} {vel}\n")
        es = []
        for seed in (11, 23, 37):
            subprocess.run([a.engine, "--model", mp, "--seed", str(seed), "--quality", "1.0", "--tail", "0.5",
                            "--dry", "--spacing", "0", npth, wp], capture_output=True, check=True)
            es.append(harmonic_envelope(read_wav(wp).mean(0)[int(0.6 * SR): int(3.0 * SR)], f0(k)))
        return 10 * np.log10(np.mean([10 ** (e / 10) for e in es], axis=0))

    cur = {v: targets[v].copy() for v in vels}
    loud = {}                                  # the loud colour does not depend on the other levels
    def delta(text, k, vel):
        if k not in loud: loud[k] = engine_env(text, k, VEL_LOUD)
        return shape(engine_env(text, k, vel) - loud[k], f0(k))
    high = lambda k: (BANDS >= NOISE_LO) & (BANDS < 11000) & (BANDS > 2.5 * f0(k))
    for rnd in range(a.rounds):
        # a) rosin noise: how dark it gets in soft playing, per register
        for r in range(4):
            ks = [i for i, k in enumerate(KEYS) if register(k) == r]
            probe = [ks[1], ks[len(ks) // 2], ks[-2]]
            want = np.mean([targets[VEL_SOFT][i][high(KEYS[i])].mean() for i in probe])
            got = []
            for fc in NOISE_GRID:
                noise[r] = fc
                text = model_text(cur)
                got.append(np.mean([delta(text, KEYS[i], VEL_SOFT)[high(KEYS[i])].mean() for i in probe]))
            order = np.argsort(got)
            noise[r] = float(np.interp(want, np.array(got)[order], np.array(NOISE_GRID, float)[order]))
            print(f"    Lage {r}: Höhen leise {want:+.1f} dB → Bogengeräusch leise bis {noise[r]:.0f} Hz")
        # b) the strings' harmonics (below the noise band), closed loop, every level
        text = model_text(cur)
        for v in vels:
            err = []
            new = cur[v].copy()
            for i, k in enumerate(KEYS):
                d = targets[v][i] - delta(text, k, v)
                ok = (BANDS >= f0(k) * 0.85) & (BANDS < 11000)
                err.append(np.mean(np.abs(d[ok])))
                d[BANDS >= NOISE_LO] = 0
                new[i] += 0.8 * d
            cur[v] = np.clip(smooth_keys(new), -40, 8)
            print(f"  Runde {rnd + 1}, Anschlag {v:.2f}: Abweichung zur Aufnahme {np.mean(err):.1f} dB")

    text = model_text(cur).replace("# learned by", "".join(f"# dynamics learned from {s}\n" for s in sources) + "# learned by", 1)
    open(a.out, "w").write(text)
    print(f"Modell geschrieben: {a.out} ({os.path.getsize(a.out) / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
