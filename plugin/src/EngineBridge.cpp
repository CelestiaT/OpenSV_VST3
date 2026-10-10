#include "EngineBridge.h"

#include "audio/ProjectRenderer.h"
#include "synthesis/VoiceDatabase.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace opensv::plugin
{
namespace
{
// Fixed internal tempo grid: notes are collected in absolute seconds and the
// default TempoMap is 120 BPM, so seconds <-> blicks is loss-free inversion.
constexpr double kBlicksPerSecond = 2.0 * static_cast<double>(sv::blicksPerQuarter); // 120 bpm

juce::String jsonField(const juce::var& object, const char* key)
{
    const juce::var value = object[key];
    return value.isString() ? value.toString() : juce::String();
}
} // namespace

EngineBridge::EngineBridge()
    : juce::Thread("OpenSV-Render")
{
    startThread();
}

EngineBridge::~EngineBridge()
{
    cancelFlag.store(true);
    stopThread(4000);
}

// ------------------------------------------------------------------ config

juce::File EngineBridge::defaultConfigFile()
{
    // Test / multi-instance override; must be an absolute path.
    if (const char* env = std::getenv("OPENSV_CONFIG"); env != nullptr && env[0] != '\0')
        return juce::File{ juce::CharPointer_UTF8{ env } };

    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("opensv-vst3")
        .getChildFile("config.json");
}

bool EngineBridge::loadConfigFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    const juce::var parsed = juce::JSON::parse(file.loadFileAsString());
    if (parsed.isUndefined() || parsed.isVoid() || !parsed.isObject())
        return false;

    Config next;
    next.voiceBank = jsonField(parsed, "voice").toStdString();
    next.dictionary = jsonField(parsed, "dict").toStdString();
    const juce::String lang = jsonField(parsed, "language");
    if (lang.isNotEmpty())
        next.language = lang.toStdString();
    const juce::String lyric = jsonField(parsed, "lyric");
    if (lyric.isNotEmpty())
        next.lyric = lyric.toStdString();
    if (const juce::var dirsVar = parsed["scanDirs"]; dirsVar.isArray())
        for (const auto& v : *dirsVar.getArray())
            next.scanDirs.emplace_back(v.toString().toStdString());

    setConfig(next);
    return true;
}

bool EngineBridge::saveConfigFile() const
{
    const Config snapshot = config();

    auto esc = [](juce::String s) { return s.replace("\\", "\\\\").replace("\"", "\\\""); };
    juce::String text = "{\n"
        "  \"voice\": \"" + esc(snapshot.voiceBank) + "\",\n"
        "  \"dict\": \"" + esc(snapshot.dictionary) + "\",\n"
        "  \"language\": \"" + esc(juce::String(snapshot.language)) + "\",\n"
        "  \"lyric\": \"" + esc(juce::String(snapshot.lyric)) + "\",\n"
        "  \"scanDirs\": [\n";
    for (std::size_t i = 0; i < snapshot.scanDirs.size(); ++i)
        text += "    \"" + esc(juce::String(snapshot.scanDirs[i])) + "\""
              + (i + 1 < snapshot.scanDirs.size() ? "," : "") + "\n";
    text += "  ]\n}\n";

    const juce::File file = defaultConfigFile();
    file.getParentDirectory().createDirectory();
    return file.replaceWithText(text);
}

std::vector<juce::File> EngineBridge::scanVoiceBanks() const
{
    const Config snapshot = config();
    std::vector<juce::File> roots;
    if (!snapshot.voiceBank.empty())
        roots.push_back(juce::File(snapshot.voiceBank).getParentDirectory());
    for (const auto& d : snapshot.scanDirs)
        roots.emplace_back(d);

    std::vector<juce::File> found;
    for (const auto& dir : roots)
    {
        if (!dir.isDirectory())
            continue;
        for (const juce::File& f : dir.findChildFiles(juce::File::findFiles, false, "*.nofs"))
        {
            if (!std::any_of(found.begin(), found.end(),
                             [&](const juce::File& x) { return x.getFullPathName() == f.getFullPathName(); }))
                found.push_back(f);
        }
    }
    std::sort(found.begin(), found.end(), [](const juce::File& a, const juce::File& b)
              { return a.getFileNameWithoutExtension().compare(b.getFileNameWithoutExtension()) < 0; });
    return found;
}

EngineBridge::Config EngineBridge::config() const
{
    const std::lock_guard<std::mutex> lock(dataMutex);
    return cfg;
}

void EngineBridge::setConfig(const Config& nextIn)
{
    Config next = nextIn;
    // Normalise to absolute paths: the config file and hosts' cwd must not
    // change what "voice"/"dict" resolve to.
    if (!next.voiceBank.empty())
        next.voiceBank = juce::File(next.voiceBank).getFullPathName().toStdString();
    if (!next.dictionary.empty())
        next.dictionary = juce::File(next.dictionary).getFullPathName().toStdString();
    {
        const std::lock_guard<std::mutex> lock(dataMutex);
        if (next.voiceBank == cfg.voiceBank && next.dictionary == cfg.dictionary
            && next.language == cfg.language && next.lyric == cfg.lyric
            && next.scanDirs == cfg.scanDirs)
            return;
        cfg = next;
    }
    requestedRevision.fetch_add(1);
}

// ------------------------------------------------------------------ notes

namespace
{
void sortAndUniqueNotes(std::vector<EngineBridge::NoteRec>& v)
{
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b)
              { return a.startSec < b.startSec || (a.startSec == b.startSec && a.endSec < b.endSec); });
    v.erase(std::unique(v.begin(), v.end(), [](const auto& a, const auto& b)
              { return std::abs(a.startSec - b.startSec) < 0.004
                        && std::abs(a.endSec - b.endSec) < 0.004
                        && a.pitch == b.pitch; }),
            v.end());
}
} // namespace

void EngineBridge::setNotes(std::vector<NoteRec> incoming)
{
    sortAndUniqueNotes(incoming);
    bool changed = false;
    {
        const std::lock_guard<std::mutex> lock(dataMutex);
        changed = (incoming != notes);
        notes = std::move(incoming);
    }
    if (changed)
        requestedRevision.fetch_add(1);
}

std::vector<EngineBridge::NoteRec> EngineBridge::notesSorted() const
{
    std::vector<NoteRec> sorted;
    {
        const std::lock_guard<std::mutex> lock(dataMutex);
        sorted = notes;
    }
    sortAndUniqueNotes(sorted);
    return sorted;
}

std::size_t EngineBridge::noteCount() const
{
    const std::lock_guard<std::mutex> lock(dataMutex);
    return notes.size();
}

void EngineBridge::setSampleRate(double rate)
{
    const std::lock_guard<std::mutex> lock(dataMutex);
    renderSampleRate = rate;
}

void EngineBridge::commitAndRender()
{
    requestedRevision.fetch_add(1);
}

// ------------------------------------------------------------------ project

sv::Project EngineBridge::buildProject(double upToSec, double& endOutSec) const
{
    const Config snapshot = config();

    sv::Project project = sv::createEmptyProject();
    project.name = "MuseScore";
    endOutSec = 0.0;
    if (project.tracks.empty())
        return project;

    sv::Track& track = project.tracks[0];
    track.name = "Lead";
    track.voice.databasePath = snapshot.voiceBank;
    track.voice.dictionaryDirectory = snapshot.dictionary;
    track.voice.language = snapshot.language;

    track.mainGroup.notes.clear();
    double lastEnd = 0.0;
    for (const auto& note : notesSorted())
    {
        if (note.pitch <= 0)
            continue;
        if (note.startSec >= upToSec)
            break; // future material belongs to the next chunk
        // enforce strictly ascending, non-zero notes
        const double start = std::max(0.0, std::max(note.startSec, lastEnd));
        const double end = std::max(start + 0.01, note.endSec);
        lastEnd = end;
        endOutSec = std::max(endOutSec, end);

        sv::Note out;
        out.id = sv::createNoteId();
        out.onset = static_cast<sv::Blick>(std::llround(start * kBlicksPerSecond));
        out.duration = std::max<sv::Blick>(1, static_cast<sv::Blick>(
            std::llround((end - start) * kBlicksPerSecond)));
        out.pitch = note.pitch;
        out.lyrics = snapshot.lyric;
        track.mainGroup.notes.push_back(std::move(out));
    }
    if (!track.mainGroup.notes.empty())
        endOutSec += 1.2; // musical tail so the last note of a chunk decays
    return project;
}

// ------------------------------------------------------------------ worker

void EngineBridge::run()
{
    sv::audio::ProjectRenderer renderer; // worker-owned: keeps voice/dict/phrase caches

    while (!threadShouldExit())
    {
        const std::uint64_t wanted = requestedRevision.load();
        if (wanted == completedRevision.load())
        {
            sleep(20);
            continue;
        }

        const Config snapshot = config();
        const std::vector<NoteRec> material = notesSorted();
        if (snapshot.voiceBank.empty() || material.empty())
        {
            {
                const std::lock_guard<std::mutex> lock(errorMutex);
                lastError = snapshot.voiceBank.empty()
                    ? "no voice bank configured (open the OpenSV panel)"
                    : juce::String();
            }
            completedRevision.store(wanted);
            continue;
        }

        renderingFlag.store(true);
        cancelFlag.store(false);

        const double rate = [&] { const std::lock_guard<std::mutex> lock(dataMutex); return renderSampleRate; }();

        // Streaming catch-up model: render forward in chunks and publish each
        // completed chunk immediately, so playback can start after ~2 s of
        // audio exists instead of waiting for the whole song. The renderer's
        // phrase cache makes every re-render of unchanged lyrics free.
        const std::string signature = snapshot.voiceBank + "|" + snapshot.dictionary
            + "|" + snapshot.language + "|" + snapshot.lyric;
        if (signature != cfgSignature)
        {
            cfgSignature = signature;
            renderedUntilSec = -1.0;
            prevMaterial.clear();
        }
        // Material that touches already-rendered territory forces a rebuild
        // from 0 (cheap: unchanged phrases hit the cache).
        const bool prefixStable = material.size() >= prevMaterial.size()
            && std::equal(prevMaterial.begin(), prevMaterial.end(), material.begin());
        if (!prefixStable)
            renderedUntilSec = -1.0;

        const double wantedEnd = material.back().endSec + 1.2;
        wantedSecondsReport.store(wantedEnd);

        bool superseded = false;
        while (renderedUntilSec < wantedEnd - 0.001 && !cancelFlag.load() && !threadShouldExit())
        {
            if (requestedRevision.load() != wanted) { superseded = true; break; }

            const double chunk = renderedUntilSec < 0.0 ? 2.5 : 6.0;
            const double target = renderedUntilSec < 0.0
                ? std::min(wantedEnd, chunk)
                : std::min(wantedEnd, renderedUntilSec + chunk);

            double endOut = 0.0;
            const sv::Project sub = buildProject(target, endOut);
            if (sub.tracks.empty() || sub.tracks[0].mainGroup.notes.empty())
                break;

            auto audio = std::make_shared<RenderedAudio>();
            audio->sampleRate = rate;
            audio->revision = wanted;

            const juce::Result result = renderer.render(
                sub, rate, audio->samples,
                [this] { return cancelFlag.load() || threadShouldExit(); });

            if (!result.wasOk())
            {
                const std::lock_guard<std::mutex> lock(errorMutex);
                lastError = result.getErrorMessage();
                break;
            }
            if (cancelFlag.load() || threadShouldExit() || requestedRevision.load() != wanted)
            { superseded = true; break; }

            if (audio->samples.getNumSamples() > 0)
            {
                const float peak = audio->samples.getMagnitude(0, audio->samples.getNumSamples());
                if (peak > 0.95f)
                    audio->samples.applyGain(0.89f / peak);
            }

            songSampleCount.store(audio->samples.getNumSamples());
            renderedSecondsReport.store(endOut);
            publish(std::move(audio));
            renderedUntilSec = endOut;
            {
                const std::lock_guard<std::mutex> lock(errorMutex);
                lastError.clear();
            }
        }

        if (!superseded)
        {
            prevMaterial = material;
            completedRevision.store(wanted);
        }
        renderingFlag.store(false);
        if (onRenderFinished)
            onRenderFinished();
    }
}

void EngineBridge::publish(std::shared_ptr<const RenderedAudio> audio)
{
    publishedAudio.store(std::move(audio), std::memory_order_release);
}

// ------------------------------------------------------------------ callback

bool EngineBridge::renderInto(juce::AudioBuffer<float>& buffer, const std::int64_t absoluteStartSample)
{
    auto audio = publishedAudio.load(std::memory_order_acquire);
    if (!audio || audio->samples.getNumSamples() <= 0)
        return false;
    if (absoluteStartSample + buffer.getNumSamples() <= 0)
        return false;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const int available = audio->samples.getNumSamples();

    for (int channel = 0; channel < numChannels && channel < audio->samples.getNumChannels(); ++channel)
    {
        const float* src = audio->samples.getReadPointer(channel);
        float* dst = buffer.getWritePointer(channel);
        for (int i = 0; i < numSamples; ++i)
        {
            const std::int64_t index = absoluteStartSample + i;
            if (index >= 0 && index < available)
                dst[i] += src[index];
        }
    }
    return true;
}

EngineBridge::Status EngineBridge::status() const
{
    Status out;
    out.rendering = renderingFlag.load();
    out.hasAudio = publishedAudio.load() != nullptr;
    out.songSeconds = renderedSecondsReport.load();
    out.wantedSeconds = wantedSecondsReport.load();
    const std::lock_guard<std::mutex> lock(errorMutex);
    out.error = lastError;
    return out;
}

} // namespace opensv::plugin
