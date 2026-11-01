// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
// Minimal CLAP host for automated tests: loads a .clap, plays a notes file
// (same format as opensection-render, incl. "cc t num value" lines sent as MIDI),
// writes a float WAV and prints timing.
//   claphost <plugin.clap> <notes.txt> <out.wav> [block=128] [sr=48000]
#include <clap/clap.h>
#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Ev { double t; int kind; int a; float v; };   // kind 0 on, 1 off, 2 cc

static const void* hostGetExt(const clap_host*, const char*) { return nullptr; }
static void hostNop(const clap_host*) {}

struct InEvents {
    std::vector<clap_event_note_t> notes;
    std::vector<clap_event_midi_t> midis;
    std::vector<const clap_event_header_t*> order;
    static uint32_t size(const clap_input_events* l) { return (uint32_t)((InEvents*)l->ctx)->order.size(); }
    static const clap_event_header_t* get(const clap_input_events* l, uint32_t i) { return ((InEvents*)l->ctx)->order[i]; }
};
static bool outTry(const clap_output_events*, const clap_event_header_t*) { return true; }

int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: claphost plugin.clap notes.txt out.wav [block] [sr]\n"); return 2; }
    const int block = argc > 4 ? std::atoi(argv[4]) : 128, sr = argc > 5 ? std::atoi(argv[5]) : 48000;
    std::string path = argv[1];
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto* entry = (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
    if (!entry || !entry->init(path.c_str())) { std::fprintf(stderr, "no clap_entry\n"); return 1; }
    auto* fac = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t* desc = fac->get_plugin_descriptor(fac, 0);
    std::printf("Plugin: %s (%s) v%s\n", desc->name, desc->id, desc->version);

    clap_host host {};
    host.clap_version = CLAP_VERSION; host.name = "opensection-test"; host.vendor = "OpenSection"; host.version = "1";
    host.get_extension = hostGetExt; host.request_restart = hostNop; host.request_process = hostNop; host.request_callback = hostNop;
    const clap_plugin_t* pl = fac->create_plugin(fac, &host, desc->id);
    if (!pl || !pl->init(pl)) { std::fprintf(stderr, "init failed\n"); return 1; }
    if (!pl->activate(pl, sr, 1, (uint32_t)block)) { std::fprintf(stderr, "activate failed\n"); return 1; }
    pl->start_processing(pl);

    std::vector<Ev> evs; double end = 0;
    std::ifstream f(argv[2]); std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        if (line.rfind("cc", 0) == 0) { std::string c; double t; int n; float v; in >> c >> t >> n >> v; evs.push_back({ t, 2, n, v }); end = std::max(end, t); continue; }
        double a, b; int k; float v;
        if (in >> a >> b >> k >> v) { evs.push_back({ a, 0, k, v }); evs.push_back({ b, 1, k, 0 }); end = std::max(end, b); }
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.t < y.t || (x.t == y.t && x.kind == 1 && y.kind != 1); });

    std::vector<float> L(block), R(block), outL, outR;
    float* ch[2] = { L.data(), R.data() };
    clap_audio_buffer_t ob {}; ob.data32 = ch; ob.channel_count = 2;
    InEvents ie; clap_input_events in { &ie, InEvents::size, InEvents::get };
    clap_output_events out { nullptr, outTry };
    const long total = (long)((end + 4.0) * sr);
    size_t ei = 0; double sum = 0, worst = 0; long blocks = 0, worstAt = 0;
    for (long t0 = 0; t0 < total; t0 += block) {
        ie.notes.clear(); ie.midis.clear(); ie.order.clear();
        ie.notes.reserve(256); ie.midis.reserve(256);
        while (ei < evs.size() && (long)(evs[ei].t * sr) < t0 + block) {
            const Ev& e = evs[ei++];
            const uint32_t off = (uint32_t)std::max<long>(0, (long)(e.t * sr) - t0);
            if (e.kind == 2) {
                clap_event_midi_t m {}; m.header = { sizeof m, off, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
                m.data[0] = 0xB0; m.data[1] = (uint8_t)e.a; m.data[2] = (uint8_t)std::lround(e.v * 127); ie.midis.push_back(m);
            } else {
                clap_event_note_t n {}; n.header = { sizeof n, off, CLAP_CORE_EVENT_SPACE_ID, (uint16_t)(e.kind == 0 ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
                n.note_id = -1; n.port_index = 0; n.channel = 0; n.key = (int16_t)e.a; n.velocity = e.v; ie.notes.push_back(n);
            }
        }
        for (auto& m : ie.midis) ie.order.push_back(&m.header);
        for (auto& n : ie.notes) ie.order.push_back(&n.header);
        std::stable_sort(ie.order.begin(), ie.order.end(), [](auto* a, auto* b) { return a->time < b->time; });
        clap_process_t p {}; p.steady_time = t0; p.frames_count = (uint32_t)block;
        p.audio_outputs = &ob; p.audio_outputs_count = 1; p.in_events = &in; p.out_events = &out;
        auto a = std::chrono::steady_clock::now();
        pl->process(pl, &p);
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
        if (us > worst) worstAt = blocks;
        sum += us; worst = std::max(worst, us); ++blocks;
        outL.insert(outL.end(), L.begin(), L.end()); outR.insert(outR.end(), R.begin(), R.end());
    }
    std::printf("Blöcke %ld, Ø %.1f %% eines Kerns, schlechtester Block %.0f µs (Block %ld)\n", blocks, 100.0 * sum / blocks / (1e6 * block / sr), worst, worstAt);
    pl->stop_processing(pl); pl->deactivate(pl); pl->destroy(pl); entry->deinit();

    FILE* w = std::fopen(argv[3], "wb");
    uint32_t n = (uint32_t)outL.size(), data = n * 8, riff = 36 + data, fmt = 16, s32 = sr, br = sr * 8;
    uint16_t pcm = 3, chn = 2, ba = 8, bps = 32;
    std::fwrite("RIFF", 1, 4, w); std::fwrite(&riff, 4, 1, w); std::fwrite("WAVEfmt ", 1, 8, w); std::fwrite(&fmt, 4, 1, w);
    std::fwrite(&pcm, 2, 1, w); std::fwrite(&chn, 2, 1, w); std::fwrite(&s32, 4, 1, w); std::fwrite(&br, 4, 1, w);
    std::fwrite(&ba, 2, 1, w); std::fwrite(&bps, 2, 1, w); std::fwrite("data", 1, 4, w); std::fwrite(&data, 4, 1, w);
    for (uint32_t i = 0; i < n; ++i) { std::fwrite(&outL[i], 4, 1, w); std::fwrite(&outR[i], 4, 1, w); }
    std::fclose(w);
    return 0;
}
