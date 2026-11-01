// OpenSection – computed string-section instrument
// Copyright (C) 2026 OpenSection contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include <array>

namespace opensection {

constexpr int kNumRegisters = 4;   // 0 bass, 1 cello, 2 viola, 3 violin

// The "fingerprint" learned from recordings: for selected MIDI keys a tone colour
// curve (dB over a 1/6-octave grid, including absolute level), plus calibrated
// performance and room values. A few kilobytes instead of gigabytes of samples.
struct Model {
    std::vector<float> bandHz;                 // grid, ascending
    std::vector<int> keys;                     // learned keys, ascending
    std::vector<std::vector<float>> curves;    // [key][band] dB
    std::array<float, kNumRegisters> unrest { 1.5f, 1.5f, 1.5f, 0.5f };  // bow-pressure unrest (dB)
    float erGain = 0.3f;                       // early reflections (room mics)
    float lateGain = 0.2f;                     // hall
    float outGain = 1.0f;                      // overall output trim
    float release = 1.0f;                      // learned release time (scale of 0.22 s)
    float hallRt60 = 2.4f;                     // learned hall decay (s)
    float hallHf = 0.45f;                      // high frequencies decay this much faster
    float erDiffuse = 0.5f;                    // 0: room reflections stay at the desk's position, 1: fully spread
    float spacing = 0.3f;                      // main-mic spacing (time differences), learned
    // dynamics: the colour at other velocities, relative to the learned colour at dynLoud.
    // Each node: one curve per learned key, dB, fundamental region = 0 dB (loudness itself is
    // handled by velocity). Empty: analytic bow-pressure tilt.
    struct DynNode { float vel = 0; std::vector<std::vector<float>> curves; };
    std::vector<DynNode> dynNodes;
    float dynLoud = 0.71f;                     // velocity at which the colour above was learned
    float dynSoft = 0.39f;                     // reference velocity of the learned rosin-noise corner
    std::array<float, kNumRegisters> noiseSoftHz {};   // rosin-noise corner at dynSoft per register (0: analytic)

    // tone colour for any key (linear interpolation between learned keys), dB at frequency f
    float colourDb(int midi, float hz) const;
    float dynDb(size_t node, int midi, float hz) const;

    static Model flat();                       // neutral model (no learning)
    static Model fromText(const std::string& text, std::string* error = nullptr);
    std::string toText() const;
    static Model builtin();                    // the model shipped with the plugin
};

} // namespace opensection
