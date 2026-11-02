// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
#include "Processor.h"
#include "Editor.h"

using namespace juce;
using APVTS = AudioProcessorValueTreeState;

namespace ids {
static const char* quality = "quality";
static const char* width = "width";
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
    add(ids::attack, "Bow attack", 0.3f, 3.f, 1.f, 1.f);
    add(ids::release, "Release", 0.3f, 4.f, 1.f, 1.f);
    add(ids::vibrato, "Vibrato", 0.f, 2.f, 1.f);
    add(ids::unrest, "Unrest", 0.f, 2.f, 1.f);
    add(ids::rosin, "Rosin", 0.f, 4.f, 1.f, 1.f);
    add(ids::detune, "Intonation spread", 0.f, 4.f, 1.2f, 1.2f);
    add(ids::spacing, "Mic spacing", 0.f, 2.f, 1.f);
    add(ids::velocity, "Velocity", 0.f, 1.f, 1.f);
    add(ids::closeMic, "Close mics", 0.f, 2.f, 1.f);
    add(ids::roomMic, "Room mics", 0.f, 2.f, 1.f);
    add(ids::hall, "Hall", 0.f, 3.f, 1.f, 1.f);
    add(ids::gain, "Volume", -24.f, 12.f, -6.f, -1, [](float v, int) { return String(v, 1) + " dB"; });
    p.push_back(std::make_unique<AudioParameterChoice>(ParameterID { ids::exprSource, 1 }, "Expression source",
                                                       StringArray { "CC11 Expression", "CC1 Mod wheel", "CC2 Breath", "Off (use knob)" }, 0));
    add(ids::expression, "Bow pressure", 0.f, 1.f, 1.f);
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
    copyXmlToBinary(*xml, destData);
}

void OpenSectionProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml && xml->hasTagName(params.state.getType()))
        params.replaceState(ValueTree::fromXml(*xml));
}

AudioProcessorEditor* OpenSectionProcessor::createEditor() { return new OpenSectionEditor(*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OpenSectionProcessor(); }
