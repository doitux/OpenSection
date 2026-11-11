// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
#include "Processor.h"
#include "Editor.h"

using namespace juce;
using APVTS = AudioProcessorValueTreeState;

namespace ids {
static const char* quality = "quality";
static const char* width = "width";
static const char* seating = "seating";
static const char* attack = "attack";
static const char* release = "release";
static const char* vibrato = "vibrato";
static const char* unrest = "unrest";
static const char* rosin = "rosin";
static const char* velocity = "velocity";
static const char* detune = "detune";
static const char* spacing = "spacing";
static const char* closeMic = "close";
static const char* roomMic = "room";
static const char* hall = "hall";
static const char* gain = "gain";
static const char* exprSource = "exprSource";
static const char* expression = "expression";
static const char* articulation = "articulation";
static const char* hallType = "hallType";
static const char* bassDouble = "bassDouble";
static const char* eqLow = "eqLow";
static const char* eqMid = "eqMid";
static const char* eqHigh = "eqHigh";
}

// factory presets: only the listed parameters change, everything else goes back to its default
struct Preset { const char* name; std::vector<std::pair<const char*, float>> values; };
static const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        { "Standard", {} },
        // pads that are needed all the time: slow, wide, calm, out of the way of the bass
        { "String Pad", { { ids::attack, 2.2f }, { ids::release, 2.2f }, { ids::width, 1.3f }, { ids::closeMic, 0.7f },
                          { ids::roomMic, 1.2f }, { ids::hall, 1.4f }, { ids::rosin, 0.4f }, { ids::vibrato, 0.75f },
                          { ids::unrest, 0.5f }, { ids::detune, 0.7f }, { ids::velocity, 0.4f }, { ids::eqLow, -1.5f },
                          { ids::eqHigh, -2.f }, { ids::seating, 0.85f }, { ids::gain, -5.5f } } },
        { "Warm Strings 1", { { ids::attack, 1.3f }, { ids::release, 1.4f }, { ids::roomMic, 1.1f }, { ids::hall, 1.1f },
                              { ids::rosin, 0.6f }, { ids::vibrato, 0.9f }, { ids::eqLow, 1.f }, { ids::eqMid, -1.f },
                              { ids::eqHigh, -2.5f }, { ids::velocity, 0.8f }, { ids::seating, 0.7f }, { ids::gain, -2.5f } } },
        { "Warm Strings 2", { { ids::attack, 1.6f }, { ids::release, 1.8f }, { ids::width, 1.2f }, { ids::closeMic, 0.85f },
                              { ids::roomMic, 1.2f }, { ids::hall, 1.3f }, { ids::rosin, 0.4f }, { ids::vibrato, 0.85f },
                              { ids::eqLow, 2.f }, { ids::eqMid, -2.f }, { ids::eqHigh, -4.f }, { ids::bassDouble, 0.4f },
                              { ids::velocity, 0.7f }, { ids::seating, 0.9f }, { ids::gain, -4.f } } },
        // the opposite: every section a tight group in its place, close and focused
        { "Ensemble", { { ids::seating, 0.1f }, { ids::width, 0.9f }, { ids::closeMic, 1.2f }, { ids::roomMic, 0.85f },
                        { ids::hall, 0.8f }, { ids::gain, -5.f } } },
        { "Hinter Klavier", { { ids::attack, 1.4f }, { ids::release, 1.3f }, { ids::closeMic, 0.8f }, { ids::roomMic, 1.1f },
                              { ids::eqLow, -2.5f }, { ids::eqMid, -1.5f }, { ids::eqHigh, -1.f }, { ids::velocity, 0.7f },
                              { ids::vibrato, 0.9f }, { ids::gain, -8.f } } },
        { "Weit & weich", { { ids::attack, 1.8f }, { ids::release, 1.8f }, { ids::width, 1.35f }, { ids::closeMic, 0.6f },
                            { ids::roomMic, 1.3f }, { ids::hall, 1.4f }, { ids::hallType, 2.f }, { ids::eqHigh, -3.f },
                            { ids::vibrato, 0.8f }, { ids::rosin, 0.6f }, { ids::gain, -7.f } } },
        { "Nah & klar", { { ids::attack, 0.8f }, { ids::release, 0.8f }, { ids::closeMic, 1.3f }, { ids::roomMic, 0.7f },
                          { ids::hall, 0.6f }, { ids::hallType, 1.f }, { ids::eqHigh, 1.5f }, { ids::spacing, 0.8f } } },
        { "Fundament", { { ids::bassDouble, 0.8f }, { ids::eqLow, 2.f }, { ids::release, 1.2f }, { ids::gain, -7.f } } },
        { "Dämpfer-Fläche", { { ids::articulation, 1.f }, { ids::attack, 1.6f }, { ids::release, 1.5f }, { ids::hall, 1.3f },
                              { ids::vibrato, 0.85f }, { ids::gain, -4.f } } },
        { "Tremolo", { { ids::articulation, 2.f }, { ids::attack, 0.7f }, { ids::hall, 1.2f } } },
        { "Pizzicato", { { ids::articulation, 3.f }, { ids::roomMic, 1.2f }, { ids::hall, 1.2f } } },
    };
    return list;
}

int OpenSectionProcessor::getNumPrograms() { return (int)presets().size(); }
const String OpenSectionProcessor::getProgramName(int i) { return i >= 0 && i < getNumPrograms() ? String::fromUTF8(presets()[(size_t)i].name) : String(); }
void OpenSectionProcessor::setCurrentProgram(int i)
{
    if (i == currentProgram) return;          // hosts re-send the program on load: keep the saved tweaks
    applyPreset(i);
}

void OpenSectionProcessor::applyPreset(int i)
{
    if (i < 0 || i >= getNumPrograms()) return;
    currentProgram = i;
    for (auto* prm : getParameters())
        if (auto* r = dynamic_cast<RangedAudioParameter*>(prm)) {
            if (r->getParameterID() == ids::exprSource || r->getParameterID() == ids::quality) continue;   // the player's setup stays
            float v = r->convertFrom0to1(r->getDefaultValue());
            for (auto& kv : presets()[(size_t)i].values) if (r->getParameterID() == kv.first) v = kv.second;
            r->setValueNotifyingHost(r->convertTo0to1(v));
        }
}

APVTS::ParameterLayout OpenSectionProcessor::layout()
{
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto pct = [](float v, int) { return String(roundToInt(v * 100)) + " %"; };
    auto add = [&](const char* id, const char* name, float lo, float hi, float def, float skewMid = -1,
                   std::function<String(float, int)> fmt = {}) {
        NormalisableRange<float> r(lo, hi);
        if (skewMid > 0) r.setSkewForCentre(skewMid);
        p.push_back(std::make_unique<AudioParameterFloat>(ParameterID { id, 1 }, name, r, def,
                                                          AudioParameterFloatAttributes().withStringFromValueFunction(fmt ? fmt : pct)));
    };
    p.push_back(std::make_unique<AudioParameterChoice>(ParameterID { ids::quality, 1 }, "Quality",
                                                       StringArray { "Eco", "Normal", "Studio" }, 1));
    add(ids::width, "Width", 0.f, 1.5f, 1.f);
    add(ids::seating, "Seating", 0.f, 1.f, 0.5f, -1, [](float v, int) {
        return String(v < 0.2f ? "Block" : v < 0.4f ? "Close" : v <= 0.6f ? "Orchestra" : v < 0.8f ? "Loose" : "Mixed"); });
    add(ids::attack, "Bow attack", 0.3f, 3.f, 1.f, 1.f);
    add(ids::release, "Release", 0.3f, 4.f, 1.f, 1.f);
    add(ids::vibrato, "Vibrato", 0.f, 2.f, 1.f);
    add(ids::unrest, "Unrest", 0.f, 2.f, 0.7f);
    add(ids::rosin, "Rosin", 0.f, 4.f, 1.f, 1.f);
    add(ids::detune, "Intonation spread", 0.f, 4.f, 0.8f, 1.2f);
    add(ids::spacing, "Mic spacing", 0.f, 2.f, 1.f);
    add(ids::velocity, "Velocity", 0.f, 1.f, 1.f);
    add(ids::closeMic, "Close mics", 0.f, 2.f, 1.f);
    add(ids::roomMic, "Room mics", 0.f, 2.f, 1.f);
    add(ids::hall, "Hall", 0.f, 3.f, 1.f, 1.f);
    add(ids::gain, "Volume", -24.f, 12.f, -6.f, -1, [](float v, int) { return String(v, 1) + " dB"; });
    p.push_back(std::make_unique<AudioParameterChoice>(ParameterID { ids::exprSource, 1 }, "Expression source",
                                                       StringArray { "CC11 Expression", "CC1 Mod wheel", "CC2 Breath", "Off (use knob)" }, 0));
    add(ids::expression, "Bow pressure", 0.f, 1.f, 1.f);
    p.push_back(std::make_unique<AudioParameterChoice>(ParameterID { ids::articulation, 1 }, "Articulation",
                                                       StringArray { "Arco", "Con sordino", "Tremolo", "Pizzicato" }, 0));
    p.push_back(std::make_unique<AudioParameterChoice>(ParameterID { ids::hallType, 1 }, "Room",
                                                       StringArray { "Concert hall", "Chamber", "Church", "Studio" }, 0));
    add(ids::bassDouble, "Bass doubling", 0.f, 1.f, 0.f);
    auto db = [](float v, int) { return (v > 0 ? "+" : "") + String(v, 1) + " dB"; };
    add(ids::eqLow, "EQ low", -12.f, 12.f, 0.f, -1, db);
    add(ids::eqMid, "EQ mid", -12.f, 12.f, 0.f, -1, db);
    add(ids::eqHigh, "EQ high", -12.f, 12.f, 0.f, -1, db);
    return { p.begin(), p.end() };
}

OpenSectionProcessor::OpenSectionProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", AudioChannelSet::stereo(), true)),
      params(*this, nullptr, "OpenSection", layout()),
      engine(opensection::Model::builtin())
{
    events.reserve(2048);
}

bool OpenSectionProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

void OpenSectionProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    engine.prepare(sampleRate, std::max(samplesPerBlock, 1));
    engine.setParams(readParams());
    blockSeconds = 0;
}

opensection::Params OpenSectionProcessor::readParams() const
{
    auto v = [this](const char* id) { return params.getRawParameterValue(id)->load(); };
    opensection::Params p;
    const int q = (int)v(ids::quality);
    p.quality = q == 0 ? 0.4f : q == 1 ? 0.7f : 1.0f;
    p.width = v(ids::width);
    p.seating = v(ids::seating);
    p.attack = v(ids::attack);
    p.release = v(ids::release);
    p.vibrato = v(ids::vibrato);
    p.unrest = v(ids::unrest);
    p.rosin = v(ids::rosin);
    p.velocity = v(ids::velocity);
    p.detune = v(ids::detune);
    p.spacing = v(ids::spacing);
    p.closeMic = v(ids::closeMic);
    p.roomMic = v(ids::roomMic);
    p.hall = v(ids::hall);
    p.gainDb = v(ids::gain);
    p.articulation = (int)v(ids::articulation);
    p.hallType = (int)v(ids::hallType);
    p.bassDouble = v(ids::bassDouble);
    p.eqLow = v(ids::eqLow); p.eqMid = v(ids::eqMid); p.eqHigh = v(ids::eqHigh);
    return p;
}

void OpenSectionProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    const auto t0 = Time::getHighResolutionTicks();
    const int n = buffer.getNumSamples();
    engine.setParams(readParams());

    const int src = (int)params.getRawParameterValue(ids::exprSource)->load();
    const int exprCC = src == 0 ? 11 : src == 1 ? 1 : src == 2 ? 2 : -1;
    using E = opensection::Event;
    events.clear();
    if (exprCC < 0) ccExpression = params.getRawParameterValue(ids::expression)->load();
    events.push_back({ E::Expression, 0, 0, ccExpression });

    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        const int off = jlimit(0, n - 1, meta.samplePosition);
        if (events.size() >= events.capacity()) break;
        if (m.isNoteOn()) events.push_back({ E::NoteOn, off, m.getNoteNumber(), m.getFloatVelocity() });
        else if (m.isNoteOff()) events.push_back({ E::NoteOff, off, m.getNoteNumber(), 0.f });
        else if (m.isSustainPedalOn()) events.push_back({ E::Sustain, off, 0, 1.f });
        else if (m.isSustainPedalOff()) events.push_back({ E::Sustain, off, 0, 0.f });
        else if (m.isController() && m.getControllerNumber() == exprCC) {
            ccExpression = m.getControllerValue() / 127.f;
            events.push_back({ E::Expression, off, 0, ccExpression });
        } else if (m.isPitchWheel()) events.push_back({ E::PitchBend, off, 0, (m.getPitchWheelValue() - 8192) / 8192.f });
        else if (m.isAllNotesOff() || m.isAllSoundOff()) events.push_back({ E::AllOff, off, 0, 0.f });
    }
    midi.clear();

    float* L = buffer.getWritePointer(0);
    float* R = buffer.getWritePointer(1);
    engine.process(L, R, n, events.data(), (int)events.size());

    // load meter for the editor
    const double used = Time::highResolutionTicksToSeconds(Time::getHighResolutionTicks() - t0);
    const double avail = n / getSampleRate();
    load += 0.05f * ((float)(used / avail) - load);
    uiLoad.store(load); uiNotes.store(engine.activeNotes()); uiPlayers.store(engine.activePlayers());
    uiExpression.store(ccExpression);
}

void OpenSectionProcessor::getStateInformation(MemoryBlock& destData)
{
    auto state = params.copyState();
    std::unique_ptr<XmlElement> xml(state.createXml());
    xml->setAttribute("program", currentProgram);
    xml->setAttribute("programName", getProgramName(currentProgram));   // indices may move when presets are added
    copyXmlToBinary(*xml, destData);
}

void OpenSectionProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml && xml->hasTagName(params.state.getType())) {
        // by name (0.2.1+); older states only have the index into the 0.2.0 list
        static const char* list020[] = { "Standard", "Hinter Klavier", "Weit & weich", "Nah & klar", "Fundament",
                                          "Dämpfer-Fläche", "Tremolo", "Pizzicato" };
        String name = xml->getStringAttribute("programName");
        const int idx = xml->getIntAttribute("program", 0);
        if (name.isEmpty() && idx >= 0 && idx < 8) name = String::fromUTF8(list020[idx]);
        currentProgram = 0;
        for (int i = 0; i < getNumPrograms(); ++i) if (getProgramName(i) == name) currentProgram = i;
        xml->removeAttribute("program"); xml->removeAttribute("programName");
        params.replaceState(ValueTree::fromXml(*xml));
    }
}

AudioProcessorEditor* OpenSectionProcessor::createEditor() { return new OpenSectionEditor(*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OpenSectionProcessor(); }
