#include "PluginProcessor.h"

#include "AssistantWorker.h"
#include "PluginEditor.h"
#include "Text.h"

#include <smix/ear/EarModel.h>

namespace
{
const juce::Identifier kStateTag ("SoundManagerState");
constexpr int kMaxLogEntries = 300;
}

//==============================================================================
class SoundManagerProcessor::RampTimer : public juce::Timer
{
public:
    explicit RampTimer (SoundManagerProcessor& o) : owner (o) {}
    void timerCallback() override { owner.advanceRamps(); }

private:
    SoundManagerProcessor& owner;
};

juce::StringArray SoundManagerProcessor::kindChoices()
{
    return { "Auto", "Track", "Bus / Group", "Master" };
}

juce::StringArray SoundManagerProcessor::roleChoices()
{
    juce::StringArray roles { "Auto" };
    for (int r = static_cast<int> (smix::InstrumentRole::Unknown); r <= static_cast<int> (smix::InstrumentRole::Master); ++r)
        roles.add (smix::toString (static_cast<smix::InstrumentRole> (r)));
    return roles;
}

juce::AudioProcessorValueTreeState::ParameterLayout SoundManagerProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "aiGain", 1 }, "AI Gain", NormalisableRange<float> (-24.0f, 12.0f, 0.01f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "autoMix", 1 }, "Auto Mix", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "gainLock", 1 }, "Lock Level", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "protect", 1 }, "Protect Channel", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "kind", 1 }, "Channel Kind", kindChoices(), 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "role", 1 }, "Instrument Role", roleChoices(), 0));
    return layout;
}

SoundManagerProcessor::SoundManagerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "PARAMS", createLayout())
{
    aiGainParam = parameters.getRawParameterValue ("aiGain");
    regenerateInstanceId();
    rampTimer = std::make_unique<RampTimer> (*this);
    assistant = std::make_unique<AssistantWorker> (*this);
    hub->add (this);
}

SoundManagerProcessor::~SoundManagerProcessor()
{
    assistant->shutdown();
    pluginWindows.clear();
    rampTimer->stopTimer();
    hub->remove (this);
    for (int i = 0; i < chain.size(); ++i)
        engine->memory().free (chain.slot (i)->stateBuffer);
}

//==============================================================================
void SoundManagerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    chain.prepare (sampleRate, samplesPerBlock);
    analyzer.prepare (sampleRate);
    gainSmoothed.reset (sampleRate, 0.05);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (aiGainParam->load()));
    setLatencySamples (chain.getLatencySamples());
}

void SoundManagerProcessor::releaseResources()
{
    chain.release();
}

bool SoundManagerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void SoundManagerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Remember when the channel last carried signal (hibernation wakes sleeping plugins on it).
    if (buffer.getMagnitude (0, buffer.getNumSamples()) > 1.0e-4f)
        lastSignalMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);

    chain.process (buffer);

    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (aiGainParam->load (std::memory_order_relaxed)));
    if (gainSmoothed.isSmoothing())
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const float g = gainSmoothed.getNextValue();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.getWritePointer (ch)[i] *= g;
        }
    }
    else
    {
        buffer.applyGain (gainSmoothed.getTargetValue());
    }

    const float* left = buffer.getReadPointer (0);
    const float* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr;
    analyzer.process (left, right, buffer.getNumSamples());

    // Only blocks that carry sound are recorded, so the recogniser hears the instrument, not the rests.
    if (earCapturing.load (std::memory_order_acquire) && buffer.getMagnitude (0, buffer.getNumSamples()) > 1.0e-3f)
    {
        int pos = earWritePos.load (std::memory_order_relaxed);
        for (int i = 0; i < buffer.getNumSamples() && pos < earLength; ++i)
            earBuffer[static_cast<size_t> (pos++)] = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];
        earWritePos.store (pos, std::memory_order_relaxed);
        if (pos >= earLength)
            earCapturing.store (false, std::memory_order_release);
    }
}

juce::AudioProcessorEditor* SoundManagerProcessor::createEditor()
{
    return new SoundManagerEditor (*this);
}

//==============================================================================
juce::String SoundManagerProcessor::getDisplayName() const
{
    if (userLabel.isNotEmpty()) return userLabel;
    if (trackName.isNotEmpty()) return trackName;
    return "Channel " + juce::String (instanceId);
}

smix::ChannelKind SoundManagerProcessor::getEffectiveKind() const
{
    switch (static_cast<int> (parameters.getRawParameterValue ("kind")->load()))
    {
        case 1: return smix::ChannelKind::Track;
        case 2: return smix::ChannelKind::Bus;
        case 3: return smix::ChannelKind::Master;
        default: return smix::guessKindFromTrackName (getDisplayName().toStdString());
    }
}

smix::InstrumentRole SoundManagerProcessor::getEffectiveRole() const
{
    const int choice = static_cast<int> (parameters.getRawParameterValue ("role")->load());
    if (choice > 0)
        return static_cast<smix::InstrumentRole> (choice - 1);
    auto role = smix::guessRoleFromTrackName (getDisplayName().toStdString());
    if (role == smix::InstrumentRole::Unknown)
    {
        const auto kind = getEffectiveKind();
        if (kind == smix::ChannelKind::Master) role = smix::InstrumentRole::Master;
        if (kind == smix::ChannelKind::Bus)    role = smix::InstrumentRole::MixBus;
    }
    return role;
}

bool SoundManagerProcessor::isAutoMixEnabled() const    { return parameters.getRawParameterValue ("autoMix")->load() > 0.5f; }
bool SoundManagerProcessor::isGainLocked() const        { return parameters.getRawParameterValue ("gainLock")->load() > 0.5f; }
bool SoundManagerProcessor::isChannelProtected() const  { return parameters.getRawParameterValue ("protect")->load() > 0.5f; }
void SoundManagerProcessor::setAutoMixEnabled (bool on) { parameters.getParameter ("autoMix")->setValueNotifyingHost (on ? 1.0f : 0.0f); }
void SoundManagerProcessor::setChannelProtected (bool on) { parameters.getParameter ("protect")->setValueNotifyingHost (on ? 1.0f : 0.0f); }

void SoundManagerProcessor::updateTrackProperties (const TrackProperties& properties)
{
    juce::MessageManager::callAsync ([safe = juce::WeakReference<SoundManagerProcessor> (this), name = properties.name] {
        if (auto* self = safe.get())
        {
            self->trackName = name;
            self->sendChangeMessage();
        }
    });
}

//==============================================================================
smix::ChannelState SoundManagerProcessor::buildChannelState (bool learnMissingMaps)
{
    latestFeatures = analyzer.snapshot();

    smix::ChannelState c;
    c.id = instanceId;
    c.name = getDisplayName().toStdString();
    c.kind = getEffectiveKind();
    c.role = getEffectiveRole();
    c.aiGainDb = aiGainParam->load();
    c.gainLocked = isGainLocked();
    c.protectedChannel = isChannelProtected();
    c.style = style.toStdString();
    c.features = latestFeatures;

    for (int i = 0; i < chain.size(); ++i)
    {
        auto* slot = chain.slot (i);
        smix::SlotState s;
        s.pluginUid = slot->uid;
        s.bypassed = slot->bypassed.load();
        s.protectedSlot = slot->protectedSlot;
        if (const auto* info = library->catalog().find (slot->uid))
            s.category = info->category;

        if (slot->instance != nullptr)
        {
            auto& instance = *slot->instance;
            s.pluginName = instance.getName().toStdString();
            slot->name = s.pluginName;
            if (s.category == smix::PluginCategory::Unknown)
                s.category = smix::PluginCatalog::classify (s.pluginName, instance.getPluginDescription().manufacturerName.toStdString(),
                                                            instance.getPluginDescription().category.toStdString());
            const auto& params = instance.getParameters();
            slot->cachedParams.clear();
            for (int p = 0; p < juce::jmin (params.size(), 256); ++p)
            {
                auto* param = params[p];
                smix::ParamInfo info;
                info.index = param->getParameterIndex();
                if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (param))
                    info.id = hosted->getParameterID().toStdString();
                info.name = param->getName (64).toStdString();
                info.label = param->getLabel().toStdString();
                info.value = param->getValue();
                info.defaultValue = param->getDefaultValue();
                info.valueText = param->getCurrentValueAsText().toStdString();
                const int steps = param->getNumSteps();
                info.numSteps = (steps > 0 && steps < 1000) ? steps : 0;
                info.isBoolean = param->isBoolean();
                info.semantic = smix::classifyParameter (info.name, s.category);
                slot->cachedParams.push_back (info);
            }
            if (learnMissingMaps && slot->mappers.empty())
                HostedChain::learnValueMaps (*slot);
        }
        else
        {
            s.pluginName = slot->name + " (절전 중)";
        }
        s.params = slot->cachedParams;
        s.mappers = slot->mappers;
        c.chain.push_back (std::move (s));
    }
    return c;
}

bool SoundManagerProcessor::setHostedParameter (int slotIndex, int paramIndex, float normalised)
{
    auto* slot = chain.slot (slotIndex);
    if (slot == nullptr)
        return false;
    if (slot->hibernated && ! wakeSlot (slotIndex))
        return false;
    const auto& params = slot->instance->getParameters();
    if (! juce::isPositiveAndBelow (paramIndex, params.size()))
        return false;

    auto* p = params[paramIndex];
    normalised = juce::jlimit (0.0f, 1.0f, normalised);
    if (p->isBoolean() || p->isDiscrete() || std::abs (p->getValue() - normalised) < 0.01f)
    {
        p->setValueNotifyingHost (normalised);
        return true;
    }
    // Continuous parameters glide over ~300 ms (no zipper noise).
    ramps.erase (std::remove_if (ramps.begin(), ramps.end(), [&] (auto& r) { return r.slot == slot && r.paramIndex == paramIndex; }), ramps.end());
    ramps.push_back ({ slot, paramIndex, p->getValue(), normalised, 0, 10 });
    if (! rampTimer->isTimerRunning())
        rampTimer->startTimerHz (30);
    return true;
}

void SoundManagerProcessor::advanceRamps()
{
    for (auto& r : ramps)
    {
        bool alive = false;
        for (int i = 0; i < chain.size(); ++i)
            alive = alive || (chain.slot (i) == r.slot && r.slot->instance != nullptr);
        if (! alive)
        {
            r.step = r.steps;
            continue;
        }
        ++r.step;
        const float t = static_cast<float> (r.step) / static_cast<float> (r.steps);
        r.slot->instance->getParameters()[r.paramIndex]->setValueNotifyingHost (r.from + (r.to - r.from) * t);
    }
    ramps.erase (std::remove_if (ramps.begin(), ramps.end(), [] (auto& r) { return r.step >= r.steps; }), ramps.end());
    if (ramps.empty())
        rampTimer->stopTimer();
}

bool SoundManagerProcessor::setAiGainDb (float db)
{
    auto* p = parameters.getParameter ("aiGain");
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (db));
    p->endChangeGesture();
    return true;
}

bool SoundManagerProcessor::setSlotBypass (int slotIndex, bool bypassed)
{
    auto* slot = chain.slot (slotIndex);
    if (slot == nullptr)
        return false;
    if (! bypassed && slot->hibernated)
        wakeSlot (slotIndex);
    if (bypassed && ! slot->bypassed)
        slot->bypassedSince = Engine::now();
    slot->bypassed = bypassed;
    return true;
}

bool SoundManagerProcessor::setSlotProtected (int slotIndex, bool isProtected)
{
    if (auto* slot = chain.slot (slotIndex))
    {
        slot->protectedSlot = isProtected;
        sendChangeMessage();
        return true;
    }
    return false;
}

bool SoundManagerProcessor::moveSlot (int from, int to)
{
    if (! juce::isPositiveAndBelow (from, chain.size()) || ! juce::isPositiveAndBelow (to, chain.size()))
        return false;
    chain.moveSlot (from, to);
    return true;
}

std::vector<std::uint8_t> SoundManagerProcessor::captureSlotState (int slotIndex)
{
    auto* slot = chain.slot (slotIndex);
    if (slot == nullptr)
        return {};
    if (slot->instance == nullptr)
        return engine->memory().read (slot->stateBuffer);
    juce::MemoryBlock mb;
    slot->instance->getStateInformation (mb);
    const auto* b = static_cast<const std::uint8_t*> (mb.getData());
    return { b, b + mb.getSize() };
}

void SoundManagerProcessor::updateLatency()
{
    setLatencySamples (chain.getLatencySamples());
}

//==============================================================================
bool SoundManagerProcessor::hibernateSlot (int slotIndex, bool forSilence)
{
    auto* slot = chain.slot (slotIndex);
    if (slot == nullptr || slot->hibernated || slot->instance == nullptr)
        return false;

    juce::MemoryBlock state;
    slot->instance->getStateInformation (state);
    const auto buffer = engine->memory().store (state.getData(), state.getSize());
    if (buffer == smix::mem::kNoBuffer && state.getSize() > 0)
        return false;  // the budget cannot hold the state: keep the plugin loaded

    // The scanned description (the one used to create it) is the reliable way back.
    slot->description = library->descriptionFor (slot->uid).value_or (slot->instance->getPluginDescription());
    slot->stateBuffer = buffer;
    closeWindowsFor (slot->instance.get());
    ramps.erase (std::remove_if (ramps.begin(), ramps.end(), [slot] (auto& r) { return r.slot == slot; }), ramps.end());
    auto instance = chain.takeInstance (slotIndex);
    slot->hibernated = true;
    slot->hibernatedForSilence = forSilence;
    instance.reset();  // frees the plugin's own memory
    updateLatency();
    addLog ("auto", juce::String (slot->name) + ko (": 사용하지 않아 절전(메모리 해제)했어요"));
    return true;
}

bool SoundManagerProcessor::wakeSlot (int slotIndex)
{
    auto* slot = chain.slot (slotIndex);
    if (slot == nullptr || ! slot->hibernated)
        return slot != nullptr;

    juce::String error;
    auto instance = library->formats().createPluginInstance (slot->description, currentSampleRate, currentBlockSize, error);
    if (instance == nullptr)
    {
        addLog ("system", ko ("플러그인을 다시 불러오지 못했어요: ") + error);
        return false;
    }
    const auto state = engine->memory().read (slot->stateBuffer);
    if (! state.empty())
        instance->setStateInformation (state.data(), static_cast<int> (state.size()));
    chain.putInstance (slotIndex, std::move (instance));
    engine->memory().free (slot->stateBuffer);
    slot->stateBuffer = smix::mem::kNoBuffer;
    slot->hibernated = false;
    slot->hibernatedForSilence = false;
    updateLatency();
    return true;
}

double SoundManagerProcessor::secondsSinceSignal() const
{
    const auto last = lastSignalMs.load (std::memory_order_relaxed);
    if (last == 0)
        return 1.0e9;
    return (juce::Time::getMillisecondCounter() - last) * 0.001;
}

void SoundManagerProcessor::hibernationTick (double now, bool aggressive)
{
    const double silent = secondsSinceSignal();
    for (int i = 0; i < chain.size(); ++i)
    {
        auto* slot = chain.slot (i);
        if (slot->hibernated)
        {
            // Audio is back on a channel that went to sleep for silence: wake its plugins.
            if (slot->hibernatedForSilence && silent < 1.0)
                wakeSlot (i);
            continue;
        }
        if (slot->bypassed && slot->bypassedSince > 0 && now - slot->bypassedSince > 30.0)
            hibernateSlot (i, false);
        else if (aggressive && silent > 120.0 && ! isLoadingChain())
            hibernateSlot (i, true);
    }
}

//==============================================================================
void SoundManagerProcessor::startEarCapture()
{
    if (earCapturing.load() || currentSampleRate <= 0)
        return;
    earLength = static_cast<int> (currentSampleRate * 3.0);
    earBuffer = std::make_unique<float[]> (static_cast<size_t> (earLength));
    earWritePos.store (0);
    earCapturing.store (true, std::memory_order_release);
}

std::vector<std::string> SoundManagerProcessor::nameSuggestions()
{
    if (earBuffer != nullptr && ! earCapturing.load (std::memory_order_acquire) && earWritePos.load() >= earLength)
    {
        const auto features = smix::ear::EarFeatures::extract (earBuffer.get(), static_cast<size_t> (earLength), currentSampleRate);
        earBuffer.reset();  // the capture is only kept while it is needed
        auto lease = engine->modules().acquire (smix::modules::ModuleId::EarModel, Engine::now());
        const auto guesses = lease ? engine->ear().classify (features) : smix::ear::heuristicGuess (features);
        earSuggestions.clear();
        for (auto& g : guesses)
            if (g.probability > 0.15f)
                earSuggestions.push_back (! g.display.empty() ? g.display
                                          : g.role != smix::InstrumentRole::Unknown ? smix::koreanName (g.role) : g.label);
    }
    return earSuggestions;
}

void SoundManagerProcessor::showHostedEditor (int slotIndex)
{
    auto* slot = chain.slot (slotIndex);
    if (slot == nullptr)
        return;
    if (slot->hibernated && ! wakeSlot (slotIndex))
        return;
    for (auto& w : pluginWindows)
    {
        if (&w->processor == slot->instance.get())
        {
            w->setVisible (true);
            w->toFront (true);
            return;
        }
    }
    pluginWindows.push_back (std::make_unique<PluginWindow> (*slot->instance));
}

void SoundManagerProcessor::closeWindowsFor (const juce::AudioProcessor* p)
{
    pluginWindows.erase (std::remove_if (pluginWindows.begin(), pluginWindows.end(), [p] (auto& w) { return &w->processor == p; }),
                         pluginWindows.end());
}

//==============================================================================
void SoundManagerProcessor::loadChain (const std::vector<std::string>& uids, std::vector<juce::MemoryBlock> states, std::vector<bool> bypassStates,
                                       std::vector<bool> protectStates)
{
    struct Job
    {
        std::vector<HostedChain::Entry> entries;
        int remaining = 0;
        int generation = 0;
    };

    auto job = std::make_shared<Job>();
    job->generation = ++loadGeneration;
    job->entries.resize (uids.size());

    auto finish = [this] (std::shared_ptr<Job> j) {
        if (j->generation != loadGeneration)
            return;
        if (restoreGeneration >= 0 && j->generation >= restoreGeneration)
        {
            const juce::ScopedLock sl (logLock);
            pendingRestoreState.reset();
            restoreGeneration = -1;
        }
        auto old = chain.rebuild (std::move (j->entries));
        for (auto& removed : old)
            if (removed != nullptr)
            {
                if (removed->instance != nullptr)
                    closeWindowsFor (removed->instance.get());
                engine->memory().free (removed->stateBuffer);
                ramps.erase (std::remove_if (ramps.begin(), ramps.end(), [&removed] (auto& r) { return r.slot == removed.get(); }), ramps.end());
            }
        old.clear();
        updateLatency();
        sendChangeMessage();
    };

    std::vector<HostedChain::Slot*> claimed;
    for (size_t i = 0; i < uids.size(); ++i)
    {
        const auto& uid = uids[i];
        const bool hasState = i < states.size() && states[i].getSize() > 0;

        HostedChain::Slot* existing = nullptr;
        for (int s = 0; s < chain.size() && existing == nullptr && ! hasState; ++s)
            if (auto* slot = chain.slot (s); slot->uid == uid && std::find (claimed.begin(), claimed.end(), slot) == claimed.end())
                existing = slot;
        if (existing != nullptr)
        {
            claimed.push_back (existing);
            job->entries[i].reuse = existing;
            continue;
        }

        const auto description = library->descriptionFor (uid);
        if (! description)
        {
            addLog ("system", ko ("플러그인을 찾을 수 없어요: ") + juce::String (uid) + ko (" (플러그인 목록을 다시 스캔해 주세요)"));
            continue;
        }

        ++job->remaining;
        ++pendingLoads;
        const auto state = hasState ? states[i] : juce::MemoryBlock();
        const bool bypass = i < bypassStates.size() ? static_cast<bool> (bypassStates[i]) : false;
        const bool isProtected = i < protectStates.size() ? static_cast<bool> (protectStates[i]) : false;
        library->formats().createPluginInstanceAsync (
            *description, currentSampleRate, currentBlockSize,
            [safe = juce::WeakReference<SoundManagerProcessor> (this), job, i, uid, state, bypass, isProtected, finish]
            (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) {
                auto* self = safe.get();
                if (self == nullptr)
                    return;
                --self->pendingLoads;
                if (instance != nullptr)
                {
                    if (state.getSize() > 0)
                        instance->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
                    auto slot = std::make_unique<HostedChain::Slot>();
                    slot->name = instance->getName().toStdString();
                    slot->instance = std::move (instance);
                    slot->uid = uid;
                    slot->bypassed = bypass;
                    slot->bypassedSince = bypass ? Engine::now() : 0.0;
                    slot->protectedSlot = isProtected;
                    job->entries[i].fresh = std::move (slot);
                }
                else
                {
                    self->addLog ("system", ko ("플러그인 로드 실패: ") + juce::String (uid) + " - " + error);
                }
                if (--job->remaining == 0)
                    finish (job);
            });
    }
    if (job->remaining == 0)
        finish (job);
}

//==============================================================================
std::vector<SoundManagerProcessor::LogEntry> SoundManagerProcessor::getLog() const
{
    const juce::ScopedLock sl (logLock);
    return log;
}

void SoundManagerProcessor::addLog (const juce::String& who, const juce::String& text)
{
    {
        const juce::ScopedLock sl (logLock);
        log.push_back ({ who, text });
        if (log.size() > kMaxLogEntries)
            log.erase (log.begin(), log.begin() + static_cast<long> (log.size() - kMaxLogEntries));
    }
    sendChangeMessage();
}

void SoundManagerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    {
        const juce::ScopedLock sl (logLock);
        if (pendingRestoreState.getSize() > 0)
        {
            destData = pendingRestoreState;
            return;
        }
    }

    juce::XmlElement root (kStateTag);
    root.setAttribute ("id", juce::String (instanceId));
    root.setAttribute ("parent", parentSetting);
    root.setAttribute ("label", userLabel);
    root.setAttribute ("style", style);
    root.setAttribute ("reference", referenceName);

    if (auto params = parameters.copyState().createXml())
        root.addChildElement (params.release());

    auto* chainXml = root.createNewChildElement ("CHAIN");
    for (int i = 0; i < chain.size(); ++i)
    {
        auto* slot = chain.slot (i);
        const auto state = captureSlotState (i);
        juce::MemoryBlock mb (state.data(), state.size());
        auto* e = chainXml->createNewChildElement ("SLOT");
        e->setAttribute ("uid", juce::String (slot->uid));
        e->setAttribute ("bypassed", slot->bypassed.load());
        e->setAttribute ("protected", slot->protectedSlot);
        e->setAttribute ("state", mb.toBase64Encoding());
    }

    // This channel's slice of the mix history (the hub reassembles it on load).
    root.createNewChildElement ("HISTORY")->addTextElement (juce::String (hub->history().exportChannel (instanceId, 40).dump()));

    auto* logXml = root.createNewChildElement ("LOG");
    const juce::ScopedLock sl (logLock);
    const size_t first = log.size() > 60 ? log.size() - 60 : 0;
    for (size_t i = first; i < log.size(); ++i)
    {
        auto* e = logXml->createNewChildElement ("E");
        e->setAttribute ("who", log[i].who);
        e->setAttribute ("text", log[i].text);
    }
    copyXmlToBinary (root, destData);
}

void SoundManagerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (kStateTag))
        return;
    {
        const juce::ScopedLock sl (logLock);
        pendingRestoreState.replaceAll (data, static_cast<size_t> (sizeInBytes));
    }

    std::shared_ptr<juce::XmlElement> root (xml.release());
    juce::MessageManager::callAsync ([safe = juce::WeakReference<SoundManagerProcessor> (this), root] {
        auto* self = safe.get();
        if (self == nullptr)
            return;
        self->instanceId = root->getStringAttribute ("id", juce::String (self->instanceId)).toStdString();
        self->hub->ensureUniqueId (*self);
        self->parentSetting = root->getStringAttribute ("parent", "auto");
        self->userLabel = root->getStringAttribute ("label");
        self->style = root->getStringAttribute ("style");
        self->referenceName = root->getStringAttribute ("reference");
        if (auto* params = root->getChildByName (self->parameters.state.getType()))
            self->parameters.replaceState (juce::ValueTree::fromXml (*params));

        std::vector<std::string> uids;
        std::vector<juce::MemoryBlock> states;
        std::vector<bool> bypass, protect;
        if (auto* chainXml = root->getChildByName ("CHAIN"))
            for (auto* e : chainXml->getChildIterator())
            {
                juce::MemoryBlock state;
                state.fromBase64Encoding (e->getStringAttribute ("state"));
                uids.push_back (e->getStringAttribute ("uid").toStdString());
                states.push_back (state);
                bypass.push_back (e->getBoolAttribute ("bypassed"));
                protect.push_back (e->getBoolAttribute ("protected"));
            }
        self->restoreGeneration = self->loadGeneration + 1;
        self->loadChain (uids, states, bypass, protect);

        if (auto* history = root->getChildByName ("HISTORY"))
        {
            const auto j = nlohmann::json::parse (history->getAllSubText().toStdString(), nullptr, false);
            if (! j.is_discarded())
                self->hub->history().importChannel (j);
        }

        {
            const juce::ScopedLock sl (self->logLock);
            self->log.clear();
            if (auto* logXml = root->getChildByName ("LOG"))
                for (auto* e : logXml->getChildIterator())
                    self->log.push_back ({ e->getStringAttribute ("who"), e->getStringAttribute ("text") });
        }
        self->sendChangeMessage();
    });
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SoundManagerProcessor();
}
