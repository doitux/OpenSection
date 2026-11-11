// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
// Hall: 16-line feedback delay network with slowly modulated delay lengths (no metallic
// ringing), Householder feedback, frequency-dependent decay, diffused input and
// decorrelated stereo outputs. Plus a small allpass diffuser for the early reflections.
#pragma once
#include <vector>
#include <cmath>
#include <array>
#include <algorithm>

namespace opensection {

class Diffuser {   // chain of allpasses: turns sparse echoes into a dense, smooth cloud
public:
    void prepare(double sr, const std::vector<float>& ms, float g)
    {
        g_ = g; st_.clear();
        for (float m : ms) { St s; s.buf.assign(std::max(1, (int)(m * 0.001 * sr)), 0.f); st_.push_back(std::move(s)); }
    }
    void clear() { for (auto& s : st_) { std::fill(s.buf.begin(), s.buf.end(), 0.f); s.pos = 0; } }
    inline float process(float x)
    {
        for (auto& s : st_) {
            const float d = s.buf[s.pos];
            const float y = -g_ * x + d;
            s.buf[s.pos] = x + g_ * y;
            if (++s.pos == (int)s.buf.size()) s.pos = 0;
            x = y;
        }
        return x;
    }
private:
    struct St { std::vector<float> buf; int pos = 0; };
    std::vector<St> st_;
    float g_ = 0.6f;
};

class Hall {
public:
    static constexpr int N = 16;

    void prepare(double sr, float rt60 = 2.4f, float hfRatio = 0.45f, float predelay = 0.025f)
    {
        sr_ = sr;
        // mutually prime-ish lengths 29..97 ms (spread for a dense, even tail)
        static const float ms[N] = { 29.3f, 31.7f, 34.1f, 37.9f, 41.3f, 43.7f, 47.9f, 53.1f,
                                     57.7f, 61.3f, 66.7f, 71.9f, 77.3f, 83.9f, 89.1f, 96.7f };
        for (int i = 0; i < N; ++i) {
            len_[i] = (float)(ms[i] * 0.001 * sr);
            const int cap = (int)len_[i] + 64;
            int sz = 1; while (sz < cap) sz <<= 1;
            line_[i].assign(sz, 0.f); mask_[i] = sz - 1; wpos_[i] = 0; lp_[i] = 0.f;
            const double t = len_[i] / sr;
            g_[i] = (float)std::pow(10.0, -3.0 * t / rt60);
            const double gHi = std::pow(10.0, -3.0 * t / (rt60 * hfRatio));
            const double r = gHi / g_[i];
            damp_[i] = (float)((1 - r) / (1 + r));
            const double w = 2 * M_PI * (0.13 + 0.07 * i) / sr;   // slow modulation as a rotating phasor
            rc_[i] = (float)std::cos(w); rs_[i] = (float)std::sin(w);
            mc_[i] = std::cos(0.7f * i); ms_[i] = std::sin(0.7f * i);
        }
        modDepth_ = (float)(0.00025 * sr);                     // ±0.25 ms: breaks up resonances, no audible wobble
        inDiff_.prepare(sr, { 4.3f, 3.1f, 8.6f, 6.3f }, 0.7f);
        pre_.assign((size_t)std::max(1.0, predelay * sr), 0.f); prePos_ = 0;
        // normalise: energy of the impulse response ~ 1 (measured once)
        norm_ = 1.f;
        clear();
        double eL = 0, eR = 0; float l, r;
        const int n = (int)(rt60 * 1.2 * sr);
        for (int i = 0; i < n; ++i) { float x = i == 0 ? 1.f : 0.f; l = r = 0.f; tick(x, l, r); eL += (double)l * l; eR += (double)r * r; }
        // the same energy on both sides (the two halves of the delay lines decay a little differently,
        // which made the hall lean to the left)
        norm_ = (float)(1.0 / std::sqrt(0.5 * (eL + eR) + 1e-20));
        normL_ = (float)std::sqrt(0.5 * (eL + eR) / (eL + 1e-20)); normR_ = (float)std::sqrt(0.5 * (eL + eR) / (eR + 1e-20));
        clear();
    }

    void clear()
    {
        for (auto& l : line_) std::fill(l.begin(), l.end(), 0.f);
        inDiff_.clear();
        std::fill(pre_.begin(), pre_.end(), 0.f);
        lp_.fill(0.f);
    }

    // mono in -> stereo out (added to outL/outR with gain)
    void process(const float* in, float* outL, float* outR, int n, float gain)
    {
        for (int s = 0; s < n; ++s) {
            float l = 0, r = 0;
            tick(in[s], l, r);
            outL[s] += gain * norm_ * normL_ * l;
            outR[s] += gain * norm_ * normR_ * r;
        }
    }

private:
    inline void tick(float in, float& outL, float& outR)
    {
        float x = pre_[prePos_]; pre_[prePos_] = in;
        if (++prePos_ == (int)pre_.size()) prePos_ = 0;
        x = inDiff_.process(x);
        float o[N], sum = 0.f;
        for (int i = 0; i < N; ++i) {
            const float c = mc_[i] * rc_[i] - ms_[i] * rs_[i];
            ms_[i] = ms_[i] * rc_[i] + mc_[i] * rs_[i]; mc_[i] = c;
            const float d = len_[i] + modDepth_ * ms_[i];
            const float rp = (float)wpos_[i] - d;
            const int i0 = (int)std::floor(rp); const float fr = rp - (float)i0;
            const float a = line_[i][i0 & mask_[i]], b = line_[i][(i0 + 1) & mask_[i]];
            const float v = a + fr * (b - a);
            lp_[i] = v + damp_[i] * (lp_[i] - v);
            o[i] = lp_[i] * g_[i];
            sum += o[i];
        }
        sum *= 2.f / N;
        for (int i = 0; i < N; ++i) {
            line_[i][wpos_[i]] = o[i] - sum + x * kIn[i];
            wpos_[i] = (wpos_[i] + 1) & mask_[i];
        }
        if (++renorm_ == 4096) {                               // keep the phasors on the unit circle
            renorm_ = 0;
            for (int i = 0; i < N; ++i) { const float m = 1.f / std::sqrt(mc_[i] * mc_[i] + ms_[i] * ms_[i]); mc_[i] *= m; ms_[i] *= m; }
        }
        float l = 0, r = 0;
        for (int i = 0; i < N; i += 2) { l += kOut[i] * o[i]; r += kOut[i + 1] * o[i + 1]; }
        outL = l; outR = r;
    }

    static constexpr float kIn[N] = { 1, -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, 1, -1, -1, 1, -1 };
    static constexpr float kOut[N] = { 1, 1, -1, 1, 1, -1, -1, -1, 1, -1, -1, 1, -1, 1, 1, 1 };
    double sr_ = 48000;
    std::array<std::vector<float>, N> line_;
    std::array<int, N> mask_ {}, wpos_ {};
    std::array<float, N> len_ {}, g_ {}, damp_ {}, lp_ {}, rc_ {}, rs_ {}, mc_ {}, ms_ {};
    int renorm_ = 0;
    float modDepth_ = 10.f, norm_ = 1.f, normL_ = 1.f, normR_ = 1.f;
    Diffuser inDiff_;
    std::vector<float> pre_;
    int prePos_ = 0;
};

} // namespace opensection
