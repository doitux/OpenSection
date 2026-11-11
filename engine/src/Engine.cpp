// OpenSection – computed string-section instrument
// Copyright (C) 2026 OpenSection contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Signal flow
//   note -> k virtual players (each: learned wavetable oscillator with own detune, drift,
//           vibrato, attack, bow changes, bow-pressure unrest, rosin noise)
//        -> one of 4 "desks" of its register (bass / cello / viola / violin)
//   desk -> its own instrument body (convolution, ~100 narrow resonances)
//        -> close mics (pan) + room mics (own early reflections, decorrelated L/R)
//   all  -> hall (FDN)
// The cost of bodies, room and hall is fixed; each player costs ~25 operations per sample.
// Internal block: 128 samples (kChunk). No added latency – the engine renders its own input.
#include "opensection/Engine.h"
#include "Convolver.h"
#include "Hall.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>
#include <array>

namespace opensection {

namespace {

constexpr float kPadLevel = 5.0f;               // synth pad level at Pad = 1 (about as loud as the strings in their room)
constexpr float kPadHall = 1.0f;                // how much of the pad goes into the hall
constexpr float kNearMix = 0.5f;                // level of the desk-bound room part (both mics summed)
constexpr int kDesks = 4;                       // desks (body + seat) per register
constexpr int kBuses = kNumRegisters * kDesks;
constexpr int kMaxVoices = 56;                // 48 playing + 8 spare slots for click-free stealing
constexpr int kSoftVoices = 48;
constexpr int kMaxPlayers = 14;                 // per note at quality 1
constexpr int kTableBits = 11, kTableLen = 1 << kTableBits;
constexpr int kKeyLo = 21, kKeyHi = 108;

struct RegisterInfo { int topKey; int players; float bodyScale; float seat; };
constexpr RegisterInfo kReg[kNumRegisters] = {
    { 43, 6, 0.30f, 0.60f },    // basses     (right)
    { 55, 8, 0.45f, 0.50f },    // celli
    { 67, 10, 0.82f, 0.10f },   // violas
    { 999, 14, 1.00f, -0.40f }, // violins    (left)
};

// Which sections play a key: every section has its real range (sounding MIDI notes) and a home
// range where it plays fully; in between the weight fades linearly, so neighbouring sections
// share the note (players are split) and nothing jumps from one key to the next.
struct SectionRange { int lowest, fullFrom, fullTo, highest; };
constexpr SectionRange kRange[kNumRegisters] = {
    { 0,   0,   38, 48 },     // basses:  everything low, fully up to D2, gone at C3
    { 36,  43,  55, 67 },     // celli:   from C2, fully G2..G3, gone at G4
    { 48,  55,  64, 76 },     // violas:  from C3, fully G3..E4, gone at E5
    { 55,  64, 127, 128 },    // violins: from G3, fully from E4
};

std::array<float, kNumRegisters> sectionWeights(int key)
{
    std::array<float, kNumRegisters> w {};
    float sum = 0;
    for (int r = 0; r < kNumRegisters; ++r) {
        const auto& R = kRange[r];
        float x = 0;
        if (key >= R.lowest && key < R.highest) {
            x = 1.f;
            if (key < R.fullFrom) x = float(key - R.lowest + 1) / float(R.fullFrom - R.lowest + 1);
            if (key > R.fullTo) x = std::min(x, float(R.highest - key) / float(R.highest - R.fullTo));
        }
        w[r] = x; sum += x;
    }
    if (sum <= 0) { w[key < 60 ? 0 : 3] = 1.f; sum = 1.f; }
    for (auto& x : w) x /= sum;
    return w;
}

inline uint32_t xorshift(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
inline float urand(uint32_t& s) { return (xorshift(s) >> 8) * (1.f / 16777216.f); }          // 0..1
inline float srand1(uint32_t& s) { return urand(s) * 2.f - 1.f; }                          // -1..1
inline float gauss(uint32_t& s) { float a = 0; for (int i = 0; i < 4; ++i) a += urand(s); return (a - 2.f) * 1.7320508f; }

// slow random walk: white noise through a 2-pole lowpass at control rate, unit std
struct Wander {
    float b0 = 0, a1 = 0, a2 = 0, x1 = 0, y1 = 0, y2 = 0, norm = 1;
    void setup(float fc, float rate)
    {
        const float w = 2.f * (float)M_PI * fc / rate, q = 0.7071f;
        const float al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
        // measure the output deviation once (deterministic)
        uint32_t s = 12345; double acc = 0; float sx1 = 0, sx2 = 0, sy1 = 0, sy2 = 0;
        const int n = 60000;
        for (int i = 0; i < n; ++i) {
            float x = gauss(s);
            float y = b0 * x + 2 * b0 * sx1 + b0 * sx2 - a1 * sy1 - a2 * sy2;
            sx2 = sx1; sx1 = x; sy2 = sy1; sy1 = y;
            if (i > 2000) acc += (double)y * y;
        }
        norm = 1.f / (float)std::sqrt(acc / (n - 2000));
    }
};
struct WanderState {
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float next(const Wander& w, uint32_t& s)
    {
        float x = gauss(s);
        float y = w.b0 * x + 2 * w.b0 * x1 + w.b0 * x2 - w.a1 * y1 - w.a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y * w.norm;
    }
    void warm(const Wander& w, uint32_t& s, int n) { for (int i = 0; i < n; ++i) next(w, s); }
};

struct Biquad {                                       // RBJ cookbook, direct form I
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void set(int type, float f, float gainDb, float q, double sr)
    {   // type 0 low shelf, 1 bell, 2 high shelf
        const float A = std::pow(10.f, gainDb / 40.f), w = 2.f * (float)M_PI * f / (float)sr;
        const float cw = std::cos(w), sw = std::sin(w), al = sw / (2.f * q), sA = 2.f * std::sqrt(A) * al;
        float B0, B1, B2, A0, A1, A2;
        if (type == 1) { B0 = 1 + al * A; B1 = -2 * cw; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * cw; A2 = 1 - al / A; }
        else if (type == 0) { B0 = A * ((A + 1) - (A - 1) * cw + sA); B1 = 2 * A * ((A - 1) - (A + 1) * cw); B2 = A * ((A + 1) - (A - 1) * cw - sA);
                              A0 = (A + 1) + (A - 1) * cw + sA; A1 = -2 * ((A - 1) + (A + 1) * cw); A2 = (A + 1) + (A - 1) * cw - sA; }
        else { B0 = A * ((A + 1) + (A - 1) * cw + sA); B1 = -2 * A * ((A - 1) + (A + 1) * cw); B2 = A * ((A + 1) + (A - 1) * cw - sA);
               A0 = (A + 1) - (A - 1) * cw + sA; A1 = 2 * ((A - 1) - (A + 1) * cw); A2 = (A + 1) - (A - 1) * cw - sA; }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    float process(float x)
    {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    void clear() { x1 = x2 = y1 = y2 = 0; }
};

struct HallType { float rt, hf, late, er, pre; };
constexpr HallType kHallType[4] = {
    { 1.00f, 1.0f, 1.00f, 1.00f, 0.025f },   // learned hall
    { 0.55f, 1.1f, 0.80f, 0.90f, 0.012f },   // chamber: shorter, a little brighter
    { 1.90f, 0.8f, 1.30f, 1.15f, 0.040f },   // church: long and dark
    { 0.40f, 1.0f, 0.45f, 0.60f, 0.008f },   // studio: close and dry
};

struct Player {
    int bus = 0;
    double phase = 0;
    float det = 0;                                    // cents
    float vibRate = 5, vibDepth = 10, vibPhase = 0, vibOn = 0.4f;
    float start = 0, att = 0.5f;
    float nextChange = 1e9f, lastChange = -1e9f;
    float scoop = 0;                                  // intonation at the entry, settles quickly (cents)
    WanderState drift, rateJit, slowLvl, unrest, press;
    int reg = 0;                                      // section this player belongs to
    float hpX = 0, hpY = 0, lpY = 0;                  // rosin-noise filters
    float inc = 0, amp = 0, mix = 0, noise = 0;       // values at chunk start
    int variant = 0;                                  // which of the two irregular table variants
    float tremRate = 9.f;                             // own tremolo speed
    uint32_t rng = 1;
};

struct Voice {
    bool active = false, released = false, sustained = false;
    int key = 0, reg = 0, nPlayers = 0;
    float vel = 0.7f, rel = 1.f, steal = 1.f;
    bool stealing = false;
    bool dbl = false;                                 // octave doubling of the lowest note (basses only)
    int art = 0;                                      // articulation at note-on (2 tremolo, 3 pizzicato)
    double t = 0;                                     // seconds since note-on
    uint64_t age = 0;
    // the section listens to itself: part of the vibrato is shared, and every note is a bit different
    float secPhase = 0, secRate = 5.f, noteGain = 1.f, noteTone = 0.f;
    std::array<Player, kMaxPlayers> pl;
    // synth pad: four slightly detuned soft oscillators (two per side) that drift slowly against each other
    float padEnv = 0.f, padLpL = 0.f, padLpR = 0.f, padT = 0.f;
    std::array<double, 4> padPh {};
    std::array<float, 4> padDet {}, padLfo {}, padRate {};
    float padFlt = 0.f, padFltRate = 0.07f;
};

constexpr int kErTaps = 20;
struct Desk {
    Convolver body;
    float panL = 0.7f, panR = 0.7f;
    int dDelL = 0, dDelR = 0;                         // arrival time at the left/right mic (spaced pair + row)
    bool back = false;                                // back desk: further away, darker, more room
    float lpA = 1.f, lpL = 0.f, lpR = 0.f, dryGain = 1.f, erScale = 1.f;
    float mixPan = 0.f, mixDepth = 0.f;               // own seat when the sections are mixed through the room
    float erUnsideL = 1.f, erUnsideR = 1.f;           // undo the room mics' side weighting
    std::array<int, kErTaps> erDelL {}, erDelR {};
    std::array<float, kErTaps> erGainL {}, erGainR {};
    std::array<float, kErTaps> erBaseL {}, erBaseR {}; // reflections without the side the desk sits on
    std::vector<float> ring;                          // body output history for reflections
    int ringPos = 0, ringMask = 0;
    bool active = false;
};

std::vector<float> makeBody(float scale, float direct, std::mt19937& rng, double sr)
{
    std::uniform_real_distribution<float> U(0.f, 1.f);
    std::vector<float> freqs;
    float f = 170.f * scale;
    while (f < 11000.f * scale && f < 0.45f * (float)sr) {
        const float dens = f < 1000.f * scale ? 1.f / 70.f : 1.f / 45.f;
        f += -std::log(1.f - U(rng)) / dens;
        freqs.push_back(f);
    }
    if (!freqs.empty()) freqs.pop_back();
    float maxTau = 0;
    struct M { float f, tau, a, ph; };
    std::vector<M> modes;
    for (float fr : freqs) {
        const float q = 18.f + 37.f * U(rng);
        // low modes ring shorter than physics would say: keeps the body light enough to play live
        const float tau = std::min(q / ((float)M_PI * fr), 0.025f);
        const float dens = fr < 1000.f * scale ? 1.f / 70.f : 1.f / 45.f;
        const float ray = std::sqrt(-2.f * std::log(1.f - U(rng) * 0.9999f));
        const float a = ray / std::sqrt(dens * tau * 2.f * (float)M_PI * fr) * std::sqrt(fr);
        modes.push_back({ fr, tau, a, U(rng) * 6.2831853f });
        maxTau = std::max(maxTau, tau);
    }
    const int n = (int)std::min(0.045 * sr, 5.0 * maxTau * sr) + 1;
    std::vector<float> ir((size_t)n, 0.f);
    for (const auto& m : modes) {
        const double w = 2 * M_PI * m.f / sr, d = std::exp(-1.0 / (m.tau * sr));
        double env = m.a, ph = m.ph;
        for (int i = 0; i < n; ++i) { ir[i] += (float)(env * std::sin(ph)); env *= d; ph += w; }
    }
    float peak = 0; for (float v : ir) peak = std::max(peak, std::fabs(v));
    ir[0] += direct * peak;                                    // direct, non-resonant part (learned)
    double e = 0; for (float v : ir) e += (double)v * v;
    const float g = (float)(1.0 / std::sqrt(e + 1e-20));
    for (auto& v : ir) v *= g;
    // trim the silent end (-60 dB energy)
    double tail = 0; size_t cut = ir.size();
    for (size_t i = ir.size(); i-- > 0;) { tail += (double)ir[i] * ir[i]; if (tail > 1e-6) { cut = i + 1; break; } }
    ir.resize(std::max<size_t>(cut, 64));
    return ir;
}

} // namespace

struct Engine::Impl {
    Model model;
    Params p, pTarget;
    double sr = 48000;
    float ctrlRate = 750;
    uint32_t seed;
    uint64_t ageCounter = 0;

    std::vector<std::vector<float>> tabs;             // one table set per bow pressure: [key][kTableLen+1]
    std::vector<float> tabQ;                          // bow pressure of each set, ascending
    std::vector<float> padTab;                        // synth pad wavetable per key: soft, dark, no edges
    float padL[kChunk], padR[kChunk];
    size_t nVar = 1;                                  // irregular variants per set (tabs[set * nVar + variant])
    std::array<float, 128> keyGain {};                 // table RMS relative to a plain saw
    Wander wDrift, wRate, wSlow, wUnrest, wFlutter;
    std::array<Voice, kMaxVoices> voices;
    std::array<Desk, kBuses> desks;
    Hall halls[4];
    int hallCur = 0, hallPrev = 0, hallTail = 0;     // hall type in use; the previous one rings out
    Biquad eq[2][3];
    Biquad mute[kBuses][2];                           // con sordino: the mute on the bridge
    float muteAmt = 0.f;
    float eqSet[3] = { 1e9f, 1e9f, 1e9f };
    int dblKey = -1;                                  // key of the doubling voice
    float zeros[kChunk] {};
    std::array<float, kNumRegisters> unrestDb {};
    float erGain = 0, lateGain = 0, releaseScale = 1.f;
    Diffuser erDiffL, erDiffR;
    bool sustain = false;
    float expression = 1.f, exprSmooth = 1.f, bend = 1.f;

    // chunk buffers
    std::array<std::array<float, kChunk>, kBuses> busIn {};
    float dryL[kChunk], dryR[kChunk], erL[kChunk], erR[kChunk], late[kChunk];
    float erLpL = 0, erLpR = 0, erLpA = 0;
    std::vector<float> fifoL, fifoR; int fifoN = 0;

    explicit Impl(const Model& m, uint32_t s) : model(m), seed(s) {}

    const float* table(const std::vector<float>& t, int key) const
    {
        key = std::clamp(key, kKeyLo, kKeyHi);
        return &t[(size_t)(key - kKeyLo) * (kTableLen + 1)];
    }

    void buildTables()
    {
        const int nKeys = kKeyHi - kKeyLo + 1;
        Fft fft(kTableLen);
        std::vector<float> re(kTableLen), im(kTableLen);
        const float fmax = std::min(18000.f, 0.45f * (float)sr / 1.15f);    // room for vibrato + bend
        // table sets: analytic (pressure 0.3 and 1.0) or one per learned dynamic level
        struct Lvl { float q; int node; };                 // node -1: the learned (loud) colour itself
        std::vector<Lvl> lv;
        if (!model.dynNodes.empty()) {
            lv.push_back({ model.dynLoud, -1 });
            for (size_t i = 0; i < model.dynNodes.size(); ++i) lv.push_back({ model.dynNodes[i].vel, (int)i });
            std::sort(lv.begin(), lv.end(), [](const Lvl& a, const Lvl& b) { return a.q < b.q; });
        } else lv = { { 0.3f, -2 }, { 1.0f, -3 } };
        tabQ.clear(); for (auto& l : lv) tabQ.push_back(l.q);
        nVar = model.harmVar > 0.f ? 2 : 1;
        tabs.assign(lv.size() * nVar, std::vector<float>((size_t)nKeys * (kTableLen + 1), 0.f));
        const float m0 = std::clamp((model.dynLoud - 0.3f) / 0.7f, 0.f, 1.f);
        std::vector<float> amp;
        for (int key = kKeyLo; key <= kKeyHi; ++key) {
            const float f0 = 440.f * std::pow(2.f, (key - 69) / 12.f);
            const int nh = std::min(kTableLen / 2 - 1, (int)(fmax / f0));
            double eRef = 0;
            for (size_t t = 0; t < lv.size() * nVar; ++t) {
                const size_t lvl = t / nVar;
                // bow irregularity: every variant gets its own small per-harmonic deviations
                std::mt19937 vr((uint32_t)(key * 7919 + (int)t * 104729 + 17));
                std::normal_distribution<float> N01(0.f, 1.f);
                amp.assign(nh + 1, 0.f);
                for (int h = 1; h <= nh; ++h) {
                    const float f = h * f0;
                    const float base = (2.f / (float)M_PI) / h * std::pow(10.f, model.colourDb(key, f) / 20.f);
                    // analytic tables: soft = bow pressure 0.3, hard = 1.0 (gentle low-pass tilt)
                    const float aS = base / std::sqrt(1.f + std::pow(f / 4300.f, 4.f));
                    const float aH = base / std::sqrt(1.f + std::pow(f / 8500.f, 4.f));
                    if (t == 0) eRef += 0.5 * (double)aH * aH;
                    const float c = aS + m0 * (aH - aS);           // what the loud colour was learned with
                    const int node = lv[lvl].node;
                    amp[h] = node == -2 ? aS : node == -3 ? aH : node == -1 ? c
                           : c * std::pow(10.f, std::clamp(model.dynDb((size_t)node, key, f), -40.f, 8.f) / 20.f);
                    // (mean power stays the same: the log-normal spread would otherwise add ~sigma^2/4.3 dB)
                    if (nVar > 1 && h > 1)
                        amp[h] *= std::pow(10.f, (model.harmVar * N01(vr) - model.harmVar * model.harmVar * 0.11513f) / 20.f);
                }
                std::fill(re.begin(), re.end(), 0.f); std::fill(im.begin(), im.end(), 0.f);
                for (int h = 1; h <= nh; ++h) { im[h] = -amp[h] / 2.f; im[kTableLen - h] = amp[h] / 2.f; }
                fft.transform(re.data(), im.data(), true);
                float* dst = &tabs[t][(size_t)(key - kKeyLo) * (kTableLen + 1)];
                for (int i = 0; i < kTableLen; ++i) dst[i] = re[i];
                dst[kTableLen] = dst[0];
            }
            keyGain[key] = (float)std::sqrt(eRef) / 0.57735f;            // rosin noise follows the loud colour
        }
    }

    // the pad: a soft harmonic series (odd harmonics a little stronger, falling 1/n^1.3) behind a gentle
    // 12 dB/oct low-pass at 2.5 x f0 (260 Hz .. 1.1 kHz) - round like a filtered synth pad, no buzz
    void buildPadTables()
    {
        const int nKeys = kKeyHi - kKeyLo + 1;
        padTab.assign((size_t)nKeys * (kTableLen + 1), 0.f);
        const float fmax = std::min(12000.f, 0.45f * (float)sr / 1.03f);
        std::vector<double> acc(kTableLen);
        for (int key = kKeyLo; key <= kKeyHi; ++key) {
            const float f0 = 440.f * std::pow(2.f, (key - 69) / 12.f);
            const float fc = std::clamp(2.5f * f0, 260.f, 1100.f);
            std::fill(acc.begin(), acc.end(), 0.0);
            for (int h = 1; h * f0 < fmax; ++h) {
                const float f = h * f0, r = f / fc;
                const float a = std::pow((float)h, -1.3f) * (h % 2 ? 1.f : 0.7f) / std::sqrt(1.f + r * r * r * r);
                if (a < 2e-4f) break;
                for (int i = 0; i < kTableLen; ++i) acc[i] += a * std::sin(2.0 * M_PI * h * i / kTableLen);
            }
            double e = 0; for (double x : acc) e += x * x;
            const float g = (float)(0.7071 / std::sqrt(e / kTableLen + 1e-20));
            float* t = &padTab[(size_t)(key - kKeyLo) * (kTableLen + 1)];
            for (int i = 0; i < kTableLen; ++i) t[i] = g * (float)acc[i];
            t[kTableLen] = t[0];
        }
    }

    void buildDesks()
    {
        std::mt19937 rng(seed * 7919u + 11u);
        std::uniform_real_distribution<float> U(0.f, 1.f);
        int ringLen = 1; while (ringLen < (int)(0.12 * sr)) ringLen <<= 1;
        for (int r = 0; r < kNumRegisters; ++r) {
            for (int k = 0; k < kDesks; ++k) {
                Desk& d = desks[r * kDesks + k];
                d.body.setIR(makeBody(kReg[r].bodyScale * (1.f + model.bodySpread * (2.f * U(rng) - 1.f)), model.bodyDirect[r], rng, sr));
                d.ring.assign(ringLen, 0.f); d.ringMask = ringLen - 1; d.ringPos = 0;
                d.back = (k % 2) == 1;                                   // every second desk sits further back
                for (int c = 0; c < 2; ++c) {
                    for (int t = 0; t < kErTaps; ++t) {
                        const float dl = 0.004f + 0.086f * U(rng) * U(rng) + 0.01f * U(rng);   // denser early, sparser late
                        const float g = (0.3f + 0.4f * U(rng)) * (U(rng) < 0.5f ? -1.f : 1.f) * std::exp(-dl / 0.035f);
                        (c == 0 ? d.erDelL : d.erDelR)[t] = (int)(dl * sr);
                        (c == 0 ? d.erBaseL : d.erBaseR)[t] = g;
                    }
                }
                {   // the random taps must not lean to one side: same energy left and right (the side the desk
                    // sits on is added in updatePans)
                    float eL = 0, eR = 0;
                    for (int t = 0; t < kErTaps; ++t) { eL += d.erBaseL[t] * d.erBaseL[t]; eR += d.erBaseR[t] * d.erBaseR[t]; }
                    const float m = std::sqrt(0.5f * (eL + eR));
                    for (int t = 0; t < kErTaps; ++t) { d.erBaseL[t] *= m / std::sqrt(eL); d.erBaseR[t] *= m / std::sqrt(eR); }
                }
            }
        }
        // mixed seating: every desk gets its own place anywhere in the room. Stratified: each section has
        // one desk in every quarter of the width and one in every depth row, so a section is spread over
        // the whole room instead of landing by chance on one side; all 16 places are different.
        std::mt19937 srng(seed * 104729u + 7u);
        auto perm4 = [&] { std::array<int, kDesks> a { 0, 1, 2, 3 }; std::shuffle(a.begin(), a.end(), srng); return a; };
        std::array<std::array<int, kDesks>, kNumRegisters> qx, qy;       // section -> desk -> quarter / row
        std::array<std::array<int, kNumRegisters>, kDesks> sx, sy;       // quarter -> section -> slot inside it
        for (int r = 0; r < kNumRegisters; ++r) {
            qx[r] = perm4();
            // balance: of the two desks in the left half one sits in front (rows 0/1) and one behind (rows
            // 2/3), the same on the right - otherwise the louder front desks could pull a section to one side
            const bool fl = srng() & 1u, bl = srng() & 1u;
            for (int k = 0; k < kDesks; ++k) {
                const bool left = qx[r][k] < 2, first = (qx[r][k] % 2) == 0;
                const bool front = first == (left ? fl : !fl);
                qy[r][k] = front ? ((left == bl) ? 0 : 1) : ((left == bl) ? 2 : 3);
            }
        }
        for (int q = 0; q < kDesks; ++q) { sx[q] = perm4(); sy[q] = perm4(); }
        for (int r = 0; r < kNumRegisters; ++r)
            for (int k = 0; k < kDesks; ++k) {
                Desk& d = desks[r * kDesks + k];
                const int slotX = qx[r][k] * kNumRegisters + sx[qx[r][k]][r], slotY = qy[r][k] * kNumRegisters + sy[qy[r][k]][r];
                d.mixPan = -0.95f + 1.9f * (slotX + 0.5f) / kBuses;
                d.mixDepth = 1.25f * (slotY + 0.5f) / kBuses;   // front row .. a bit behind the back desks
            }
        updatePans();
    }

    void updatePans()
    {
        for (int r = 0; r < kNumRegisters; ++r)
            for (int k = 0; k < kDesks; ++k) {
                Desk& d = desks[r * kDesks + k];
                // seating: below 0.5 every section closes up into a tight block around its seat (a group of
                // about 3 m instead of the whole stage width), above 0.5 the desks move to their own places
                // anywhere in the room - left, right, front, back - whatever section they belong to
                const float s = std::clamp(p.seating, 0.f, 1.f);
                const float spreadK = s < 0.5f ? 0.12f + (0.75f - 0.12f) * (s / 0.5f) : 0.75f;
                const float m = s > 0.5f ? (s - 0.5f) / 0.5f : 0.f;
                const float orch = kReg[r].seat * std::min(1.f, p.width) + (k - 1.5f) / 1.5f * spreadK * p.width;
                const float pan = std::clamp(orch + m * (d.mixPan * p.width - orch), -1.f, 1.f);
                // distance: 0 front row, 1 the back desk of the orchestra layout (7 ms later, -2.5 dB, darker,
                // more room), up to 1.25 when mixed
                const float depth = (d.back ? 1.f : 0.f) + m * (d.mixDepth - (d.back ? 1.f : 0.f));
                d.dryGain = 1.f - 0.25f * depth;
                d.erScale = 1.f + 0.6f * depth;
                d.lpA = depth > 0.f ? 1.f - std::exp(-2.f * (float)M_PI * std::max(3500.f, 20000.f * std::pow(0.325f, depth)) / (float)sr) : 1.f;
                for (int t = 0; t < kErTaps; ++t) {                     // the room mic on the desk's side hears it louder
                    d.erGainL[t] = (1.f - 0.5f * pan) * d.erBaseL[t];
                    d.erGainR[t] = (1.f + 0.5f * pan) * d.erBaseR[t];
                }
                d.erUnsideL = 1.f / (1.f - 0.5f * pan); d.erUnsideR = 1.f / (1.f + 0.5f * pan);
                // mild level panning + time difference like a spaced microphone pair (up to ~1.1 ms),
                // back desks arrive a little later: separate sources in a room, not phantom speakers
                const float lvl = 0.6f * pan;
                d.panL = std::cos((lvl + 1.f) * (float)M_PI / 4.f) * d.dryGain;
                d.panR = std::sin((lvl + 1.f) * (float)M_PI / 4.f) * d.dryGain;
                const float itd = 0.0011f * model.spacing * p.spacing * (float)sr, row = 0.007f * depth * (float)sr;
                d.dDelL = (int)(row + itd * std::max(pan, 0.f));
                d.dDelR = (int)(row + itd * std::max(-pan, 0.f));
            }
    }

    // ------------------------------------------------------------------ notes
    void noteOn(int key, float vel)
    {
        for (auto& v : voices)                                  // same key: the old bow stops
            if (v.active && v.key == key && !v.released && !v.dbl) { v.released = true; v.rel = std::min(v.rel, 1.f); }
        int playing = 0;
        for (auto& c : voices) playing += c.active && !c.stealing;
        if (playing >= kSoftVoices) {                           // fade out the oldest (released first)
            Voice* best = nullptr;
            for (auto& c : voices) {
                if (!c.active || c.stealing) continue;
                if (!best || (c.released && !best->released) || (c.released == best->released && c.age < best->age)) best = &c;
            }
            if (best) best->stealing = true;
        }
        Voice* v = nullptr;
        for (auto& c : voices) if (!c.active) { v = &c; break; }
        if (!v) {                                               // all slots busy: take the most faded
            for (auto& c : voices) if (c.stealing && (!v || c.steal < v->steal)) v = &c;
            if (!v) return;
        }
        startVoice(*v, key, std::clamp(vel, 0.05f, 1.f));
    }

    Voice* freeVoice()
    {
        for (auto& c : voices) if (!c.active) return &c;
        Voice* v = nullptr;
        for (auto& c : voices) if (c.stealing && (!v || c.steal < v->steal)) v = &c;
        return v;
    }

    // basses double the lowest sounding note an octave below (as in a real string arrangement)
    void updateDouble()
    {
        int low = 999; float vel = 0.7f;
        if (p.bassDouble > 0.f)
            for (auto& v : voices)
                if (v.active && !v.released && !v.dbl && v.key < low) { low = v.key; vel = v.vel; }
        const int want = (low <= 55 && low - 12 >= 24) ? low - 12 : -1;     // up to g, not below C1
        if (want == dblKey) return;
        for (auto& v : voices) if (v.active && v.dbl && !v.released) v.released = true;
        dblKey = want;
        if (want < 0) return;
        if (Voice* v = freeVoice()) { startVoice(*v, want, vel, true); v->dbl = true; }
    }

    void startVoice(Voice& v, int key, float vel, bool bassOnly = false)
    {
        v = Voice();
        v.active = true; v.key = key; v.vel = vel;
        v.art = bassOnly ? (p.articulation == 3 ? 3 : 0) : p.articulation;
        v.age = ++ageCounter;
        uint32_t s = seed * 2654435761u ^ (uint32_t)(v.age * 40503u + (uint32_t)key * 9176u);
        xorshift(s); xorshift(s);
        // sections sharing this key and their players (largest remainder, no lonely soloists)
        auto w = sectionWeights(key);
        if (bassOnly) w = { 1.f, 0.f, 0.f, 0.f };
        float base = 0;
        for (int r = 0; r < kNumRegisters; ++r) base += w[r] * kReg[r].players;
        v.nPlayers = std::clamp((int)std::lround(base * p.quality), 2, kMaxPlayers);
        int nSec[kNumRegisters] {}, given = 0;
        float rem[kNumRegisters] {};
        v.reg = 0;
        for (int r = 0; r < kNumRegisters; ++r) {
            nSec[r] = (int)std::floor(w[r] * v.nPlayers); rem[r] = w[r] * v.nPlayers - nSec[r]; given += nSec[r];
            if (w[r] > w[v.reg]) v.reg = r;
        }
        while (given < v.nPlayers) {
            int b = 0; for (int r = 1; r < kNumRegisters; ++r) if (rem[r] > rem[b]) b = r;
            ++nSec[b]; rem[b] = -1.f; ++given;
        }
        for (int r = 0; r < kNumRegisters; ++r)
            if (r != v.reg && nSec[r] == 1 && w[r] < 0.12f) { nSec[r] = 0; ++nSec[v.reg]; }
        if (p.solo) { v.nPlayers = 1; for (auto& n : nSec) n = 0; nSec[v.reg] = 1; }
        v.secRate = 4.9f + 0.9f * urand(s);
        v.secPhase = 6.2831853f * urand(s);
        v.noteGain = std::pow(10.f, 1.2f * gauss(s) / 20.f) * (bassOnly ? p.bassDouble : 1.f);
        v.noteTone = 0.07f * gauss(s);
        int deskOrder[kDesks] = { 0, 1, 2, 3 };
        for (int i = kDesks - 1; i > 0; --i) std::swap(deskOrder[i], deskOrder[xorshift(s) % (i + 1)]);
        for (int i = 0, r = 0, inSec = 0; i < v.nPlayers; ++i, ++inSec) {
            while (inSec >= nSec[r]) { ++r; inSec = 0; }
            Player& pl = v.pl[i];
            pl = Player();
            pl.rng = xorshift(s) | 1u;
            pl.reg = r;
            pl.bus = r * kDesks + deskOrder[(inSec + r) % kDesks];
            pl.phase = urand(pl.rng);
            pl.det = gauss(pl.rng) * 6.f;
            pl.vibRate = 4.8f + 1.2f * urand(pl.rng);
            pl.vibDepth = 7.f + 13.f * urand(pl.rng);
            pl.vibPhase = 6.2831853f * urand(pl.rng);
            pl.vibOn = 0.25f + 0.45f * urand(pl.rng);
            pl.start = (0.03f + 0.07f * (1.f - vel)) * urand(pl.rng);       // soft notes: looser entries
            pl.scoop = 9.f * gauss(pl.rng) - 3.f;
            pl.att = 0.35f + 0.45f * urand(pl.rng);
            pl.nextChange = pl.start + 1.5f + 3.f * urand(pl.rng);
            pl.drift.warm(wDrift, pl.rng, 40); pl.rateJit.warm(wRate, pl.rng, 20);   // short warm-up: cheap note-on
            pl.slowLvl.warm(wSlow, pl.rng, 20); pl.unrest.warm(wUnrest, pl.rng, 10); pl.press.warm(wFlutter, pl.rng, 10);
            pl.variant = i & 1;
            pl.tremRate = model.tremRate[r] * (1.f + 0.12f * gauss(pl.rng));
            if (v.art == 2) { pl.lastChange = pl.start; pl.nextChange = pl.start + urand(pl.rng) / pl.tremRate; }
            if (v.art == 3) { pl.start = 0.05f * urand(pl.rng); pl.vibDepth = 0.f; pl.scoop = 0.f; }   // plucked together
            controls(v, pl, 0.0, pl.inc, pl.amp, pl.mix, pl.noise);
        }
        {   // the section as a whole plays in tune: only the spread around the note remains
            float m = 0; for (int i = 0; i < v.nPlayers; ++i) m += v.pl[i].det;
            m /= (float)v.nPlayers;
            for (int i = 0; i < v.nPlayers; ++i) v.pl[i].det -= m;
        }
        static const float kPadDet[4] = { -7.f, 2.5f, -2.5f, 7.f };     // cents; left: 0 and 1, right: 2 and 3
        for (int o = 0; o < 4; ++o) {
            v.padPh[o] = urand(s);
            v.padDet[o] = kPadDet[o] + 1.5f * gauss(s);
            v.padLfo[o] = 6.2831853f * urand(s);
            v.padRate[o] = 0.05f + 0.07f * urand(s);                     // a full drift cycle every 8..20 s
        }
        v.padFlt = 6.2831853f * urand(s); v.padFltRate = 0.04f + 0.05f * urand(s);
    }

    void noteOff(int key)
    {
        for (auto& v : voices)
            if (v.active && v.key == key && !v.released && !v.sustained && !v.dbl && v.art != 3) {   // pizzicato rings out
                if (sustain) v.sustained = true; else v.released = true;
            }
    }

    void setSustain(bool on)
    {
        sustain = on;
        if (!on) for (auto& v : voices) if (v.active && v.sustained) { v.sustained = false; v.released = true; }
    }

    void updateEq()
    {
        const float want[3] = { p.eqLow, p.eqMid, p.eqHigh };
        for (int b = 0; b < 3; ++b) {
            if (std::fabs(want[b] - eqSet[b]) < 0.01f) continue;
            eqSet[b] = want[b];
            for (int c = 0; c < 2; ++c) eq[c][b].set(b == 0 ? 0 : b == 1 ? 1 : 2, b == 0 ? 200.f : b == 1 ? 1200.f : 6000.f, want[b], b == 1 ? 0.8f : 0.7071f, sr);
        }
    }

    // control values of one player at voice time t (seconds)
    void controls(const Voice& v, Player& pl, double t, float& inc, float& amp, float& mix, float& noise)
    {
        const float dt = 1.f / ctrlRate;
        const float drift = 2.5f * pl.drift.next(wDrift, pl.rng);
        const float rate = pl.vibRate * (1.f + 0.06f * pl.rateJit.next(wRate, pl.rng));
        pl.vibPhase += 6.2831853f * rate * dt;
        if (pl.vibPhase > 6.2831853f) pl.vibPhase -= 6.2831853f;
        const float ramp = std::clamp(((float)t - pl.vibOn) / 0.9f, 0.f, 1.f);
        const float kShared = p.coupling;                    // how much the players follow each other
        const float vib = (1.f - kShared) * std::sin(pl.vibPhase) + kShared * std::sin(v.secPhase);
        const float tIn = std::max(0.f, (float)t - pl.start);
        const float cents = p.detune * (pl.det + drift) + pl.scoop * std::exp(-tIn / 0.12f) + p.vibrato * pl.vibDepth * ramp * vib;
        const float f0 = 440.f * std::pow(2.f, (v.key - 69) / 12.f + cents / 1200.f) * bend;
        inc = f0 / (float)sr;

        // envelope: own entry, soft bow attack, bow changes, unrest, release
        const float ts = (float)t - pl.start;
        // hard keys: the bow bites faster (like sampled sections: attack shrinks with velocity)
        const float velAtt = 1.f + p.velocity * (0.71f - 1.0f * v.vel);           // 0.71 -> 1.0, 1.0 -> 0.7, 0.2 -> 1.5
        const float att = pl.att * p.attack * std::max(0.25f, velAtt);
        float x = std::clamp(ts / att, 0.f, 1.f);
        float env = x * x * (3.f - 2.f * x);
        const bool pizz = v.art == 3, trem = v.art == 2;
        if (pizz) {                                           // plucked: fast onset, free decay
            const float tau = model.pizzDecay[pl.reg] * std::pow(2.f, -(v.key - 48) / 36.f);   // higher notes ring shorter
            env = ts < 0 ? 0.f : 1.6f * std::min(1.f, ts / 0.003f) * std::exp(-ts / tau);   // a pluck starts louder than a bowed note
        }
        if ((float)t >= pl.nextChange && !v.released && !pizz) {
            pl.lastChange = pl.nextChange;
            pl.nextChange += trem ? (0.85f + 0.3f * urand(pl.rng)) / pl.tremRate : 2.5f + 2.5f * urand(pl.rng);
        }
        const float dc = (float)t - pl.lastChange;
        if (trem) {                                           // tremolo: a quick bow change every stroke
            const float w = model.tremWidth / pl.tremRate;
            env *= 1.3f * (1.f - model.tremDepth[pl.reg] * std::exp(-(dc / w) * (dc / w)));   // same loudness as arco on average
        } else if (!pizz)
            env *= 1.f - 0.45f * std::exp(-(dc / 0.035f) * (dc / 0.035f));
        env *= std::pow(10.f, (1.0f * pl.slowLvl.next(wSlow, pl.rng)
                               + p.unrest * unrestDb[pl.reg] * pl.unrest.next(wUnrest, pl.rng)) / 20.f);
        // velocity: mostly tone, a little level (normalised to the learning velocity 0.71)
        // velocity -> level like a sampled section (sfz amp_veltrack 100: gain = vel^2),
        // normalised to the learning velocity 0.71; p.velocity blends towards no dynamics
        const float velAmp = std::pow(std::max(v.vel, 0.02f) / 0.85f, 2.f * p.velocity);   // headroom for hard chords
        const float ex = exprSmooth;
        const float exAmp = ex * std::sqrt(ex);                  // pedal fully up = silent (fade pads in/out)
        amp = env * velAmp * exAmp * v.noteGain * v.rel * v.steal / std::sqrt((float)v.nPlayers);

        const float pressure = std::clamp(v.vel * (0.45f + 0.55f * ex), 0.f, 1.f);
        // bow pressure -> table sets; the bow never presses evenly: a little flutter on the colour
        const float flut = model.flutter[pl.reg] * p.unrest * pl.press.next(wFlutter, pl.rng);
        mix = std::clamp(pressure + 0.7f * v.noteTone + flut, tabQ.front(), tabQ.back());
        float scratch = 0.25f;
        if (pizz) {                                           // the brilliance of the pluck fades faster than the tone
            const float tau = model.pizzDecay[pl.reg] * std::pow(2.f, -(v.key - 48) / 36.f) * model.pizzBright;
            const float q0 = std::min(tabQ.back(), 0.45f + 0.55f * v.vel);
            mix = std::clamp(tabQ.front() + (q0 - tabQ.front()) * std::exp(-std::max(ts, 0.f) / tau), tabQ.front(), tabQ.back());
            scratch = ts >= 0 ? 2.5f * v.vel * std::exp(-ts / 0.006f) : 0.f;      // finger noise only at the pluck
        } else {
            if (ts >= 0) scratch += 1.4f * v.vel * std::exp(-ts / 0.07f);
            if (dc >= 0) scratch += 0.8f * v.vel * std::exp(-dc / (trem ? 0.02f : 0.05f));
        }
        noise = p.rosin * model.rosin[pl.reg] * (p.articulation == 1 ? 0.6f : 1.f) * scratch * 1.7320508f * keyGain[std::clamp(v.key, kKeyLo, kKeyHi)];
    }

    // ------------------------------------------------------------------ render one 64-sample chunk
    // synth pad for one voice: slow swell (follows Attack/Release, slower than the bow), level from
    // velocity (gently) and the expression pedal, four detuned oscillators beating slowly against
    // each other, a low-pass that breathes very slowly
    void renderPad(Voice& v, float chunkDur)
    {
        const float target = v.released ? 0.f : 1.f;
        const float tau = v.released ? 0.75f * p.release : 0.5f * p.attack;
        const float e0 = v.padEnv;
        v.padEnv = target + (e0 - target) * std::exp(-chunkDur / std::max(0.02f, tau));
        if (e0 < 1e-5f && v.padEnv < 1e-5f) return;
        v.padT += chunkDur;
        const float f0 = 440.f * std::pow(2.f, (v.key - 69) / 12.f) * bend;
        const float lvl = kPadLevel * p.pad * std::sqrt(v.vel) * (0.35f + 0.65f * exprSmooth) * (v.stealing ? v.steal : 1.f);
        const float* t = &padTab[(size_t)(std::clamp(v.key, kKeyLo, kKeyHi) - kKeyLo) * (kTableLen + 1)];
        double inc[4];
        for (int o = 0; o < 4; ++o) {
            v.padLfo[o] += 6.2831853f * v.padRate[o] * chunkDur;
            const float cents = v.padDet[o] + 3.f * std::sin(v.padLfo[o]);
            inc[o] = f0 * std::pow(2.f, cents / 1200.f) / sr;
        }
        v.padFlt += 6.2831853f * v.padFltRate * chunkDur;
        const float fc = std::clamp(2.2f * f0, 220.f, 1000.f) * (1.f + 0.25f * std::sin(v.padFlt)) * (0.75f + 0.5f * exprSmooth);
        const float a = 1.f - std::exp(-2.f * (float)M_PI * std::min(fc, 0.2f * (float)sr) / (float)sr);
        const float k = 1.f / kChunk, dE = (v.padEnv - e0) * k;
        float env = e0, lpL = v.padLpL, lpR = v.padLpR;
        for (int n = 0; n < kChunk; ++n) {
            env += dE;
            float o4[4];
            for (int o = 0; o < 4; ++o) {
                double& ph = v.padPh[o]; ph += inc[o]; if (ph >= 1.0) ph -= 1.0;
                const float pos = (float)ph * kTableLen; const int i0 = (int)pos; const float fr = pos - i0;
                o4[o] = t[i0] + fr * (t[i0 + 1] - t[i0]);
            }
            // each side mostly its own pair, a little of the other: wide but mono-safe (no phase tricks)
            const float l = o4[0] + o4[1] + 0.35f * (o4[2] + o4[3]), r = o4[2] + o4[3] + 0.35f * (o4[0] + o4[1]);
            lpL += a * (l - lpL); lpR += a * (r - lpR);
            const float g = lvl * env * env;                     // squared: an even softer start
            padL[n] += g * lpL; padR[n] += g * lpR;
        }
        v.padLpL = lpL; v.padLpR = lpR;
    }

    void renderChunk(float* outL, float* outR)
    {
        for (auto& b : busIn) std::memset(b.data(), 0, sizeof(float) * kChunk);
        std::memset(padL, 0, sizeof padL); std::memset(padR, 0, sizeof padR);
        exprSmooth += (expression - exprSmooth) * 0.08f;
        const bool padOn = p.pad > 0.f;
        if (!padOn) for (auto& v : voices) v.padEnv = 0.f;
        const float chunkDur = kChunk / (float)sr;
        const float hpA = std::exp(-2.f * (float)M_PI * 2000.f / (float)sr);
        for (auto& v : voices) {
            if (!v.active) continue;
            // rosin noise gets darker with less bow pressure; below the learning velocity the
            // learned corner (per register) is approached exponentially
            const float pressure = v.vel * (0.45f + 0.55f * exprSmooth);
            float lpAReg[kNumRegisters];
            for (int r = 0; r < kNumRegisters; ++r) {
                float fcNoise = 2500.f + 6000.f * pressure;
                const float fcSoft = model.noiseSoftHz[r];
                if (fcSoft > 0.f && pressure < model.dynLoud) {
                    const float fcLoud = 2500.f + 6000.f * model.dynLoud;
                    fcNoise = fcLoud * std::pow(fcSoft / fcLoud, (model.dynLoud - pressure) / (model.dynLoud - model.dynSoft));
                    fcNoise = std::max(fcNoise, 400.f);
                }
                lpAReg[r] = std::exp(-2.f * (float)M_PI * fcNoise / (float)sr);
            }
            v.t += chunkDur;
            v.secPhase += 6.2831853f * v.secRate * chunkDur;
            if (v.secPhase > 6.2831853f) v.secPhase -= 6.2831853f;
            if (v.released) v.rel *= std::exp(-chunkDur / (v.art == 3 ? 0.12f * p.release : 0.22f * releaseScale * p.release));
            if (v.art == 3 && v.t > 9.0 * model.pizzDecay[v.reg]) v.released = true, v.rel = std::min(v.rel, 1e-5f);   // rung out
            if (v.stealing) v.steal *= std::exp(-chunkDur / 0.012f);
            const bool stringsGone = padOn && v.released && v.rel < 1e-4f;    // only the pad still rings
            for (int i = 0; i < (stringsGone ? 0 : v.nPlayers); ++i) {
                Player& pl = v.pl[i];
                float inc1, amp1, mix1, noi1;
                controls(v, pl, v.t, inc1, amp1, mix1, noi1);
                const float k = 1.f / kChunk;
                const float dInc = (inc1 - pl.inc) * k, dAmp = (amp1 - pl.amp) * k;
                // the two table sets around this player's bow pressure (chosen per chunk)
                size_t seg = 0;
                while (seg + 2 < tabQ.size() && mix1 > tabQ[seg + 1]) ++seg;
                const float qLo = tabQ[seg], qSpan = tabQ[seg + 1] - qLo;
                const float w0 = std::clamp((pl.mix - qLo) / qSpan, 0.f, 1.f), w1 = std::clamp((mix1 - qLo) / qSpan, 0.f, 1.f);
                const size_t va = (size_t)pl.variant % nVar, vb = nVar > 1 ? 1 - va : 0;   // different variants: harmonics move independently
                const float* ts = table(tabs[seg * nVar + va], v.key);
                const float* th = table(tabs[(seg + 1) * nVar + vb], v.key);
                const float dMix = (w1 - w0) * k, dNoi = (noi1 - pl.noise) * k;
                float inc = pl.inc, amp = pl.amp, mix = w0, noi = pl.noise;
                double ph = pl.phase;
                float hpX = pl.hpX, hpY = pl.hpY, lpY = pl.lpY;
                const float lpA = lpAReg[pl.reg];
                uint32_t rs = pl.rng;
                float* bus = busIn[pl.bus].data();
                for (int n = 0; n < kChunk; ++n) {
                    inc += dInc; amp += dAmp; mix += dMix; noi += dNoi;
                    ph += inc; if (ph >= 1.0) ph -= 1.0;
                    const float pos = (float)ph * kTableLen;
                    const int i0 = (int)pos; const float fr = pos - i0;
                    const float s0 = ts[i0] + fr * (ts[i0 + 1] - ts[i0]);
                    const float s1 = th[i0] + fr * (th[i0 + 1] - th[i0]);
                    // rosin: noise burst at the slip moment of each period
                    float x = 1.f - (float)ph; x *= x; x *= x; const float x8 = x * x;
                    const float nz = srand1(rs) * x8 * x * noi;
                    hpY = hpA * (hpY + nz - hpX); hpX = nz;              // highpass 2 kHz
                    lpY += (1.f - lpA) * (hpY - lpY);                    // bow-pressure lowpass
                    bus[n] += amp * (s0 + mix * (s1 - s0) + lpY);
                }
                pl.inc = inc1; pl.amp = amp1; pl.mix = mix1; pl.noise = noi1;
                pl.phase = ph; pl.hpX = hpX; pl.hpY = hpY; pl.lpY = lpY; pl.rng = rs;
            }
            if (padOn && !v.dbl && v.art != 3) renderPad(v, chunkDur);
            if ((v.released && v.rel < 1e-4f && v.padEnv < 1e-4f) || (v.stealing && v.steal < 1e-4f)) v.active = false;
        }

        // desks: body -> close mics + room mics
        std::memset(dryL, 0, sizeof dryL); std::memset(dryR, 0, sizeof dryR);
        std::memset(erL, 0, sizeof erL); std::memset(erR, 0, sizeof erR);
        float body[kChunk];
        const float erDiff = model.erDiffuse;
        const float muteWant = p.articulation == 1 ? 1.f : 0.f;
        if (muteAmt == 0.f && muteWant > 0.f) for (auto& m : mute) { m[0].clear(); m[1].clear(); }
        muteAmt += (muteWant - muteAmt) * 0.03f;
        if (muteAmt < 1e-3f && muteWant == 0.f) muteAmt = 0.f;
        for (int b = 0; b < kBuses; ++b) {
            Desk& d = desks[b];
            if (muteAmt > 0.f) {                               // heavier bridge: the bridge hill (2-4 kHz) is gone, a little nasal
                float* x = busIn[b].data();
                for (int n = 0; n < kChunk; ++n) {
                    const float y = mute[b][1].process(mute[b][0].process(x[n]));
                    x[n] += muteAmt * (y - x[n]);
                }
            }
            const float* in = busIn[b].data();
            bool silent = true;
            for (int n = 0; n < kChunk; ++n) if (in[n] != 0.f) { silent = false; break; }
            d.active = d.body.process(in, body, silent);
            if (!d.active) {
                // keep the reflection ring decaying correctly: write zeros
                for (int n = 0; n < kChunk; ++n) { d.ring[d.ringPos] = 0.f; d.ringPos = (d.ringPos + 1) & d.ringMask; }
                continue;
            }
            for (int n = 0; n < kChunk; ++n) {
                d.ring[d.ringPos] = body[n];
                float dl = d.ring[(d.ringPos - d.dDelL) & d.ringMask], dr = d.ring[(d.ringPos - d.dDelR) & d.ringMask];
                d.lpL += d.lpA * (dl - d.lpL); d.lpR += d.lpA * (dr - d.lpR);   // air absorption for back desks
                dryL[n] += d.panL * d.lpL; dryR[n] += d.panR * d.lpR;
                float l = 0, r = 0;
                for (int t = 0; t < kErTaps; ++t) {
                    l += d.erGainL[t] * d.ring[(d.ringPos - d.erDelL[t]) & d.ringMask];
                    r += d.erGainR[t] * d.ring[(d.ringPos - d.erDelR[t]) & d.ringMask];
                }
                // part of the room stays with the desk (the player sits *in* the room), part is spread wide
                const float c = (1.f - erDiff) * 1.41421356f;
                // (the part that stays with the desk is taken from both room mics: taken from the left one
                // only, it added up with the left channel's own reflections and the room leaned left)
                const float near = kNearMix * (l * d.erUnsideL + r * d.erUnsideR);
                erL[n] += d.erScale * (c * d.panL * near + erDiff * l);
                erR[n] += d.erScale * (c * d.panR * near + erDiff * r);
                d.ringPos = (d.ringPos + 1) & d.ringMask;
            }
        }
        const float g = model.outGain * std::pow(10.f, p.gainDb / 20.f);
        if (p.dryOnly) {
            for (int n = 0; n < kChunk; ++n) { outL[n] = g * (dryL[n] + padL[n]); outR[n] = g * (dryR[n] + padR[n]); }
            return;
        }
        const int ht = std::clamp(p.hallType, 0, 3);
        if (ht != hallCur) { hallPrev = hallCur; hallCur = ht; halls[hallCur].clear(); hallTail = (int)(4.0 * sr / kChunk); }
        const HallType& H = kHallType[hallCur];
        const float eg = erGain * p.roomMic * H.er, lg = lateGain * p.hall * H.late, cg = p.closeMic;
        for (int n = 0; n < kChunk; ++n) {                     // walls swallow highs
            erLpL += erLpA * (erL[n] - erLpL); erL[n] = erDiffL.process(erLpL);   // dense, not comb-like
            erLpR += erLpA * (erR[n] - erLpR); erR[n] = erDiffR.process(erLpR);
            late[n] = 0.5f * ((dryL[n] + eg * erL[n]) + (dryR[n] + eg * erR[n]) + kPadHall * (padL[n] + padR[n]));
            outL[n] = cg * dryL[n] + eg * erL[n] + padL[n];
            outR[n] = cg * dryR[n] + eg * erR[n] + padR[n];
        }
        halls[hallCur].process(late, outL, outR, kChunk, lg);
        if (hallTail > 0) {                                    // the previous room rings out
            --hallTail;
            halls[hallPrev].process(zeros, outL, outR, kChunk, lateGain * p.hall * kHallType[hallPrev].late);
        }
        const bool eqOn = eqSet[0] != 0.f || eqSet[1] != 0.f || eqSet[2] != 0.f;
        for (int n = 0; n < kChunk; ++n) {
            float l = outL[n] * g, r = outR[n] * g;
            if (eqOn) for (int b = 0; b < 3; ++b) { l = eq[0][b].process(l); r = eq[1][b].process(r); }
            outL[n] = l; outR[n] = r;
        }
    }
};

// ---------------------------------------------------------------------- public
Engine::Engine(const Model& model, uint32_t seed) : d(new Impl(model, seed)) {}
Engine::~Engine() = default;

void Engine::prepare(double sampleRate, int maxBlock)
{
    d->sr = sampleRate;
    d->ctrlRate = (float)(sampleRate / kChunk);
    d->wDrift.setup(0.3f, d->ctrlRate);
    d->wRate.setup(0.8f, d->ctrlRate);
    d->wSlow.setup(0.7f, d->ctrlRate);
    d->wUnrest.setup(7.0f, d->ctrlRate);
    d->wFlutter.setup(4.0f, d->ctrlRate);
    d->unrestDb = d->model.unrest;
    d->erGain = d->model.erGain; d->lateGain = d->model.lateGain;
    d->buildTables();
    d->buildPadTables();
    d->buildDesks();
    for (int i = 0; i < 4; ++i)
        d->halls[i].prepare(sampleRate, d->model.hallRt60 * kHallType[i].rt, std::min(0.95f, d->model.hallHf * kHallType[i].hf), kHallType[i].pre);
    for (auto& c : d->eq) for (auto& b : c) b.clear();
    for (auto& m : d->mute) { m[0].set(1, 2800.f, -11.f, 0.6f, sampleRate); m[1].set(2, 5500.f, -6.f, 0.7071f, sampleRate); }
    for (auto& e : d->eqSet) e = 1e9f;
    d->releaseScale = d->model.release;
    d->erDiffL.prepare(sampleRate, { 3.1f, 5.3f, 7.9f, 11.3f }, 0.6f);
    d->erDiffR.prepare(sampleRate, { 3.1f, 5.3f, 7.9f, 11.3f }, 0.6f);   // same diffusion: keeps the coherent part coherent
    d->erLpA = 1.f - std::exp(-2.f * (float)M_PI * 5000.f / (float)sampleRate);
    d->fifoL.assign((size_t)maxBlock + kChunk, 0.f); d->fifoR.assign((size_t)maxBlock + kChunk, 0.f);
    reset();
}

void Engine::setParams(const Params& p)
{
    const bool widthChanged = p.width != d->p.width || p.spacing != d->p.spacing || p.seating != d->p.seating;
    d->p = p;
    if (widthChanged) d->updatePans();
    d->updateEq();
}

void Engine::setUnrestOverride(int reg, float db) { if (reg >= 0 && reg < kNumRegisters) d->unrestDb[reg] = db; }
void Engine::setRoomOverride(float er, float late) { d->erGain = er; d->lateGain = late; }
void Engine::setReleaseOverride(float release, float rt60, float hf)
{
    d->releaseScale = release;
    for (int i = 0; i < 4; ++i)
        d->halls[i].prepare(d->sr, rt60 * kHallType[i].rt, std::min(0.95f, hf * kHallType[i].hf), kHallType[i].pre);
}

void Engine::reset()
{
    for (auto& v : d->voices) v.active = false;
    for (auto& ds : d->desks) { ds.body.clear(); std::fill(ds.ring.begin(), ds.ring.end(), 0.f); }
    for (auto& h : d->halls) h.clear();
    d->erDiffL.clear(); d->erDiffR.clear(); d->dblKey = -1;
    d->fifoN = 0; d->sustain = false;
}

void Engine::process(float* left, float* right, int numSamples, const Event* events, int numEvents)
{
    Impl& m = *d;
    int done = 0, ev = 0;
    while (done < numSamples) {
        if (m.fifoN == 0) {
            // apply all events that belong before the next chunk
            while (ev < numEvents && events[ev].offset <= done) {
                const Event& e = events[ev++];
                switch (e.type) {
                case Event::NoteOn: if (e.value > 0) m.noteOn(e.key, e.value); else m.noteOff(e.key); break;
                case Event::NoteOff: m.noteOff(e.key); break;
                case Event::Sustain: m.setSustain(e.value >= 0.5f); break;
                case Event::Expression: m.expression = std::clamp(e.value, 0.f, 1.f); break;
                case Event::PitchBend: m.bend = std::pow(2.f, 2.f * e.value / 12.f); break;
                case Event::AllOff: for (auto& v : m.voices) if (v.active) v.released = true; break;
                }
            }
            m.updateDouble();
            m.renderChunk(m.fifoL.data(), m.fifoR.data());
            m.fifoN = kChunk;
        }
        const int take = std::min(m.fifoN, numSamples - done);
        const int from = kChunk - m.fifoN;
        std::memcpy(left + done, m.fifoL.data() + from, sizeof(float) * take);
        std::memcpy(right + done, m.fifoR.data() + from, sizeof(float) * take);
        m.fifoN -= take; done += take;
    }
    while (ev < numEvents) {                                   // events at the very end
        const Event& e = events[ev++];
        if (e.type == Event::NoteOn && e.value > 0) m.noteOn(e.key, e.value);
        else if (e.type == Event::NoteOff || e.type == Event::NoteOn) m.noteOff(e.key);
        else if (e.type == Event::Sustain) m.setSustain(e.value >= 0.5f);
        else if (e.type == Event::Expression) m.expression = e.value;
    }
}

int Engine::activeNotes() const { int n = 0; for (auto& v : d->voices) n += v.active; return n; }
int Engine::activePlayers() const { int n = 0; for (auto& v : d->voices) if (v.active) n += v.nPlayers; return n; }

} // namespace opensection
