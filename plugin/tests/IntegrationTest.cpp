// End-to-end check of the real plugin code with real (fixture) VST3 plugins:
//   scan -> allow -> AI chain plan -> hosting/audio -> scope/analysis -> chat request
//   -> parameter moves on the hosted EQ/compressor -> session save/restore -> auto mix.
// Runs on the JUCE message loop, driven by a timer state machine.

#include "../Source/PluginProcessor.h"

#include <cmath>
#include <iostream>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;

struct Signal
{
    double phase = 0.0;
    long n = 0;
    std::function<float (long, double&)> fn;
};

float kickSample (long n, double&)
{
    const double t = std::fmod (static_cast<double> (n) / kSampleRate, 0.5);  // 2 hits per second
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
    const double vibrato = 0.8 + 0.2 * std::sin (2.0 * juce::MathConstants<double>::pi * 0.5 * n / kSampleRate);
    return static_cast<float> (0.25 * v * vibrato);
}

class Test : private juce::Timer
{
public:
    Test()
    {
        startTimer (20);
    }

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
        const int blocks = static_cast<int> (seconds * kSampleRate / kBlock);
        juce::AudioBuffer<float> k (2, kBlock), v (2, kBlock), m (2, kBlock);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const float ks = kickSample (kick.n++, kick.phase);
                const float vs = vocalGain * vocalSample (vocal.n++, vocal.phase);
                k.setSample (0, i, ks); k.setSample (1, i, ks);
                v.setSample (0, i, vs); v.setSample (1, i, vs);
            }
            kickProc->processBlock (k, midi);
            vocalProc->processBlock (v, midi);
            m.copyFrom (0, 0, k, 0, 0, kBlock); m.copyFrom (1, 0, k, 1, 0, kBlock);
            m.addFrom (0, 0, v, 0, 0, kBlock);  m.addFrom (1, 0, v, 1, 0, kBlock);
            masterProc->processBlock (m, midi);
        }
    }

    float hostedValue (SoundManagerProcessor& p, int slot, const juce::String& name, juce::String* text = nullptr)
    {
        if (auto* s = p.getChain().slot (slot))
            for (auto* param : s->instance->getParameters())
                if (param->getName (64) == name)
                {
                    if (text != nullptr)
                        *text = param->getCurrentValueAsText();
                    return param->getValue();
                }
        return -1.0f;
    }

    bool waitUntil (std::function<bool()> condition, double timeoutSeconds)
    {
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
                std::cout << "Sound Manager integration test" << std::endl;
                masterProc = makeProcessor ("Master");
                kickProc = makeProcessor ("Kick In");
                vocalProc = makeProcessor ("Lead Vox");

                auto& lib = kickProc->getLibrary();
                lib.setApiKey ({});  // offline interpreter (no network in CI)
                const auto eq = lib.addPluginFile (SMX_TEST_EQ_PATH);
                const auto comp = lib.addPluginFile (SMX_TEST_COMP_PATH);
                check (eq.size() == 1 && comp.size() == 1, "fixture VST3s scanned");
                if (eq.empty() || comp.empty()) { stage = 99; return; }
                eqUid = eq[0];
                compUid = comp[0];
                check (lib.catalog().find (eqUid)->category == smix::PluginCategory::EQ, "EQ fixture classified as EQ");
                check (lib.catalog().find (compUid)->category == smix::PluginCategory::Compressor, "compressor fixture classified");
                lib.catalog().setAllowed (eqUid, true);
                lib.catalog().setAllowed (compUid, true);

                render (6.0);  // let the ears settle before planning
                auto& hub = kickProc->getHub();
                const auto plan = hub.planChainFor (kickProc->getInstanceId());
                const auto uids = plan.pluginUids();
                for (auto& slot : plan.slots)
                    std::cout << "    plan: " << slot.pluginName << " - " << slot.purpose << std::endl;
                check (uids.size() >= 2 && uids[0] == eqUid && uids[1] == compUid, "AI plan for kick starts EQ -> compressor");
                plannedSize = static_cast<int> (uids.size());
                smix::MixAction a;
                a.type = smix::ActionType::SetChain;
                a.channelId = kickProc->getInstanceId();
                a.plugins = uids;
                const auto outcomes = hub.apply ({ a }, kickProc->getInstanceId());
                check (! outcomes.empty() && outcomes[0].ok, "set_chain accepted");
                next();
                break;
            }

            case 1:
                if (waitUntil ([this] { return kickProc->getChain().size() == plannedSize; }, 10.0))
                {
                    check (true, "user plugins loaded inside Sound Manager");
                    render (6.0);
                    auto& hub = kickProc->getHub();
                    hub.refresh();
                    const auto& session = hub.getSession();
                    check (session.scopeOf (masterProc->getInstanceId()).size() == 3, "master scope = all 3 channels");
                    check (session.scopeOf (kickProc->getInstanceId()).size() == 1, "track scope = itself");
                    const auto* k = session.find (kickProc->getInstanceId());
                    check (k != nullptr && k->role == smix::InstrumentRole::Kick, "role inferred from track name");
                    check (k != nullptr && k->features.valid && k->features.shortTermLufs > -40.0f, "kick analysed: "
                               + juce::String (k->features.shortTermLufs, 1) + " LUFS, crest " + juce::String (k->features.crestDb, 1) + " dB");
                    check (k != nullptr && ! k->chain.empty() && ! k->chain[0].mappers.empty(), "EQ value curves learned from display text");

                    eqBand2Before = hostedValue (*kickProc, 0, "Band 2 Gain");
                    attackBefore = hostedValue (*kickProc, 1, "Attack");
                    masterProc->getAgent().submit (juce::String::fromUTF8 ("드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어"));
                    next();
                }
                break;

            case 2:
                if (waitUntil ([this] {
                        for (auto& e : masterProc->getLog())
                            if (e.who == "AI")
                                return true;
                        return false;
                    }, 10.0))
                {
                    for (auto& e : masterProc->getLog())
                        if (e.who == "AI")
                            std::cout << "  ---- chat reply ----\n" << e.text << "\n  --------------------" << std::endl;
                    next();
                }
                break;

            case 3:
                // parameter ramps take ~330 ms
                if (waitUntil ([this] { return ++ticks > 30; }, 5.0))
                {
                    juce::String eqText, attackText;
                    const float eqAfter = hostedValue (*kickProc, 0, "Band 2 Gain", &eqText);
                    const float attackAfter = hostedValue (*kickProc, 1, "Attack", &attackText);
                    check (eqAfter < eqBand2Before - 0.01f, "chat: kick EQ 400 Hz band cut -> " + eqText);
                    check (attackAfter > attackBefore, "chat: kick compressor attack slower -> " + attackText);

                    // Save the session and restore it into a fresh instance (as a DAW does on project load).
                    kickProc->getStateInformation (savedState);
                    restored = makeProcessor ("Kick Restored");
                    restored->setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));
                    {
                        juce::MemoryBlock duringLoad;
                        restored->getStateInformation (duringLoad);
                        check (duringLoad == savedState, "saving while the restore is loading returns the original state");
                    }
                    next();
                }
                break;

            case 4:
                if (waitUntil ([this] { return restored->getChain().size() == plannedSize; }, 10.0))
                {
                    const float a = hostedValue (*kickProc, 0, "Band 2 Gain");
                    const float b = hostedValue (*restored, 0, "Band 2 Gain");
                    check (std::abs (a - b) < 1.0e-4f, "session restore keeps hosted plugin settings");
                    check (restored->getInstanceId() != kickProc->getInstanceId(), "duplicated instance gets a new id");
                    juce::MemoryBlock resaved;
                    restored->getStateInformation (resaved);
                    check (resaved.getSize() > 0 && resaved != savedState, "after restore, state is rebuilt from the live chain");
                    restored.reset();

                    // Auto mix: the vocal is far too loud; the master instance should rebalance.
                    vocalGain = 4.0f;
                    vocalGainBefore = vocalProc->getParameters().getRawParameterValue ("aiGain")->load();
                    masterProc->getParameters().getParameter ("autoMix")->setValueNotifyingHost (1.0f);
                    ticks = 0;
                    next();
                }
                break;

            case 5:
                render (0.02);  // keep audio flowing in real time while the hub's auto-mix timer runs
                if (waitUntil ([this] { return ++ticks > 300; }, 12.0))
                {
                    const float after = vocalProc->getParameters().getRawParameterValue ("aiGain")->load();
                    check (after < vocalGainBefore - 0.5f, "auto mix pulled the loud vocal down: " + juce::String (after, 1) + " dB");
                    juce::String log;
                    for (auto& e : masterProc->getLog())
                        if (e.who == "auto")
                            log << "    " << e.text << "\n";
                    std::cout << "  auto-mix log:\n" << log << std::flush;
                    stage = 99;
                }
                break;

            default:
                stopTimer();
                restored.reset();
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
    std::unique_ptr<SoundManagerProcessor> masterProc, kickProc, vocalProc, restored;
    std::string eqUid, compUid;
    Signal kick { 0.0, 0, kickSample }, vocal { 0.0, 0, vocalSample };
    float vocalGain = 1.0f, vocalGainBefore = 0.0f;
    float eqBand2Before = 0, attackBefore = 0;
    juce::MemoryBlock savedState;
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
