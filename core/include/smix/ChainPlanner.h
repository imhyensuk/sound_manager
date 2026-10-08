#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/PerceptualProfile.h"
#include "smix/PluginCatalog.h"

namespace smix
{

struct PlannedSlot
{
    std::string pluginUid;
    std::string pluginName;
    PluginCategory category = PluginCategory::Unknown;
    std::string purpose;  // why this processor is here, in this position
};

struct ChainPlan
{
    std::vector<PlannedSlot> slots;
    std::vector<std::string> notes;  // e.g. missing categories the user could allow

    std::vector<std::string> pluginUids() const;
    nlohmann::json toJson() const;
};

/**
    Deterministic, explainable chain planner (requirement 4).

    It follows established mixing signal-flow practice per channel kind and role and only
    includes a processor when the analysis says it is needed. Only plugins the user allowed
    are used. The language model can still override the plan via a set_chain action.
*/
class ChainPlanner
{
public:
    ChainPlan plan (ChannelKind, InstrumentRole, const PerceptualProfile&, const PluginCatalog&) const;
};

} // namespace smix
