// OpenSV VST3 instrument — singing-synthesis processor with a small native
// control panel (voice bank pick + expression sliders + render status).
//
// Design contract (docs/PLAN.md):
//   * The editor is our own minimal panel, NOT the OpenSV standalone GUI.
//     The full score/lyric UI stays on the MuseScore side (QML bridge, M4).
//   * Voice banks, dictionary, language and placeholder syllable live in
//     config.json next to the log file and are hot-reloaded (1 s poll).
//   * M1 scope: parameters, state persistence, MIDI intake, transport probing
//     and a test-tone audition. OpenSVEngine is wired into processBlock in M2
//     behind this same skeleton.
#pragma once

#include <array>
#include <atomic>
#include <map>

#include <juce_audio_processors/juce_audio_processors.h>

#include "EngineBridge.h"

namespace opensv::plugin
{

class OpenSVProcessor final : public juce::AudioProcessor,
                              private juce::Timer
{
public:
    // Parameter identifiers (docs/PLAN.md §6).
    static const juce::String idVoice;       // choice: scanned voice banks (M3)
    static const juce::String idGender;      // -0.5..0.5
    static const juce::String idBreath;      //  0..1
    static const juce::String idVibber;      //  0..1
    static const juce::String idAttenuation; //  0..1
    static const juce::String idTestTone;    // bool: M1 audition mode
    static const juce::String idSyncNow;     // trigger: request bridge reload

    OpenSVProcessor();
    ~OpenSVProcessor() override;

    // -- AudioProcessor ------------------------------------------------------
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "OpenSV"; }

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.05; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // -- probing helpers (M1 validation, see docs/M1-VALIDATION.md) ----------
    juce::String probeSummary() const;

    // -- editor panel access ---------------------------------------------------
    EngineBridge& engineBridge() { return engine; }
    void triggerCommitRender();      // commit collected notes and render now
    void reloadConfigFile();         // re-read config.json from disk

private:
    void timerCallback() override; // message-thread logging; no GUI
    void handleMidi(juce::MidiBuffer&, std::int64_t blockStartSample, double sampleRate);
    void handleSyncTrigger();

    template <typename FloatType>
    void processBuffer(juce::AudioBuffer<FloatType>&, juce::MidiBuffer&);

    struct TestVoice
    {
        int pitch = -1;
        bool active = false;
        double phase = 0.0;
        double envelope = 0.0;
    };

    static constexpr int maxTestVoices = 16;
    std::array<TestVoice, maxTestVoices> testVoices;

    // Cached parameter values, refreshed once per block (audio-thread safe).
    struct Controls
    {
        float attenuation = 0.8f;
        bool testTone = true;
    };
    Controls controls;

    // Owned-by-base parameter pointers (fixed at construction, stable).
    juce::AudioParameterChoice* voiceParam = nullptr;
    juce::AudioParameterFloat* genderParam = nullptr;
    juce::AudioParameterFloat* breathParam = nullptr;
    juce::AudioParameterFloat* vibberParam = nullptr;
    juce::AudioParameterFloat* attenuationParam = nullptr;
    juce::AudioParameterBool* testToneParam = nullptr;
    juce::AudioParameterBool* syncNowParam = nullptr;

    // Transport/MIDI snapshot exported from the audio thread via atomics.
    std::atomic<std::int64_t> lastSamplePos{-1};
    std::atomic<std::int64_t> lastPpqPos{0};
    std::atomic<double> lastBpm{0.0};
    std::atomic<bool> lastPlaying{false};
    std::atomic<int> lastActiveNotes{0};
    std::atomic<std::int64_t> processBlockCounter{0};
    std::atomic<std::int64_t> syncRequests{0};

    std::int64_t lastLoggedBlocks = 0;
    bool lastSyncTriggerValue = false;
    std::int64_t lastSyncCount = 0;

    // ---- M2: engine wiring -------------------------------------------------
    EngineBridge engine;
    juce::CriticalSection pendingNotesCS;
    std::vector<EngineBridge::NoteRec> pendingNotes;   // guarded by pendingNotesCS
    std::map<int, double> openNoteTimes;               // audio-thread only
    std::int64_t fallbackPosSample = 0;                // when host gives no position
    bool lastTransportPlaying = false;
    juce::Time lastConfigCheck{};
    std::atomic<std::int64_t> lastPushedNotes{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OpenSVProcessor)
};

} // namespace opensv::plugin

// Single-config entry point required by juce_audio_plugin_client (JUCE 9).
// Must live in the global namespace.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
