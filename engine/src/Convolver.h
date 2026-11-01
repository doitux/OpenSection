// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
// Uniformly partitioned overlap-save convolution (partition = engine block = kChunk).
// Zero added latency: the engine renders its own input chunk by chunk.
#pragma once
#include "Fft.h"
#include <vector>
#include <algorithm>
#include <cstring>

namespace opensection {

constexpr int kChunk = 128;   // engine block; bigger partitions = less convolution work per sample

class Convolver {
public:
    void setIR(const std::vector<float>& ir)
    {
        fft_.initReal(N);
        parts_ = std::max<int>(1, (int)((ir.size() + kChunk - 1) / kChunk));
        hRe_.assign((size_t)parts_ * B, 0.f); hIm_.assign((size_t)parts_ * B, 0.f);
        float x[N];
        for (int p = 0; p < parts_; ++p) {
            std::fill(x, x + N, 0.f);
            for (int i = 0; i < kChunk; ++i) {
                size_t k = (size_t)p * kChunk + i;
                if (k < ir.size()) x[i] = ir[k];
            }
            fft_.realForward(x, &hRe_[(size_t)p * B], &hIm_[(size_t)p * B]);
        }
        xRe_.assign((size_t)parts_ * B, 0.f); xIm_.assign((size_t)parts_ * B, 0.f);
        prev_.assign(kChunk, 0.f);
        pos_ = 0; silent_ = parts_ + 1;
    }

    // in/out: kChunk samples. Returns false (and writes zeros) when fully idle.
    bool process(const float* in, float* out, bool inputSilent)
    {
        if (inputSilent) {
            if (silent_ > parts_) { std::memset(out, 0, sizeof(float) * kChunk); return false; }
            ++silent_;
        } else {
            if (silent_ > parts_) clear();
            silent_ = 0;
        }
        float x[N];
        std::memcpy(x, prev_.data(), sizeof(float) * kChunk);
        std::memcpy(x + kChunk, in, sizeof(float) * kChunk);
        std::memcpy(prev_.data(), in, sizeof(float) * kChunk);
        pos_ = (pos_ + parts_ - 1) % parts_;
        fft_.realForward(x, &xRe_[(size_t)pos_ * B], &xIm_[(size_t)pos_ * B]);

        float accRe[B] = {}, accIm[B] = {};
        for (int p = 0; p < parts_; ++p) {
            const int slot = (pos_ + p) % parts_;
            const float* xr = &xRe_[(size_t)slot * B];
            const float* xi = &xIm_[(size_t)slot * B];
            const float* hr = &hRe_[(size_t)p * B];
            const float* hi = &hIm_[(size_t)p * B];
            for (int k = 0; k < B; ++k) {
                accRe[k] += xr[k] * hr[k] - xi[k] * hi[k];
                accIm[k] += xr[k] * hi[k] + xi[k] * hr[k];
            }
        }
        fft_.realInverse(accRe, accIm, x);
        const float sc = 2.f / N;
        for (int i = 0; i < kChunk; ++i) out[i] = x[i + kChunk] * sc;
        return true;
    }

    void clear()
    {
        std::fill(xRe_.begin(), xRe_.end(), 0.f); std::fill(xIm_.begin(), xIm_.end(), 0.f);
        std::fill(prev_.begin(), prev_.end(), 0.f);
    }

private:
    static constexpr int N = 2 * kChunk, B = kChunk + 1;
    Fft fft_;
    int parts_ = 1, pos_ = 0, silent_ = 0;
    std::vector<float> hRe_, hIm_, xRe_, xIm_, prev_;
};

} // namespace opensection
