// End-to-end check of the real plugin code with real (fixture) VST3 plugins, fully offline:
//   scan -> out-of-process profiling into the knowledge base -> AI chain plan -> hosting/audio
//   -> chat request through the assistant worker -> listen-again feedback question -> history
//   undo -> protection request -> naming question -> hibernation -> reference analysis
//   -> session save/restore -> auto mix.
// Runs on the JUCE message loop, driven by a timer state machine.

#include "../Source/AssistantWorker.h"
#include "../Source/PluginProcessor.h"

#include <smix/GainBalancer.h>
#include <smix/WavFile.h>
#include <smix/style/GenreProfile.h>

#include <cmath>
#include <iostream>
#include <random>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;

float kickSample (long n)
{
    const double t = std::fmod (static_cast<double> (n) / kSampleRate, 0.5);
    const double body = std::sin (2.0 * juce::MathConstants<double>::pi * 55.0 * t) * std::exp (-t * 9.0);
    const double box = 0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * 380.0 * t) * std::exp (-t * 14.0);
    const double click = t < 0.003 ? 0.5 * (1.0 - t / 0.003) : 0.0;
    return static_cast<float> (0.7 * (body + box + click));
}

float vocalSample (long n, double& phase)
{
    phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / kSampleRate;
    double v = 0.0;
    for (int h = 1; h <= 8; ++h)
        v += std::sin (phase * h) / h;
    return static_cast<float> (0.25 * v * (0.8 + 0.2 * std::sin (2.0 * juce::MathConstants<double>::pi * 0.5 * n / kSampleRate)));
}

class Test : private juce::Timer
{
public:
    Test() { startTimer (20); }
    int failures() const { return failed; }

private:
    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
        if (! ok)
            ++failed;
    }

    std::unique_ptr<SoundManagerProcessor> makeProcessor (const juce::String& label)
    {
        auto p = std::make_unique<SoundManagerProcessor>();
        p->setUserLabel (label);
        p->setPlayConfigDetails (2, 2, kSampleRate, kBlock);
        p->prepareToPlay (kSampleRate, kBlock);
        return p;
    }

    void render (double seconds)
    {
        const int blocks = std::max (1, static_cast<int> (seconds * kSampleRate / kBlock));
        juce::AudioBuffer<float> k (2, kBlock), v (2, kBlock), m (2, kBlock), x (2, kBlock);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const float ks = kickSample (kickN++);
                const float vs = vocalGain * vocalSample (vocalN++, vocalPhase);
                k.setSample (0, i, ks); k.setSample (1, i, ks);
                v.setSample (0, i, vs); v.setSample (1, i, vs);
                x.setSample (0, i, 0.3f * ks); x.setSample (1, i, 0.3f * ks);
            }
            kickProc->processBlock (k, midi);
            vocalProc->processBlock (v, midi);
            if (unnamedProc != nullptr)
                unnamedProc->processBlock (x, midi);
            m.copyFrom (0, 0, k, 0, 0, kBlock); m.copyFrom (1, 0, k, 1, 0, kBlock);
            m.addFrom (0, 0, v, 0, 0, kBlock);  m.addFrom (1, 0, v, 1, 0, kBlock);
            masterProc->processBlock (m, midi);
        }
    }

    float hostedValue (SoundManagerProcessor& p, int slot, const juce::String& name, juce::String* text = nullptr)
    {
        if (auto* s = p.getChain().slot (slot); s != nullptr && s->instance != nullptr)
            for (auto* param : s->instance->getParameters())
                if (param->getName (64) == name)
                {
                    if (text != nullptr)
                        *text = param->getText (param->getValue(), 64) + " " + param->getLabel();
                    return param->getValue();
                }
        return -1.0f;
    }

    bool logContains (SoundManagerProcessor& p, const juce::String& who, const juce::String& text)
    {
        for (auto& e : p.getLog())
            if (e.who == who && e.text.contains (text))
                return true;
        return false;
    }

    std::optional<AssistantWorker::PendingQuestion> question (SoundManagerProcessor& p, const juce::String& containing)
    {
        for (auto& q : p.getAssistant().pendingQuestions())
            if (q.text.contains (containing))
                return q;
        return std::nullopt;
    }

    bool waitUntil (std::function<bool()> condition, double timeoutSeconds, bool keepPlaying = true)
    {
        if (keepPlaying)
            render (0.02);  // audio keeps flowing in real time
        if (condition())
            return true;
        if (stageStart <= 0.0)
            stageStart = juce::Time::getMillisecondCounterHiRes();
        if (juce::Time::getMillisecondCounterHiRes() - stageStart > timeoutSeconds * 1000.0)
        {
            check (false, "timed out in stage " + juce::String (stage));
            stage = 99;
        }
        return false;
    }

    void next() { ++stage; stageStart = 0.0; }

    void timerCallback() override
    {
        switch (stage)
        {
            case 0:
            {
                std::cout << "Sound Manager integration test (offline, memopro "
                          << (smix::mem::Runtime::memoproCompiledIn() ? "on" : "off") << ")" << std::endl;
                masterProc = makeProcessor ("Master");
                kickProc = makeProcessor ("Kick In");
                vocalProc = makeProcessor ("Lead Vox");

                auto& lib = kickProc->getLibrary();
                lib.setSetting ("profilerPath", SMX_PROFILER_PATH);
                lib.setSetting ("llmModelPath", "/nonexistent.gguf");  // rules: the test must not depend on a model
                const auto eq = lib.addPluginFile (SMX_TEST_EQ_PATH);
                const auto comp = lib.addPluginFile (SMX_TEST_COMP_PATH);
                check (eq.size() == 1 && comp.size() == 1, "fixture VST3s scanned");
                if (eq.empty() || comp.empty()) { stage = 99; return; }
                eqUid = eq[0];
                compUid = comp[0];
                lib.catalog().setAllowed (eqUid, true);
                lib.catalog().setAllowed (compUid, true);
                lib.saveCatalog();

                check (Engine::ProfilerJob::helperExecutable().existsAsFile(), "profiler helper found");
                kickProc->getEngine().profiler().start ({ eqUid, compUid });
                next();
                break;
            }

            case 1:  // knowledge: plugins measured in a separate process
                if (waitUntil ([this] { return ! kickProc->getEngine().profiler().isRunning(); }, 120.0, false))
                {
                    auto& e = kickProc->getEngine();
                    std::cout << "    " << e.profiler().describe() << std::endl;
                    juce::MessageManager::callAsync ([] {});  // let queued knowledge updates land
                    next();
                }
                break;

            case 2:
                if (waitUntil ([this] { return ++ticks > 10; }, 5.0, false))
                {
                    ticks = 0;
                    auto& e = kickProc->getEngine();
                    auto lease = e.modules().acquire (smix::modules::ModuleId::Knowledge, Engine::now());
                    const auto eqProfile = e.knowledge().get (eqUid);
                    const auto compProfile = e.knowledge().get (compUid);
                    check (eqProfile && ! eqProfile->failed && eqProfile->measuredCategory == smix::PluginCategory::EQ,
                           "EQ learned in a separate process; measured as EQ");
                    check (compProfile && compProfile->measuredCategory == smix::PluginCategory::Compressor, "compressor measured as compressor");
                    if (eqProfile && ! eqProfile->effects.empty())
                        std::cout << "    " << eqProfile->effects.front().name << ": " << eqProfile->effects.front().summary << std::endl;
                    const auto hits = e.knowledge().search ("압축하는 플러그인", 1);
                    check (! hits.empty() && hits[0].uid == compUid, "knowledge search (Korean) finds the compressor");
                    check (e.knowledgeFile().existsAsFile(), "knowledge saved to disk");

                    render (6.0);
                    auto& hub = kickProc->getHub();
                    const auto plan = hub.planChainFor (kickProc->getInstanceId());
                    const auto uids = plan.pluginUids();
                    check (uids.size() >= 2 && uids[0] == eqUid && uids[1] == compUid, "AI plan for kick starts EQ -> compressor");
                    plannedSize = static_cast<int> (uids.size());
                    smix::MixAction a;
                    a.type = smix::ActionType::SetChain;
                    a.channelId = kickProc->getInstanceId();
                    a.plugins = uids;
                    hub.apply ({ a }, kickProc->getInstanceId(), "AI 체인 계획", "plan");
                    next();
                }
                break;

            case 3:
                if (waitUntil ([this] { return kickProc->getChain().size() == plannedSize; }, 10.0))
                {
                    render (6.0);
                    auto& hub = kickProc->getHub();
                    hub.refresh();
                    check (hub.getSession().scopeOf (masterProc->getInstanceId()).size() == 3, "master scope = all 3 channels");
                    const auto* k = hub.getSession().find (kickProc->getInstanceId());
                    check (k != nullptr && k->features.valid && k->features.shortTermLufs > -40.0f, "kick analysed");
                    eqBefore = hostedValue (*kickProc, 0, "Band 2 Gain");
                    attackBefore = hostedValue (*kickProc, 1, "Attack");
                    masterProc->getAssistant().submit (juce::String::fromUTF8 ("드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어"));
                    next();
                }
                break;

            case 4:  // the assistant changed the hosted plugins
                if (waitUntil ([this] { return logContains (*masterProc, "AI", juce::String::fromUTF8 ("단단하게")); }, 15.0))
                {
                    for (auto& e : masterProc->getLog())
                        if (e.who == "AI")
                            std::cout << "  ---- assistant ----\n" << e.text << "\n  -------------------" << std::endl;
                    next();
                }
                break;

            case 5:  // ...listens again after a few seconds and asks how it sounds
                if (waitUntil ([this] {
                        return question (*masterProc, juce::String::fromUTF8 ("어떠세요")).has_value()
                               || question (*masterProc, juce::String::fromUTF8 ("됐나요")).has_value()
                               || logContains (*masterProc, "AI", juce::String::fromUTF8 ("한 단계 더"));
                    }, 25.0))
                {
                    juce::String eqText, attackText;
                    check (hostedValue (*kickProc, 0, "Band 2 Gain", &eqText) < eqBefore - 0.01f, "chat: kick EQ band near 400 Hz cut -> " + eqText);
                    check (hostedValue (*kickProc, 1, "Attack", &attackText) > attackBefore, "chat: compressor attack slower -> " + attackText);
                    check (true, "assistant listened again and followed up");
                    next();
                }
                break;

            case 6:  // history: undo brings the EQ back
                if (waitUntil ([this] { return masterProc->getHub().history().snapshots().size() >= 3; }, 10.0))
                {
                    std::cout << "    history: " << masterProc->getHub().history().snapshots().size() << " snapshots, "
                              << masterProc->getHub().history().blobCount() << " plugin states" << std::endl;
                    juce::String report;
                    // Undo twice: the possible self-correction step and the chat change.
                    masterProc->getHub().undo (masterProc->getInstanceId(), &report);
                    ticks = 0;
                    next();
                }
                break;

            case 7:
                if (waitUntil ([this] { return ++ticks > 40; }, 10.0))
                {
                    // Go to the very first snapshot of the kick: the state before any AI change.
                    std::int64_t first = 0;
                    for (auto& s : masterProc->getHub().history().snapshots())
                        if (first == 0)
                            for (auto& c : s.channels)
                                if (c.channelId == kickProc->getInstanceId() && c.chain.size() == static_cast<size_t> (plannedSize))
                                    first = s.id;
                    juce::String report;
                    check (first != 0 && masterProc->getHub().restore (first, masterProc->getInstanceId(), &report), "restore to a recorded point");
                    ticks = 0;
                    next();
                }
                break;

            case 8:
                if (waitUntil ([this] { return ++ticks > 40; }, 10.0))
                {
                    check (std::abs (hostedValue (*kickProc, 0, "Band 2 Gain") - eqBefore) < 0.01f, "history restore brought the EQ back");
                    // Protection: the user locks the kick EQ, then asks for something that needs it.
                    kickProc->setSlotProtected (0, true);
                    eqBefore = hostedValue (*kickProc, 0, "Band 2 Gain");
                    masterProc->getAssistant().submit (juce::String::fromUTF8 ("킥이 너무 탁해"));
                    next();
                }
                break;

            case 9:
                if (waitUntil ([this] { return question (*masterProc, juce::String::fromUTF8 ("보호")).has_value(); }, 15.0))
                {
                    check (std::abs (hostedValue (*kickProc, 0, "Band 2 Gain") - eqBefore) < 0.001f, "protected EQ untouched");
                    check (true, "AI asked the user to make the protected change: "
                                     + question (*masterProc, juce::String::fromUTF8 ("보호"))->text.substring (0, 70));
                    kickProc->setSlotProtected (0, false);
                    // Naming: a channel with a meaningless name gets a question.
                    unnamedProc = makeProcessor ("Audio 3");
                    next();
                }
                break;

            case 10:
                if (waitUntil ([this] { return question (*masterProc, "Audio 3").has_value(); }, 15.0))
                {
                    const auto q = *question (*masterProc, "Audio 3");
                    check (q.options.size() >= 3, "naming question with suggestions: " + q.options.joinIntoString (", "));
                    masterProc->getAssistant().answer (q.id, -1, false, juce::String::fromUTF8 ("오버헤드"));
                    next();
                }
                break;

            case 11:
                if (waitUntil ([this] { return unnamedProc->getDisplayName() == juce::String::fromUTF8 ("오버헤드"); }, 10.0))
                {
                    check (unnamedProc->getEffectiveRole() == smix::InstrumentRole::Overheads, "named channel -> role overheads");
                    // Hibernation: a compressor bypassed for a while is unloaded, its state kept in memopro.
                    attackBefore = hostedValue (*kickProc, 1, "Attack");
                    kickProc->setSlotBypass (1, true);
                    kickProc->getChain().slot (1)->bypassedSince = Engine::now() - 31.0;
                    kickProc->hibernationTick (Engine::now(), false);
                    check (kickProc->getChain().slot (1)->hibernated && kickProc->getChain().slot (1)->instance == nullptr,
                           "bypassed plugin hibernated (unloaded)");
                    render (0.5);  // audio passes while it sleeps
                    kickProc->setSlotBypass (1, false);
                    check (! kickProc->getChain().slot (1)->hibernated, "woken up when switched back on");
                    check (std::abs (hostedValue (*kickProc, 1, "Attack") - attackBefore) < 1.0e-5f, "settings survived hibernation");

                    // Reference upload: a bright, limited 'song'.
                    smix::WavData wav;
                    wav.sampleRate = 44100;
                    wav.channels = 2;
                    wav.samples.assign (2, std::vector<float> (44100 * 6));
                    std::mt19937 rng (5);
                    std::normal_distribution<float> n (0, 0.1f);
                    for (size_t i = 0; i < wav.samples[0].size(); ++i)
                    {
                        wav.samples[0][i] = juce::jlimit (-0.3f, 0.3f, n (rng));
                        wav.samples[1][i] = 0.8f * wav.samples[0][i] + 0.2f * juce::jlimit (-0.3f, 0.3f, n (rng));
                    }
                    referenceFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("smx-reference.wav");
                    smix::writeWav (referenceFile.getFullPathName().toStdString(), wav);
                    masterProc->getEngine().referenceJob().analyse (referenceFile);
                    next();
                }
                break;

            case 12:
                if (waitUntil ([this] { return masterProc->getEngine().findReference ("smx-reference") != nullptr; }, 20.0))
                {
                    const auto* ref = masterProc->getEngine().findReference ("smx-reference");
                    check (ref->seconds > 5.9 && ref->seconds < 6.1, "reference analysed: " + masterProc->getEngine().referenceJob().describe());
                    referenceFile.deleteFile();
                    masterProc->setReferenceName ("smx-reference");

                    kickProc->getStateInformation (savedState);
                    restored = makeProcessor ("Kick Restored");
                    restored->setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));
                    next();
                }
                break;

            case 13:
                if (waitUntil ([this] { return restored->getChain().size() == plannedSize; }, 10.0))
                {
                    check (std::abs (hostedValue (*kickProc, 0, "Band 2 Gain") - hostedValue (*restored, 0, "Band 2 Gain")) < 1.0e-4f,
                           "session restore keeps hosted plugin settings");
                    check (restored->getInstanceId() != kickProc->getInstanceId(), "duplicated instance gets a new id");
                    restored.reset();
                    vocalGain = 4.0f;
                    vocalGainBefore = vocalProc->getParameters().getRawParameterValue ("aiGain")->load();
                    masterProc->setAutoMixEnabled (true);
                    ticks = 0;
                    next();
                }
                break;

            case 14:
                if (waitUntil ([this] { return ++ticks > 300; }, 15.0))
                {
                    const float after = vocalProc->getParameters().getRawParameterValue ("aiGain")->load();
                    check (after < vocalGainBefore - 0.5f, "auto mix pulled the loud vocal down: " + juce::String (after, 1) + " dB");
                    const auto stats = masterProc->getEngine().memory().stats();
                    std::cout << "    memory runtime: " << (stats.memopro ? "memopro" : "fallback") << ", " << smix::mem::formatBytes (stats.used)
                              << " used of " << smix::mem::formatBytes (stats.budget) << ", written to disk " << stats.writtenBytes << " B" << std::endl;
                    check (stats.writtenBytes == 0 && stats.used <= stats.budget, "memory stays within the memopro budget, nothing on disk");

                    // Genre profile learned by smix_learn_mix: dropped into models/ and picked up.
                    auto& e = masterProc->getEngine();
                    smix::style::GenreProfile g;
                    g.name = "SmxTestGenre";
                    g.songs = 8;
                    g.balanceLu[smix::InstrumentRole::Kick] = -3.5f;
                    auto genreFile = PluginLibrary::modelsDirectory().getChildFile ("SmxTestGenre.smxgenre");
                    genreFile.replaceWithText (juce::String (g.toJson().dump()));
                    e.selectGenre ("SmxTestGenre");
                    check (e.currentGenre() == "SmxTestGenre" && e.genreNames().contains ("SmxTestGenre"), "genre profile loaded from models/");
                    check (std::abs (smix::GainBalancer::targetOffsetLu (smix::InstrumentRole::Kick) + 3.5f) < 0.01f,
                           "genre balance used by the level balancer");
                    e.selectGenre ("none");
                    check (e.currentGenre().isEmpty(), "genre profile switched off");
                    genreFile.deleteFile();
                    e.selectGenre ({});
                    stage = 99;
                }
                break;

            default:
                stopTimer();
                restored.reset();
                unnamedProc.reset();
                kickProc.reset();
                vocalProc.reset();
                masterProc.reset();
                std::cout << (failed == 0 ? "ALL PASSED" : juce::String (failed) + " FAILED") << std::endl;
                juce::MessageManager::getInstance()->stopDispatchLoop();
                break;
        }
    }

    int stage = 0, failed = 0, ticks = 0, plannedSize = 0;
    double stageStart = 0.0;
    std::unique_ptr<SoundManagerProcessor> masterProc, kickProc, vocalProc, unnamedProc, restored;
    std::string eqUid, compUid;
    long kickN = 0, vocalN = 0;
    double vocalPhase = 0.0;
    float vocalGain = 1.0f, vocalGainBefore = 0.0f, eqBefore = 0, attackBefore = 0;
    juce::MemoryBlock savedState;
    juce::File referenceFile;
};
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    int failures = 0;
    {
        Test test;
        juce::MessageManager::getInstance()->runDispatchLoop();
        failures = test.failures();
    }
    return failures == 0 ? 0 : 1;
}
