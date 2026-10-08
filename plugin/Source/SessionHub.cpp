#include "SessionHub.h"

#include "PluginProcessor.h"
#include "Text.h"

//==============================================================================
class SessionHub::Controller : public smix::MixController
{
public:
    explicit Controller (SessionHub& h) : hub (h) {}

    bool setParameter (const std::string& id, int slot, int paramIndex, float v) override
    {
        auto* p = hub.findInstance (id);
        return p != nullptr && p->setHostedParameter (slot, paramIndex, v);
    }
    bool setGain (const std::string& id, float db) override
    {
        auto* p = hub.findInstance (id);
        return p != nullptr && p->setAiGainDb (db);
    }
    bool setBypass (const std::string& id, int slot, bool b) override
    {
        auto* p = hub.findInstance (id);
        return p != nullptr && p->setSlotBypass (slot, b);
    }
    bool setChain (const std::string& id, const std::vector<std::string>& uids) override
    {
        auto* p = hub.findInstance (id);
        if (p == nullptr)
            return false;
        p->loadChain (uids);
        return true;
    }
    bool moveSlot (const std::string& id, int from, int to) override
    {
        auto* p = hub.findInstance (id);
        return p != nullptr && p->moveSlot (from, to);
    }

private:
    SessionHub& hub;
};

//==============================================================================
SessionHub::SessionHub() : controller (std::make_unique<Controller> (*this))
{
    startTimer (500);
}

SessionHub::~SessionHub()
{
    stopTimer();
}

void SessionHub::add (SoundManagerProcessor* p)
{
    instances.push_back (p);
    ensureUniqueId (*p);
}

void SessionHub::remove (SoundManagerProcessor* p)
{
    instances.erase (std::remove (instances.begin(), instances.end(), p), instances.end());
    session.remove (p->getInstanceId());
    sendChangeMessage();
}

void SessionHub::ensureUniqueId (SoundManagerProcessor& p)
{
    for (auto* other : instances)
    {
        if (other != &p && other->getInstanceId() == p.getInstanceId())
        {
            // Duplicated track: give the copy a fresh identity.
            p.regenerateInstanceId();
            return;
        }
    }
}

SoundManagerProcessor* SessionHub::findInstance (const std::string& id) const
{
    for (auto* p : instances)
        if (p->getInstanceId() == id)
            return p;
    return nullptr;
}

std::vector<SoundManagerProcessor*> SessionHub::possibleParents (const SoundManagerProcessor& child) const
{
    std::vector<SoundManagerProcessor*> out;
    for (auto* p : instances)
        if (p != &child && p->getEffectiveKind() != smix::ChannelKind::Track)
            out.push_back (p);
    return out;
}

std::string SessionHub::resolveParent (const SoundManagerProcessor& p) const
{
    if (p.getEffectiveKind() == smix::ChannelKind::Master)
        return {};

    const auto setting = p.getParentSetting();
    if (setting.isNotEmpty() && setting != "auto")
        return findInstance (setting.toStdString()) != nullptr ? setting.toStdString() : std::string {};

    // Automatic routing guess: drums -> the (single) drum bus, vocals -> a vocal bus, otherwise master.
    const auto role = p.getEffectiveRole();
    std::vector<SoundManagerProcessor*> candidates;
    for (auto* bus : possibleParents (p))
    {
        if (bus->getEffectiveKind() != smix::ChannelKind::Bus)
            continue;
        const auto busRole = bus->getEffectiveRole();
        const auto busName = bus->getDisplayName().toLowerCase();
        const bool drumMatch = smix::isDrumRole (role) && role != smix::InstrumentRole::DrumBus && busRole == smix::InstrumentRole::DrumBus;
        const bool vocalMatch = smix::isVocalRole (role) && (busName.contains ("vox") || busName.contains ("vocal") || busName.contains (ko ("보컬")));
        if (drumMatch || vocalMatch)
            candidates.push_back (bus);
    }
    return candidates.size() == 1 ? candidates.front()->getInstanceId() : std::string {};
}

void SessionHub::refresh()
{
    smix::MixSession fresh;
    for (auto* p : instances)
    {
        auto c = p->buildChannelState (true);
        c.parentId = resolveParent (*p);
        fresh.upsert (std::move (c));
    }
    session = std::move (fresh);
}

std::vector<smix::ActionOutcome> SessionHub::apply (const std::vector<smix::MixAction>& actions, const std::string& rootId)
{
    refresh();
    auto* root = findInstance (rootId);
    if (root == nullptr)
        return {};

    smix::ActionExecutor executor (session, root->getLibrary().catalog(), *controller);
    auto outcomes = executor.executeAll (actions, rootId);

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    for (auto& o : outcomes)
    {
        if (! o.ok)
            continue;
        if (o.action.type == smix::ActionType::SetChain)
        {
            // Only the channel's own instance initialises it, so nothing is applied twice.
            if (auto* owner = findInstance (o.action.channelId))
                owner->getAutoMixer().markForInitialisation (o.action.channelId, o.action.plugins);
        }
        else if (o.action.type == smix::ActionType::SetParam || o.action.type == smix::ActionType::NudgeParam)
        {
            for (auto* p : instances)
                p->getAutoMixer().holdChannel (o.action.channelId, now);
        }
    }

    sendChangeMessage();
    return outcomes;
}

smix::ChainPlan SessionHub::planChainFor (const std::string& channelId)
{
    refresh();
    const auto* c = session.find (channelId);
    auto* p = findInstance (channelId);
    if (c == nullptr || p == nullptr)
        return {};
    const auto profile = smix::PerceptualProfile::analyse (c->features, c->role);
    return smix::ChainPlanner().plan (c->kind, c->role, profile, p->getLibrary().catalog());
}

bool SessionHub::isAutoMixedByAncestor (const std::string& channelId) const
{
    const auto* c = session.find (channelId);
    for (int depth = 0; c != nullptr && depth < 32; ++depth)
    {
        const std::string parentId = c->parentId;
        const smix::ChannelState* parent = nullptr;
        if (parentId.empty())
        {
            for (auto& other : session.channels())
                if (other.kind == smix::ChannelKind::Master && other.id != c->id)
                    parent = &other;
        }
        else
        {
            parent = session.find (parentId);
        }
        if (parent == nullptr)
            return false;
        if (auto* p = findInstance (parent->id); p != nullptr && p->isAutoMixEnabled())
            return true;
        c = parent;
    }
    return false;
}

void SessionHub::runAutoMix()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    for (auto* p : instances)
    {
        auto& mixer = p->getAutoMixer();
        const bool fullAutoMix = p->isAutoMixEnabled() && ! isAutoMixedByAncestor (p->getInstanceId());

        // Without auto mix, only give freshly inserted plugins their starting settings.
        const auto saved = mixer.getOptions();
        if (! fullAutoMix)
        {
            auto& o = mixer.getOptions();
            o.planChains = o.correctTone = o.balanceLevels = false;
        }
        auto tick = mixer.tick (session, p->getInstanceId(), p->getLibrary().catalog(), now);
        mixer.getOptions() = saved;

        if (tick.actions.empty())
            continue;

        smix::ActionExecutor executor (session, p->getLibrary().catalog(), *controller);
        const auto outcomes = executor.executeAll (tick.actions, p->getInstanceId());

        for (auto& o : outcomes)
            if (o.ok && o.action.type == smix::ActionType::SetChain)
                if (auto* owner = findInstance (o.action.channelId))
                    owner->getAutoMixer().markForInitialisation (o.action.channelId, o.action.plugins);

        for (auto& line : tick.log)
            p->addLog ("auto", line);
        for (auto& o : outcomes)
            if (! o.ok)
                p->addLog ("auto", ko ("건너뜀: ") + juce::String (o.message));
    }
}

void SessionHub::timerCallback()
{
    refresh();
    if (++tickCount % 4 == 0)  // auto mix every 2 s, then listen again
        runAutoMix();
    sendChangeMessage();
}
