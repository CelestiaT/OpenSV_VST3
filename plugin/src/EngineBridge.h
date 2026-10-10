// M2a: OpenSV engine integration for the headless VST3 plugin.
//
// Pipeline: collected MIDI notes (absolute seconds) + optional per-note lyrics
//   -> sv::Project  -> ProjectRenderer (background thread, whole-song render)
//   -> std::atomic<std::shared_ptr<const RenderedAudio>>  -> callback samples
//   by host projectTimeSamples (fallback: accumulated block position).
//
// Voice bank configuration is a small JSON file so the plugin stays headless
// and XDG-clean:  ~/.config/opensv-vst3/config.json
//   { "voice": "/abs/path.nofs", "dict": "/abs/clf-data", "language": "japanese",
//     "lyric": "la" }
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "core/Project.h"

namespace opensv::plugin
{

struct RenderedAudio
{
    juce::AudioBuffer<float> samples; // channel 0/1 = stereo, project-relative
    double sampleRate = 48000.0;
    std::uint64_t revision = 0;
};

class EngineBridge : private juce::Thread
{
public:
    struct NoteRec
    {
        double startSec = 0.0;
        double endSec = 0.0;
        int pitch = 60;

        bool operator==(const NoteRec&) const = default;
    };

    struct Config
    {
        std::string voiceBank;        // .nofs path
        std::string dictionary;       // clf-data dir
        std::string language = "japanese";
        std::string lyric = "la";     // G2P fallback syllable for MIDI mode
    };

    struct Status
    {
        bool rendering = false;
        bool hasAudio = false;
        double songSeconds = 0.0;
        juce::String error;
    };

    EngineBridge();
    ~EngineBridge() override;

    static juce::File defaultConfigFile();
    bool loadConfigFile(const juce::File& file); // returns true when changed
    Config config() const;
    void setConfig(const Config& cfg);           // bumps revision

    void setNotes(std::vector<NoteRec> notes);   // bumps revision (debounced)
    [[nodiscard]] std::size_t noteCount() const;

    void setSampleRate(double rate);             // render rate for next pass
    void commitAndRender();                      // trigger render now

    // Audio thread: fill output with engine audio at absolute project
    // position (samples). Returns false if no audio for that position yet.
    bool renderInto(juce::AudioBuffer<float>& buffer, std::int64_t absoluteStartSample);

    bool isRendering() const { return renderingFlag.load(); }
    Status status() const;

    // test hook: skip transport gating, render on demand (unit/debug).
    std::function<void()> onRenderFinished; // message-thread safe? no — worker thread, debug only

private:
    void run() override;
    sv::Project buildProject() const;
    void publish(std::shared_ptr<const RenderedAudio> audio);

    mutable std::mutex dataMutex;
    Config cfg;
    std::vector<NoteRec> notes;
    double renderSampleRate = 48000.0;
    std::atomic<std::uint64_t> requestedRevision{ 1 };
    std::atomic<std::uint64_t> completedRevision{ 0 };
    std::atomic<bool> cancelFlag{ false };

    std::atomic<std::shared_ptr<const RenderedAudio>> publishedAudio;
    std::atomic<bool> renderingFlag{ false };
    juce::String lastError; // guarded by errorMutex for status()
    mutable std::mutex errorMutex;
    std::atomic<std::int64_t> songSampleCount{ 0 }; // at renderSampleRate

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EngineBridge)
};

} // namespace opensv::plugin
