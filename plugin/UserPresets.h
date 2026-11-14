// SPDX-License-Identifier: GPL-3.0-or-later
// User presets, shared by OpenSection and Sanctuary: one small XML file per preset in the user's
// settings folder
//   Linux   ~/.config/<Plugin>/Presets
//   Windows %APPDATA%\<Plugin>\Presets
//   macOS   ~/Library/<Plugin>/Presets
// Values are stored in real units (dB, seconds, %), so presets survive range changes in later
// versions; a parameter that did not exist when a preset was saved gets its default. The files can be
// copied to another computer.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <memory>

class UserPresets {
public:
    UserPresets(juce::String pluginName, juce::StringArray idsToKeep)
        : plugin(std::move(pluginName)), keep(std::move(idsToKeep)) {}

    juce::File folder() const
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile(plugin).getChildFile("Presets");
    }

    juce::StringArray list() const
    {
        juce::StringArray out;
        for (auto& f : folder().findChildFiles(juce::File::findFiles, false, "*.xml"))   // the file name is the preset's name
            if (auto xml = juce::XmlDocument::parse(f))
                if (xml->hasTagName("Preset")) out.add(f.getFileNameWithoutExtension());
        out.sortNatural();
        return out;
    }

    bool exists(const juce::String& name) const { return fileFor(name).existsAsFile(); }

    bool save(const juce::String& name, juce::AudioProcessor& proc, const juce::String& version) const
    {
        if (name.trim().isEmpty() || !folder().createDirectory()) return false;
        juce::XmlElement xml("Preset");
        xml.setAttribute("name", name.trim());
        xml.setAttribute("plugin", plugin);
        xml.setAttribute("version", version);
        for (auto* prm : proc.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*>(prm)) {
                if (keep.contains(r->getParameterID())) continue;
                auto* e = xml.createNewChildElement("Param");
                e->setAttribute("id", r->getParameterID());
                e->setAttribute("value", (double)r->convertFrom0to1(r->getValue()));
            }
        return xml.writeTo(fileFor(name));                                          // (a renamed file is a renamed preset)
    }

    // message thread only (parameters are set with host notification, so automation and undo see it)
    bool load(const juce::String& name, juce::AudioProcessor& proc) const
    {
        auto xml = juce::XmlDocument::parse(fileFor(name));
        if (xml == nullptr || !xml->hasTagName("Preset")) return false;
        for (auto* prm : proc.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*>(prm)) {
                if (keep.contains(r->getParameterID())) continue;
                float v = r->convertFrom0to1(r->getDefaultValue());
                for (auto* e : xml->getChildWithTagNameIterator("Param"))
                    if (e->getStringAttribute("id") == r->getParameterID()) v = (float)e->getDoubleAttribute("value", v);
                r->setValueNotifyingHost(r->convertTo0to1(v));
            }
        return true;
    }

    bool remove(const juce::String& name) const { return fileFor(name).deleteFile(); }

    // --- dialogs for the plugin window (asynchronous, no nested event loops) ---

    // asks for a name; if a preset of that name exists, asks before overwriting; then calls done(name)
    void askName(juce::Component* parent, const juce::String& suggestion, std::function<void(juce::String)> done) const
    {
        auto w = std::make_shared<juce::AlertWindow>("Save preset", "Name of your preset:", juce::MessageBoxIconType::NoIcon, parent);
        w->addTextEditor("name", suggestion);
        w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        auto self = *this;
        juce::Component::SafePointer<juce::Component> safeParent(parent);
        w->enterModalState(true, juce::ModalCallbackFunction::create([w, self, safeParent, done](int result) mutable {
            const auto name = w->getTextEditorContents("name").trim();
            juce::MessageManager::callAsync([w] {});           // the window is deleted after this callback
            if (result != 1 || name.isEmpty() || safeParent == nullptr) return;
            if (!self.exists(name)) { done(name); return; }
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                             .withIconType(juce::MessageBoxIconType::NoIcon)
                                             .withTitle("Overwrite preset?")
                                             .withMessage("\"" + name + "\" already exists. Replace it with the current settings?")
                                             .withButton("Overwrite").withButton("Cancel")
                                             .withAssociatedComponent(safeParent.getComponent()),
                                         [done, name](int r) { if (r == 1) done(name); });
        }), false);
        if (auto* ed = w->getTextEditor("name")) { ed->selectAll(); ed->grabKeyboardFocus(); }
    }

    static void askDelete(juce::Component* parent, const juce::String& name, std::function<void()> done)
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::NoIcon)
                                         .withTitle("Delete preset?")
                                         .withMessage("Delete your preset \"" + name + "\"? This cannot be undone.")
                                         .withButton("Delete").withButton("Cancel")
                                         .withAssociatedComponent(parent),
                                     [done](int r) { if (r == 1) done(); });
    }

private:
    juce::File fileFor(const juce::String& name) const
    {
        return folder().getChildFile(juce::File::createLegalFileName(name.trim()) + ".xml");
    }

    juce::String plugin;
    juce::StringArray keep;      // parameters that belong to the player's setup, not to a sound
};
