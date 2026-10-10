#pragma once

#include <juce_events/juce_events.h>
#include <juce_core/juce_core.h>
#include <smix/ChainPlanner.h>
#include <smix/MixAction.h>
#include <smix/MixHistory.h>
#include <smix/MixSession.h>

#include <set>

#include "Engine.h"
#include "SessionLink.h"

class SoundManagerProcessor;

/**
    Links every Sound Manager instance living in this DAW process (shared via
    juce::SharedResourcePointer): one MixSession, routing of actions to the instance that owns a
    channel, the mix history, the auto-mix loop and plugin hibernation.

    Hosts that sandbox each plugin in its own process give every instance its own hub;
    see docs/ARCHITECTURE.md. Message thread only.
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

    void refresh();
    const smix::MixSession& getSession() const noexcept { return session; }

    /**
        Validated execution on behalf of the instance rootId, recorded in the history as `label`.
        User-driven changes protect the touched channels from auto-mix tone corrections and give
        freshly inserted plugins starting settings.
    */
    std::vector<smix::ActionOutcome> apply (const std::vector<smix::MixAction>&, const std::string& rootId,
                                            const std::string& label = "변경", const std::string& source = "chat");

    smix::MixHistory& history() noexcept { return mixHistory; }

    /** Back to a recorded state (requirement 11). */
    bool restore (std::int64_t snapshotId, const std::string& rootId, juce::String* report = nullptr);
    /** Back to the state before the latest recorded change in this scope. */
    bool undo (const std::string& rootId, juce::String* report = nullptr);

    smix::ChainPlan planChainFor (const std::string& channelId);
    std::vector<SoundManagerProcessor*> possibleParents (const SoundManagerProcessor&) const;
    bool isAutoMixedByAncestor (const std::string& channelId) const;

    /** Instances in other processes of this computer (SessionLink). */
    bool isRemote (const std::string& channelId) const { return remoteIds.count (channelId) > 0; }
    juce::String linkStatus() const { return link != nullptr ? link->describe() : juce::String ("off"); }
    void restartLink();

private:
    class Controller;
    void timerCallback() override;
    std::string resolveParent (const SoundManagerProcessor&) const;
    void runAutoMix();
    void handleRemoteCall (const nlohmann::json&);
    void recordSnapshot (const std::string& rootId, const std::string& label, const std::string& source);
    Engine& engine() { return *enginePtr; }

    juce::SharedResourcePointer<Engine> enginePtr;
    std::vector<SoundManagerProcessor*> instances;
    smix::MixSession session;
    std::unique_ptr<Controller> controller;
    smix::MixHistory mixHistory;
    std::map<std::string, double> lastAutoSnapshot;
    int tickCount = 0;
    std::unique_ptr<SessionLink> link;
    std::vector<smix::ChannelState> remote;  // channels owned by other processes (this refresh)
    std::set<std::string> remoteIds;
};
