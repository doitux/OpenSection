// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "opensection/Engine.h"
#include <atomic>

class OpenSectionProcessor : public juce::AudioProcessor {
public:
    OpenSectionProcessor();
    ~OpenSectionProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "OpenSection"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram(int) override;
    void applyPreset(int);                    // from the editor: always applies
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState params;

    // for the editor
    std::atomic<int> uiNotes { 0 }, uiPlayers { 0 };
    std::atomic<float> uiLoad { 0.f }, uiExpression { 1.f };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    opensection::Params readParams() const;

    opensection::Engine engine;
    std::vector<opensection::Event> events;
    float ccExpression = 1.f;
    double blockSeconds = 0;
    float load = 0;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenSectionProcessor)
};
