// OpenSV VST3 instrument — headless processor (M1 plumbing shell).
// Implementation notes: docs/PLAN.md §4 M1, §6 parameter table.
#include "OpenSVProcessor.h"

#include <algorithm>
#include <cmath>

namespace opensv::plugin
{

const juce::String OpenSVProcessor::idVoice{ "voice" };
const juce::String OpenSVProcessor::idGender{ "gender" };
const juce::String OpenSVProcessor::idBreath{ "breath" };
const juce::String OpenSVProcessor::idVibber{ "vibber" };
const juce::String OpenSVProcessor::idAttenuation{ "attenuation" };
const juce::String OpenSVProcessor::idTestTone{ "testTone" };
const juce::String OpenSVProcessor::idSyncNow{ "syncNow" };

namespace
{
constexpr double kAttackSeconds = 0.005;
constexpr double kReleaseSeconds = 0.030;
constexpr std::int32_t kStateMagic = 0x4F535631; // 'OSV1'

juce::File logFile()
{
    // XDG-friendly: ~/.local/share/OpenSV/logs/vst3.log
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("OpenSV")
        .getChildFile("logs")
        .getChildFile("vst3.log");
}

juce::FileLogger& fileLogger()
{
    static std::unique_ptr<juce::FileLogger> instance = []
    {
        return std::unique_ptr<juce::FileLogger>(
            new juce::FileLogger(logFile(), "OpenSV VST3 log", 4 << 20));
    }();
    return *instance;
}

void logLine(const juce::String& text)
{
    fileLogger().writeToLog(text);
}
} // namespace

// ---------------------------------------------------------------- construction

OpenSVProcessor::OpenSVProcessor()
{
    auto voice = std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ idVoice, 1 }, "Voice",
        juce::StringArray{ "none (set via bridge)" }, 0);
    auto gender = std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ idGender, 2 }, "Gender",
        juce::NormalisableRange<float>{ -0.5f, 0.5f, 0.001f }, 0.0f);
    auto breath = std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ idBreath, 3 }, "Breathiness",
        juce::NormalisableRange<float>{ 0.0f, 1.0f, 0.001f }, 0.2f);
    auto vibber = std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ idVibber, 4 }, "Vibber",
        juce::NormalisableRange<float>{ 0.0f, 1.0f, 0.001f }, 0.0f);
    auto attenuation = std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ idAttenuation, 5 }, "Attenuation",
        juce::NormalisableRange<float>{ 0.0f, 1.0f, 0.001f }, 0.8f);
    auto testTone = std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{ idTestTone, 6 }, "Test Tone (M1)", true);
    auto syncNow = std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{ idSyncNow, 7 }, "Sync Now", false);

    voiceParam = voice.get();
    genderParam = gender.get();
    breathParam = breath.get();
    vibberParam = vibber.get();
    attenuationParam = attenuation.get();
    testToneParam = testTone.get();
    syncNowParam = syncNow.get();

    addParameter(voice.release());
    addParameter(gender.release());
    addParameter(breath.release());
    addParameter(vibber.release());
    addParameter(attenuation.release());
    addParameter(testTone.release());
    addParameter(syncNow.release());

    startTimerHz(1);
    if (const juce::File cfgFile = EngineBridge::defaultConfigFile(); cfgFile.existsAsFile())
        engine.loadConfigFile(cfgFile);
    logLine("OpenSV VST3 created (M2 engine bridge, headless)");
}

OpenSVProcessor::~OpenSVProcessor()
{
    stopTimer();
    logLine("OpenSV VST3 destroyed");
}

// ---------------------------------------------------------------- lifecycle

void OpenSVProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    for (auto& voice : testVoices)
        voice = {};
    fallbackPosSample = 0;
    openNoteTimes.clear();
    {
        const juce::ScopedLock lock(pendingNotesCS);
        pendingNotes.clear();
    }
    engine.setSampleRate(sampleRate);
    logLine(juce::String("prepareToPlay fs=") + juce::String(sampleRate, 1)
        + " block=" + juce::String(samplesPerBlock));
}

void OpenSVProcessor::releaseResources()
{
    for (auto& voice : testVoices)
        voice = {};
}

// ---------------------------------------------------------------- audio thread

template <typename FloatType>
void OpenSVProcessor::processBuffer(juce::AudioBuffer<FloatType>& buffer, juce::MidiBuffer& midi)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const double sampleRate = getSampleRate();
    if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0)
        return;

    // Snapshot cached control values once per block (pointer members, no
    // parameter-list walk on the audio thread).
    controls.attenuation = attenuationParam != nullptr ? attenuationParam->get() : 0.8f;
    controls.testTone = testToneParam != nullptr && testToneParam->get();

    // Transport: prefer the host-provided absolute position (M1 risk R2:
    // MuseScore fills VstProcessContext.projectTimeSamples; JUCE exposes it as
    // PositionInfo::getTimeInSamples()). Fall back to our accumulated counter.
    std::int64_t blockStartSample = fallbackPosSample;
    bool hostProvidedPos = false;
    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (auto samplePos = position->getTimeInSamples())
            {
                blockStartSample = static_cast<std::int64_t>(*samplePos);
                hostProvidedPos = true;
                lastSamplePos.store(blockStartSample);
            }
            if (auto ppq = position->getPpqPosition())
                lastPpqPos.store(static_cast<std::int64_t>(std::llround(*ppq)));
            if (auto bpm = position->getBpm())
                lastBpm.store(*bpm);
            lastPlaying.store(position->getIsPlaying());
        }
    }
    if (!hostProvidedPos)
        fallbackPosSample += numSamples;

    handleMidi(midi, blockStartSample, sampleRate);
    handleSyncTrigger();

    buffer.clear(0, 0, numSamples);

    // Engine path: whole-song render, sampled by absolute position. Only the
    // float bus is wired (MuseScore drives plugins in kSample32).
    bool engineProduced = false;
    if constexpr (std::is_same_v<FloatType, float>)
        engineProduced = engine.renderInto(buffer, blockStartSample);

    if (engineProduced)
    {
        buffer.applyGain(0, numSamples, controls.attenuation);
    }
    else if (controls.testTone)
    {
        const double gain = static_cast<double>(controls.attenuation) * 0.25;
        const double attackInc = 1.0 / (kAttackSeconds * sampleRate);
        const double releaseInc = 1.0 / (kReleaseSeconds * sampleRate);
        FloatType* const* channels = buffer.getArrayOfWritePointers();

        for (auto& voice : testVoices)
        {
            if (!voice.active && voice.envelope <= 0.0)
                continue;

            const double frequency = 440.0 * std::pow(2.0, (voice.pitch - 69) / 12.0);
            const double phaseInc = juce::MathConstants<double>::twoPi * frequency / sampleRate;

            for (int sample = 0; sample < numSamples; ++sample)
            {
                voice.envelope += voice.active ? attackInc : -releaseInc;
                voice.envelope = juce::jlimit(0.0, 1.0, voice.envelope);
                if (!voice.active && voice.envelope <= 0.0)
                    break;

                voice.phase += phaseInc;
                if (voice.phase > juce::MathConstants<double>::twoPi)
                    voice.phase -= juce::MathConstants<double>::twoPi;

                const auto value = static_cast<FloatType>(
                    std::sin(voice.phase) * voice.envelope * gain);
                for (int channel = 0; channel < numChannels; ++channel)
                    channels[channel][sample] += value;
            }
        }
    }

    processBlockCounter.fetch_add(1, std::memory_order_relaxed);
}

void OpenSVProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    processBuffer(buffer, midi);
}

void OpenSVProcessor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi)
{
    processBuffer(buffer, midi);
}

void OpenSVProcessor::handleMidi(juce::MidiBuffer& midi, const std::int64_t blockStartSample, const double sampleRate)
{
    for (const auto message : midi)
    {
        const juce::MidiMessage& mm = message.getMessage();
        const double eventSec = static_cast<double>(blockStartSample + message.samplePosition) / sampleRate;

        if (mm.isNoteOn())
        {
            const int pitch = mm.getNoteNumber();
            openNoteTimes[pitch] = eventSec; // engine collection (last wins)

            int slot = -1;
            for (std::size_t i = 0; i < testVoices.size(); ++i)
            {
                if (testVoices[i].active && testVoices[i].pitch == pitch)
                {
                    slot = static_cast<int>(i); // retrigger same pitch
                    break;
                }
            }
            if (slot < 0)
            {
                for (std::size_t i = 0; i < testVoices.size(); ++i)
                {
                    if (!testVoices[i].active && testVoices[i].envelope <= 0.0)
                    {
                        slot = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (slot < 0)
                slot = 0; // naive voice steal

            auto& voice = testVoices[static_cast<std::size_t>(slot)];
            voice.pitch = pitch;
            voice.active = true;
            voice.phase = 0.0;
        }
        else if (mm.isNoteOff())
        {
            const int pitch = mm.getNoteNumber();
            if (const auto it = openNoteTimes.find(pitch); it != openNoteTimes.end())
            {
                const juce::ScopedLock lock(pendingNotesCS);
                pendingNotes.push_back({ it->second, eventSec, pitch });
                openNoteTimes.erase(it);
            }
            for (auto& voice : testVoices)
                if (voice.active && voice.pitch == pitch)
                    voice.active = false;
        }
    }

    int activeNotes = 0;
    for (const auto& voice : testVoices)
        if (voice.active)
            ++activeNotes;
    lastActiveNotes.store(activeNotes, std::memory_order_relaxed);
}

void OpenSVProcessor::handleSyncTrigger()
{
    // Approximate trigger semantics: hosts automate the boolean impulse.
    // M3 replaces polling with proper automation-event capture.
    if (syncNowParam == nullptr)
        return;

    const bool value = syncNowParam->get() > 0.5f;
    if (value && !lastSyncTriggerValue)
    {
        syncRequests.fetch_add(1, std::memory_order_relaxed);
        syncNowParam->setValueNotifyingHost(0.0f);
    }
    lastSyncTriggerValue = value;
}

// ---------------------------------------------------------------- state
// Hand-rolled deterministic blob (schema OSV1); M3 appends bridge path and
// revision sections to the same format without JSON API churn:
//   i32 magic 'OSV1' | i32 count | { i32 idLen, utf8 id bytes, f32 value }*

void OpenSVProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    struct Entry { const juce::String* id; juce::AudioProcessorParameter* param; };
    const std::array<Entry, 6> entries = {{
        { &idVoice, voiceParam }, { &idGender, genderParam }, { &idBreath, breathParam },
        { &idVibber, vibberParam }, { &idAttenuation, attenuationParam },
        { &idTestTone, testToneParam },
    }};

    juce::MemoryOutputStream stream(destData, true);
    stream.writeInt(kStateMagic);
    stream.writeInt(static_cast<int>(entries.size()));
    for (const auto& entry : entries)
    {
        const std::string utf8 = entry.id->toStdString();
        stream.writeInt(static_cast<int>(utf8.size()));
        stream.write(utf8.data(), utf8.size());
        const float value = entry.param != nullptr ? entry.param->getValue() : 0.0f;
        stream.write(&value, sizeof(value)); // native-endian f32, same-host blob
    }
    destData.setSize(static_cast<size_t>(stream.getPosition()));
}

void OpenSVProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::MemoryInputStream stream(data, static_cast<size_t>(sizeInBytes), false);
    if (stream.readInt() != kStateMagic)
    {
        logLine("setStateInformation: unknown magic, ignoring");
        return;
    }

    const int count = stream.readInt();
    for (int i = 0; i < count; ++i)
    {
        const int idLen = stream.readInt();
        if (idLen <= 0 || idLen > 64)
            break;
        juce::HeapBlock<char> idBuf;
        idBuf.calloc(static_cast<size_t>(idLen) + 1);
        if (stream.read(idBuf.getData(), idLen) != idLen)
            break;
        const juce::String id = juce::String::fromUTF8(idBuf.getData(), idLen);

        float value = 0.0f;
        if (stream.read(&value, sizeof(value)) != static_cast<int>(sizeof(value)))
            break;

        juce::AudioProcessorParameter* param = nullptr;
        if (id == idVoice) param = voiceParam;
        else if (id == idGender) param = genderParam;
        else if (id == idBreath) param = breathParam;
        else if (id == idVibber) param = vibberParam;
        else if (id == idAttenuation) param = attenuationParam;
        else if (id == idTestTone) param = testToneParam;

        if (param != nullptr)
            param->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    }
}

// ---------------------------------------------------------------- probing

juce::String OpenSVProcessor::probeSummary() const
{
    return "samplePos=" + juce::String(lastSamplePos.load())
        + " ppq=" + juce::String(lastPpqPos.load())
        + " bpm=" + juce::String(lastBpm.load())
        + " playing=" + juce::String(lastPlaying.load() ? 1 : 0)
        + " activeNotes=" + juce::String(lastActiveNotes.load())
        + " blocks=" + juce::String(processBlockCounter.load())
        + " syncRequests=" + juce::String(syncRequests.load());
}

void OpenSVProcessor::timerCallback()
{
    // ---- config watch (voice bank swap without restart) ----
    static juce::Time nextConfigCheck{};
    const juce::Time now = juce::Time::getCurrentTime();
    if (now >= nextConfigCheck)
    {
        nextConfigCheck = now + juce::RelativeTime::seconds(1.0);
        if (const juce::File cfgFile = EngineBridge::defaultConfigFile(); cfgFile.existsAsFile())
        {
            static juce::int64 lastMtime = -1;
            const juce::int64 mtime = cfgFile.getLastModificationTime().toMilliseconds();
            if (mtime != lastMtime)
            {
                lastMtime = mtime;
                engine.loadConfigFile(cfgFile);
                logLine("config reloaded: voice=" + juce::String(engine.config().voiceBank));
            }
        }
    }

    // ---- commit collected notes when transport stops (and on demand) ----
    const bool playing = lastPlaying.load();
    const bool stoppedNow = lastTransportPlaying && !playing;
    lastTransportPlaying = playing;

    std::int64_t syncCount = syncRequests.load();
    static std::int64_t lastSyncCount = 0;
    const bool syncPressed = syncCount != lastSyncCount;
    lastSyncCount = syncCount;

    if (stoppedNow || syncPressed)
    {
        std::vector<EngineBridge::NoteRec> snapshot;
        {
            const juce::ScopedLock lock(pendingNotesCS);
            snapshot = pendingNotes; // keep for repeat commits until re-collected
        }
        if (!snapshot.empty())
        {
            engine.setNotes(std::move(snapshot));
            engine.commitAndRender();
            logLine(juce::String("committed ") + juce::String(static_cast<int>(snapshot.size()))
                + " notes -> render requested");
        }
    }

    // ---- transport/activity log (throttled) ----
    const std::int64_t blocks = processBlockCounter.load();
    if (blocks != lastLoggedBlocks)
    {
        lastLoggedBlocks = blocks;
        juce::String line = "transport: " + probeSummary();
        const auto st = engine.status();
        const char* state = st.hasAudio ? "audio" : (st.rendering ? "RENDERING" : "idle");
        line += " | engine: ";
        line += state;
        line += " song=" + juce::String(st.songSeconds, 1) + "s";
        if (st.error.isNotEmpty())
            line += " ERR=" + st.error;
        logLine(line);
    }
}

} // namespace opensv::plugin

// ---------------------------------------------------------------- entry point
// Global namespace: required symbol signature for juce_audio_plugin_client.

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new opensv::plugin::OpenSVProcessor();
}
