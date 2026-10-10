// Headless smoke test for the VST3 plugin's EngineBridge (M2):
//   notes -> sv::Project -> ProjectRenderer -> RenderedAudio -> renderInto.
// Usage: opensv-engine-smoke <voice.nofs> <clf-data-dir> [out.wav]
#include "EngineBridge.h"

#include "audio/WaveFile.h"

#include <juce_events/juce_events.h>

#include <cstdio>
#include <string>

using namespace opensv::plugin;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <voice.nofs> <clf-data> [out.wav]\n", argv[0]);
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI scopedJuce; // message manager for Thread/Timer

    EngineBridge bridge;
    bridge.setSampleRate(48000.0);

    EngineBridge::Config cfg;
    cfg.voiceBank = argv[1];
    cfg.dictionary = argv[2];
    cfg.language = "japanese";
    cfg.lyric = "la";
    bridge.setConfig(cfg);

    // C-major-ish arpeggio sung with "la": 8 quarter notes at 120bpm, ~0.5s each.
    std::vector<EngineBridge::NoteRec> notes;
    const int pitches[] = { 60, 64, 67, 72, 71, 67, 64, 60 };
    for (int i = 0; i < 8; ++i)
        notes.push_back({ 0.2 + i * 0.5, 0.2 + (i + 1) * 0.5 - 0.05, pitches[i] });
    bridge.setNotes(notes);
    bridge.commitAndRender();

    // Wait (bounded) for the worker.
    const double deadlineSeconds = 120.0;
    const juce::int64 start = juce::Time::getMillisecondCounter();
    while (juce::Time::getMillisecondCounterHiRes() * 0.001 - start * 0.001 < deadlineSeconds * 1000.0)
    {
        const auto st = bridge.status();
        if (!st.rendering)
        {
            if (st.hasAudio)
                break;
            if (st.error.isNotEmpty())
            {
                std::fprintf(stderr, "render error: %s\n", st.error.toRawUTF8());
                return 1;
            }
        }
        juce::Thread::sleep(50);
    }

    auto status = bridge.status();
    if (!status.hasAudio)
    {
        std::fprintf(stderr, "no audio produced; error=%s\n", status.error.toRawUTF8());
        return 1;
    }

    // Pull the rendered song through the callback API (position 0..N).
    const int totalSamples = static_cast<int>(status.songSeconds * 48000.0) + 1024;
    juce::AudioBuffer<float> out(2, totalSamples);
    out.clear(0, totalSamples); // AudioBuffer does not zero-initialise
    const bool filled = bridge.renderInto(out, 0);
    if (!filled)
    {
        std::fprintf(stderr, "renderInto returned false\n");
        return 1;
    }

    float peak = 0.0f;
    const int songSamples = std::min(totalSamples, static_cast<int>(status.songSeconds * 48000.0));
    for (int ch = 0; ch < 2; ++ch)
        peak = std::max(peak, out.getMagnitude(0, songSamples));
    std::printf("song=%.2fs peak=%.3f (%.1f%% FS)\n", status.songSeconds, peak, 100.0 * peak);

    const char* outPath = argc > 3 ? argv[3] : "/tmp/opensv-engine-smoke.wav";
    const juce::Result written = sv::audio::writeWaveFile(juce::File(outPath), out, 48000.0);
    if (written.failed())
    {
        std::fprintf(stderr, "failed writing %s: %s\n", outPath,
                     written.getErrorMessage().toRawUTF8());
        return 1;
    }
    std::printf("wrote %s\n", outPath);
    return peak > 0.01f ? 0 : 1;
}
