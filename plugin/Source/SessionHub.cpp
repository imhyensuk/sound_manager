#include "SessionHub.h"

#include "Engine.h"
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
SessionHub::SessionHub() : controller (std::make_unique<Controller> (*this)), mixHistory (&enginePtr->memory(), 200)
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
        if (other != &p && other->getInstanceId() == p.getInstanceId())
        {
            p.regenerateInstanceId();  // duplicated track: give the copy a fresh identity
            return;
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
    if (setting == "master")
        return {};
    if (setting.isNotEmpty() && setting != "auto")
        return findInstance (setting.toStdString()) != nullptr ? setting.toStdString() : std::string {};

    const auto role = p.getEffectiveRole();
    std::vector<SoundManagerProcessor*> candidates;
    for (auto* bus : possibleParents (p))
    {
        if (bus->getEffectiveKind() != smix::ChannelKind::Bus)
            continue;
        const auto busName = bus->getDisplayName().toLowerCase();
        const bool drumMatch = smix::isDrumRole (role) && role != smix::InstrumentRole::DrumBus && bus->getEffectiveRole() == smix::InstrumentRole::DrumBus;
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

void SessionHub::recordSnapshot (const std::string& rootId, const std::string& label, const std::string& source)
{
    if (! engine().modules().isEnabled (smix::modules::ModuleId::History))
        return;
    refresh();
    mixHistory.record (session, session.scopeOf (rootId), label, source, juce::Time::currentTimeMillis() / 1000.0,
                       [this] (const std::string& channelId, int slot) {
                           auto* p = findInstance (channelId);
                           return p != nullptr ? p->captureSlotState (slot) : std::vector<std::uint8_t> {};
                       });
}

std::vector<smix::ActionOutcome> SessionHub::apply (const std::vector<smix::MixAction>& actions, const std::string& rootId,
                                                    const std::string& label, const std::string& source)
{
    refresh();
    auto* root = findInstance (rootId);
    if (root == nullptr || actions.empty())
        return {};

    // The first change of a scope also records where it started from, so it can be undone.
    bool knownStart = false;
    for (auto& s : mixHistory.snapshots())
        for (auto& c : s.channels)
            knownStart = knownStart || c.channelId == rootId;
    if (! knownStart)
        recordSnapshot (rootId, "시작 상태", "user");

    smix::ActionExecutor executor (session, root->getLibrary().catalog(), *controller);
    auto outcomes = executor.executeAll (actions, rootId);

    const double now = Engine::now();
    bool changed = false;
    for (auto& o : outcomes)
    {
        if (! o.ok)
            continue;
        changed = true;
        if (o.action.type == smix::ActionType::SetChain)
        {
            if (auto* owner = findInstance (o.action.channelId))
                owner->getAutoMixer().markForInitialisation (o.action.channelId, o.action.plugins);
        }
        else if (source != "auto" && (o.action.type == smix::ActionType::SetParam || o.action.type == smix::ActionType::NudgeParam))
        {
            for (auto* p : instances)
                p->getAutoMixer().holdChannel (o.action.channelId, now);
        }
    }

    // Parameter ramps take ~300 ms: record the result once they have settled.
    if (changed)
        juce::Timer::callAfterDelay (450, [this, rootId, label, source] {
            if (findInstance (rootId) != nullptr)
                recordSnapshot (rootId, label, source);
            sendChangeMessage();
        });
    sendChangeMessage();
    return outcomes;
}

bool SessionHub::restore (std::int64_t id, const std::string& rootId, juce::String* report)
{
    refresh();
    auto* root = findInstance (rootId);
    if (root == nullptr)
        return false;
    const auto plan = mixHistory.planRestore (id, session);
    smix::ActionExecutor executor (session, root->getLibrary().catalog(), *controller, smix::ActionLimits::forRestore());
    int changed = 0;
    for (auto& o : executor.executeAll (plan.actions, rootId))
        changed += o.ok ? 1 : 0;

    for (auto& reload : plan.reloads)
        if (auto* p = findInstance (reload.channelId))
        {
            std::vector<juce::MemoryBlock> states;
            for (auto& s : reload.states)
                states.emplace_back (s.data(), s.size());
            p->loadChain (reload.pluginUids, states, reload.bypassed);
            ++changed;
        }

    if (report != nullptr)
    {
        const auto* snap = mixHistory.find (id);
        *report = ko ("'") + juce::String (snap != nullptr ? snap->label : std::string ("?")) + ko ("' 시점으로 되돌렸어요 (")
                  + juce::String (changed) + ko ("개 항목)");
        for (auto& n : plan.notes)
            *report << "\n- " << juce::String (n);
    }
    juce::Timer::callAfterDelay (500, [this, rootId] {
        if (findInstance (rootId) != nullptr)
            recordSnapshot (rootId, "되돌림", "restore");
        sendChangeMessage();
    });
    return true;
}

bool SessionHub::undo (const std::string& rootId, juce::String* report)
{
    // The snapshot before the latest one that involves this scope.
    std::vector<std::int64_t> ids;
    for (auto& s : mixHistory.snapshots())
        for (auto& c : s.channels)
            if (session.inScope (rootId, c.channelId))
            {
                ids.push_back (s.id);
                break;
            }
    if (ids.size() < 2)
        return false;
    return restore (ids[ids.size() - 2], rootId, report);
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
        const smix::ChannelState* parent = nullptr;
        if (c->parentId.empty())
        {
            for (auto& other : session.channels())
                if (other.kind == smix::ChannelKind::Master && other.id != c->id)
                    parent = &other;
        }
        else
        {
            parent = session.find (c->parentId);
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
    const double now = Engine::now();
    const bool autoModule = engine().modules().isEnabled (smix::modules::ModuleId::AutoMix);
    for (auto* p : instances)
    {
        auto& mixer = p->getAutoMixer();
        const bool fullAutoMix = autoModule && p->isAutoMixEnabled() && ! isAutoMixedByAncestor (p->getInstanceId());
        if (fullAutoMix)
            engine().modules().touch (smix::modules::ModuleId::AutoMix, now);

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
            p->addLog ("auto", juce::String (line));

        // Auto-mix steps go into the history at most every 30 s per scope.
        auto& last = lastAutoSnapshot[p->getInstanceId()];
        if (now - last > 30.0)
        {
            last = now;
            recordSnapshot (p->getInstanceId(), "자동 믹스", "auto");
        }
    }
}

void SessionHub::timerCallback()
{
    refresh();
    ++tickCount;
    if (tickCount % 4 == 0)  // every 2 s: auto mix, then listen again
        runAutoMix();

    if (tickCount % 4 == 2 && engine().modules().isEnabled (smix::modules::ModuleId::Hibernation))
    {
        const bool aggressive = engine().library().getSetting ("hibernateSilent", "0") == "1";
        for (auto* p : instances)
            p->hibernationTick (Engine::now(), aggressive);
    }
    sendChangeMessage();
}
