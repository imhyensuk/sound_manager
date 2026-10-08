#include "Engine.h"

#include <cstring>
#include <fstream>

#include "Text.h"

#include <smix/style/GenreProfile.h>

using namespace smix;

namespace
{
std::uint64_t mib (const juce::String& s, std::uint64_t fallback)
{
    const auto v = s.getLargeIntValue();
    return static_cast<std::uint64_t> (v > 0 ? v : static_cast<juce::int64> (fallback)) << 20;
}
} // namespace

double Engine::now()
{
    return juce::Time::getMillisecondCounterHiRes() * 0.001;
}

Engine::Engine()
{
    // One ceiling for all large data of every instance (knowledge, history, plugin states, model caches).
    runtime = std::make_unique<mem::Runtime> (mib (lib->getSetting ("memoryBudgetMiB"), 512));
    moduleManager.setMemoryBudget (mib (lib->getSetting ("moduleBudgetMiB"), 4096));
    kb = std::make_unique<knowledge::KnowledgeBase> (runtime.get());
    localLlm = std::make_unique<llm::LocalLlm> (runtime.get());
    llmIntentModel = std::make_unique<llm::LlmIntentModel> (*localLlm);
    earModel = std::make_unique<ear::EarModel> (runtime.get());

    profilerJob = std::make_unique<ProfilerJob> (*this);
    refJob = std::make_unique<ReferenceJob> (*this);
    registerModules();
    earLease = moduleManager.acquire (modules::ModuleId::Ear, now());  // the ear always runs
    loadReferences();
    loadGenre();
    startTimer (1000);
}

Engine::~Engine()
{
    stopTimer();
    earLease = {};
    profilerJob.reset();
    refJob.reset();
    moduleManager.unloadAll();
    llmIntentModel.reset();
    localLlm.reset();
    earModel.reset();
    kb.reset();
}

void Engine::registerModules()
{
    using modules::ModuleId;
    using modules::LambdaModule;
    auto enabled = [this] (ModuleId id, bool fallback) {
        return lib->getSetting ("module." + juce::String (modules::toString (id)), fallback ? "1" : "0") == "1";
    };

    // The ear (analysis + perception) runs inside every instance; it is cheap and always on.
    moduleManager.add (std::make_unique<LambdaModule> (ModuleId::Ear, nullptr, nullptr, 64 * 1024), { 0.0, true, true });

    moduleManager.add (std::make_unique<LambdaModule> (
                           ModuleId::EarModel,
                           [this] (std::string&) {
                               std::string err;
                               const auto f = earModelFile();
                               if (f.existsAsFile())
                                   earModel->load (f.getFullPathName().toStdString(), err);
                               return true;  // without a trained model the heuristic recogniser is used
                           },
                           [this] { earModel->unload(); }, 2 * 1024 * 1024),
                       { 60.0, enabled (ModuleId::EarModel, true), false });

    struct LlmModule : modules::Module
    {
        Engine& e;
        explicit LlmModule (Engine& en) : e (en) {}
        modules::ModuleId id() const override { return modules::ModuleId::Intent; }
        bool load (std::string& error) override
        {
            const auto f = e.llmModelFile();
            if (! f.existsAsFile())
            {
                error = "로컬 언어 모델(GGUF)이 없어요. 설정 탭에서 모델 파일을 지정하거나 models 폴더에 넣어 주세요.";
                return false;
            }
            llm::LocalLlm::Config c;
            c.modelPath = f.getFullPathName().toStdString();
            c.contextTokens = e.lib->getSetting ("llmContext", "4096").getIntValue();
            c.threads = e.lib->getSetting ("llmThreads", "0").getIntValue();
            c.gpuLayers = e.lib->getSetting ("llmGpuLayers", "0").getIntValue();
            return e.localLlm->load (c, error);
        }
        void unload() override { e.localLlm->unload(); }
        std::uint64_t memoryEstimate() const override
        {
            return static_cast<std::uint64_t> (e.llmModelFile().getSize()) + 400ull * 1024 * 1024;
        }
        double loadSecondsEstimate() const override { return e.llmLoadSecondsEstimate(); }
    };
    moduleManager.add (std::make_unique<LlmModule> (*this),
                       { lib->getSetting ("llmIdleSeconds", "180").getDoubleValue(), enabled (ModuleId::Intent, true), false });

    moduleManager.add (std::make_unique<LambdaModule> (
                           ModuleId::Knowledge,
                           [this] (std::string&) {
                               if (kb->size() == 0)
                                   kb->load (knowledgeFile().getFullPathName().toStdString());
                               return true;
                           },
                           [this] { kb = std::make_unique<knowledge::KnowledgeBase> (runtime.get()); },
                           8 * 1024 * 1024),
                       { 120.0, enabled (ModuleId::Knowledge, true), false });

    for (auto id : { ModuleId::Style, ModuleId::AutoMix, ModuleId::Visualizer, ModuleId::History, ModuleId::Profiler,
                     ModuleId::Hibernation })
        moduleManager.add (std::make_unique<LambdaModule> (id, nullptr, nullptr, 256 * 1024),
                           { 30.0, enabled (id, true), false });
}

juce::File Engine::llmModelFile() const
{
    const auto configured = lib->getSetting ("llmModelPath");
    if (configured.isNotEmpty() && juce::File::isAbsolutePath (configured))
        return juce::File (configured);
    auto files = PluginLibrary::modelsDirectory().findChildFiles (juce::File::findFiles, false, "*.gguf");
    files.sort();
    return files.isEmpty() ? juce::File() : files.getFirst();
}

juce::StringArray Engine::genreNames() const
{
    juce::StringArray names;
    for (auto& f : PluginLibrary::modelsDirectory().findChildFiles (juce::File::findFiles, false, "*.smxgenre"))
        names.add (f.getFileNameWithoutExtension());
    names.sort (true);
    return names;
}

juce::String Engine::currentGenre() const
{
    const auto g = style::activeGenre();
    return g ? juce::String::fromUTF8 (g->name.c_str()) : juce::String();
}

void Engine::selectGenre (const juce::String& setting)
{
    lib->setSetting ("genre", setting);
    loadGenre();
}

void Engine::loadGenre()
{
    auto choice = lib->getSetting ("genre");
    if (choice == "none")
    {
        style::setActiveGenre (nullptr);
        return;
    }
    const auto names = genreNames();
    if (choice.isEmpty() || ! names.contains (choice))
        choice = names.isEmpty() ? juce::String() : names[0];
    if (choice.isEmpty())
    {
        style::setActiveGenre (nullptr);
        return;
    }
    try
    {
        const auto text = PluginLibrary::modelsDirectory().getChildFile (choice + ".smxgenre").loadFileAsString();
        auto g = std::make_shared<style::GenreProfile> (style::GenreProfile::fromJson (nlohmann::json::parse (text.toStdString())));
        if (g->name.empty())
            g->name = choice.toStdString();
        style::setActiveGenre (std::move (g));
    }
    catch (const std::exception&)
    {
        style::setActiveGenre (nullptr);
    }
}

juce::File Engine::earModelFile() const
{
    const auto configured = lib->getSetting ("earModelPath");
    if (configured.isNotEmpty() && juce::File::isAbsolutePath (configured))
        return juce::File (configured);
    return PluginLibrary::modelsDirectory().getChildFile ("ear.smxear");
}

double Engine::llmLoadSecondsEstimate() const
{
    // Memory-mapped loading: mostly reading the file once (~400 MB/s on an SSD) plus context setup.
    return 0.5 + static_cast<double> (llmModelFile().getSize()) / (400.0 * 1024 * 1024);
}

void Engine::saveKnowledge()
{
    kb->save (knowledgeFile().getFullPathName().toStdString());
}

std::vector<std::string> Engine::unprofiledPlugins()
{
    auto lease = moduleManager.acquire (modules::ModuleId::Knowledge, now());
    std::vector<std::string> out;
    for (auto& p : lib->catalog().all())
        if (! kb->isCurrent (p.uid))
            out.push_back (p.uid);
    return out;
}

void Engine::addReference (const style::StyleProfile& p)
{
    removeReference (p.name);
    refs.push_back (p);
    saveReferences();
    sendChangeMessage();
}

void Engine::removeReference (const std::string& name)
{
    refs.erase (std::remove_if (refs.begin(), refs.end(), [&] (auto& r) { return r.name == name; }), refs.end());
    saveReferences();
}

const style::StyleProfile* Engine::findReference (const std::string& name) const
{
    for (auto& r : refs)
        if (r.name == name)
            return &r;
    return nullptr;
}

void Engine::loadReferences()
{
    std::ifstream in (PluginLibrary::dataDirectory().getChildFile ("references.json").getFullPathName().toStdString());
    if (! in)
        return;
    const auto j = nlohmann::json::parse (in, nullptr, false);
    if (j.is_array())
        for (auto& r : j)
            refs.push_back (style::StyleProfile::fromJson (r));
}

void Engine::saveReferences()
{
    nlohmann::json arr = nlohmann::json::array();
    for (auto& r : refs)
        arr.push_back (r.toJson());
    std::ofstream out (PluginLibrary::dataDirectory().getChildFile ("references.json").getFullPathName().toStdString());
    out << arr.dump (1);
}

juce::String Engine::statusText()
{
    const auto s = runtime->stats();
    juce::String t;
    t << ko ("메모리 런타임: ") << (s.memopro ? "memopro " + juce::String (mem::Runtime::memoproVersion()) : ko ("기본 할당기"))
      << " · " << juce::String (mem::formatBytes (s.used)) << " / " << juce::String (mem::formatBytes (s.budget))
      << ko (" 사용 (압축 ") << juce::String (mem::formatBytes (s.compressedBytes)) << ko (", 디스크 기록 0)\n");
    t << ko ("모듈: ");
    const auto status = moduleManager.status (now());  // keep the JSON alive while iterating
    for (auto& m : status["modules"])
        t << juce::String (m.value ("name", std::string {})) << "=" << juce::String (m.value ("state", std::string {})) << "  ";
    t << ko ("\n언어 모델: ") << (llmModelFile().existsAsFile() ? llmModelFile().getFileName() : ko ("없음(규칙 기반으로 동작)"));
    t << ko ("\n플러그인 지식: ") << juce::String (static_cast<int> (kb->size())) << ko ("개 분석됨");
    return t;
}

void Engine::timerCallback()
{
    const auto unloaded = moduleManager.tick (now());
    if (! unloaded.empty())
        sendChangeMessage();
}

//==============================================================================
juce::File Engine::ProfilerJob::helperExecutable()
{
    const auto configured = juce::SharedResourcePointer<PluginLibrary>()->getSetting ("profilerPath");
    if (configured.isNotEmpty() && juce::File (configured).existsAsFile())
        return juce::File (configured);
   #if JUCE_WINDOWS
    const juce::String name = "SoundManagerProfiler.exe";
   #else
    const juce::String name = "SoundManagerProfiler";
   #endif
    // Installed next to the plugin binary (copied into every bundle at build time).
    const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    for (auto dir : { self.getParentDirectory(), self.getParentDirectory().getParentDirectory().getChildFile ("Resources") })
        if (dir.getChildFile (name).existsAsFile())
            return dir.getChildFile (name);
    return {};
}

void Engine::ProfilerJob::start (std::vector<std::string> uids)
{
    if (isThreadRunning() || uids.empty())
        return;
    {
        const juce::ScopedLock sl (lock);
        queue = std::move (uids);
        failures = 0;
        lastError.clear();
        // Prior: about 12 s per plugin (instantiation + ~100 short measurements).
        progress.start ("플러그인 분석", static_cast<double> (queue.size()), 12.0, Engine::now());
    }
    startThread();
}

void Engine::ProfilerJob::cancel()
{
    signalThreadShouldExit();
    stopThread (8000);
}

nlohmann::json Engine::ProfilerJob::progressJson() const
{
    const juce::ScopedLock sl (lock);
    auto j = progress.toJson (Engine::now());
    j["current"] = current.toStdString();
    j["failures"] = failures;
    j["error"] = lastError.toStdString();
    return j;
}

juce::String Engine::ProfilerJob::describe() const
{
    const juce::ScopedLock sl (lock);
    juce::String s (progress.describe (Engine::now()));
    if (current.isNotEmpty() && progress.running())
        s << " (" << current << ")";
    if (failures > 0)
        s << ko (" · 실패 ") << failures;
    if (lastError.isNotEmpty())
        s << " · " << lastError;
    return s;
}

void Engine::ProfilerJob::run()
{
    std::vector<std::string> remaining;
    {
        const juce::ScopedLock sl (lock);
        remaining = queue;
    }
    while (! remaining.empty() && ! threadShouldExit())
        if (! runHelper (remaining))
            break;

    juce::MessageManager::callAsync ([this] {
        engine.saveKnowledge();
        engine.sendChangeMessage();
    });
    const juce::ScopedLock sl (lock);
    progress.finish (Engine::now());
    current.clear();
}

bool Engine::ProfilerJob::runHelper (std::vector<std::string>& remaining)
{
    const auto helper = helperExecutable();
    if (! helper.existsAsFile())
    {
        const juce::ScopedLock sl (lock);
        lastError = ko ("분석 도우미(SoundManagerProfiler)를 찾을 수 없어요");
        return false;
    }

    // The uid list goes through a file: uids contain spaces and punctuation.
    const auto list = PluginLibrary::dataDirectory().getChildFile ("profile-queue.txt");
    juce::StringArray lines;
    for (auto& u : remaining)
        lines.add (juce::String (u));
    list.replaceWithText (lines.joinIntoString ("\n"));

    juce::ChildProcess child;
    if (! child.start (juce::StringArray { helper.getFullPathName(), "--data", PluginLibrary::dataDirectory().getFullPathName(),
                                           "--queue", list.getFullPathName() },
                       juce::ChildProcess::wantStdOut))
    {
        const juce::ScopedLock sl (lock);
        lastError = ko ("분석 도우미를 실행하지 못했어요");
        return false;
    }

    std::string pending;
    char buf[8192];
    while (! threadShouldExit())
    {
        const int n = child.readProcessOutput (buf, sizeof (buf));
        if (n <= 0)
        {
            if (! child.isRunning())
                break;
            juce::Thread::sleep (20);
            continue;
        }
        pending.append (buf, static_cast<size_t> (n));
        size_t nl;
        while ((nl = pending.find ('\n')) != std::string::npos)
        {
            const auto line = pending.substr (0, nl);
            pending.erase (0, nl + 1);
            if (line.rfind ("START ", 0) == 0)
            {
                const juce::ScopedLock sl (lock);
                current = juce::String::fromUTF8 (line.substr (6).c_str());
            }
            else if (line.rfind ("RESULT ", 0) == 0)
            {
                const auto j = nlohmann::json::parse (line.substr (7), nullptr, false);
                if (j.is_discarded())
                    continue;
                auto profile = knowledge::PluginProfile::fromJson (j);
                remaining.erase (std::remove (remaining.begin(), remaining.end(), profile.uid), remaining.end());
                juce::MessageManager::callAsync ([this, profile] {
                    auto lease = engine.modules().acquire (modules::ModuleId::Knowledge, Engine::now());
                    engine.knowledge().upsert (profile);
                    engine.sendChangeMessage();
                });
                const juce::ScopedLock sl (lock);
                progress.advance (1, Engine::now());
                if (profile.failed)
                    ++failures;
            }
        }
    }
    if (threadShouldExit())
    {
        child.kill();
        return false;
    }

    // The helper died in the middle of a plugin: record it as failed and continue with the rest.
    if (! remaining.empty())
    {
        knowledge::PluginProfile failed;
        failed.uid = remaining.front();
        if (const auto* info = engine.library().catalog().find (failed.uid))
        {
            failed.name = info->name;
            failed.vendor = info->manufacturer;
            failed.declaredCategory = info->category;
        }
        failed.failed = true;
        failed.version = knowledge::PluginProfile::kVersion;
        failed.error = "the plugin crashed or hung while being measured";
        remaining.erase (remaining.begin());
        juce::MessageManager::callAsync ([this, failed] {
            auto lease = engine.modules().acquire (modules::ModuleId::Knowledge, Engine::now());
            engine.knowledge().upsert (failed);
        });
        const juce::ScopedLock sl (lock);
        ++failures;
        progress.advance (1, Engine::now());
    }
    return true;
}

//==============================================================================
void Engine::ReferenceJob::analyse (const juce::File& f)
{
    if (isThreadRunning())
        return;
    file = f;
    {
        const juce::ScopedLock sl (lock);
        result.clear();
        // Prior: decoding + analysis at ~40x real time; refined from the measured speed.
        progress.start ("레퍼런스 분석", 100.0, 0.05, Engine::now());
    }
    startThread();
}

nlohmann::json Engine::ReferenceJob::progressJson() const
{
    const juce::ScopedLock sl (lock);
    return progress.toJson (Engine::now());
}

juce::String Engine::ReferenceJob::describe() const
{
    const juce::ScopedLock sl (lock);
    return progress.running() ? juce::String (progress.describe (Engine::now())) : result;
}

juce::String Engine::ReferenceJob::lastResult() const
{
    const juce::ScopedLock sl (lock);
    return result;
}

void Engine::ReferenceJob::setProgress (double done, double total)
{
    const juce::ScopedLock sl (lock);
    progress.setTotal (total);
    const double already = progress.fraction() * total;
    if (done > already)
        progress.advance (done - already, Engine::now());
}

bool Engine::ReferenceJob::decodeWithJuce (std::unique_ptr<style::StyleAnalyzer>& analyzer)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();  // WAV, AIFF, FLAC, Ogg, MP3, and on macOS/Windows the OS decoders (m4a, mp4, mov, wma...)
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    analyzer = std::make_unique<style::StyleAnalyzer> (reader->sampleRate);
    constexpr int chunk = 65536;
    juce::AudioBuffer<float> buf (2, chunk);
    const double totalSeconds = static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
    for (juce::int64 pos = 0; pos < reader->lengthInSamples && ! threadShouldExit(); pos += chunk)
    {
        const int n = static_cast<int> (juce::jmin<juce::int64> (chunk, reader->lengthInSamples - pos));
        reader->read (&buf, 0, n, pos, true, true);
        analyzer->process (buf.getReadPointer (0), reader->numChannels > 1 ? buf.getReadPointer (1) : nullptr, n);
        setProgress (static_cast<double> (pos + n) / reader->sampleRate, totalSeconds);
    }
    return ! threadShouldExit();
}

bool Engine::ReferenceJob::decodeWithFfmpeg (std::unique_ptr<style::StyleAnalyzer>& analyzer)
{
    // A locally installed ffmpeg (video files, rare codecs). Streams raw float PCM through a pipe:
    // nothing is written to disk.
    juce::String ffmpeg = juce::SharedResourcePointer<PluginLibrary>()->getSetting ("ffmpegPath", "ffmpeg");
    const double rate = 48000.0;

    double totalSeconds = 0.0;
    {
        juce::ChildProcess probe;
        if (probe.start (juce::StringArray { ffmpeg, "-hide_banner", "-i", file.getFullPathName() }, juce::ChildProcess::wantStdErr))
        {
            const auto info = probe.readAllProcessOutput();
            const auto d = info.fromFirstOccurrenceOf ("Duration: ", false, false).upToFirstOccurrenceOf (",", false, false);
            juce::StringArray hms;
            hms.addTokens (d, ":", "");
            if (hms.size() == 3)
                totalSeconds = hms[0].getDoubleValue() * 3600 + hms[1].getDoubleValue() * 60 + hms[2].getDoubleValue();
        }
    }

    juce::ChildProcess child;
    if (! child.start (juce::StringArray { ffmpeg, "-nostdin", "-v", "error", "-i", file.getFullPathName(), "-vn", "-f", "f32le", "-ac", "2",
                                           "-ar", "48000", "-" },
                       juce::ChildProcess::wantStdOut))
        return false;

    analyzer = std::make_unique<style::StyleAnalyzer> (rate);
    std::vector<char> raw (1 << 16);
    std::vector<float> l, r;
    std::string carry;
    double seconds = 0;
    while (! threadShouldExit())
    {
        const int n = child.readProcessOutput (raw.data(), static_cast<int> (raw.size()));
        if (n <= 0)
        {
            if (! child.isRunning())
                break;
            juce::Thread::sleep (5);
            continue;
        }
        carry.append (raw.data(), static_cast<size_t> (n));
        const size_t frames = carry.size() / 8;
        l.resize (frames);
        r.resize (frames);
        for (size_t i = 0; i < frames; ++i)
        {
            std::memcpy (&l[i], carry.data() + i * 8, 4);
            std::memcpy (&r[i], carry.data() + i * 8 + 4, 4);
        }
        carry.erase (0, frames * 8);
        analyzer->process (l.data(), r.data(), static_cast<int> (frames));
        seconds += static_cast<double> (frames) / rate;
        setProgress (seconds, juce::jmax (totalSeconds, seconds + 1.0));
    }
    return analyzer->secondsProcessed() > 1.0 && ! threadShouldExit();
}

void Engine::ReferenceJob::run()
{
    std::unique_ptr<style::StyleAnalyzer> analyzer;
    bool ok = decodeWithJuce (analyzer);
    if (! ok && ! threadShouldExit())
        ok = decodeWithFfmpeg (analyzer);

    if (! ok)
    {
        const juce::ScopedLock sl (lock);
        progress.finish (Engine::now());
        result = ko ("이 파일에서 소리를 읽지 못했어요. 영상은 ffmpeg가 설치되어 있어야 해요(설정 탭에서 경로 지정).");
        return;
    }

    const auto profile = analyzer->finish (file.getFileNameWithoutExtension().toStdString(), file.getFullPathName().toStdString());
    juce::MessageManager::callAsync ([this, profile] { engine.addReference (profile); });

    juce::String desc;
    for (auto& d : profile.descriptors())
        desc << juce::String (d) << ", ";
    const juce::ScopedLock sl (lock);
    progress.finish (Engine::now());
    result = "'" + juce::String (profile.name) + ko ("' 분석 완료: ") + juce::String (profile.integratedLufs, 1) + " LUFS, "
             + desc.trimCharactersAtEnd (", ");
}
