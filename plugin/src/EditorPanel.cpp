#include "EditorPanel.h"

#include <algorithm>

#include "OpenSVProcessor.h"

namespace opensv::plugin
{

namespace
{
constexpr juce::uint32 kBackground = 0xff1b1d22;
constexpr juce::uint32 kPanelText  = 0xffd8dee4;
constexpr juce::uint32 kDimText    = 0xff8a919e;
constexpr juce::uint32 kAccent     = 0xff9a6cff;
constexpr juce::uint32 kFieldBg    = 0xff23262e;
constexpr juce::uint32 kOutline    = 0xff3a3f4a;

const char* kLanguages[] = { "japanese", "english", "mandarin", "cantonese", "spanish" };
} // namespace

// ------------------------------------------------------------------ lifecycle

OpenSVEditor::OpenSVEditor(OpenSVProcessor& owner)
    : AudioProcessorEditor(&owner), owner_(owner), bridge_(owner.engineBridge())
{
    setSize(460, 500);
    setResizeLimits(430, 480, 1600, 1200);

    titleLabel_.setText("OpenSV — singing synthesis", juce::dontSendNotification);
    titleLabel_.setFont(juce::Font(18.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, juce::Colour{ kPanelText });
    hintLabel_.setText("Voice bank settings persist to config.json (hot-reloaded). Lyrics need the score bridge (M3).",
                       juce::dontSendNotification);
    hintLabel_.setFont(juce::Font(11.0f));
    hintLabel_.setColour(juce::Label::textColourId, juce::Colour{ kDimText });

    voiceLabel_.setText("Voice bank", juce::dontSendNotification);
    dictLabel_.setText("Dictionary", juce::dontSendNotification);
    langLabel_.setText("Language", juce::dontSendNotification);
    lyricLabel_.setText("Syllable", juce::dontSendNotification);
    for (juce::Label* l : { &hintLabel_, &voiceLabel_, &dictLabel_, &langLabel_, &lyricLabel_,
                            &genderLabel_, &breathLabel_, &vibberLabel_, &attenLabel_, &statusLabel_ })
        l->setColour(juce::Label::textColourId, juce::Colour{ kPanelText });

    for (const char* lang : kLanguages)
        langBox_.addItem(lang, langBox_.getNumItems() + 1);

    auto styleTextEditor = [](juce::TextEditor& te)
    {
        te.setColour(juce::TextEditor::backgroundColourId, juce::Colour{ kFieldBg });
        te.setColour(juce::TextEditor::outlineColourId, juce::Colour{ kOutline });
        te.setColour(juce::TextEditor::textColourId, juce::Colour{ kPanelText });
    };
    styleTextEditor(dictEdit_);
    styleTextEditor(lyricEdit_);
    dictEdit_.setTextToShowWhenEmpty("/path/to/clf-data", juce::Colour{ kDimText });
    lyricEdit_.setTextToShowWhenEmpty("la", juce::Colour{ kDimText });

    genderLabel_.setText("Gender", juce::dontSendNotification);
    breathLabel_.setText("Breathiness", juce::dontSendNotification);
    vibberLabel_.setText("Vibber", juce::dontSendNotification);
    attenLabel_.setText("Attenuation", juce::dontSendNotification);

    auto styleSlider = [](juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    };
    styleSlider(genderSlider_);
    styleSlider(breathSlider_);
    styleSlider(vibberSlider_);
    styleSlider(attenSlider_);

    auto attach = [this](juce::Slider& slider, const juce::String& id)
    {
        if (auto* param = paramById(id))
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(param))
                attachments_.push_back(std::make_unique<juce::SliderParameterAttachment>(*ranged, slider));
    };
    attach(genderSlider_, OpenSVProcessor::idGender);
    attach(breathSlider_, OpenSVProcessor::idBreath);
    attach(vibberSlider_, OpenSVProcessor::idVibber);
    attach(attenSlider_, OpenSVProcessor::idAttenuation);

    if (auto* tone = dynamic_cast<juce::AudioParameterBool*>(paramById(OpenSVProcessor::idTestTone)))
        testToneToggle_.setToggleState(tone->get(), juce::dontSendNotification);
    testToneToggle_.onClick = [this]
    {
        if (auto* tone = dynamic_cast<juce::AudioParameterBool*>(paramById(OpenSVProcessor::idTestTone)))
            *tone = testToneToggle_.getToggleState();
    };

    // ---- change handlers ----
    voiceBox_.onChange = [this] { if (!refreshing_) applyConfigFromUi(); };
    langBox_.onChange = [this] { if (!refreshing_) applyConfigFromUi(); };
    lyricEdit_.onReturnKey = [this] { applyConfigFromUi(); };
    dictEdit_.onReturnKey = [this] { applyConfigFromUi(); };

    voiceFolderButton_.onClick = [this]
    {
        pickFolder([this](const juce::File& dir)
        {
            auto next = bridge_.config();
            const auto path = dir.getFullPathName().toStdString();
            if (std::find(next.scanDirs.begin(), next.scanDirs.end(), path) == next.scanDirs.end())
                next.scanDirs.push_back(path);
            bridge_.setConfig(next);
            bridge_.saveConfigFile();
            rebuildVoiceList();
        });
    };

    dictButton_.onClick = [this]
    {
        pickFolder([this](const juce::File& dir)
        {
            dictEdit_.setText(dir.getFullPathName(), juce::dontSendNotification);
            applyConfigFromUi();
        });
    };

    renderButton_.onClick = [this] { owner_.triggerCommitRender(); };
    reloadButton_.onClick = [this]
    {
        owner_.reloadConfigFile();
        pullConfigIntoUi();
        rebuildVoiceList();
    };

    statusLabel_.setFont(juce::Font(12.0f));
    statusLabel_.setJustificationType(juce::Justification::topLeft);
    pathLabel_.setFont(juce::Font(10.5f));
    pathLabel_.setColour(juce::Label::textColourId, juce::Colour{ kDimText });

    addAndMakeVisible(titleLabel_);
    addAndMakeVisible(hintLabel_);
    addAndMakeVisible(voiceLabel_);
    addAndMakeVisible(voiceBox_);
    addAndMakeVisible(voiceFolderButton_);
    addAndMakeVisible(dictLabel_);
    addAndMakeVisible(dictEdit_);
    addAndMakeVisible(dictButton_);
    addAndMakeVisible(langLabel_);
    addAndMakeVisible(langBox_);
    addAndMakeVisible(lyricLabel_);
    addAndMakeVisible(lyricEdit_);
    addAndMakeVisible(genderLabel_);
    addAndMakeVisible(genderSlider_);
    addAndMakeVisible(breathLabel_);
    addAndMakeVisible(breathSlider_);
    addAndMakeVisible(vibberLabel_);
    addAndMakeVisible(vibberSlider_);
    addAndMakeVisible(attenLabel_);
    addAndMakeVisible(attenSlider_);
    addAndMakeVisible(testToneToggle_);
    addAndMakeVisible(renderButton_);
    addAndMakeVisible(reloadButton_);
    addAndMakeVisible(statusLabel_);
    addAndMakeVisible(pathLabel_);

    pullConfigIntoUi();
    rebuildVoiceList();
    startTimerHz(3);
}

OpenSVEditor::~OpenSVEditor()
{
    stopTimer();
}

// ------------------------------------------------------------------ painting

void OpenSVEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour{ kBackground });
    g.setColour(juce::Colour{ kAccent });
    g.fillRect(0, 0, 3, getHeight());
}

void OpenSVEditor::resized()
{
    auto area = getLocalBounds().reduced(18, 14);
    const int labelW = 104;
    const int rowH = 26;
    const int gap = 8;

    titleLabel_.setBounds(area.removeFromTop(26));
    hintLabel_.setBounds(area.removeFromTop(18));
    area.removeFromTop(6);

    auto row = [&](juce::Label& label, juce::Component& first, juce::Component* second,
                   int secondW)
    {
        const int x = area.getX();
        const int y = area.getY();
        label.setBounds(x, y, labelW, rowH);
        const juce::Rectangle<int> rest{ x + labelW + gap, y, area.getWidth() - labelW - gap, rowH };
        if (second != nullptr)
        {
            first.setBounds(rest.withWidth(rest.getWidth() - secondW - gap));
            second->setBounds(rest.getRight() - secondW, y, secondW, rowH);
        }
        else
        {
            first.setBounds(rest);
        }
        area.removeFromTop(rowH + gap);
    };

    row(voiceLabel_, voiceBox_, &voiceFolderButton_, 84);
    row(dictLabel_, dictEdit_, &dictButton_, 40);
    row(langLabel_, langBox_, nullptr, 0);
    row(lyricLabel_, lyricEdit_, nullptr, 0);

    row(genderLabel_, genderSlider_, nullptr, 0);
    row(breathLabel_, breathSlider_, nullptr, 0);
    row(vibberLabel_, vibberSlider_, nullptr, 0);
    row(attenLabel_, attenSlider_, nullptr, 0);

    testToneToggle_.setBounds(area.removeFromTop(rowH).withWidth(240));
    area.removeFromTop(gap);

    auto buttons = area.removeFromTop(rowH + 4);
    renderButton_.setBounds(buttons.removeFromLeft(160));
    reloadButton_.setBounds(buttons.removeFromLeft(130).reduced(8, 0));

    area.removeFromTop(10);
    statusLabel_.setBounds(area.removeFromTop(46));
    pathLabel_.setBounds(area.removeFromTop(16));
}

// ------------------------------------------------------------------ config i/o

void OpenSVEditor::applyConfigFromUi()
{
    auto next = bridge_.config();

    const int voiceId = voiceBox_.getSelectedId();
    if (voiceId >= 1 && voiceId <= voicePaths_.size())
        next.voiceBank = voicePaths_[voiceId - 1].toStdString();
    else if (voiceId >= 1 && voicePaths_.isEmpty())
        next.voiceBank.clear();

    const auto dict = dictEdit_.getText().trim();
    if (dict.isNotEmpty())
        next.dictionary = dict.toStdString();

    const int langId = langBox_.getSelectedId();
    if (langId >= 1 && langId <= static_cast<int>(std::size(kLanguages)))
        next.language = kLanguages[langId - 1];

    const auto lyric = lyricEdit_.getText().trim();
    if (lyric.isNotEmpty())
        next.lyric = lyric.toStdString();

    bridge_.setConfig(next);
    bridge_.saveConfigFile();
    rebuildVoiceList();
}

void OpenSVEditor::pullConfigIntoUi()
{
    const auto cfg = bridge_.config();
    dictEdit_.setText(juce::String(cfg.dictionary), juce::dontSendNotification);
    lyricEdit_.setText(juce::String(cfg.lyric), juce::dontSendNotification);
    for (int i = 0; i < static_cast<int>(std::size(kLanguages)); ++i)
    {
        if (cfg.language == kLanguages[i])
        {
            langBox_.setSelectedId(i + 1, juce::dontSendNotification);
            break;
        }
    }
}

// ------------------------------------------------------------------ voice list

void OpenSVEditor::rebuildVoiceList()
{
    juce::StringArray paths;
    for (const auto& f : bridge_.scanVoiceBanks())
        paths.add(f.getFullPathName());

    const auto current = juce::String(bridge_.config().voiceBank);

    if (paths == lastVoicePaths_ && voiceBox_.getNumItems() > 0)
    {
        refreshing_ = true;
        voiceBox_.setSelectedId(paths.indexOf(current) + 1, juce::dontSendNotification);
        refreshing_ = false;
        return;
    }
    lastVoicePaths_ = paths;

    refreshing_ = true;
    voiceBox_.clear(juce::dontSendNotification);
    int selectedId = -1;
    for (int i = 0; i < paths.size(); ++i)
    {
        voiceBox_.addItem(juce::File(paths[i]).getFileNameWithoutExtension(), i + 1);
        if (paths[i] == current)
            selectedId = i + 1;
    }
    voicePaths_ = paths;
    if (selectedId < 0)
    {
        voiceBox_.addItem("(none)", 1);
        voiceBox_.setSelectedId(1, juce::dontSendNotification);
    }
    else
    {
        voiceBox_.setSelectedId(selectedId, juce::dontSendNotification);
    }
    refreshing_ = false;
}

// ------------------------------------------------------------------ helpers

void OpenSVEditor::pickFolder(std::function<void(const juce::File&)> onPicked)
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Choose a folder", juce::File::getSpecialLocation(juce::File::userHomeDirectory));

    const int flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    auto safe = safeThis_;
    chooser_->launchAsync(flags, [safe, onPicked](const juce::FileChooser& fc)
    {
        if (safe == nullptr)
            return;
        const auto result = fc.getResult();
        if (result.isDirectory())
            onPicked(result);
    });
}

juce::AudioProcessorParameter* OpenSVEditor::paramById(const juce::String& id)
{
    for (auto* param : owner_.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(param))
            if (withId->paramID == id)
                return param;
    return nullptr;
}

void OpenSVEditor::timerCallback()
{
    const auto st = bridge_.status();
    const auto cfg = bridge_.config();
    const juce::File cfgFile = EngineBridge::defaultConfigFile();

    juce::String line = "Voice: ";
    line << (cfg.voiceBank.empty() ? "(none)" : juce::File(cfg.voiceBank).getFileNameWithoutExtension());
    line << "   notes: " << juce::String(static_cast<int>(bridge_.noteCount()));
    if (st.rendering)
        line << "   rendering " << juce::String(st.songSeconds, 1) << "/" << juce::String(st.wantedSeconds, 1) << "s...";
    else if (st.hasAudio)
        line << "   audio ready (" << juce::String(st.songSeconds, 1) << "s)";
    else
        line << "   idle (notes stream in during playback)";
    if (st.error.isNotEmpty())
        line << "\nerror: " << st.error;

    statusLabel_.setText(line, juce::dontSendNotification);
    pathLabel_.setText(cfgFile.getFullPathName(), juce::dontSendNotification);
}

} // namespace opensv::plugin
