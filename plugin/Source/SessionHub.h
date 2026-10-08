#pragma once

#include <juce_events/juce_events.h>
#include <smix/ChainPlanner.h>
#include <smix/MixAction.h>
#include <smix/MixSession.h>

class SoundManagerProcessor;

/**
    Links every Sound Manager instance living in this DAW process (shared via
    juce::SharedResourcePointer). It turns them into one smix::MixSession, routes
    actions to the instance that owns a channel and runs the auto-mix loop.

    Note: hosts that sandbox each plugin in its own process (e.g. Bitwig's per-plugin
    mode) give every instance its own hub; see docs/ARCHITECTURE.md for the IPC plan.

    Message thread only.
*/
class SessionHub : public juce::ChangeBroadcaster,
                   private juce::Timer
{
public:
    SessionHub();
    ~SessionHub() override;

    void add (SoundManagerProcessor*);
    void remove (SoundManagerProcessor*);
    void ensureUniqueId (SoundManagerProcessor&);

    SoundManagerProcessor* findInstance (const std::string& id) const;
    const std::vector<SoundManagerProcessor*>& getInstances() const noexcept { return instances; }

    /** Rebuilds the session model from all instances. */
    void refresh();
    const smix::MixSession& getSession() const noexcept { return session; }

    /**
        Validated execution of actions on behalf of the instance rootId (its scope applies).
        Use this for user-initiated changes (chat, buttons): touched channels are protected from
        auto-mix tone corrections and freshly inserted plugins get starting settings.
    */
    std::vector<smix::ActionOutcome> apply (const std::vector<smix::MixAction>&, const std::string& rootId);

    smix::ChainPlan planChainFor (const std::string& channelId);

    /** Bus/master instances a channel can be routed to (for the routing menu). */
    std::vector<SoundManagerProcessor*> possibleParents (const SoundManagerProcessor&) const;

    /** True if an ancestor of this channel is auto-mixing it already. */
    bool isAutoMixedByAncestor (const std::string& channelId) const;

private:
    class Controller;
    void timerCallback() override;
    std::string resolveParent (const SoundManagerProcessor&) const;
    void runAutoMix();

    std::vector<SoundManagerProcessor*> instances;
    smix::MixSession session;
    std::unique_ptr<Controller> controller;
    int tickCount = 0;
};
