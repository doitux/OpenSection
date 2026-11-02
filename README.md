# OpenSection

**A computed string section – no gigabytes of samples.**
Free and open source (GPLv3), for Linux, Windows and macOS.

OpenSection builds an orchestral string section the way a physical-modelling piano builds a piano:
every player is computed in real time. A few kilobytes of *learned statistics* replace gigabytes of
recordings.

> Status: **early development** – real-time engine, learning tool and plugin (VST3 / CLAP / LV2 /
> standalone; AU on macOS) work. The full user interface (stage view, EQ, hall types, presets) is next.

## How it works

1. **Every player is computed.** A bowed string moves in a sawtooth-like "Helmholtz motion": the bow
   grabs the string, it slips, flies back, is grabbed again – hundreds of times per second. Each virtual
   player has their own slight detune, drift, vibrato (a random walk, not an LFO), entry time, bow
   changes, bow-pressure unrest and rosin noise at every slip.
2. **A section is a swarm.** Up to 6 basses, 8 celli, 10 violas, 14 violins per note (scalable). The
   ensemble sound emerges from many small differences – no chorus effect. Every section keeps to
   its real range and has a home range where it plays fully; in between, neighbouring sections
   share a note and fade smoothly (e.g. c' is played by celli, violas and violins, mostly violas;
   violins never below g, celli never below C).
3. **Every desk has its own instrument.** Bodies with ~100 narrow resonances each. Vibrato sweeps the
   harmonics across these resonances – the "shimmer" of real strings.
4. **Learned room.** Part of the room stays with each desk (the players sit *in* the hall), part is
   spread wide; back desks are further away (later, darker, more room). Room level, how much stays at
   the desk, mic spacing, hall decay and the release are calibrated against the reference.
5. **Learned fingerprint.** A learning tool plays the engine against a sampled section (key by key,
   closed loop) and stores only the tone-colour curve per key, the unrest per register, and the
   room calibration (stereo width and reverb energy) – about 22 KB.
6. **Learned dynamics.** Two more sources teach how the colour changes with bow pressure: a section
   with recorded soft and loud layers (VSCO2-CE) and solo players from pianissimo to fortissimo
   (Philharmonia Orchestra, University of Iowa). The engine keeps one wavetable set per learned level
   (pp · soft · loud · ff); key velocity and the expression pedal glide between them – soft
   playing gets darker and the bow noise softer, fortissimo brighter. Still only two table reads
   per sample, and the loud colour stays exactly as learned.
7. **Room.** Close mics, room mics with per-desk early reflections (decorrelated left/right),
   and an FDN hall.

### Real-time design

| part | cost |
|---|---|
| player (wavetable that already contains the learned colour, rosin noise, envelopes) | ~25 operations / sample |
| 16 desk bodies (partitioned FFT convolution, idle desks are skipped) | fixed |
| room reflections, hall | fixed |

Measured (48 kHz, 128-sample buffer, one core of a shared 2.8 GHz Xeon – numbers vary by a few percent between runs):

| scenario | quality | average load | 99 % of blocks below |
|---|---|---|---|
| piano-style arpeggios with sustain pedal, up to 20 notes / 146 players | normal (0.7) | 12–15 % | 0.9 ms of 2.67 ms |
| same | eco (0.4) | 10 % | 0.58 ms |
| same | studio (1.0) | 14 % | 1.0 ms |

On the GitHub build machines (same sustain test, quality *studio*, 204 players): Linux 4.8 %,
Windows 6.2 %, macOS 5.9 % of one core; 99 % of blocks finish in 0.2–0.6 ms.

No added latency. Tone colour of the learned model deviates from the reference by 0.7 dB on
average (1/6-octave smoothed, 80 Hz–12 kHz); stereo width and reverb energy are calibrated to it.

## Building

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/opensection-render notes.txt out.wav        # offline render
./build/opensection-render --bench notes.txt        # live-performance benchmark
```

`notes.txt`: one line per note `start_s end_s midi velocity(0..1)`, plus optional
`cc time_s 64|11 value(0..1)` lines for sustain pedal / expression.

## Re-learning the model

```bash
python3 tools/learn/learn.py --vpo <Virtual-Playing-Orchestra> --sfizz-render <sfizz_render> \
    --engine build/opensection-render --reference <progression.wav> --notes <progression.txt> \
    --out models/opensection.osm
python3 tools/learn/sources/vsco2_sfz.py <VSCO-2-CE> vsco2-strings.sfz
python3 tools/learn/sources/philharmonia.py <Philharmonia download>/Strings philharmonia-dyn.npz
bash tools/learn/sources/fetch_iowa_mis.sh            # on a machine with internet access
python3 tools/learn/sources/iowa.py <unpacked Iowa-MIS-*.zip> iowa-dyn.npz
python3 tools/learn/learn_dynamics.py --model models/opensection.osm --sfz vsco2-strings.sfz \
    --philharmonia philharmonia-dyn.npz --iowa iowa-dyn.npz --sfizz-render <sfizz_render> \
    --engine build/opensection-render --out models/opensection.osm
python3 tools/learn/embed.py
```

## Credits

The shipped model was learned from **Virtual Playing Orchestra** by Paul Battersby
(<http://virtualplaying.com>), which combines Sonatina Symphonic Orchestra, No Budget Orchestra,
Mattias Westlund's and VSCO2-CE string samples. OpenSection contains no audio from these libraries –
only derived statistics (spectral envelopes per key, modulation depth, room width and decay).

The dynamics (how soft playing differs in colour from loud playing) were learned from
**VSCO2 Community Edition** by Versilian Studios (<https://vis.versilstudios.com/vsco-community.html>,
CC0) and from the **Philharmonia Orchestra sound samples** (<https://philharmonia.co.uk/resources/sound-samples/>,
CC BY-SA 3.0; sound samples © Philharmonia Orchestra) and the **University of Iowa Electronic Music
Studios Musical Instrument Samples** (<https://theremin.music.uiowa.edu/MIS.html>, free for any project). Again only difference curves are stored, no
audio. Details in [CREDITS.md](CREDITS.md).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

## Installing (Linux)

Download the build from the GitHub Actions artifacts (or build yourself), unpack and run
`bash OpenSection/install.sh`. It installs VST3, CLAP and LV2 into your home folder
(`~/.vst3`, `~/.clap`, `~/.lv2`) plus the standalone app. `bash install.sh --remove` uninstalls.

Windows and macOS builds are produced by CI (VST3, CLAP, standalone; AU on macOS) – testers welcome.
