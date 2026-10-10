// M2 option-A: our own small native control panel for the OpenSV VST3.
//
// Deliberately NOT the OpenSV standalone GUI: this is a compact dark panel
// with voice-bank picking, expression sliders and render status only. The
// score-side UX (lyrics, per-note editing) belongs to the MuseScore QML
// bridge (docs/PLAN.md M4); everything here maps 1:1 onto config.json.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "EngineBridge.h"

namespace opensv::plugin
{

class OpenSVProcessor;

class OpenSVEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit OpenSVEditor(OpenSVProcessor& owner);
    ~OpenSVEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    void rebuildVoiceList();
    void applyConfigFromUi();      // UI -> bridge.setConfig + saveConfigFile
    void pullConfigIntoUi();       // bridge.config() -> widgets
    void pickFolder(std::function<void(const juce::File&)> onPicked);
    [[nodiscard]] juce::AudioProcessorParameter* paramById(const juce::String& id);

    OpenSVProcessor& owner_;
    EngineBridge& bridge_;

    juce::Label titleLabel_, hintLabel_;
    juce::Label voiceLabel_, dictLabel_, langLabel_, lyricLabel_;
    juce::ComboBox voiceBox_;
    juce::TextButton voiceFolderButton_{ "+ folder" };
    juce::TextEditor dictEdit_;
    juce::TextButton dictButton_{ "..." };
    juce::ComboBox langBox_;
    juce::TextEditor lyricEdit_;

    juce::Label genderLabel_, breathLabel_, vibberLabel_, attenLabel_;
    juce::Slider genderSlider_, breathSlider_, vibberSlider_, attenSlider_;
    juce::ToggleButton testToneToggle_{ "Test tone (placeholder)" };

    juce::TextButton renderButton_{ "Commit & Render" };
    juce::TextButton reloadButton_{ "Reload config" };
    juce::Label statusLabel_, pathLabel_;

    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> attachments_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Component::SafePointer<OpenSVEditor> safeThis_{ this };
    juce::StringArray voicePaths_; // combo itemID-1 -> absolute .nofs path
    juce::StringArray lastVoicePaths_;
    bool refreshing_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenSVEditor)
};

} // namespace opensv::plugin
