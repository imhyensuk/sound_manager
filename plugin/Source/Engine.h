#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <smix/Progress.h>
#include <smix/ear/EarModel.h>
#include <smix/knowledge/KnowledgeBase.h>
#include <smix/llm/LocalLlm.h>
#include <smix/mem/MemoryRuntime.h>
#include <smix/modules/ModuleManager.h>
#include <smix/style/StyleProfile.h>

#include "PluginLibrary.h"

/**
    The process-wide AI engine shared by every Sound Manager instance
    (juce::SharedResourcePointer): one memopro memory runtime, one module manager and one copy
    of each model, however many instances the project has.

    Everything runs locally; there is no network code in the plugin.
*/
class Engine : public juce::ChangeBroadcaster,
               private juce::Timer
{
public:
    Engine();
    ~Engine() override;

    static double now();  // monotonic seconds

    smix::mem::Runtime& memory() noexcept { return *runtime; }
    smix::modules::ModuleManager& modules() noexcept { return moduleManager; }
    PluginLibrary& library() noexcept { return *lib; }

    // --- models (use only while holding a module lease) -------------------------------
    smix::knowledge::KnowledgeBase& knowledge() noexcept { return *kb; }
    smix::llm::LocalLlm& llm() noexcept { return *localLlm; }
    smix::llm::LlmIntentModel& llmIntent() noexcept { return *llmIntentModel; }
    smix::ear::EarModel& ear() noexcept { return *earModel; }

    juce::File llmModelFile() const;   // configured GGUF, or the first one in the models folder
    juce::File earModelFile() const;
    juce::File knowledgeFile() const { return PluginLibrary::dataDirectory().getChildFile ("knowledge.jsonl"); }
    void saveKnowledge();

    /** Estimated seconds to load the language model (for the ETA display). */
    double llmLoadSecondsEstimate() const;

    // --- reference tracks (message thread) ---------------------------------------------
    const std::vector<smix::style::StyleProfile>& references() const noexcept { return refs; }
    void addReference (const smix::style::StyleProfile&);
    void removeReference (const std::string& name);
    const smix::style::StyleProfile* findReference (const std::string& name) const;

    // --- long jobs ----------------------------------------------------------------------
    class ProfilerJob;
    class ReferenceJob;
    ProfilerJob& profiler() noexcept { return *profilerJob; }
    ReferenceJob& referenceJob() noexcept { return *refJob; }

    /** Plugins that the knowledge base does not know yet (or knows from an older profiler). */
    std::vector<std::string> unprofiledPlugins();

    juce::String statusText();

private:
    void timerCallback() override;
    void registerModules();
    void loadReferences();
    void saveReferences();

    juce::SharedResourcePointer<PluginLibrary> lib;
    std::unique_ptr<smix::mem::Runtime> runtime;
    smix::modules::ModuleManager moduleManager;
    smix::modules::ModuleManager::Lease earLease;
    std::unique_ptr<smix::knowledge::KnowledgeBase> kb;
    std::unique_ptr<smix::llm::LocalLlm> localLlm;
    std::unique_ptr<smix::llm::LlmIntentModel> llmIntentModel;
    std::unique_ptr<smix::ear::EarModel> earModel;
    std::vector<smix::style::StyleProfile> refs;
    std::unique_ptr<ProfilerJob> profilerJob;
    std::unique_ptr<ReferenceJob> refJob;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};

//==============================================================================
/**
    Learns every installed plugin (requirement 4). Profiling runs in a separate helper
    process (SoundManagerProfiler): a plugin that crashes takes down only the helper, which is
    restarted with the remaining plugins. Results go into the knowledge base.
*/
class Engine::ProfilerJob : private juce::Thread
{
public:
    explicit ProfilerJob (Engine& e) : juce::Thread ("Sound Manager profiler"), engine (e) {}
    ~ProfilerJob() override { cancel(); }

    void start (std::vector<std::string> uids);
    void cancel();
    bool isRunning() const { return isThreadRunning(); }

    nlohmann::json progressJson() const;
    juce::String describe() const;
    static juce::File helperExecutable();

private:
    void run() override;
    bool runHelper (std::vector<std::string>& remaining);

    Engine& engine;
    std::vector<std::string> queue;
    mutable juce::CriticalSection lock;
    smix::ProgressEstimator progress;
    juce::String current, lastError;
    int failures = 0;
};

/**
    Analyses an uploaded reference (requirement 2): audio files through JUCE's decoders, video
    or exotic formats through a local ffmpeg if one is installed. Streamed in chunks, so even a
    long video needs only a few megabytes.
*/
class Engine::ReferenceJob : private juce::Thread
{
public:
    explicit ReferenceJob (Engine& e) : juce::Thread ("Sound Manager reference"), engine (e) {}
    ~ReferenceJob() override { stopThread (4000); }

    void analyse (const juce::File&);
    bool isRunning() const { return isThreadRunning(); }
    nlohmann::json progressJson() const;
    juce::String describe() const;
    juce::String lastResult() const;

private:
    void run() override;
    bool decodeWithJuce (std::unique_ptr<smix::style::StyleAnalyzer>&);
    bool decodeWithFfmpeg (std::unique_ptr<smix::style::StyleAnalyzer>&);
    void setProgress (double done, double total);

    Engine& engine;
    juce::File file;
    mutable juce::CriticalSection lock;
    smix::ProgressEstimator progress;
    juce::String result;
};
