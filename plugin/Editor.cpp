// OpenSection – SPDX-License-Identifier: GPL-3.0-or-later
#include "Editor.h"

using namespace juce;

namespace col {
const Colour bg { 0xff15171c }, panel { 0xff1e2128 }, line { 0xff2c313b };
const Colour text { 0xffe8e4dc }, dim { 0xff8d929c }, accent { 0xffd9a35b }, accent2 { 0xff7fa7c9 };
}

OpenSectionLook::OpenSectionLook()
{
    setColour(Slider::textBoxTextColourId, col::text);
    setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour(Slider::textBoxHighlightColourId, col::accent.withAlpha(0.3f));
    setColour(TextEditor::outlineColourId, Colours::transparentBlack);
    setColour(TextEditor::focusedOutlineColourId, col::accent);
    setColour(TextEditor::backgroundColourId, col::panel);
    setColour(Label::textColourId, col::dim);
    setColour(ComboBox::backgroundColourId, col::panel);
    setColour(ComboBox::outlineColourId, col::line);
    setColour(ComboBox::textColourId, col::text);
    setColour(ComboBox::arrowColourId, col::accent);
    setColour(PopupMenu::backgroundColourId, col::panel);
    setColour(PopupMenu::textColourId, col::text);
    setColour(PopupMenu::highlightedBackgroundColourId, col::accent.withAlpha(0.3f));
}

void OpenSectionLook::drawRotarySlider(Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider&)
{
    const auto b = Rectangle<float>((float)x, (float)y, (float)w, (float)h).reduced(6.f);
    const float r = jmin(b.getWidth(), b.getHeight()) / 2.f, cx = b.getCentreX(), cy = b.getCentreY();
    const float ang = start + pos * (end - start), th = 4.f;
    Path track; track.addCentredArc(cx, cy, r - th, r - th, 0.f, start, end, true);
    g.setColour(col::line); g.strokePath(track, PathStrokeType(th, PathStrokeType::curved, PathStrokeType::rounded));
    Path val; val.addCentredArc(cx, cy, r - th, r - th, 0.f, start, ang, true);
    g.setColour(col::accent); g.strokePath(val, PathStrokeType(th, PathStrokeType::curved, PathStrokeType::rounded));
    g.setColour(col::panel.brighter(0.15f)); g.fillEllipse(cx - r * 0.62f, cy - r * 0.62f, r * 1.24f, r * 1.24f);
    Path tick; tick.addRoundedRectangle(-1.5f, -r * 0.6f, 3.f, r * 0.32f, 1.5f);
    g.setColour(col::text); g.fillPath(tick, AffineTransform::rotation(ang).translated(cx, cy));
}

OpenSectionEditor::Knob& OpenSectionEditor::knob(const char* id, const char* text)
{
    auto k = std::make_unique<Knob>();
    k->slider.setSliderStyle(Slider::RotaryHorizontalVerticalDrag);
    k->slider.setTextBoxStyle(Slider::TextBoxBelow, false, 70, 16);
    k->slider.setPopupDisplayEnabled(false, false, this);
    k->slider.setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
    k->slider.setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    k->label.setText(text, dontSendNotification);
    k->label.setJustificationType(Justification::centred);
    k->label.setFont(FontOptions(13.f));
    addAndMakeVisible(k->slider); addAndMakeVisible(k->label);
    k->att = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(proc.params, id, k->slider);
    knobs.push_back(std::move(k));
    return *knobs.back();
}

OpenSectionEditor::OpenSectionEditor(OpenSectionProcessor& p) : AudioProcessorEditor(p), proc(p)
{
    setLookAndFeel(&look);
    groups.push_back({ "PLAYING", { &knob("velocity", "Velocity"), &knob("width", "Width"), &knob("seating", "Seating"), &knob("bassDouble", "Bass double") } });
    groups.push_back({ "BOWING", { &knob("attack", "Attack"), &knob("release", "Release"), &knob("vibrato", "Vibrato"),
                                   &knob("unrest", "Unrest"), &knob("rosin", "Rosin"), &knob("detune", "Intonation") } });
    groups.push_back({ "MICROPHONES", { &knob("close", "Close"), &knob("spacing", "Spacing"), &knob("room", "Room"), &knob("hall", "Hall") } });
    groups.push_back({ "TONE", { &knob("eqLow", "Low"), &knob("eqMid", "Mid"), &knob("eqHigh", "High") } });
    groups.push_back({ "OUTPUT", { &knob("expression", "Bow pressure"), &knob("gain", "Volume") } });

    auto setupCombo = [&](ComboBox& c, Label& l, const char* id, const char* text,
                          std::unique_ptr<AudioProcessorValueTreeState::ComboBoxAttachment>& att) {
        c.addItemList(proc.params.getParameter(id)->getAllValueStrings(), 1);
        addAndMakeVisible(c);
        att = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment>(proc.params, id, c);
        l.setText(text, dontSendNotification); l.setFont(FontOptions(13.f));
        addAndMakeVisible(l);
    };
    setupCombo(quality, qualityLabel, "quality", "Quality", qualityAtt);
    setupCombo(exprSource, exprLabel, "exprSource", "Expression pedal", exprAtt);
    setupCombo(articulation, artLabel, "articulation", "Articulation", artAtt);
    setupCombo(hallType, hallLabel, "hallType", "Room", hallAtt);
    for (int i = 0; i < proc.getNumPrograms(); ++i) preset.addItem(proc.getProgramName(i), i + 1);
    preset.setSelectedId(proc.getCurrentProgram() + 1, dontSendNotification);
    preset.onChange = [this] { if (preset.getSelectedId() > 0) proc.applyPreset(preset.getSelectedId() - 1); };
    addAndMakeVisible(preset);
    presetLabel.setText("Preset", dontSendNotification); presetLabel.setFont(FontOptions(13.f));
    addAndMakeVisible(presetLabel);

    setResizable(true, true);
    setResizeLimits(820, 560, 1640, 1120);
    getConstrainer()->setFixedAspectRatio(820.0 / 560.0);
    setSize(820, 560);
    startTimerHz(15);
}

OpenSectionEditor::~OpenSectionEditor() { setLookAndFeel(nullptr); }

void OpenSectionEditor::timerCallback()
{
    status = String(proc.uiNotes.load()) + " notes  ·  " + String(proc.uiPlayers.load()) + " players  ·  CPU "
           + String(roundToInt(proc.uiLoad.load() * 100.f)) + " %";
    exprShown = proc.uiExpression.load();
    repaint(0, 0, getWidth(), 56);
}

void OpenSectionEditor::paint(Graphics& g)
{
    const float s = getWidth() / 820.f;
    g.fillAll(col::bg);
    g.setColour(col::text);
    g.setFont(FontOptions(26.f * s, Font::bold));
    g.drawText("OpenSection", Rectangle<float>(20 * s, 10 * s, 300 * s, 30 * s), Justification::left);
    g.setColour(col::dim);
    g.setFont(FontOptions(12.f * s));
    g.drawText(String::fromUTF8("computed string section  \xc2\xb7  v" JucePlugin_VersionString), Rectangle<float>(22 * s, 36 * s, 400 * s, 14 * s), Justification::left);
    g.drawText(status, Rectangle<float>(420 * s, 16 * s, 380 * s, 16 * s), Justification::right);
    // expression meter
    const auto m = Rectangle<float>(680 * s, 38 * s, 120 * s, 5 * s);
    g.setColour(col::line); g.fillRoundedRectangle(m, 2.5f * s);
    g.setColour(col::accent2); g.fillRoundedRectangle(m.withWidth(m.getWidth() * exprShown), 2.5f * s);
    g.setFont(FontOptions(11.f * s));
    g.drawText("pedal", Rectangle<float>(620 * s, 33 * s, 55 * s, 14 * s), Justification::right);

    for (auto& gr : groups) {
        auto a = gr.area.toFloat();
        g.setColour(col::panel); g.fillRoundedRectangle(a, 8.f * s);
        g.setColour(col::accent.withAlpha(0.85f));
        g.setFont(FontOptions(11.f * s, Font::bold));
        g.drawText(gr.title, a.removeFromTop(24 * s).reduced(12 * s, 0), Justification::left);
    }
}

void OpenSectionEditor::resized()
{
    const float s = getWidth() / 820.f;
    auto sc = [s](float v) { return roundToInt(v * s); };
    auto r = getLocalBounds().reduced(sc(16));
    auto head = r.removeFromTop(sc(48));
    // preset selector in the header, between title and status
    auto pr = head.withTrimmedLeft(sc(250)).withWidth(sc(300)).withSizeKeepingCentre(sc(300), sc(26));
    presetLabel.setBounds(pr.removeFromLeft(sc(52)));
    preset.setBounds(pr);
    auto bottom = r.removeFromBottom(sc(36));
    r.removeFromBottom(sc(8));

    // two rows of groups: playing + bowing, then microphones + tone + output
    const int gap = sc(10);
    auto rowA = r.removeFromTop((r.getHeight() - gap) / 2);
    r.removeFromTop(gap);
    auto layoutRow = [&](Rectangle<int> row, std::vector<Group*> gs) {
        float total = 0; for (auto* g : gs) total += (float)g->knobs.size();
        const int avail = row.getWidth() - gap * ((int)gs.size() - 1);
        for (auto* gr : gs) {
            gr->area = gr == gs.back() ? row : row.removeFromLeft((int)(avail * gr->knobs.size() / total));
            row.removeFromLeft(gap);
            auto a = gr->area.reduced(sc(6));
            a.removeFromTop(sc(22));
            const int kw = a.getWidth() / (int)gr->knobs.size();
            for (auto* k : gr->knobs) {
                auto c = a.removeFromLeft(kw);
                k->label.setBounds(c.removeFromTop(sc(18)));
                k->label.setFont(FontOptions(13.f * s));
                k->slider.setTextBoxStyle(Slider::TextBoxBelow, false, sc(70), sc(16));
                const int maxH = sc(118);
                if (c.getHeight() > maxH) c = c.withSizeKeepingCentre(c.getWidth(), maxH);
                k->slider.setBounds(c.withSizeKeepingCentre(std::min(c.getWidth(), sc(84)), c.getHeight()));
            }
        }
    };
    layoutRow(rowA, { &groups[0], &groups[1] });
    layoutRow(r, { &groups[2], &groups[3], &groups[4] });

    auto place = [&](Label& l, ComboBox& c, int lw, int cw) {
        l.setBounds(bottom.removeFromLeft(sc(lw)));
        c.setBounds(bottom.removeFromLeft(sc(cw)).reduced(0, sc(4)));
        bottom.removeFromLeft(sc(14));
    };
    place(artLabel, articulation, 76, 120);
    place(hallLabel, hallType, 42, 120);
    place(qualityLabel, quality, 50, 90);
    place(exprLabel, exprSource, 112, 150);
}
