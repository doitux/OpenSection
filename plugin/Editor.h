// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Processor.h"

class OpenSectionLook : public juce::LookAndFeel_V4 {
public:
    OpenSectionLook();
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
};

class OpenSectionEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit OpenSectionEditor(OpenSectionProcessor&);
    ~OpenSectionEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Knob {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    struct Group { juce::String title; std::vector<Knob*> knobs; juce::Rectangle<int> area; };

    void timerCallback() override;
    Knob& knob(const char* id, const char* text);

    OpenSectionProcessor& proc;
    OpenSectionLook look;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<Group> groups;
    juce::ComboBox quality, exprSource, articulation, hallType, preset;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> qualityAtt, exprAtt, artAtt, hallAtt;
    juce::Label qualityLabel, exprLabel, artLabel, hallLabel, presetLabel;
    juce::String status;
    float exprShown = 1.f;
};
