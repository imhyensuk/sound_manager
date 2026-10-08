#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <smix/AudioAnalyzer.h>
#include <smix/AutoMixer.h>
#include <smix/MixSession.h>

#include "AgentWorker.h"
#include "HostedChain.h"
#include "PluginLibrary.h"
#include "PluginWindow.h"
#include "SessionHub.h"

/**
    One Sound Manager instance = one mixer channel.

    Signal flow:  input -> [user's plugins, AI-ordered] -> AI gain stage -> analyser ("ears") -> output

    Instances find each other through the SessionHub, so a bus/master instance can mix
    every channel routed into it (scope rules in smix::MixSession).
*/
class SoundManagerProcessor : public juce::AudioProcessor,
                              public juce::ChangeBroadcaster
{
public:
    SoundManagerProcessor();
    ~SoundManagerProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    void updateTrackProperties (const TrackProperties&) override;

    //==============================================================================
    // Identity & routing (message thread)
    const std::string& getInstanceId() const noexcept { return instanceId; }
    void regenerateInstanceId() { instanceId = juce::Uuid().toString().substring (0, 8).toStdString(); }
    juce::String getDisplayName() const;
    smix::ChannelKind getEffectiveKind() const;
    smix::InstrumentRole getEffectiveRole() const;

    /** "" or "auto" = guess (drum tracks -> the drum bus instance, otherwise master). */
    juce::String getParentSetting() const { return parentSetting; }
    void setParentSetting (const juce::String& id) { parentSetting = id; }
    void setUserLabel (const juce::String& l) { userLabel = l; }

    bool isAutoMixEnabled() const;
    bool isGainLocked() const;

    //==============================================================================
    // Called by the SessionHub / UI on the message thread
    smix::ChannelState buildChannelState (bool learnMissingMaps);

    bool setHostedParameter (int slot, int paramIndex, float normalised);
    bool setAiGainDb (float db);
    bool setSlotBypass (int slot, bool bypassed);
    bool moveSlot (int from, int to);

    /** Loads plugins (async) and replaces the chain. Plugins already in the chain keep their state. */
    void loadChain (const std::vector<std::string>& uids,
                    std::vector<juce::MemoryBlock> states = {},    // per position, for restoring a session
                    std::vector<bool> bypassStates = {});
    bool isLoadingChain() const noexcept { return pendingLoads > 0; }

    /** Opens the hosted plugin's own editor in a floating window. */
    void showHostedEditor (int slot);

    HostedChain& getChain() noexcept { return chain; }
    smix::AutoMixer& getAutoMixer() noexcept { return autoMixer; }
    PluginLibrary& getLibrary() noexcept { return *library; }
    SessionHub& getHub() noexcept { return *hub; }
    AgentWorker& getAgent() noexcept { return agent; }
    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }

    // Chat & activity log (message thread)
    struct LogEntry { juce::String who, text; };
    const std::vector<LogEntry>& getLog() const noexcept { return log; }
    void addLog (const juce::String& who, const juce::String& text);

    static juce::StringArray kindChoices();
    static juce::StringArray roleChoices();

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::SharedResourcePointer<PluginLibrary> library;
    juce::SharedResourcePointer<SessionHub> hub;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* aiGainParam = nullptr;

    std::string instanceId;
    juce::String trackName, userLabel, parentSetting { "auto" };

    HostedChain chain;
    smix::AudioAnalyzer analyzer;
    smix::AudioFeatures latestFeatures;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gainSmoothed;

    smix::AutoMixer autoMixer;
    AgentWorker agent;

    std::vector<std::unique_ptr<PluginWindow>> pluginWindows;
    void closeWindowsFor (const juce::AudioProcessor*);

    std::vector<LogEntry> log;
    juce::CriticalSection logLock;  // getStateInformation may run on a non-message thread
    int pendingLoads = 0;
    int loadGeneration = 0;

    // While a restored session is still loading its plugins, saving must return the original state.
    juce::MemoryBlock pendingRestoreState;
    int restoreGeneration = -1;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;

    struct Ramp { HostedChain::Slot* slot; int paramIndex; float from, to; int step, steps; };
    class RampTimer;
    std::vector<Ramp> ramps;
    std::unique_ptr<RampTimer> rampTimer;
    void advanceRamps();

    JUCE_DECLARE_WEAK_REFERENCEABLE (SoundManagerProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundManagerProcessor)
};
