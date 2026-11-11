// OpenSection – computed string-section instrument
// Copyright (C) 2026 OpenSection contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Model.h"
#include <memory>
#include <cstdint>

namespace opensection {

// Everything a user can change. Live parameters are cheap (smoothed per 64-sample chunk).
struct Params {
    // ensemble
    float quality = 0.7f;      // 0.3 (eco) .. 1.0 (studio): players per note
    float width = 1.0f;        // 0 .. 1.5   stereo spread of the desks
    float pad = 0.0f;          // 0 .. 1     soft synth pad under the strings (dark, slow, gently moving)
    float seating = 0.5f;      // 0 .. 1     0 each section a tight block, 0.5 orchestra layout, 1 desks mixed through the room
    float spacing = 1.0f;      // 0 .. 2     distance of the two main mics (time differences)
    // bowing
    float attack = 1.0f;       // 0.3 .. 3   scale of the bow attack time
    float release = 1.0f;      // 0.3 .. 4   scale of the release
    float vibrato = 1.0f;      // 0 .. 2     vibrato depth
    float unrest = 0.7f;       // 0 .. 2     scale of the learned bow-pressure unrest
    float rosin = 1.0f;        // 0 .. 4     bow noise / scratch (x the learned level per section)
    float velocity = 1.0f;     // 0 .. 1     key velocity -> loudness + faster/harder attack
    float coupling = 0.1f;     // 0 .. 1     how much of the vibrato the section shares
    float detune = 0.8f;       // 0 .. 4     spread of the players' tuning (x 6 cents; the section stays centred)
    float expression = 1.0f;   // 0 .. 1     bow pressure (CC11 by default)
    // microphones / room
    float closeMic = 1.0f;     // 0 .. 2     dry (close) signal
    float roomMic = 1.0f;      // 0 .. 2     scale of learned early reflections
    float hall = 1.0f;         // 0 .. 3     scale of learned hall
    float gainDb = 0.0f;       // output
    int hallType = 0;          // 0 learned hall, 1 chamber, 2 church, 3 studio
    float eqLow = 0, eqMid = 0, eqHigh = 0;   // dB: shelf 200 Hz, bell 1.2 kHz, shelf 6 kHz
    float bassDouble = 0.0f;   // 0 .. 1  basses double the lowest held note an octave below (0 = off)
    int articulation = 0;      // 0 arco, 1 con sordino (muted), 2 tremolo, 3 pizzicato (new notes)
    // studio switches (not live)
    bool dryOnly = false;      // no ER / hall (used for learning)
    bool solo = false;         // one player per note, main section only (used for learning the bodies)
};

struct Event {
    enum Type : uint8_t { NoteOn, NoteOff, Sustain, Expression, PitchBend, AllOff } type;
    int offset = 0;            // sample offset inside the block
    int key = 0;
    float value = 0;           // velocity 0..1, sustain 0/1, expression 0..1, bend -1..1
};

class Engine {
public:
    explicit Engine(const Model& model, uint32_t seed = 1);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void prepare(double sampleRate, int maxBlock);      // rebuilds tables/bodies (not real-time safe)
    void setParams(const Params& p);                    // real-time safe
    void setUnrestOverride(int reg, float db);          // for learning
    void setRoomOverride(float er, float late);         // for learning
    void setReleaseOverride(float release, float rt60, float hf);   // for learning (not real-time safe)
    // real-time: events must be sorted by offset
    void process(float* left, float* right, int numSamples, const Event* events, int numEvents);
    void reset();

    int activeNotes() const;
    int activePlayers() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace opensection
