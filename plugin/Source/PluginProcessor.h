#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <smix/AudioAnalyzer.h>
#include <smix/AutoMixer.h>
#include <smix/MixSession.h>

#include "Engine.h"
#include "HostedChain.h"
#include "PluginLibrary.h"
#include "PluginWindow.h"
#include "SessionHub.h"

class AssistantWorker;

/**
    One Sound Manager instance = one mixer channel.

    Signal flow:  input -> [user's plugins, AI-ordered] -> AI gain stage -> analyser ("ears") -> output

    Instances are light: the heavy parts (language model, knowledge base, ear model, memory
    runtime) live once per process in the shared Engine and are loaded only when needed.
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
    // Identity, routing, user choices (message thread)
    const std::string& getInstanceId() const noexcept { return instanceId; }
    void regenerateInstanceId() { instanceId = juce::Uuid().toString().substring (0, 8).toStdString(); }
    juce::String getDisplayName() const;
    juce::String getUserLabel() const { return userLabel; }
    void setUserLabel (const juce::String& l) { userLabel = l; sendChangeMessage(); }
    smix::ChannelKind getEffectiveKind() const;
    smix::InstrumentRole getEffectiveRole() const;

    juce::String getParentSetting() const { return parentSetting; }
    void setParentSetting (const juce::String& id) { parentSetting = id; }
    juce::String getStyle() const { return style; }
    void setStyle (const juce::String& s) { style = s; sendChangeMessage(); }
    juce::String getReferenceName() const { return referenceName; }
    void setReferenceName (const juce::String& n) { referenceName = n; sendChangeMessage(); }

    bool isAutoMixEnabled() const;
    void setAutoMixEnabled (bool);
    bool isGainLocked() const;
    bool isChannelProtected() const;
    void setChannelProtected (bool);

    //==============================================================================
    // Called by the SessionHub / assistant / UI on the message thread
    smix::ChannelState buildChannelState (bool learnMissingMaps);

    bool setHostedParameter (int slot, int paramIndex, float normalised);
    bool setAiGainDb (float db);
    bool setSlotBypass (int slot, bool bypassed);
    bool setSlotProtected (int slot, bool isProtected);
    bool moveSlot (int from, int to);
    std::vector<std::uint8_t> captureSlotState (int slot);

    void loadChain (const std::vector<std::string>& uids, std::vector<juce::MemoryBlock> states = {}, std::vector<bool> bypassStates = {},
                    std::vector<bool> protectStates = {});
    bool isLoadingChain() const noexcept { return pendingLoads > 0; }

    /** Hibernation (memory): unload a hosted plugin, keeping its state in the memopro runtime. */
    bool hibernateSlot (int slot, bool forSilence);
    bool wakeSlot (int slot);
    void hibernationTick (double now, bool aggressive);
    double secondsSinceSignal() const;

    /** Instrument recognition for the naming question: records 3 s, then the ear model listens. */
    void startEarCapture();
    std::vector<std::string> nameSuggestions();  // empty until a capture was classified

    void showHostedEditor (int slot);

    /** The assistant asks the UI to show an analysis view ("rta", "waterfall", ...). */
    void requestView (const juce::String& v) { requestedView = v; sendChangeMessage(); }
    juce::String consumeRequestedView() { auto v = requestedView; requestedView.clear(); return v; }

    HostedChain& getChain() noexcept { return chain; }
    smix::AudioAnalyzer& getAnalyzer() noexcept { return analyzer; }
    smix::AutoMixer& getAutoMixer() noexcept { return autoMixer; }
    PluginLibrary& getLibrary() noexcept { return *library; }
    Engine& getEngine() noexcept { return *engine; }
    SessionHub& getHub() noexcept { return *hub; }
    AssistantWorker& getAssistant() noexcept { return *assistant; }
    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }

    struct LogEntry { juce::String who, text; };
    std::vector<LogEntry> getLog() const;
    void addLog (const juce::String& who, const juce::String& text);

    static juce::StringArray kindChoices();
    static juce::StringArray roleChoices();

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void closeWindowsFor (const juce::AudioProcessor*);
    void advanceRamps();
    void updateLatency();

    juce::SharedResourcePointer<PluginLibrary> library;
    juce::SharedResourcePointer<Engine> engine;
    juce::SharedResourcePointer<SessionHub> hub;

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* aiGainParam = nullptr;

    std::string instanceId;
    juce::String trackName, userLabel, parentSetting { "auto" }, style, referenceName, requestedView;

    HostedChain chain;
    smix::AudioAnalyzer analyzer;
    smix::AudioFeatures latestFeatures;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gainSmoothed;
    std::atomic<juce::uint32> lastSignalMs { 0 };

    // Ear capture (allocated only while recognising the instrument)
    std::unique_ptr<float[]> earBuffer;
    std::atomic<int> earWritePos { 0 };
    std::atomic<bool> earCapturing { false };
    int earLength = 0;
    std::vector<std::string> earSuggestions;

    smix::AutoMixer autoMixer;
    std::unique_ptr<AssistantWorker> assistant;

    std::vector<LogEntry> log;
    mutable juce::CriticalSection logLock;
    int pendingLoads = 0;
    int loadGeneration = 0;
    juce::MemoryBlock pendingRestoreState;
    int restoreGeneration = -1;
    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;

    struct Ramp { HostedChain::Slot* slot; int paramIndex; float from, to; int step, steps; };
    class RampTimer;
    std::vector<Ramp> ramps;
    std::unique_ptr<RampTimer> rampTimer;
    std::vector<std::unique_ptr<PluginWindow>> pluginWindows;

    JUCE_DECLARE_WEAK_REFERENCEABLE (SoundManagerProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundManagerProcessor)
};
