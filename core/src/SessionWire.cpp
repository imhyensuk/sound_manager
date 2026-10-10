#include "smix/SessionWire.h"

namespace smix::wire
{

std::string slotSignature (const SlotState& s)
{
    return s.pluginUid + "#" + std::to_string (s.params.size()) + "#" + std::to_string (s.mappers.size());
}

nlohmann::json channelToJson (const ChannelState& c, bool withMappers, size_t maxParams)
{
    nlohmann::json chain = nlohmann::json::array();
    for (auto& s : c.chain)
    {
        nlohmann::json params = nlohmann::json::array(), mappers = nlohmann::json::object();
        for (auto* p : mixRelevantParameters (s.params, maxParams))
        {
            params.push_back (p->toJson (true));
            if (withMappers)
                if (auto it = s.mappers.find (p->index); it != s.mappers.end() && it->second.isUsable())
                    mappers[std::to_string (p->index)] = it->second.toJson();
        }
        nlohmann::json sj { { "uid", s.pluginUid }, { "name", s.pluginName }, { "category", toString (s.category) },
                            { "bypassed", s.bypassed }, { "protected", s.protectedSlot }, { "params", params },
                            { "sig", slotSignature (s) } };
        if (withMappers)
            sj["mappers"] = mappers;
        chain.push_back (sj);
    }
    return { { "id", c.id }, { "name", c.name }, { "kind", toString (c.kind) }, { "role", toString (c.role) },
             { "parent", c.parentId }, { "ai_gain_db", c.aiGainDb }, { "gain_locked", c.gainLocked },
             { "protected", c.protectedChannel }, { "style", c.style }, { "features", c.features.toJson() }, { "chain", chain } };
}

ChannelState channelFromJson (const nlohmann::json& j)
{
    ChannelState c;
    c.id = j.value ("id", std::string {});
    c.name = j.value ("name", std::string {});
    c.kind = channelKindFromString (j.value ("kind", std::string {})).value_or (ChannelKind::Track);
    c.role = roleFromString (j.value ("role", std::string {})).value_or (InstrumentRole::Unknown);
    c.parentId = j.value ("parent", std::string {});
    c.aiGainDb = j.value ("ai_gain_db", 0.0f);
    c.gainLocked = j.value ("gain_locked", false);
    c.protectedChannel = j.value ("protected", false);
    c.style = j.value ("style", std::string {});
    if (j.contains ("features"))
        c.features = AudioFeatures::fromJson (j["features"]);
    if (j.contains ("chain") && j["chain"].is_array())
    {
        const auto chain = j["chain"];
        for (auto& sj : chain)
        {
            SlotState s;
            s.pluginUid = sj.value ("uid", std::string {});
            s.pluginName = sj.value ("name", std::string {});
            s.category = categoryFromString (sj.value ("category", std::string {})).value_or (PluginCategory::Unknown);
            s.bypassed = sj.value ("bypassed", false);
            s.protectedSlot = sj.value ("protected", false);
            if (sj.contains ("params") && sj["params"].is_array())
                for (auto& pj : sj["params"])
                {
                    auto p = ParamInfo::fromJson (pj);
                    p.semantic = classifyParameter (p.name, s.category);
                    s.params.push_back (std::move (p));
                }
            if (sj.contains ("mappers") && sj["mappers"].is_object())
            {
                const auto mappers = sj["mappers"];
                for (auto it = mappers.begin(); it != mappers.end(); ++it)
                    s.mappers[std::stoi (it.key())] = ValueMapper::fromJson (it.value());
            }
            c.chain.push_back (std::move (s));
        }
    }
    return c;
}

} // namespace smix::wire
