// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
// Feedback-delay-network hall: 8 lines, Householder feedback, frequency-dependent decay
// (high frequencies die ~2x faster), diffused input, decorrelated stereo outputs.
#pragma once
#include <vector>
#include <cmath>
#include <array>

namespace opensection {

class Hall {
public:
    void prepare(double sr, float rt60 = 2.6f, float predelay = 0.03f)
    {
        sr_ = sr;
        static const int base[8] = { 1557, 1617, 1491, 1422, 1277, 1356, 1188, 1116 };   // primes-ish @44.1k
        const double sc = sr / 44100.0 * 1.6;                                          // bigger room
        for (int i = 0; i < 8; ++i) {
            len_[i] = (int)(base[i] * sc);
            line_[i].assign(len_[i], 0.f);
            pos_[i] = 0; lp_[i] = 0.f;
            const double t = len_[i] / sr;
            g_[i] = (float)std::pow(10.0, -3.0 * t / rt60);
            const double gHi = std::pow(10.0, -3.0 * t / (rt60 * 0.45));
            // one-pole lowpass in the loop: DC gain 1, Nyquist gain gHi/g
            const double r = gHi / g_[i];
            damp_[i] = (float)((1 - r) / (1 + r));
        }
        static const int ap[4] = { 142, 107, 379, 277 };
        for (int i = 0; i < 4; ++i) { apLen_[i] = (int)(ap[i] * sr / 44100.0); apBuf_[i].assign(apLen_[i], 0.f); apPos_[i] = 0; }
        pre_.assign((size_t)std::max(1.0, predelay * sr), 0.f); prePos_ = 0;
        fadeIn_ = 0.f;
    }

    void clear()
    {
        for (auto& l : line_) std::fill(l.begin(), l.end(), 0.f);
        for (auto& a : apBuf_) std::fill(a.begin(), a.end(), 0.f);
        std::fill(pre_.begin(), pre_.end(), 0.f);
        for (auto& v : lp_) v = 0.f;
    }

    // mono in -> stereo out (added to outL/outR with gain)
    void process(const float* in, float* outL, float* outR, int n, float gain)
    {
        for (int s = 0; s < n; ++s) {
            float x = pre_[prePos_]; pre_[prePos_] = in[s];
            if (++prePos_ == (int)pre_.size()) prePos_ = 0;
            for (int a = 0; a < 4; ++a) {                     // input diffusion
                float d = apBuf_[a][apPos_[a]];
                float y = -0.7f * x + d;
                apBuf_[a][apPos_[a]] = x + 0.7f * y;
                if (++apPos_[a] == apLen_[a]) apPos_[a] = 0;
                x = y;
            }
            float o[8], sum = 0.f;
            for (int i = 0; i < 8; ++i) {
                float v = line_[i][pos_[i]];
                lp_[i] = v + damp_[i] * (lp_[i] - v);         // damping
                o[i] = lp_[i] * g_[i];
                sum += o[i];
            }
            sum *= 0.25f;                                     // Householder: o - 2/N * sum
            for (int i = 0; i < 8; ++i) {
                line_[i][pos_[i]] = o[i] - sum + x * kIn[i];
                if (++pos_[i] == len_[i]) pos_[i] = 0;
            }
            outL[s] += gain * kOut * (o[0] - o[2] + o[4] - o[6]);
            outR[s] += gain * kOut * (o[1] - o[3] + o[5] - o[7]);
        }
    }

private:
    static constexpr float kIn[8] = { 1, -1, 1, 1, -1, 1, -1, -1 };
    static constexpr float kOut = 0.35f;
    double sr_ = 48000;
    std::array<std::vector<float>, 8> line_;
    std::array<int, 8> len_ {}, pos_ {};
    std::array<float, 8> g_ {}, damp_ {}, lp_ {};
    std::array<std::vector<float>, 4> apBuf_;
    std::array<int, 4> apLen_ {}, apPos_ {};
    std::vector<float> pre_;
    int prePos_ = 0;
    float fadeIn_ = 0.f;
};

} // namespace opensection
