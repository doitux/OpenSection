// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
// Small radix-2 complex FFT, split real/imag arrays (vectorises well), power-of-two sizes.
#pragma once
#include <vector>
#include <cmath>

namespace opensection {

class Fft {
public:
    explicit Fft(int n = 128) { init(n); }
    void init(int n)
    {
        n_ = n;
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        rev_.resize(n);
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev_[i] = r;
        }
        cs_.resize(n / 2); sn_.resize(n / 2);
        for (int i = 0; i < n / 2; ++i) {
            cs_[i] = (float)std::cos(2 * M_PI * i / n);
            sn_[i] = (float)-std::sin(2 * M_PI * i / n);
        }
    }
    int size() const { return n_; }

    // in-place; inverse is unscaled
    void transform(float* re, float* im, bool inverse) const
    {
        const int n = n_;
        for (int i = 0; i < n; ++i) {
            int j = rev_[i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        const float sg = inverse ? -1.f : 1.f;
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1, step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int k = 0; k < half; ++k) {
                    const float wr = cs_[k * step], wi = sg * sn_[k * step];
                    const int a = i + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
            }
        }
    }

    // ---- real signals of length n via a complex FFT of length n/2 (twice as fast)
    // forward: x[n] -> re/im[0..n/2]; inverse: re/im[0..n/2] -> x[n] (unscaled: result * n/2... see realInverse)
    void initReal(int n)
    {
        init(n / 2);
        rn_ = n;
        wr_.resize(n / 2 + 1); wi_.resize(n / 2 + 1);
        for (int k = 0; k <= n / 2; ++k) { wr_[k] = (float)std::cos(2 * M_PI * k / n); wi_[k] = (float)-std::sin(2 * M_PI * k / n); }
        zr_.resize(n / 2); zi_.resize(n / 2);
    }
    void realForward(const float* x, float* re, float* im)
    {
        const int h = rn_ / 2;
        for (int i = 0; i < h; ++i) { zr_[i] = x[2 * i]; zi_[i] = x[2 * i + 1]; }
        transform(zr_.data(), zi_.data(), false);
        for (int k = 0; k <= h; ++k) {
            const int a = k % h, b = (h - k) % h;
            const float er = 0.5f * (zr_[a] + zr_[b]), ei = 0.5f * (zi_[a] - zi_[b]);    // even part
            const float orr = 0.5f * (zi_[a] + zi_[b]), oi = -0.5f * (zr_[a] - zr_[b]);  // odd part
            re[k] = er + wr_[k] * orr - wi_[k] * oi;
            im[k] = ei + wr_[k] * oi + wi_[k] * orr;
        }
    }
    // inverse of realForward; output x scaled by n/2 (like an unscaled length-n inverse / 2)
    void realInverse(const float* re, const float* im, float* x)
    {
        const int h = rn_ / 2;
        for (int k = 0; k < h; ++k) {
            const int b = h - k;
            const float er = 0.5f * (re[k] + re[b]), ei = 0.5f * (im[k] - im[b]);
            const float dr = 0.5f * (re[k] - re[b]), di = 0.5f * (im[k] + im[b]);
            // odd part = d * conj(w)
            const float orr = dr * wr_[k] + di * wi_[k], oi = di * wr_[k] - dr * wi_[k];
            zr_[k] = er - oi; zi_[k] = ei + orr;
        }
        transform(zr_.data(), zi_.data(), true);
        for (int i = 0; i < h; ++i) { x[2 * i] = zr_[i]; x[2 * i + 1] = zi_[i]; }
    }

private:
    int rn_ = 0;
    std::vector<float> wr_, wi_, zr_, zi_;
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<float> cs_, sn_;
};

} // namespace opensection
