#include "smix/MixSession.h"

#include <algorithm>

#include "smix/PerceptualProfile.h"

namespace smix
{

const ParamInfo* SlotState::findParam (int index) const
{
    for (auto& p : params)
        if (p.index == index)
            return &p;
    return nullptr;
}

const ParamInfo* SlotState::findParam (const std::string& name) const
{
    for (auto& p : params)
        if (p.name == name)
            return &p;
    const auto lower = toLowerAscii (name);
    for (auto& p : params)
        if (toLowerAscii (p.name) == lower)
            return &p;
    for (auto& p : params)
        if (toLowerAscii (p.name).find (lower) != std::string::npos)
            return &p;
    return nullptr;
}

const ParamInfo* SlotState::findByRole (ParamRole role, int band) const
{
    for (auto& p : params)
        if (p.semantic.role == role && (band < 0 || p.semantic.band == band))
            return &p;
    return nullptr;
}

std::vector<const ParamInfo*> SlotState::findAllByRole (ParamRole role) const
{
    std::vector<const ParamInfo*> out;
    for (auto& p : params)
        if (p.semantic.role == role)
            out.push_back (&p);
    return out;
}

nlohmann::json SlotState::toJson (bool includeParams, size_t maxParams) const
{
    nlohmann::json j { { "plugin_uid", pluginUid }, { "plugin", pluginName },
                       { "category", toString (category) }, { "bypassed", bypassed },
                       { "num_params", params.size() } };
    if (includeParams)
    {
        nlohmann::json arr = nlohmann::json::array();
        for (auto* p : mixRelevantParameters (params, maxParams))
            arr.push_back (p->toJson (false));
        j["params"] = arr;
    }
    return j;
}

nlohmann::json ChannelState::toJson (bool includeParams, size_t maxParamsPerSlot) const
{
    nlohmann::json chainJson = nlohmann::json::array();
    for (size_t i = 0; i < chain.size(); ++i)
    {
        auto s = chain[i].toJson (includeParams, maxParamsPerSlot);
        s["slot"] = i;
        chainJson.push_back (s);
    }

    const auto profile = PerceptualProfile::analyse (features, role);
    return { { "id", id }, { "name", name }, { "kind", toString (kind) }, { "role", toString (role) },
             { "parent", parentId.empty() ? "master" : parentId }, { "ai_gain_db", aiGainDb },
             { "gain_locked", gainLocked }, { "features", features.toJson() },
             { "perception", profile.toJson() }, { "chain", chainJson } };
}

void MixSession::upsert (ChannelState c)
{
    for (auto& existing : all)
    {
        if (existing.id == c.id)
        {
            existing = std::move (c);
            return;
        }
    }
    all.push_back (std::move (c));
}

bool MixSession::remove (const std::string& id)
{
    const auto before = all.size();
    all.erase (std::remove_if (all.begin(), all.end(), [&] (auto& c) { return c.id == id; }), all.end());
    return all.size() != before;
}

ChannelState* MixSession::find (const std::string& id)
{
    for (auto& c : all)
        if (c.id == id)
            return &c;
    return nullptr;
}

const ChannelState* MixSession::find (const std::string& id) const
{
    for (auto& c : all)
        if (c.id == id)
            return &c;
    return nullptr;
}

bool MixSession::isDescendant (const ChannelState& c, const std::string& ancestorId) const
{
    const ChannelState* current = &c;
    for (int depth = 0; depth < 32 && current != nullptr; ++depth)  // depth guard against routing cycles
    {
        if (current->parentId.empty())
        {
            const auto* ancestor = find (ancestorId);
            return ancestor != nullptr && ancestor->kind == ChannelKind::Master && current->id != ancestorId;
        }
        if (current->parentId == ancestorId)
            return true;
        current = find (current->parentId);
    }
    return false;
}

std::vector<std::string> MixSession::scopeOf (const std::string& rootId) const
{
    std::vector<std::string> out;
    const auto* root = find (rootId);
    if (root == nullptr)
        return out;

    out.push_back (rootId);
    if (root->kind == ChannelKind::Track)
        return out;

    for (auto& c : all)
    {
        if (c.id == rootId)
            continue;
        if (root->kind == ChannelKind::Master || isDescendant (c, rootId))
            out.push_back (c.id);
    }
    return out;
}

bool MixSession::inScope (const std::string& rootId, const std::string& channelId) const
{
    const auto scope = scopeOf (rootId);
    return std::find (scope.begin(), scope.end(), channelId) != scope.end();
}

std::vector<std::string> MixSession::resolveTarget (const std::string& target, const std::string& rootId) const
{
    std::vector<std::string> out;
    const auto scope = scopeOf (rootId);
    const auto t = toLowerAscii (target);

    auto add = [&out] (const std::string& id) {
        if (std::find (out.begin(), out.end(), id) == out.end())
            out.push_back (id);
    };

    for (auto& id : scope)
        if (id == target)
            return { id };

    for (auto& id : scope)
        if (auto* c = find (id); c != nullptr && ! c->name.empty() && toLowerAscii (c->name) == t)
            add (id);
    if (! out.empty())
        return out;

    // Role match: by explicit role name or by alias ("킥", "vox", ...).
    auto role = roleFromString (t);
    if (! role)
    {
        const auto guessed = guessRoleFromTrackName (target);
        if (guessed != InstrumentRole::Unknown)
            role = guessed;
    }

    if (role)
    {
        const bool wantsAllDrums = *role == InstrumentRole::DrumBus;
        for (auto& id : scope)
        {
            auto* c = find (id);
            if (c->role == *role)
                add (id);
        }
        // "drums" with no drum bus instance -> every drum channel.
        if (out.empty() && wantsAllDrums)
            for (auto& id : scope)
                if (isDrumRole (find (id)->role))
                    add (id);
        if (! out.empty())
            return out;
    }

    for (auto& id : scope)
        if (auto* c = find (id); c != nullptr && ! c->name.empty() && toLowerAscii (c->name).find (t) != std::string::npos)
            add (id);
    return out;
}

nlohmann::json MixSession::snapshotJson (const std::string& rootId, bool includeParams) const
{
    nlohmann::json channelsJson = nlohmann::json::array();
    for (auto& id : scopeOf (rootId))
        if (auto* c = find (id))
            channelsJson.push_back (c->toJson (includeParams));

    const auto* root = find (rootId);
    return { { "scope_root", rootId },
             { "scope_kind", root != nullptr ? toString (root->kind) : "unknown" },
             { "channels", channelsJson } };
}

} // namespace smix
