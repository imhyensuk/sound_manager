#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/AudioFeatures.h"
#include "smix/ParamSemantics.h"
#include "smix/Types.h"
#include "smix/ValueMapper.h"

namespace smix
{

/** One hosted plugin inside a channel's chain. */
struct SlotState
{
    std::string pluginUid;
    std::string pluginName;
    PluginCategory category = PluginCategory::Unknown;
    bool bypassed = false;
    bool protectedSlot = false;  // the user forbids the AI to touch this plugin
    std::vector<ParamInfo> params;
    std::map<int, ValueMapper> mappers;  // param index -> learned value curve (filled by the host layer)

    const ParamInfo* findParam (int index) const;
    const ParamInfo* findParam (const std::string& name) const;  // exact, then case-insensitive, then substring
    const ParamInfo* findByRole (ParamRole role, int band = -1) const;
    std::vector<const ParamInfo*> findAllByRole (ParamRole role) const;

    nlohmann::json toJson (bool includeParams, size_t maxParams) const;
};

/** One plugin instance = one mixer channel (track, bus or master). */
struct ChannelState
{
    std::string id;          // unique per plugin instance
    std::string name;        // DAW track name (if the host reports it) or user label
    ChannelKind kind = ChannelKind::Track;
    InstrumentRole role = InstrumentRole::Unknown;
    std::string parentId;    // bus/master this channel feeds; empty = master
    float aiGainDb = 0.0f;   // gain stage controlled by the AI (balance)
    bool gainLocked = false; // user forbids the AI to touch the level
    bool protectedChannel = false;  // the user forbids the AI to change anything on this channel
    std::string style;       // chosen mixing style for this channel ("" = not asked yet)
    AudioFeatures features;
    std::vector<SlotState> chain;

    nlohmann::json toJson (bool includeParams, size_t maxParamsPerSlot = 24) const;
};

/**
    All linked plugin instances of one DAW project.

    Scope rules (requirement 2):
      - Track instance  : only itself.
      - Bus instance    : itself + every channel whose parent chain reaches it.
      - Master instance : every channel in the session.
*/
class MixSession
{
public:
    void upsert (ChannelState);
    bool remove (const std::string& id);

    ChannelState* find (const std::string& id);
    const ChannelState* find (const std::string& id) const;

    const std::vector<ChannelState>& channels() const noexcept { return all; }

    std::vector<std::string> scopeOf (const std::string& rootId) const;
    bool inScope (const std::string& rootId, const std::string& channelId) const;

    /**
        Resolves a free-form target ("kick", "킥", "drums", "Lead Vox", an id...) to channel ids
        inside the scope of rootId.
    */
    std::vector<std::string> resolveTarget (const std::string& target, const std::string& rootId) const;

    /** Snapshot given to the AI: channels in scope with features, perception and chains. */
    nlohmann::json snapshotJson (const std::string& rootId, bool includeParams) const;

private:
    bool isDescendant (const ChannelState& c, const std::string& ancestorId) const;
    std::vector<ChannelState> all;
};

} // namespace smix
