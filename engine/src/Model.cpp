// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
#include "opensection/Model.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace opensection {

static float interpLog(const std::vector<float>& xs, const std::vector<float>& ys, float x)
{
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back()) return ys.back();
    auto it = std::upper_bound(xs.begin(), xs.end(), x);
    size_t i = (size_t)(it - xs.begin());
    float x0 = std::log2(xs[i - 1]), x1 = std::log2(xs[i]);
    float w = (std::log2(x) - x0) / (x1 - x0);
    return ys[i - 1] + w * (ys[i] - ys[i - 1]);
}

static float interpKeys(const std::vector<int>& keys, const std::vector<float>& bandHz,
                        const std::vector<std::vector<float>>& curves, int midi, float hz)
{
    if (keys.empty() || curves.size() != keys.size()) return 0.f;
    if (midi <= keys.front()) return interpLog(bandHz, curves.front(), hz);
    if (midi >= keys.back()) return interpLog(bandHz, curves.back(), hz);
    auto it = std::upper_bound(keys.begin(), keys.end(), midi);
    size_t i = (size_t)(it - keys.begin());
    float w = float(midi - keys[i - 1]) / float(keys[i] - keys[i - 1]);
    return (1 - w) * interpLog(bandHz, curves[i - 1], hz) + w * interpLog(bandHz, curves[i], hz);
}

float Model::colourDb(int midi, float hz) const { return interpKeys(keys, bandHz, curves, midi, hz); }
float Model::dynDb(size_t node, int midi, float hz) const { return interpKeys(keys, bandHz, dynNodes[node].curves, midi, hz); }

Model Model::flat()
{
    Model m;
    for (int i = 0; i < 55; ++i) {
        float f = 40.f * std::pow(2.f, i / 6.f);
        if (f >= 20000.f) break;
        m.bandHz.push_back(f);
    }
    m.keys = { 60 };
    m.curves = { std::vector<float>(m.bandHz.size(), 0.f) };
    return m;
}

Model Model::fromText(const std::string& text, std::string* error)
{
    Model m;
    std::istringstream in(text);
    std::string tok;
    auto fail = [&](const char* what) { if (error) *error = what; return Model::flat(); };
    if (!(in >> tok) || tok != "OSM1") return fail("not an OpenSection model");
    while (in >> tok) {
        if (tok == "#") { std::getline(in, tok); continue; }
        if (tok == "bands") {
            int n; in >> n; m.bandHz.resize(n);
            for (auto& f : m.bandHz) in >> f;
        } else if (tok == "key") {
            int k; in >> k; m.keys.push_back(k);
            m.curves.emplace_back(m.bandHz.size());
            for (auto& v : m.curves.back()) in >> v;
        } else if (tok == "unrest") {
            for (auto& u : m.unrest) in >> u;
        } else if (tok == "flutter") {
            for (auto& u : m.flutter) in >> u;
        } else if (tok == "harmvar") { in >> m.harmVar;
        } else if (tok == "bodydirect") {
            for (auto& u : m.bodyDirect) in >> u;
        } else if (tok == "er") { in >> m.erGain;
        } else if (tok == "late") { in >> m.lateGain;
        } else if (tok == "out") { in >> m.outGain;
        } else if (tok == "release") { in >> m.release;
        } else if (tok == "hall") { in >> m.hallRt60 >> m.hallHf;
        } else if (tok == "erdiff") { in >> m.erDiffuse;
        } else if (tok == "spacing") { in >> m.spacing;
        } else if (tok == "dyn") { in >> m.dynLoud >> m.dynSoft;
        } else if (tok == "noisesoft") { for (auto& f : m.noiseSoftHz) in >> f;
        } else if (tok == "dynnode") {
            m.dynNodes.emplace_back(); in >> m.dynNodes.back().vel;
        } else if (tok == "dk") {                          // curve of the latest node, keys in order
            int k; in >> k;
            if (m.dynNodes.empty()) return fail("dk before dynnode");
            auto& c = m.dynNodes.back().curves;
            c.emplace_back(m.bandHz.size());
            for (auto& v : c.back()) in >> v;
            if (c.size() > m.keys.size() || m.keys[c.size() - 1] != k) return fail("dynamics curves do not match the keys");
        } else return fail("unknown token in model");
    }
    if (m.bandHz.empty() || m.keys.empty()) return fail("model incomplete");
    bool okDyn = true;
    for (auto& n : m.dynNodes)
        okDyn = okDyn && n.curves.size() == m.keys.size() && std::fabs(n.vel - m.dynLoud) > 0.02f;
    if (!okDyn) { m.dynNodes.clear(); m.noiseSoftHz = {}; }
    return m;
}

std::string Model::toText() const
{
    std::ostringstream o;
    o << "OSM1\n# OpenSection model: learned tone colour per key (dB on 1/6-octave grid)\n";
    o << "bands " << bandHz.size();
    for (float f : bandHz) o << ' ' << f;
    o << '\n';
    for (size_t i = 0; i < keys.size(); ++i) {
        o << "key " << keys[i];
        for (float v : curves[i]) o << ' ' << std::round(v * 100.f) / 100.f;
        o << '\n';
    }
    o << "unrest"; for (float u : unrest) o << ' ' << u; o << '\n';
    o << "flutter"; for (float u : flutter) o << ' ' << u; o << '\n';
    o << "harmvar " << harmVar << '\n';
    o << "bodydirect"; for (float u : bodyDirect) o << ' ' << u; o << '\n';
    o << "er " << erGain << "\nlate " << lateGain << "\nout " << outGain << '\n';
    o << "release " << release << "\nhall " << hallRt60 << ' ' << hallHf << '\n';
    o << "erdiff " << erDiffuse << "\nspacing " << spacing << '\n';
    if (!dynNodes.empty()) {
        o << "dyn " << dynLoud << ' ' << dynSoft << '\n';
        o << "noisesoft"; for (float f : noiseSoftHz) o << ' ' << f; o << '\n';
        for (auto& n : dynNodes) {
            o << "dynnode " << n.vel << '\n';
            for (size_t i = 0; i < keys.size(); ++i) {
                o << "dk " << keys[i];
                for (float v : n.curves[i]) o << ' ' << std::round(v * 100.f) / 100.f;
                o << '\n';
            }
        }
    }
    return o.str();
}

} // namespace opensection
