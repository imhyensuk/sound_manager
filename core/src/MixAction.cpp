#include "smix/MixAction.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace smix
{
namespace
{
const std::vector<std::pair<ActionType, const char*>> kActionNames {
    { ActionType::SetParam, "set_param" }, { ActionType::NudgeParam, "nudge_param" },
    { ActionType::SetGain, "set_gain" },   { ActionType::NudgeGain, "nudge_gain" },
    { ActionType::SetBypass, "set_bypass" }, { ActionType::SetChain, "set_chain" },
    { ActionType::MoveSlot, "move_slot" }
};

bool isNormalisedUnit (const std::string& unit)
{
    const auto u = toLowerAscii (unit);
    return u.empty() || u == "normalized" || u == "normalised" || u == "norm";
}

std::string formatValue (double v, const std::string& unit)
{
    std::ostringstream os;
    os.setf (std::ios::fixed);
    os.precision (std::abs (v) >= 100.0 ? 0 : (std::abs (v) >= 10.0 ? 1 : 2));
    if (unit == "dB" && v > 0)
        os << '+';
    os << v;
    if (! unit.empty())
        os << ' ' << unit;
    return os.str();
}
} // namespace

std::string toString (ActionType t)
{
    for (auto& [type, name] : kActionNames)
        if (type == t)
            return name;
    return "unknown";
}

nlohmann::json MixAction::toJson() const
{
    nlohmann::json j { { "type", toString (type) }, { "channel", channelId } };
    if (slot >= 0) j["slot"] = slot;
    if (! param.empty()) j["param"] = param;
    if (paramIndex >= 0) j["param_index"] = paramIndex;
    if (type == ActionType::SetGain && value) j["value_db"] = *value;
    else if (value) j["value"] = *value;
    if (type == ActionType::NudgeGain) j["delta_db"] = delta;
    else if (type == ActionType::NudgeParam) j["delta"] = delta;
    if (! unit.empty()) j["unit"] = unit;
    if (type == ActionType::SetBypass) j["bypass"] = bypass;
    if (type == ActionType::SetChain) j["plugins"] = plugins;
    if (type == ActionType::MoveSlot) j["to_slot"] = toSlot;
    if (! reason.empty()) j["reason"] = reason;
    return j;
}

ParseResult parseActions (const nlohmann::json& actions)
{
    ParseResult r;
    if (! actions.is_array())
    {
        r.errors.push_back ("actions must be an array");
        return r;
    }

    for (size_t i = 0; i < actions.size(); ++i)
    {
        const auto& a = actions[i];
        auto fail = [&r, i] (const std::string& msg) { r.errors.push_back ("action " + std::to_string (i) + ": " + msg); };

        if (! a.is_object()) { fail ("not an object"); continue; }

        MixAction m;
        const auto typeName = a.value ("type", std::string {});
        auto it = std::find_if (kActionNames.begin(), kActionNames.end(), [&] (auto& p) { return typeName == p.second; });
        if (it == kActionNames.end()) { fail ("unknown type '" + typeName + "'"); continue; }
        m.type = it->first;

        m.channelId = a.value ("channel", std::string {});
        if (m.channelId.empty()) { fail ("missing channel"); continue; }

        try
        {
            m.slot = a.value ("slot", -1);
            m.param = a.value ("param", std::string {});
            m.paramIndex = a.value ("param_index", -1);
            m.unit = a.value ("unit", std::string {});
            m.reason = a.value ("reason", std::string {});

            switch (m.type)
            {
                case ActionType::SetParam:
                    if (! a.contains ("value") || ! a["value"].is_number()) { fail ("set_param needs numeric value"); continue; }
                    m.value = a["value"].get<double>();
                    break;
                case ActionType::NudgeParam:
                    if (! a.contains ("delta") || ! a["delta"].is_number()) { fail ("nudge_param needs numeric delta"); continue; }
                    m.delta = a["delta"].get<double>();
                    break;
                case ActionType::SetGain:
                    if (! a.contains ("value_db") || ! a["value_db"].is_number()) { fail ("set_gain needs value_db"); continue; }
                    m.value = a["value_db"].get<double>();
                    m.unit = "dB";
                    break;
                case ActionType::NudgeGain:
                    if (! a.contains ("delta_db") || ! a["delta_db"].is_number()) { fail ("nudge_gain needs delta_db"); continue; }
                    m.delta = a["delta_db"].get<double>();
                    m.unit = "dB";
                    break;
                case ActionType::SetBypass:
                    m.bypass = a.value ("bypass", true);
                    break;
                case ActionType::SetChain:
                    if (! a.contains ("plugins") || ! a["plugins"].is_array()) { fail ("set_chain needs plugins[]"); continue; }
                    for (auto& p : a["plugins"])
                        if (p.is_string())
                            m.plugins.push_back (p.get<std::string>());
                    break;
                case ActionType::MoveSlot:
                    m.toSlot = a.value ("to_slot", -1);
                    break;
            }
        }
        catch (const nlohmann::json::exception& e)
        {
            fail (std::string ("malformed field: ") + e.what());
            continue;
        }

        const bool needsSlot = m.type == ActionType::SetParam || m.type == ActionType::NudgeParam
                               || m.type == ActionType::SetBypass || m.type == ActionType::MoveSlot;
        if (needsSlot && m.slot < 0) { fail ("missing slot"); continue; }
        if ((m.type == ActionType::SetParam || m.type == ActionType::NudgeParam) && m.param.empty() && m.paramIndex < 0)
        {
            fail ("missing param or param_index");
            continue;
        }

        r.actions.push_back (std::move (m));
    }
    return r;
}

nlohmann::json ActionOutcome::toJson() const
{
    return { { "action", action.toJson() }, { "ok", ok }, { "result", message } };
}

//==============================================================================
ActionExecutor::ActionExecutor (MixSession& s, const PluginCatalog& c, MixController& mc, ActionLimits l)
    : session (s), catalog (c), controller (mc), limits (l)
{
}

std::vector<ActionOutcome> ActionExecutor::executeAll (const std::vector<MixAction>& actions, const std::string& scopeRootId)
{
    std::vector<ActionOutcome> out;
    out.reserve (actions.size());
    for (auto& a : actions)
        out.push_back (execute (a, scopeRootId));
    return out;
}

ActionOutcome ActionExecutor::execute (const MixAction& a, const std::string& scopeRootId)
{
    auto* channel = session.find (a.channelId);
    if (channel == nullptr)
        return { a, false, "unknown channel '" + a.channelId + "'" };
    if (! session.inScope (scopeRootId, a.channelId))
        return { a, false, "channel '" + a.channelId + "' is outside this instance's scope" };

    switch (a.type)
    {
        case ActionType::SetParam:
        case ActionType::NudgeParam:
            return executeParam (a, *channel);

        case ActionType::SetGain:
        case ActionType::NudgeGain:
            return executeGain (a, *channel);

        case ActionType::SetBypass:
        {
            if (a.slot < 0 || a.slot >= static_cast<int> (channel->chain.size()))
                return { a, false, "slot out of range" };
            if (! controller.setBypass (a.channelId, a.slot, a.bypass))
                return { a, false, "host refused bypass change" };
            channel->chain[static_cast<size_t> (a.slot)].bypassed = a.bypass;
            return { a, true, channel->chain[static_cast<size_t> (a.slot)].pluginName + (a.bypass ? " bypassed" : " enabled") };
        }

        case ActionType::SetChain:
            return executeChain (a, *channel);

        case ActionType::MoveSlot:
        {
            const int n = static_cast<int> (channel->chain.size());
            if (a.slot < 0 || a.slot >= n || a.toSlot < 0 || a.toSlot >= n)
                return { a, false, "slot out of range" };
            if (! controller.moveSlot (a.channelId, a.slot, a.toSlot))
                return { a, false, "host refused reorder" };
            auto slot = std::move (channel->chain[static_cast<size_t> (a.slot)]);
            channel->chain.erase (channel->chain.begin() + a.slot);
            channel->chain.insert (channel->chain.begin() + a.toSlot, std::move (slot));
            return { a, true, "moved slot " + std::to_string (a.slot) + " -> " + std::to_string (a.toSlot) };
        }
    }
    return { a, false, "unhandled action" };
}

ActionOutcome ActionExecutor::executeParam (const MixAction& a, ChannelState& channel)
{
    if (a.slot < 0 || a.slot >= static_cast<int> (channel.chain.size()))
        return { a, false, "slot out of range" };

    auto& slot = channel.chain[static_cast<size_t> (a.slot)];
    const ParamInfo* found = a.paramIndex >= 0 ? slot.findParam (a.paramIndex) : slot.findParam (a.param);
    if (found == nullptr)
        return { a, false, "parameter '" + a.param + "' not found on " + slot.pluginName };

    auto* param = const_cast<ParamInfo*> (found);
    const float current = param->value;
    float target = current;

    const auto mapperIt = slot.mappers.find (param->index);
    const ValueMapper* mapper = (mapperIt != slot.mappers.end() && mapperIt->second.isUsable()) ? &mapperIt->second : nullptr;

    if (isNormalisedUnit (a.unit))
    {
        target = a.type == ActionType::SetParam ? static_cast<float> (*a.value) : current + static_cast<float> (a.delta);
    }
    else
    {
        if (mapper == nullptr)
            return { a, false, "no unit map for '" + param->name + "' (current text: " + param->valueText
                                   + "); use a normalized value instead" };

        double real = 0.0;
        if (a.type == ActionType::SetParam)
        {
            real = *a.value;
        }
        else
        {
            const auto now = mapper->toReal (current);
            if (! now)
                return { a, false, "cannot read current value of '" + param->name + "'" };
            const auto delta = canonicalise (a.delta, a.unit);
            real = *now + delta.value;
        }

        const auto norm = mapper->toNormalised (real, a.type == ActionType::SetParam ? a.unit : mapper->unit());
        if (! norm)
            return { a, false, "unit '" + a.unit + "' does not match parameter unit '" + mapper->unit() + "'" };
        target = *norm;
    }

    if (param->numSteps > 1)
    {
        const float steps = static_cast<float> (param->numSteps - 1);
        target = std::round (target * steps) / steps;
    }

    target = std::clamp (target, 0.0f, 1.0f);
    target = std::clamp (target, current - limits.maxNormalisedStep, current + limits.maxNormalisedStep);

    if (! controller.setParameter (channel.id, a.slot, param->index, target))
        return { a, false, "host refused parameter change" };

    std::string description = slot.pluginName + " / " + param->name + ": ";
    if (mapper != nullptr)
        description += formatValue (*mapper->toReal (current), mapper->unit()) + " -> "
                       + formatValue (*mapper->toReal (target), mapper->unit());
    else
        description += formatValue (current, "") + " -> " + formatValue (target, "") + " (normalized)";

    param->value = target;
    if (mapper != nullptr)
        param->valueText = formatValue (*mapper->toReal (target), mapper->unit());

    return { a, true, description };
}

ActionOutcome ActionExecutor::executeGain (const MixAction& a, ChannelState& channel)
{
    if (channel.gainLocked)
        return { a, false, "the user locked the level of '" + channel.name + "'" };

    const float current = channel.aiGainDb;
    float target = a.type == ActionType::SetGain ? static_cast<float> (*a.value) : current + static_cast<float> (a.delta);
    target = std::clamp (target, current - limits.maxGainStepDb, current + limits.maxGainStepDb);
    target = std::clamp (target, limits.minGainDb, limits.maxGainDb);

    if (! controller.setGain (channel.id, target))
        return { a, false, "host refused gain change" };

    channel.aiGainDb = target;
    return { a, true, (channel.name.empty() ? channel.id : channel.name) + " gain: " + formatValue (current, "dB") + " -> "
                          + formatValue (target, "dB") };
}

ActionOutcome ActionExecutor::executeChain (const MixAction& a, ChannelState& channel)
{
    if (a.plugins.size() > limits.maxChainLength)
        return { a, false, "chain too long" };

    std::vector<SlotState> newChain;
    for (auto& uid : a.plugins)
    {
        const auto* info = catalog.find (uid);
        if (info == nullptr)
            return { a, false, "unknown plugin '" + uid + "'" };
        if (! info->allowed)
            return { a, false, "plugin '" + info->name + "' is not in the user's allowed list" };

        // Keep the state of plugins that are already in the chain (re-ordering keeps settings).
        auto existing = std::find_if (channel.chain.begin(), channel.chain.end(), [&] (auto& s) { return s.pluginUid == uid; });
        if (existing != channel.chain.end())
        {
            newChain.push_back (std::move (*existing));
            channel.chain.erase (existing);
        }
        else
        {
            SlotState s;
            s.pluginUid = uid;
            s.pluginName = info->name;
            s.category = info->category;
            newChain.push_back (std::move (s));
        }
    }

    if (! controller.setChain (channel.id, a.plugins))
        return { a, false, "host could not load the chain" };

    channel.chain = std::move (newChain);

    std::string names;
    for (auto& s : channel.chain)
        names += (names.empty() ? "" : " -> ") + s.pluginName;
    return { a, true, "chain: " + (names.empty() ? std::string ("(empty)") : names) };
}

} // namespace smix
