// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
// Command-line renderer and live-performance benchmark for the engine.
//
//   opensection-render [options] <notes.txt> <out.wav>
//   opensection-render --bench [options] <notes.txt>
//
// notes.txt lines:  <on_s> <off_s> <midi> <velocity 0..1>
//                   cc <time_s> <64|11> <value 0..1>        (sustain / expression)
// options: --model file.osm  --sr 48000  --block 128  --seed N  --dry  --quality q
//          --unrest r0,r1,r2,r3  --room er,late  --release scale,rt60,hfratio  --tail seconds
//          --solo  --vibrato v  --flutter f0,f1,f2,f3   (learning helpers)
//          --bassdouble level  --halltype 0..3  --eq low,mid,high (dB)
//          --set id=value   any plugin parameter by its id (attack, release, vibrato, unrest, rosin, velocity,
//                           detune, width, seating, pad, spacing, close, room, hall, gain, expression, articulation,
//                           hallType, bassDouble, eqLow, eqMid, eqHigh) – e.g. to render a factory preset
#include "opensection/Engine.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace opensection;

struct Ev { double t; Event e; };

static bool readNotes(const char* path, std::vector<Ev>& out, double& end)
{
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    end = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        if (line.rfind("cc", 0) == 0) {
            std::string cc; double t; int num; float v;
            in >> cc >> t >> num >> v;
            Event e{}; e.type = num == 64 ? Event::Sustain : Event::Expression; e.value = v;
            out.push_back({ t, e }); end = std::max(end, t);
            continue;
        }
        double on, off; int key; float vel;
        if (!(in >> on >> off >> key >> vel)) continue;
        Event a{}; a.type = Event::NoteOn; a.key = key; a.value = vel;
        Event b{}; b.type = Event::NoteOff; b.key = key;
        out.push_back({ on, a }); out.push_back({ off, b });
        end = std::max(end, off);
    }
    std::stable_sort(out.begin(), out.end(), [](const Ev& x, const Ev& y) {
        if (x.t != y.t) return x.t < y.t;
        return x.e.type == Event::NoteOff && y.e.type != Event::NoteOff;   // offs first
    });
    return true;
}

static void writeWav(const char* path, const std::vector<float>& L, const std::vector<float>& R, int sr)
{
    FILE* f = std::fopen(path, "wb");
    uint32_t n = (uint32_t)L.size(), data = n * 8, riff = 36 + data, fmt = 16, sr32 = sr, br = sr * 8;
    uint16_t pcm = 3, ch = 2, ba = 8, bps = 32;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&sr32, 4, 1, f);
    std::fwrite(&br, 4, 1, f); std::fwrite(&ba, 2, 1, f); std::fwrite(&bps, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < n; ++i) { std::fwrite(&L[i], 4, 1, f); std::fwrite(&R[i], 4, 1, f); }
    std::fclose(f);
}

int main(int argc, char** argv)
{
    std::string modelPath; int sr = 48000, block = 128; uint32_t seed = 1;
    bool dry = false, bench = false, solo = false, setFlutter = false; float bassDouble = 0, eq[3] = {}; int hallType = 0, articulation = 0; float trem[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0.38f}, pz[5] = {}; bool setTrem = false, setPizz = false; float vibrato = -1, flutter[4] = {}, harmvar = -1, bdir[4] = {}; bool setBody = false; float quality = 1.0f, tail = 4.0f;
    float coupling = -1, detune = -1, rosin = -1, spacing = -1, width = -1;
    std::vector<std::string> sets;
    float unrest[4] = { -1, -1, -1, -1 }; float er = -1, late = -1; float rel = -1, rt60 = -1, hf = -1;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--model") modelPath = next();
        else if (a == "--sr") sr = std::atoi(next());
        else if (a == "--block") block = std::atoi(next());
        else if (a == "--seed") seed = (uint32_t)std::atoi(next());
        else if (a == "--dry") dry = true;
        else if (a == "--solo") solo = true;
        else if (a == "--vibrato") vibrato = (float)std::atof(next());
        else if (a == "--harmvar") harmvar = (float)std::atof(next());
        else if (a == "--bodydirect") { std::sscanf(next(), "%f,%f,%f,%f", &bdir[0], &bdir[1], &bdir[2], &bdir[3]); setBody = true; }
        else if (a == "--flutter") { std::sscanf(next(), "%f,%f,%f,%f", &flutter[0], &flutter[1], &flutter[2], &flutter[3]); setFlutter = true; }
        else if (a == "--bench") bench = true;
        else if (a == "--quality") quality = (float)std::atof(next());
        else if (a == "--tail") tail = (float)std::atof(next());
        else if (a == "--unrest") std::sscanf(next(), "%f,%f,%f,%f", &unrest[0], &unrest[1], &unrest[2], &unrest[3]);
        else if (a == "--room") std::sscanf(next(), "%f,%f", &er, &late);
        else if (a == "--width") width = (float)std::atof(next());
        else if (a == "--spacing") spacing = (float)std::atof(next());
        else if (a == "--coupling") coupling = (float)std::atof(next());
        else if (a == "--detune") detune = (float)std::atof(next());
        else if (a == "--rosin") rosin = (float)std::atof(next());
        else if (a == "--bassdouble") bassDouble = (float)std::atof(next());
        else if (a == "--halltype") hallType = std::atoi(next());
        else if (a == "--art") articulation = std::atoi(next());
        else if (a == "--trem") { std::sscanf(next(), "%f,%f,%f,%f,%f,%f,%f,%f,%f", &trem[0], &trem[1], &trem[2], &trem[3], &trem[4], &trem[5], &trem[6], &trem[7], &trem[8]); setTrem = true; }
        else if (a == "--pizz") { std::sscanf(next(), "%f,%f,%f,%f,%f", &pz[0], &pz[1], &pz[2], &pz[3], &pz[4]); setPizz = true; }
        else if (a == "--set") sets.push_back(next());
        else if (a == "--eq") std::sscanf(next(), "%f,%f,%f", &eq[0], &eq[1], &eq[2]);
        else if (a == "--release") std::sscanf(next(), "%f,%f,%f", &rel, &rt60, &hf);
        else pos.push_back(argv[i]);
    }
    if (pos.size() < (bench ? 1u : 2u)) { std::fprintf(stderr, "usage: see source header\n"); return 2; }

    Model model = Model::builtin();
    if (!modelPath.empty()) {
        std::ifstream f(modelPath); std::stringstream ss; ss << f.rdbuf();
        std::string err; model = Model::fromText(ss.str(), &err);
        if (!err.empty()) { std::fprintf(stderr, "model: %s\n", err.c_str()); return 1; }
    }
    if (setFlutter) for (int r = 0; r < 4; ++r) model.flutter[r] = flutter[r];
    if (setBody) for (int r = 0; r < 4; ++r) model.bodyDirect[r] = bdir[r];
    if (harmvar >= 0) model.harmVar = harmvar;
    if (setTrem) { for (int r = 0; r < 4; ++r) { model.tremRate[r] = trem[r]; model.tremDepth[r] = trem[4 + r]; } model.tremWidth = trem[8]; }
    if (setPizz) { for (int r = 0; r < 4; ++r) model.pizzDecay[r] = pz[r]; model.pizzBright = pz[4]; }
    std::vector<Ev> evs; double end;
    if (!readNotes(pos[0], evs, end)) { std::fprintf(stderr, "cannot read %s\n", pos[0]); return 1; }

    Engine eng(model, seed);
    eng.prepare(sr, block);
    Params p; p.quality = quality; p.dryOnly = dry; p.solo = solo;
    if (coupling >= 0) p.coupling = coupling;
    if (vibrato >= 0) p.vibrato = vibrato;
    p.bassDouble = bassDouble; p.hallType = hallType; p.articulation = articulation; p.eqLow = eq[0]; p.eqMid = eq[1]; p.eqHigh = eq[2];
    if (detune >= 0) p.detune = detune;
    if (rosin >= 0) p.rosin = rosin;
    if (spacing >= 0) p.spacing = spacing;
    if (width >= 0) p.width = width;
    for (auto& kv : sets) {
        const auto eq = kv.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "--set needs id=value: %s\n", kv.c_str()); return 2; }
        const std::string id = kv.substr(0, eq); const float v = (float)std::atof(kv.c_str() + eq + 1);
        if (id == "attack") p.attack = v; else if (id == "release") p.release = v; else if (id == "vibrato") p.vibrato = v;
        else if (id == "unrest") p.unrest = v; else if (id == "rosin") p.rosin = v; else if (id == "velocity") p.velocity = v;
        else if (id == "detune") p.detune = v; else if (id == "width") p.width = v; else if (id == "seating") p.seating = v; else if (id == "pad") p.pad = v; else if (id == "spacing") p.spacing = v;
        else if (id == "close") p.closeMic = v; else if (id == "room") p.roomMic = v; else if (id == "hall") p.hall = v;
        else if (id == "gain") p.gainDb = v; else if (id == "expression") p.expression = v;
        else if (id == "articulation") p.articulation = (int)v; else if (id == "hallType") p.hallType = (int)v;
        else if (id == "bassDouble") p.bassDouble = v; else if (id == "eqLow") p.eqLow = v; else if (id == "eqMid") p.eqMid = v;
        else if (id == "eqHigh") p.eqHigh = v;
        else { std::fprintf(stderr, "unknown parameter: %s\n", id.c_str()); return 2; }
    }
    eng.setParams(p);
    for (int r = 0; r < 4; ++r) if (unrest[r] >= 0) eng.setUnrestOverride(r, unrest[r]);
    if (er >= 0) eng.setRoomOverride(er, late);
    if (rel >= 0) eng.setReleaseOverride(rel, rt60, hf);

    const long total = (long)((end + tail) * sr);
    std::vector<float> L, R; if (!bench) { L.reserve(total); R.reserve(total); }
    std::vector<float> bl(block), br(block);
    std::vector<Event> blockEv;
    size_t ei = 0;
    double sum = 0, worst = 0; long blocks = 0; std::vector<double> times; int peakPlayers = 0, peakNotes = 0;
    for (long t0 = 0; t0 < total; t0 += block) {
        const int n = (int)std::min<long>(block, total - t0);
        blockEv.clear();
        while (ei < evs.size() && (long)(evs[ei].t * sr) < t0 + n) {
            Event e = evs[ei].e; e.offset = (int)std::max<long>(0, (long)(evs[ei].t * sr) - t0);
            blockEv.push_back(e); ++ei;
        }
        auto a = std::chrono::steady_clock::now();
        eng.process(bl.data(), br.data(), n, blockEv.data(), (int)blockEv.size());
        auto b = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(b - a).count();
        sum += us; worst = std::max(worst, us); ++blocks; times.push_back(us);
        peakPlayers = std::max(peakPlayers, eng.activePlayers()); peakNotes = std::max(peakNotes, eng.activeNotes());
        if (!bench) { L.insert(L.end(), bl.begin(), bl.begin() + n); R.insert(R.end(), br.begin(), br.begin() + n); }
    }
    const double blockUs = 1e6 * block / sr;
    std::sort(times.begin(), times.end());
    auto pct = [&](double q) { return times[std::min(times.size() - 1, (size_t)(q * times.size()))]; };
    std::printf("Perzentile: 50 %% %.0f µs, 99 %% %.0f µs, 99,9 %% %.0f µs (Zeitfenster %.0f µs)\n", pct(0.5), pct(0.99), pct(0.999), blockUs);
    std::printf("Blöcke %ld à %d, Ø %.1f µs (%.1f %% eines Kerns), schlechtester Block %.1f µs (%.0f %% des Zeitfensters), max %d Töne / %d Spieler\n",
                blocks, block, sum / blocks, 100.0 * sum / blocks / blockUs, worst, 100.0 * worst / blockUs, peakNotes, peakPlayers);
    if (!bench) writeWav(pos[1], L, R, sr);
    return 0;
}
