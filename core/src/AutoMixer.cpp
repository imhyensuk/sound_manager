#include "smix/AutoMixer.h"

#include <algorithm>
#include <cmath>

namespace smix
{

void AutoMixer::reset()
{
    correctionCounts.clear();
    lastCorrection.clear();
}

void AutoMixer::holdChannel (const std::string& channelId, double now, double holdSeconds)
{
    heldUntil[channelId] = now + holdSeconds;

    // The user is already shaping these plugins: starting settings would overwrite their choices.
    const auto prefix = channelId + "|";
    for (auto it = pendingInit.begin(); it != pendingInit.end();)
        it = it->compare (0, prefix.size(), prefix) == 0 ? pendingInit.erase (it) : std::next (it);
}

bool AutoMixer::isHeld (const std::string& channelId, double now) const
{
    const auto it = heldUntil.find (channelId);
    return it != heldUntil.end() && now < it->second;
}

void AutoMixer::markForInitialisation (const std::string& channelId, const std::vector<std::string>& pluginUids)
{
    for (auto& uid : pluginUids)
        pendingInit.insert (channelId + "|" + uid);
}

AutoMixer::Tick AutoMixer::tick (const MixSession& session, const std::string& rootId, const PluginCatalog& catalog, double now)
{
    Tick t;

    for (auto& id : session.scopeOf (rootId))
    {
        const auto* c = session.find (id);
        if (c == nullptr)
            continue;
        const auto label = c->name.empty() ? c->id : c->name;
        const auto profile = PerceptualProfile::analyse (c->features, c->role);

        // 1) Chain planning for empty channels.
        if (options.planChains && c->chain.empty() && plannedChannels.count (id) == 0
            && c->features.valid && c->features.secondsAnalysed >= options.minAnalysisSeconds)
        {
            plannedChannels.insert (id);
            const auto plan = planner.plan (c->kind, c->role, profile, catalog);
            for (auto& n : plan.notes)
                t.log.push_back (label + ": " + n);
            if (! plan.slots.empty())
            {
                MixAction a;
                a.type = ActionType::SetChain;
                a.channelId = id;
                a.plugins = plan.pluginUids();
                a.reason = "AI chain plan for " + toString (c->role);
                t.actions.push_back (std::move (a));
                t.log.push_back (label + ": chain planned (" + std::to_string (plan.slots.size()) + " plugins)");
                continue;  // let the chain load before touching parameters
            }
        }

        // 2) Starting settings for plugins the AI just inserted, once their parameters are known.
        //    Plugins the user placed or already tuned are never reset.
        if (options.initialiseNewPlugins)
        {
            for (size_t s = 0; s < c->chain.size(); ++s)
            {
                const auto& slot = c->chain[s];
                const auto key = id + "|" + slot.pluginUid;
                if (pendingInit.count (key) == 0 || slot.params.empty() || slot.mappers.empty())
                    continue;
                pendingInit.erase (key);
                auto res = recipes.initialSettings (*c, static_cast<int> (s));
                t.actions.insert (t.actions.end(), res.actions.begin(), res.actions.end());
                if (! res.actions.empty())
                    t.log.push_back (label + ": initial settings for " + slot.pluginName);
            }
        }

        // 3) Gentle tonal corrections.
        if (! options.correctTone || ! c->features.valid || c->features.secondsAnalysed < options.minAnalysisSeconds
            || isHeld (id, now))
            continue;
        if (auto it = lastCorrection.find (id); it != lastCorrection.end() && now - it->second < options.secondsBetweenCorrections)
            continue;

        struct Rule { Descriptor d; bool whenPositive; SoundGoal goal; };
        static const Rule rules[] = {
            { Descriptor::Muddy,    true,  SoundGoal::LessMuddy },
            { Descriptor::Harsh,    true,  SoundGoal::LessHarsh },
            { Descriptor::Boomy,    true,  SoundGoal::LessBoomy },
            { Descriptor::Sibilant, true,  SoundGoal::LessSibilance },
            { Descriptor::Thin,     true,  SoundGoal::MoreBody },
            { Descriptor::Bright,   false, SoundGoal::Brighter },
            { Descriptor::Bright,   true,  SoundGoal::Darker },
        };

        for (auto& rule : rules)
        {
            const float score = profile.score (rule.d);
            const bool triggered = rule.whenPositive ? score > options.correctionThreshold : score < -options.correctionThreshold;
            if (! triggered)
                continue;
            if (rule.goal == SoundGoal::LessSibilance && ! isVocalRole (c->role))
                continue;

            const auto key = id + "|" + toString (rule.goal);
            if (correctionCounts[key] >= options.maxCorrectionsPerGoal)
                continue;

            const float amount = options.correctionAmount * std::min (1.0f, std::abs (score));
            auto res = recipes.actionsFor (*c, { rule.goal, amount });
            if (res.actions.empty())
                continue;

            ++correctionCounts[key];
            lastCorrection[id] = now;
            t.actions.insert (t.actions.end(), res.actions.begin(), res.actions.end());
            t.log.push_back (label + ": " + koreanLabel (rule.goal) + " (" + toString (rule.d) + " "
                             + std::to_string (static_cast<int> (std::lround (score * 100))) + "%)");
            break;  // one correction per channel per tick, then listen again
        }
    }

    // 4) Level balance between the channels of this bus/master.
    if (options.balanceLevels)
    {
        auto gains = gainBalancer.balance (session, rootId);
        t.actions.insert (t.actions.end(), gains.begin(), gains.end());
    }
    return t;
}

} // namespace smix
