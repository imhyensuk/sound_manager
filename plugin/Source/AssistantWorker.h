#pragma once

#include <juce_core/juce_core.h>
#include <smix/MixAssistant.h>

#include <deque>
#include <optional>

class SoundManagerProcessor;

/**
    Runs the active assistant (smix::MixAssistant) of one instance on its own thread: the local
    language model can take seconds, the DAW's message thread must never wait for it.

    Everything that touches the mix is marshalled to the message thread; the assistant itself is
    only ever used from this worker, so it needs no locking.
*/
class AssistantWorker : private juce::Thread
{
public:
    explicit AssistantWorker (SoundManagerProcessor&);
    ~AssistantWorker() override;

    // Message thread -------------------------------------------------------------------
    void submit (const juce::String& userText);
    void answer (int questionId, int option, bool skip, const juce::String& text = {});
    void resetConversation();
    void shutdown();

    struct PendingQuestion
    {
        int id = 0;
        juce::String text;
        juce::StringArray options;
        int defaultOption = -1;
        bool freeText = false;
        double remainingSeconds = -1.0;
    };
    std::vector<PendingQuestion> pendingQuestions() const;

    /** "응답 생성 중 · 약 6초" while the assistant works, empty when idle. */
    juce::String status() const;
    bool isBusy() const noexcept { return busy.load(); }
    juce::String reasoningEngine() const;  // "local-llm" or "rules"

private:
    struct Task
    {
        enum Type { User, Answer, Reset } type = User;
        juce::String text;
        int questionId = 0, option = -1;
        bool skip = false;
    };

    void run() override;
    void handle (const Task&);
    void publish (const std::vector<smix::MixAssistant::Message>&);
    void setStatus (const juce::String&);

    class Host;
    SoundManagerProcessor& owner;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::unique_ptr<Host> host;
    smix::llm::RuleIntentModel rules;
    std::unique_ptr<smix::MixAssistant> assistant;

    mutable juce::CriticalSection lock;
    std::deque<Task> queue;
    std::vector<PendingQuestion> questions;
    juce::String currentStatus, engineName { "rules" };
    juce::WaitableEvent wake;
    std::atomic<bool> busy { false };
};
