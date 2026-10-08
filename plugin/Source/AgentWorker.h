#pragma once

#include <juce_core/juce_core.h>
#include <smix/ClaudeMixAgent.h>
#include <smix/LocalIntentInterpreter.h>

#include "JuceHttpTransport.h"

class SoundManagerProcessor;

/**
    Runs chat requests off the message thread.

    With an Anthropic API key it uses smix::ClaudeMixAgent (the model hears the analysis,
    inspects plugins and applies validated actions). Without one it falls back to the
    offline smix::LocalIntentInterpreter.
*/
class AgentWorker : private juce::Thread
{
public:
    explicit AgentWorker (SoundManagerProcessor&);
    ~AgentWorker() override;

    /** Message thread. */
    void submit (const juce::String& userText);
    void resetConversation();
    bool isBusy() const noexcept { return busy.load(); }

    /** Must be called by the owner before it starts tearing down. */
    void shutdown();

private:
    void run() override;
    void handle (const juce::String& text);

    SoundManagerProcessor& owner;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    juce::CriticalSection queueLock;
    juce::StringArray queue;
    juce::WaitableEvent wake;
    std::atomic<bool> busy { false };
    std::atomic<bool> resetRequested { false };

    JuceHttpTransport transport;
    smix::ClaudeMixAgent agent { {}, transport };
    smix::LocalIntentInterpreter local;
};
