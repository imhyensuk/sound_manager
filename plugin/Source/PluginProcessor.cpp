#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "Text.h"

namespace
{
const juce::Identifier kStateTag ("SoundManagerState");
constexpr int kMaxLogEntries = 200;
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

//==============================================================================
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
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "aiGain", 1 }, "AI Gain",
                                                       NormalisableRange<float> (-24.0f, 12.0f, 0.01f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "autoMix", 1 }, "Auto Mix", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "gainLock", 1 }, "Lock Level", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "kind", 1 }, "Channel Kind", kindChoices(), 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "role", 1 }, "Instrument Role", roleChoices(), 0));
    return layout;
}

SoundManagerProcessor::SoundManagerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "PARAMS", createLayout()),
      agent (*this)
{
    aiGainParam = parameters.getRawParameterValue ("aiGain");
    instanceId = juce::Uuid().toString().substring (0, 8).toStdString();
    rampTimer = std::make_unique<RampTimer> (*this);
    hub->add (this);
}

SoundManagerProcessor::~SoundManagerProcessor()
{
    agent.shutdown();
    pluginWindows.clear();
    rampTimer->stopTimer();
    hub->remove (this);
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

    analyzer.process (buffer.getReadPointer (0), buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr,
                      buffer.getNumSamples());
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
    const int choice = static_cast<int> (parameters.getRawParameterValue ("kind")->load());
    switch (choice)
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

bool SoundManagerProcessor::isAutoMixEnabled() const { return parameters.getRawParameterValue ("autoMix")->load() > 0.5f; }
bool SoundManagerProcessor::isGainLocked() const     { return parameters.getRawParameterValue ("gainLock")->load() > 0.5f; }

void SoundManagerProcessor::updateTrackProperties (const TrackProperties& properties)
{
    // Hosts may call this from any thread.
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
    c.features = latestFeatures;

    for (int i = 0; i < chain.size(); ++i)
    {
        auto* slot = chain.slot (i);
        auto& instance = *slot->instance;

        smix::SlotState s;
        s.pluginUid = slot->uid;
        s.pluginName = instance.getName().toStdString();
        if (const auto* info = library->catalog().find (slot->uid))
            s.category = info->category;
        else
            s.category = smix::PluginCatalog::classify (s.pluginName, instance.getPluginDescription().manufacturerName.toStdString(),
                                                        instance.getPluginDescription().category.toStdString());
        s.bypassed = slot->bypassed.load();

        const auto& params = instance.getParameters();
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
            s.params.push_back (std::move (info));
        }

        if (learnMissingMaps && slot->mappers.empty())
            HostedChain::learnValueMaps (*slot);
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
    const auto& params = slot->instance->getParameters();
    if (! juce::isPositiveAndBelow (paramIndex, params.size()))
        return false;

    auto* p = params[paramIndex];
    normalised = juce::jlimit (0.0f, 1.0f, normalised);

    // Stepped/boolean parameters jump; continuous ones glide over ~300 ms to avoid zipper noise.
    if (p->isBoolean() || p->isDiscrete() || std::abs (p->getValue() - normalised) < 0.01f)
    {
        p->setValueNotifyingHost (normalised);
        return true;
    }

    ramps.erase (std::remove_if (ramps.begin(), ramps.end(), [&] (auto& r) { return r.slot == slot && r.paramIndex == paramIndex; }),
                 ramps.end());
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
            alive = alive || chain.slot (i) == r.slot;
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
    if (auto* slot = chain.slot (slotIndex))
    {
        slot->bypassed = bypassed;
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

void SoundManagerProcessor::loadChain (const std::vector<std::string>& uids,
                                       std::vector<juce::MemoryBlock> states,
                                       std::vector<bool> bypassStates)
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

    std::vector<HostedChain::Slot*> claimed;
    auto finish = [this] (std::shared_ptr<Job> j) {
        if (j->generation != loadGeneration)
            return;  // a newer chain request superseded this one
        if (restoreGeneration >= 0 && j->generation >= restoreGeneration)  // restore done (or superseded by a newer chain)
        {
            const juce::ScopedLock sl (logLock);
            pendingRestoreState.reset();
            restoreGeneration = -1;
        }
        auto old = chain.rebuild (std::move (j->entries));
        for (auto& removed : old)
            if (removed != nullptr)
                closeWindowsFor (removed->instance.get());
        old.clear();  // destroy removed plugins here, on the message thread
        setLatencySamples (chain.getLatencySamples());
        sendChangeMessage();
    };

    for (size_t i = 0; i < uids.size(); ++i)
    {
        const auto& uid = uids[i];

        HostedChain::Slot* existing = nullptr;
        for (int s = 0; s < chain.size() && existing == nullptr; ++s)
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
        const auto state = i < states.size() ? states[i] : juce::MemoryBlock();
        const bool bypass = i < bypassStates.size() ? static_cast<bool> (bypassStates[i]) : false;

        library->formats().createPluginInstanceAsync (
            *description, currentSampleRate, currentBlockSize,
            [safe = juce::WeakReference<SoundManagerProcessor> (this), job, i, uid, state, bypass, finish]
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
                    slot->instance = std::move (instance);
                    slot->uid = uid;
                    slot->bypassed = bypass;
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

void SoundManagerProcessor::showHostedEditor (int slotIndex)
{
    auto* slot = chain.slot (slotIndex);
    if (slot == nullptr)
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
    pluginWindows.erase (std::remove_if (pluginWindows.begin(), pluginWindows.end(),
                                         [p] (auto& w) { return &w->processor == p; }),
                         pluginWindows.end());
}

//==============================================================================
void SoundManagerProcessor::addLog (const juce::String& who, const juce::String& text)
{
    const juce::ScopedLock sl (logLock);
    log.push_back ({ who, text });
    if (log.size() > kMaxLogEntries)
        log.erase (log.begin(), log.begin() + static_cast<long> (log.size() - kMaxLogEntries));
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

    if (auto params = parameters.copyState().createXml())
        root.addChildElement (params.release());

    auto* chainXml = root.createNewChildElement ("CHAIN");
    for (int i = 0; i < chain.size(); ++i)
    {
        auto* slot = chain.slot (i);
        juce::MemoryBlock state;
        slot->instance->getStateInformation (state);
        auto* e = chainXml->createNewChildElement ("SLOT");
        e->setAttribute ("uid", juce::String (slot->uid));
        e->setAttribute ("bypassed", slot->bypassed.load());
        e->setAttribute ("state", state.toBase64Encoding());
    }

    auto* logXml = root.createNewChildElement ("LOG");
    const juce::ScopedLock sl (logLock);
    const size_t first = log.size() > 50 ? log.size() - 50 : 0;
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

        // A duplicated track carries a copy of this state: the hub keeps ids unique.
        self->instanceId = root->getStringAttribute ("id", juce::String (self->instanceId)).toStdString();
        self->hub->ensureUniqueId (*self);
        self->parentSetting = root->getStringAttribute ("parent", "auto");
        self->userLabel = root->getStringAttribute ("label");

        if (auto* params = root->getChildByName (self->parameters.state.getType()))
            self->parameters.replaceState (juce::ValueTree::fromXml (*params));

        std::vector<std::string> uids;
        std::vector<juce::MemoryBlock> states;
        std::vector<bool> bypass;
        if (auto* chainXml = root->getChildByName ("CHAIN"))
        {
            for (auto* e : chainXml->getChildIterator())
            {
                const auto uid = e->getStringAttribute ("uid").toStdString();
                juce::MemoryBlock state;
                state.fromBase64Encoding (e->getStringAttribute ("state"));
                uids.push_back (uid);
                states.push_back (state);
                bypass.push_back (e->getBoolAttribute ("bypassed"));
            }
        }
        self->restoreGeneration = self->loadGeneration + 1;  // the generation loadChain is about to use
        self->loadChain (uids, states, bypass);

        const juce::ScopedLock sl (self->logLock);
        self->log.clear();
        if (auto* logXml = root->getChildByName ("LOG"))
            for (auto* e : logXml->getChildIterator())
                self->log.push_back ({ e->getStringAttribute ("who"), e->getStringAttribute ("text") });
        self->sendChangeMessage();
    });
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SoundManagerProcessor();
}
