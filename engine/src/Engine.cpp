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

int registerOf(int key)
{
    for (int r = 0; r < kNumRegisters; ++r)
        if (key < kReg[r].topKey) return r;
    return kNumRegisters - 1;
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

struct Player {
    int bus = 0;
    double phase = 0;
    float det = 0;                                    // cents
    float vibRate = 5, vibDepth = 10, vibPhase = 0, vibOn = 0.4f;
    float start = 0, att = 0.5f;
    float nextChange = 1e9f, lastChange = -1e9f;
    float scoop = 0;                                  // intonation at the entry, settles quickly (cents)
    WanderState drift, rateJit, slowLvl, unrest;
    float hpX = 0, hpY = 0, lpY = 0;                  // rosin-noise filters
    float inc = 0, amp = 0, mix = 0, noise = 0;       // values at chunk start
    uint32_t rng = 1;
};

struct Voice {
    bool active = false, released = false, sustained = false;
    int key = 0, reg = 0, nPlayers = 0;
    float vel = 0.7f, rel = 1.f, steal = 1.f;
    bool stealing = false;
    double t = 0;                                     // seconds since note-on
    uint64_t age = 0;
    // the section listens to itself: part of the vibrato is shared, and every note is a bit different
    float secPhase = 0, secRate = 5.f, noteGain = 1.f, noteTone = 0.f;
    std::array<Player, kMaxPlayers> pl;
};

struct Desk {
    Convolver body;
    float panL = 0.7f, panR = 0.7f;
    std::array<int, 12> erDelL {}, erDelR {};
    std::array<float, 12> erGainL {}, erGainR {};
    std::vector<float> ring;                          // body output history for reflections
    int ringPos = 0, ringMask = 0;
    bool active = false;
};

std::vector<float> makeBody(float scale, std::mt19937& rng, double sr)
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
    ir[0] += 0.3f * peak;                                      // direct, non-resonant part
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

    std::vector<float> tabSoft, tabHard;              // [key][kTableLen+1]
    std::array<float, 128> keyGain {};                 // table RMS relative to a plain saw
    Wander wDrift, wRate, wSlow, wUnrest;
    std::array<Voice, kMaxVoices> voices;
    std::array<Desk, kBuses> desks;
    Hall hall;
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
        tabSoft.assign((size_t)nKeys * (kTableLen + 1), 0.f);
        tabHard.assign((size_t)nKeys * (kTableLen + 1), 0.f);
        Fft fft(kTableLen);
        std::vector<float> re(kTableLen), im(kTableLen);
        const float fmax = std::min(18000.f, 0.45f * (float)sr / 1.15f);    // room for vibrato + bend
        for (int key = kKeyLo; key <= kKeyHi; ++key) {
            const float f0 = 440.f * std::pow(2.f, (key - 69) / 12.f);
            const int nh = std::min(kTableLen / 2 - 1, (int)(fmax / f0));
            for (int which = 0; which < 2; ++which) {
                const float pr = which == 0 ? 0.3f : 1.0f;           // bow pressure of this table
                const float fc = 2500.f + 6000.f * pr;
                std::fill(re.begin(), re.end(), 0.f); std::fill(im.begin(), im.end(), 0.f);
                for (int h = 1; h <= nh; ++h) {
                    const float f = h * f0;
                    const float lp = 1.f / std::sqrt(1.f + std::pow(f / fc, 4.f));
                    const float a = (2.f / (float)M_PI) / h * lp * std::pow(10.f, model.colourDb(key, f) / 20.f);
                    im[h] = -a / 2.f; im[kTableLen - h] = a / 2.f;
                }
                fft.transform(re.data(), im.data(), true);
                float* dst = &(which == 0 ? tabSoft : tabHard)[(size_t)(key - kKeyLo) * (kTableLen + 1)];
                double e = 0;
                for (int i = 0; i < kTableLen; ++i) { dst[i] = re[i]; e += (double)re[i] * re[i]; }
                dst[kTableLen] = dst[0];
                if (which == 1) keyGain[key] = (float)std::sqrt(e / kTableLen) / 0.57735f;
            }
        }
    }

    void buildDesks()
    {
        std::mt19937 rng(seed * 7919u + 11u);
        std::uniform_real_distribution<float> U(0.f, 1.f);
        int ringLen = 1; while (ringLen < (int)(0.09 * sr)) ringLen <<= 1;
        for (int r = 0; r < kNumRegisters; ++r) {
            for (int k = 0; k < kDesks; ++k) {
                Desk& d = desks[r * kDesks + k];
                d.body.setIR(makeBody(kReg[r].bodyScale * (0.95f + 0.1f * U(rng)), rng, sr));
                d.ring.assign(ringLen, 0.f); d.ringMask = ringLen - 1; d.ringPos = 0;
                const float pan = std::clamp(kReg[r].seat + (k - 1.5f) / 1.5f * 0.75f, -1.f, 1.f);
                for (int c = 0; c < 2; ++c) {
                    const float near = 1.f + 0.5f * (c == 1 ? pan : -pan);
                    for (int t = 0; t < 12; ++t) {
                        const float dl = 0.004f + 0.071f * U(rng);
                        const float g = near * (0.3f + 0.4f * U(rng)) * (U(rng) < 0.5f ? -1.f : 1.f) * std::exp(-dl / 0.035f);
                        (c == 0 ? d.erDelL : d.erDelR)[t] = (int)(dl * sr);
                        (c == 0 ? d.erGainL : d.erGainR)[t] = g;
                    }
                }
            }
        }
        updatePans();
    }

    void updatePans()
    {
        for (int r = 0; r < kNumRegisters; ++r)
            for (int k = 0; k < kDesks; ++k) {
                Desk& d = desks[r * kDesks + k];
                const float pan = std::clamp(kReg[r].seat * std::min(1.f, p.width) + (k - 1.5f) / 1.5f * 0.75f * p.width, -1.f, 1.f);
                d.panL = std::cos((pan + 1.f) * (float)M_PI / 4.f);
                d.panR = std::sin((pan + 1.f) * (float)M_PI / 4.f);
            }
    }

    // ------------------------------------------------------------------ notes
    void noteOn(int key, float vel)
    {
        for (auto& v : voices)                                  // same key: the old bow stops
            if (v.active && v.key == key && !v.released) { v.released = true; v.rel = std::min(v.rel, 1.f); }
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

    void startVoice(Voice& v, int key, float vel)
    {
        v = Voice();
        v.active = true; v.key = key; v.vel = vel; v.reg = registerOf(key);
        v.age = ++ageCounter;
        uint32_t s = seed * 2654435761u ^ (uint32_t)(v.age * 40503u + (uint32_t)key * 9176u);
        xorshift(s); xorshift(s);
        const int base = kReg[v.reg].players;
        v.nPlayers = std::clamp((int)std::lround(base * p.quality), 2, std::min(base, kMaxPlayers));
        v.secRate = 4.9f + 0.9f * urand(s);
        v.secPhase = 6.2831853f * urand(s);
        v.noteGain = std::pow(10.f, 1.2f * gauss(s) / 20.f);
        v.noteTone = 0.07f * gauss(s);
        int deskOrder[kDesks] = { 0, 1, 2, 3 };
        for (int i = kDesks - 1; i > 0; --i) std::swap(deskOrder[i], deskOrder[xorshift(s) % (i + 1)]);
        for (int i = 0; i < v.nPlayers; ++i) {
            Player& pl = v.pl[i];
            pl = Player();
            pl.rng = xorshift(s) | 1u;
            pl.bus = v.reg * kDesks + deskOrder[i % kDesks];
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
            pl.slowLvl.warm(wSlow, pl.rng, 20); pl.unrest.warm(wUnrest, pl.rng, 10);
            controls(v, pl, 0.0, pl.inc, pl.amp, pl.mix, pl.noise);
        }
    }

    void noteOff(int key)
    {
        for (auto& v : voices)
            if (v.active && v.key == key && !v.released && !v.sustained) {
                if (sustain) v.sustained = true; else v.released = true;
            }
    }

    void setSustain(bool on)
    {
        sustain = on;
        if (!on) for (auto& v : voices) if (v.active && v.sustained) { v.sustained = false; v.released = true; }
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
        if ((float)t >= pl.nextChange && !v.released) {
            pl.lastChange = pl.nextChange;
            pl.nextChange += 2.5f + 2.5f * urand(pl.rng);
        }
        const float dc = (float)t - pl.lastChange;
        env *= 1.f - 0.45f * std::exp(-(dc / 0.035f) * (dc / 0.035f));
        env *= std::pow(10.f, (1.0f * pl.slowLvl.next(wSlow, pl.rng)
                               + p.unrest * unrestDb[v.reg] * pl.unrest.next(wUnrest, pl.rng)) / 20.f);
        // velocity: mostly tone, a little level (normalised to the learning velocity 0.71)
        // velocity -> level like a sampled section (sfz amp_veltrack 100: gain = vel^2),
        // normalised to the learning velocity 0.71; p.velocity blends towards no dynamics
        const float velAmp = std::pow(std::max(v.vel, 0.02f) / 0.85f, 2.f * p.velocity);   // headroom for hard chords
        const float ex = exprSmooth;
        const float exAmp = ex * std::sqrt(ex);                  // pedal fully up = silent (fade pads in/out)
        amp = env * velAmp * exAmp * v.noteGain * v.rel * v.steal / std::sqrt((float)v.nPlayers);

        const float pressure = std::clamp(v.vel * (0.45f + 0.55f * ex), 0.f, 1.f);
        mix = std::clamp((pressure - 0.3f) / 0.7f + v.noteTone, 0.f, 1.f);

        float scratch = 0.25f;
        if (ts >= 0) scratch += 1.4f * v.vel * std::exp(-ts / 0.07f);
        if (dc >= 0) scratch += 0.8f * v.vel * std::exp(-dc / 0.05f);
        noise = p.rosin * scratch * 1.7320508f * keyGain[std::clamp(v.key, kKeyLo, kKeyHi)];
    }

    // ------------------------------------------------------------------ render one 64-sample chunk
    void renderChunk(float* outL, float* outR)
    {
        for (auto& b : busIn) std::memset(b.data(), 0, sizeof(float) * kChunk);
        exprSmooth += (expression - exprSmooth) * 0.08f;
        const float chunkDur = kChunk / (float)sr;
        const float hpA = std::exp(-2.f * (float)M_PI * 2000.f / (float)sr);
        for (auto& v : voices) {
            if (!v.active) continue;
            const float* ts = table(tabSoft, v.key);
            const float* th = table(tabHard, v.key);
            const float lpA = std::exp(-2.f * (float)M_PI * (2500.f + 6000.f * v.vel) / (float)sr);
            v.t += chunkDur;
            v.secPhase += 6.2831853f * v.secRate * chunkDur;
            if (v.secPhase > 6.2831853f) v.secPhase -= 6.2831853f;
            if (v.released) v.rel *= std::exp(-chunkDur / (0.22f * releaseScale * p.release));
            if (v.stealing) v.steal *= std::exp(-chunkDur / 0.012f);
            for (int i = 0; i < v.nPlayers; ++i) {
                Player& pl = v.pl[i];
                float inc1, amp1, mix1, noi1;
                controls(v, pl, v.t, inc1, amp1, mix1, noi1);
                const float k = 1.f / kChunk;
                const float dInc = (inc1 - pl.inc) * k, dAmp = (amp1 - pl.amp) * k;
                const float dMix = (mix1 - pl.mix) * k, dNoi = (noi1 - pl.noise) * k;
                float inc = pl.inc, amp = pl.amp, mix = pl.mix, noi = pl.noise;
                double ph = pl.phase;
                float hpX = pl.hpX, hpY = pl.hpY, lpY = pl.lpY;
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
            if ((v.released && v.rel < 1e-4f) || (v.stealing && v.steal < 1e-4f)) v.active = false;
        }

        // desks: body -> close mics + room mics
        std::memset(dryL, 0, sizeof dryL); std::memset(dryR, 0, sizeof dryR);
        std::memset(erL, 0, sizeof erL); std::memset(erR, 0, sizeof erR);
        float body[kChunk];
        for (int b = 0; b < kBuses; ++b) {
            Desk& d = desks[b];
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
                dryL[n] += d.panL * body[n]; dryR[n] += d.panR * body[n];
                d.ring[d.ringPos] = body[n];
                float l = 0, r = 0;
                for (int t = 0; t < 12; ++t) {
                    l += d.erGainL[t] * d.ring[(d.ringPos - d.erDelL[t]) & d.ringMask];
                    r += d.erGainR[t] * d.ring[(d.ringPos - d.erDelR[t]) & d.ringMask];
                }
                erL[n] += l; erR[n] += r;
                d.ringPos = (d.ringPos + 1) & d.ringMask;
            }
        }
        const float g = model.outGain * std::pow(10.f, p.gainDb / 20.f);
        if (p.dryOnly) {
            for (int n = 0; n < kChunk; ++n) { outL[n] = g * dryL[n]; outR[n] = g * dryR[n]; }
            return;
        }
        const float eg = erGain * p.roomMic, lg = lateGain * p.hall, cg = p.closeMic;
        for (int n = 0; n < kChunk; ++n) {                     // walls swallow highs
            erLpL += erLpA * (erL[n] - erLpL); erL[n] = erDiffL.process(erLpL);   // dense, not comb-like
            erLpR += erLpA * (erR[n] - erLpR); erR[n] = erDiffR.process(erLpR);
            late[n] = 0.5f * ((dryL[n] + eg * erL[n]) + (dryR[n] + eg * erR[n]));
            outL[n] = cg * dryL[n] + eg * erL[n];
            outR[n] = cg * dryR[n] + eg * erR[n];
        }
        hall.process(late, outL, outR, kChunk, lg);
        for (int n = 0; n < kChunk; ++n) { outL[n] *= g; outR[n] *= g; }
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
    d->unrestDb = d->model.unrest;
    d->erGain = d->model.erGain; d->lateGain = d->model.lateGain;
    d->buildTables();
    d->buildDesks();
    d->hall.prepare(sampleRate, d->model.hallRt60, d->model.hallHf);
    d->releaseScale = d->model.release;
    d->erDiffL.prepare(sampleRate, { 3.1f, 5.3f, 7.9f, 11.3f }, 0.6f);
    d->erDiffR.prepare(sampleRate, { 3.7f, 4.9f, 8.3f, 10.7f }, 0.6f);
    d->erLpA = 1.f - std::exp(-2.f * (float)M_PI * 5000.f / (float)sampleRate);
    d->fifoL.assign((size_t)maxBlock + kChunk, 0.f); d->fifoR.assign((size_t)maxBlock + kChunk, 0.f);
    reset();
}

void Engine::setParams(const Params& p)
{
    const bool widthChanged = p.width != d->p.width;
    d->p = p;
    if (widthChanged) d->updatePans();
}

void Engine::setUnrestOverride(int reg, float db) { if (reg >= 0 && reg < kNumRegisters) d->unrestDb[reg] = db; }
void Engine::setRoomOverride(float er, float late) { d->erGain = er; d->lateGain = late; }
void Engine::setReleaseOverride(float release, float rt60, float hf)
{
    d->releaseScale = release;
    d->hall.prepare(d->sr, rt60, hf);
}

void Engine::reset()
{
    for (auto& v : d->voices) v.active = false;
    for (auto& ds : d->desks) { ds.body.clear(); std::fill(ds.ring.begin(), ds.ring.end(), 0.f); }
    d->hall.clear(); d->erDiffL.clear(); d->erDiffR.clear();
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
