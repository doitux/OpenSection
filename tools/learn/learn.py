#!/usr/bin/env python3
# OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
"""Teach the engine the sound of a sampled string section.

The engine plays every second key, its output is compared with the sampled instrument,
and the tone-colour curves are corrected (closed loop, several rounds, averaged over
random ensembles). Then bow-pressure unrest is calibrated per register (presence-band
flicker), and finally room mics + hall are calibrated on a chord progression
(stereo width and reverb energy after release).

Only statistics end up in the model (a few KB). No audio of the reference is stored.

usage: learn.py --vpo <Virtual-Playing-Orchestra dir> --sfizz-render <path>
                --engine <opensection-render> --reference <progression.wav> --notes <prog.txt>
                --out models/opensection.osm
"""
import argparse, os, re, struct, subprocess, sys, tempfile
import numpy as np
from scipy.signal import welch, butter, sosfilt

SR = 48000
BANDS = np.array([40 * 2 ** (i / 6) for i in range(55) if 40 * 2 ** (i / 6) < 20000])
KEYS = list(range(28, 97, 2))
REG_TOP = [43, 55, 67, 999]


def register(k):
    return next(i for i, t in enumerate(REG_TOP) if k < t)


def read_wav(path):
    """float32 or int16 stereo/mono WAV -> (channels, n) float32"""
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "2", "-ar", str(SR), "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).reshape(-1, 2).T.copy()


# ------------------------------------------------------------------ measurements
def harmonic_envelope(x, f0):
    f, p = welch(x, SR, nperseg=16384)
    hs, ds, h = [], [], 1
    while h * f0 < 16000:
        m = (f > (h - 0.3) * f0) & (f < (h + 0.3) * f0)
        hs.append(h * f0); ds.append(10 * np.log10(p[m].sum() + 1e-20)); h += 1
    return np.interp(np.log2(BANDS), np.log2(hs), ds)


PRESENCE = butter(4, [1000 / (SR / 2), 4000 / (SR / 2)], "band", output="sos")


def shimmer_db(x):
    y = sosfilt(PRESENCE, x)
    hop = int(0.02 * SR)
    r = np.sqrt(np.array([np.mean(y[i:i + hop] ** 2) for i in range(0, len(y) - hop, hop)]) + 1e-12)
    db = 20 * np.log10(r)
    trend = np.convolve(db, np.ones(25) / 25, mode="same")
    return float(np.std((db - trend)[12:-12]))


def width(x, sl):
    s, m = x[0] - x[1], x[0] + x[1]
    return float(np.sqrt((s[sl] ** 2).mean() / (m[sl] ** 2).mean()))


COH_BANDS = [butter(4, [lo / (SR / 2), hi / (SR / 2)], "band", output="sos") for lo, hi in ((150, 500), (500, 2000), (2000, 8000))]


def coherence(x, sl):
    """how similar left and right are (per band, max. correlation within ±1 ms, 100 ms windows)"""
    out = []
    for sos in COH_BANDS:
        y = sosfilt(sos, x[:, sl], axis=1)
        h, cs = int(0.1 * SR), []
        for i in range(0, y.shape[1] - h, h):
            l, r = y[0, i:i + h], y[1, i + 48:i + h - 48]
            cc = np.correlate(l, r, mode="valid") / np.sqrt((l ** 2).sum() * (r ** 2).sum() + 1e-20)
            cs.append(np.abs(cc).max())
        out.append(np.mean(cs))
    return np.array(out)


def tail_db(x, sus, tl):
    return float(10 * np.log10((x[:, tl] ** 2).mean() / (x[:, sus] ** 2).mean()))


def envelope_db(x):
    f, p = welch(x, SR, nperseg=8192)
    out = []
    for fc in BANDS:
        m = (f >= fc * 2 ** (-1 / 12)) & (f < fc * 2 ** (1 / 12))
        out.append(10 * np.log10(p[m].mean() + 1e-20) if m.any() else -200)
    return np.array(out)


# ------------------------------------------------------------------ the two instruments
def target_notes(vpo, sfizz_render, cache):
    if os.path.exists(cache):
        d = np.load(cache)
        return {int(k): d[k] for k in d.files}
    sfz = os.path.join(vpo, "Strings", "all-strings-SEC-sustain.sfz")
    out = {}
    with tempfile.TemporaryDirectory() as t:
        for k in KEYS:
            trk = b"\x00\xff\x51\x03" + struct.pack(">I", 500000)[1:] + b"\x00" + bytes([0x90, k, 90]) \
                + b"\x9e\x60" + bytes([0x80, k, 0]) + b"\x00\xff\x2f\x00"
            mid, wav = os.path.join(t, "n.mid"), os.path.join(t, f"{k}.wav")
            open(mid, "wb").write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(trk)) + trk)
            subprocess.run([sfizz_render, "--sfz", sfz, "--midi", mid, "--wav", wav, "-s", str(SR)], capture_output=True, check=True)
            out[k] = read_wav(wav).mean(0)[int(0.5 * SR): int(3.0 * SR)]
    np.savez(cache, **{str(k): v for k, v in out.items()})
    return out


class EngineRunner:
    def __init__(self, exe, tmp):
        self.exe, self.tmp = exe, tmp

    def render(self, model_text, notes, seed=1, dry=False, quality=1.0, extra=(), tail=1.5):
        mp, npth, wp = (os.path.join(self.tmp, n) for n in ("m.osm", "n.txt", "o.wav"))
        open(mp, "w").write(model_text)
        open(npth, "w").write("".join(f"{a} {b} {k} {v}\n" for a, b, k, v in notes))
        cmd = [self.exe, "--model", mp, "--seed", str(seed), "--quality", str(quality), "--tail", str(tail)]
        if dry: cmd.append("--dry")
        cmd += list(extra) + [npth, wp]
        subprocess.run(cmd, capture_output=True, check=True)
        return read_wav(wp)


EXTRA = []          # model lines learned elsewhere (dynamics, bow, bodies): carried over on --resume
HALL = {"release": 1.0, "rt60": 2.4, "hf": 0.45, "erdiff": 0.5, "spacing": 0.3}   # learned release, hall, room


def model_text(curves, unrest, er, late, out=1.0):
    lines = ["OSM1", "# learned by tools/learn/learn.py from Virtual Playing Orchestra (all-strings-SEC-sustain)",
             "bands %d %s" % (len(BANDS), " ".join("%.3f" % b for b in BANDS))]
    for k in KEYS:
        lines.append("key %d %s" % (k, " ".join("%.2f" % v for v in curves[k])))
    lines.append("unrest " + " ".join("%.3f" % u for u in unrest))
    lines += ["er %.4f" % er, "late %.4f" % late, "out %.4f" % out,
              "release %.3f" % HALL["release"], "hall %.3f %.3f" % (HALL["rt60"], HALL["hf"]),
              "erdiff %.3f" % HALL["erdiff"], "spacing %.3f" % HALL["spacing"]]
    return "\n".join(lines + EXTRA) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vpo", required=True); ap.add_argument("--sfizz-render", required=True)
    ap.add_argument("--engine", required=True); ap.add_argument("--reference", required=True)
    ap.add_argument("--notes", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--cache", default="vpo-targets.npz"); ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--resume", help="existing model: skip tone colour + unrest, only room and overall colour")
    ap.add_argument("--keep-room", action="store_true", help="with --resume: also keep room + release, only overall colour and loudness")
    a = ap.parse_args()
    tmp = tempfile.mkdtemp()
    eng = EngineRunner(a.engine, tmp)
    recs = target_notes(a.vpo, a.sfizz_render, a.cache)
    print(f"{len(recs)} Tasten des Sample-Instruments gemessen")

    curves = {k: np.zeros(len(BANDS)) for k in KEYS}
    unrest = [1.5, 1.5, 1.5, 0.5]
    er, late = 0.3, 0.2
    note = lambda k: [(0.0, 3.0, k, 0.71)]

    def mono(k, seed, extra=()):
        # no mic spacing here: time differences would comb-filter the mono analysis
        y = eng.render(model_text(curves, unrest, er, late), note(k), seed=seed, dry=True, extra=("--spacing", "0") + tuple(extra))
        return y.mean(0)[int(0.5 * SR): int(3.0 * SR)]

    if a.resume:
        for line in open(a.resume):
            t = line.split()
            if t and t[0] == "key": curves[int(t[1])] = np.array(list(map(float, t[2:])))
            if t and t[0] == "unrest": unrest = list(map(float, t[1:]))
            if t and t[0] in ("release",): HALL["release"] = float(t[1])
            if t and t[0] in ("flutter", "harmvar", "bodydirect", "dyn", "noisesoft", "dynnode", "dk"): EXTRA.append(line.rstrip("\n"))
            if a.keep_room and t:
                if t[0] == "er": er = float(t[1])
                if t[0] == "late": late = float(t[1])
                if t[0] == "hall": HALL["rt60"], HALL["hf"] = float(t[1]), float(t[2])
                if t[0] in ("erdiff", "spacing"): HALL[t[0]] = float(t[1])
        print("  Klangfarbe + Unruhe übernommen aus", a.resume)

    # 1) tone colour, closed loop
    for rnd in range(0 if a.resume else a.rounds):
        err = []
        new = {}
        for k in KEYS:
            f0 = 440 * 2 ** ((k - 69) / 12)
            envs = [harmonic_envelope(mono(k, s), f0) for s in (11, 23, 37, 51)]
            ey = 10 * np.log10(np.mean([10 ** (e / 10) for e in envs], axis=0))
            d = harmonic_envelope(recs[k], f0) - ey
            ok = BANDS >= f0 * 0.85
            d[~ok] = d[ok][0]
            err.append(np.mean(np.abs(d[ok & (BANDS < 12000)] - np.median(d[ok]))))
            new[k] = curves[k] + d
        arr = np.array([new[k] for k in KEYS])
        sm = arr.copy(); sm[1:-1] = 0.25 * arr[:-2] + 0.5 * arr[1:-1] + 0.25 * arr[2:]
        mean = np.mean(np.median(sm, axis=1))
        curves = {k: np.clip(sm[i] - mean, -80, 40) for i, k in enumerate(KEYS)}
        print(f"  Klangfarbe Runde {rnd + 1}: Abweichung zur Aufnahme {np.mean(err):.1f} dB")

    # 2) bow-pressure unrest per register
    grid = [0.0, 1.5, 3.0, 4.5]
    for r in range(0 if a.resume else 4):
        ks = [k for k in KEYS if register(k) == r]
        target = np.mean([shimmer_db(recs[k]) for k in ks])
        probe = [ks[1], ks[len(ks) // 2], ks[-2]]
        meas = []
        for u in grid:
            uu = list(unrest); uu[r] = u
            ov = ("--unrest", ",".join(str(v) for v in uu))
            meas.append(np.mean([shimmer_db(mono(k, s, ov)) for k in probe for s in (5, 9)]))
        meas = np.maximum.accumulate(meas)
        unrest[r] = float(np.interp(target, meas, grid))
        print(f"  Lage {r}: Flimmern Aufnahme {target:.2f} dB, Modell {meas[0]:.2f}…{meas[-1]:.2f} → Unruhe {unrest[r]:.2f} dB")

    # 3) room mics + hall on the progression
    ref = read_wav(a.reference)
    prog = [tuple(map(float, l.split())) for l in open(a.notes) if l.strip() and not l.startswith(("#", "cc"))]
    prog = [(x, y, int(k), v) for x, y, k, v in prog]
    last = max(p[1] for p in prog)
    sus, tl = slice(int(2 * SR), int((last - 0.5) * SR)), slice(int((last + 0.3) * SR), int((last + 2.5) * SR))
    W, T, C = width(ref, sus), tail_db(ref, sus, tl), coherence(ref, sus)
    print(f"  Ziel: Breite {W:.2f}, L/R-Ähnlichkeit {' / '.join(f'{c:.2f}' for c in C)}, Nachhall nach Loslassen {T:.1f} dB")
    def room_cost(e, l):
        y = eng.render(model_text(curves, unrest, e, l), prog, seed=3, tail=4)
        w, t, co = width(y, sus), tail_db(y, sus, tl), coherence(y, sus)
        return (w - W) ** 2 * 50 + np.sum((co - C) ** 2) * 100 + (t - T) ** 2 / 4, w, t, co

    def room_search(er, late, passes=2):
        """coordinate search over room mic level, hall level, how much room stays with the
        desk (coherent) and the main-mic spacing"""
        grids = {"er": np.linspace(0.1, 2.0, 7), "late": np.linspace(0.1, 1.0, 7),
                 "erdiff": np.array([0.0, 0.25, 0.5, 0.75, 1.0]), "spacing": np.array([0.0, 0.25, 0.5, 0.8])}
        cur = {"er": er, "late": late}
        best = room_cost(er, late)
        for _ in range(passes):
            for name, grid in grids.items():
                for v in grid:
                    old_v = cur.get(name, HALL.get(name))
                    if name in cur: cur[name] = v
                    else: HALL[name] = v
                    c = room_cost(cur["er"], cur["late"])
                    if c[0] < best[0]:
                        best = c
                    else:
                        if name in cur: cur[name] = old_v
                        else: HALL[name] = old_v
        return cur["er"], cur["late"], best

    if not a.keep_room:
        er, late, (c, w, t, co) = room_search(0.6, 0.2)
        print(f"  Raum: frühe Refl. {er:.2f}, Hall {late:.2f}, Raum am Platz {1 - HALL['erdiff']:.2f}, Mikroabstand {HALL['spacing']:.2f}"
              f" → Breite {w:.2f}, L/R {' / '.join(f'{v:.2f}' for v in co)}, Nachhall {t:.1f} dB")

        # 3b) the release: how the sound dies after the keys are let go (3 bands, 50 ms steps)
        DEC_BANDS = [butter(4, [lo / (SR / 2), hi / (SR / 2)], "band", output="sos") for lo, hi in ((250, 1000), (1000, 4000), (4000, 10000))]
        def decay(x):
            m = x.mean(0); out = []
            for sos in DEC_BANDS:
                y = sosfilt(sos, m)
                ref_e = (y[sus] ** 2).mean() + 1e-20
                hop = int(0.05 * SR); t0 = int(last * SR)
                out.append([10 * np.log10((y[t0 + i * hop: t0 + (i + 1) * hop] ** 2).mean() / ref_e + 1e-12) for i in range(44)])
            return np.array(out)
        D_ref = decay(ref)
        valid = D_ref > -55
        print("  Ziel-Ausklang: −30 dB nach %.0f ms (mittleres Band)" % (50 * np.argmax(D_ref[1] < -30)))
        best = None
        for rel in (0.6, 1.0, 1.6, 2.5, 4.0):
            for rt in (1.2, 1.8, 2.4, 3.0):
                for hf in (0.35, 0.6):
                    HALL.update(release=rel, rt60=rt, hf=hf)
                    y = eng.render(model_text(curves, unrest, er, late), prog, seed=3, tail=4)
                    c = np.mean(np.abs(decay(y) - D_ref)[valid])
                    if best is None or c < best[0]:
                        best = (c, rel, rt, hf)
        c, rel, rt, hf = best
        HALL.update(release=rel, rt60=rt, hf=hf)
        print(f"  Ausklang: Spieler ×{rel:.1f}, Hall {rt:.1f} s, Höhen ×{hf:.2f} → Abweichung {c:.1f} dB")
        er, late, (c, w, t, co) = room_search(er, late, passes=1)
        print(f"  Raum nachgeeicht: frühe Refl. {er:.2f}, Hall {late:.2f}, Raum am Platz {1 - HALL['erdiff']:.2f}, Mikroabstand {HALL['spacing']:.2f}"
              f" → Breite {w:.2f}, L/R {' / '.join(f'{v:.2f}' for v in co)}, Nachhall {t:.1f} dB")

    # 4) overall colour of the complete sound (close + room + hall) against the reference:
    #    one gentle correction shared by all keys, two rounds
    def env13(x):
        e = envelope_db(x)
        return np.convolve(np.pad(e, 1, mode="edge"), np.ones(3) / 3, mode="valid")
    for rnd in range(2):
        y = eng.render(model_text(curves, unrest, er, late), prog, seed=3, tail=4)
        d = env13(ref.mean(0)[sus]) - env13(y.mean(0)[sus])
        d -= np.median(d[(BANDS > 150) & (BANDS < 8000)])
        d = np.clip(d, -6, 6)
        curves = {k: curves[k] + d for k in KEYS}
        print(f"  Gesamtklang Runde {rnd + 1}: mittlere Abweichung {np.mean(np.abs(d[(BANDS > 80) & (BANDS < 12000)])):.1f} dB")

    # 5) loudness: the progression should sit around the reference level
    y = eng.render(model_text(curves, unrest, er, late), prog, seed=3, tail=4)
    out = 10 ** ((10 * np.log10((ref[:, sus] ** 2).mean()) - 10 * np.log10((y[:, sus] ** 2).mean())) / 20)
    open(a.out, "w").write(model_text(curves, unrest, er, late, out))
    print(f"Modell geschrieben: {a.out} ({os.path.getsize(a.out) / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
