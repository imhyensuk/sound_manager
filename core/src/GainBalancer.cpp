#include "smix/GainBalancer.h"

#include <algorithm>
#include <cmath>

#include "smix/style/GenreProfile.h"

namespace smix
{

float GainBalancer::targetOffsetLu (InstrumentRole role)
{
    // Learned from the user's finished mixes of the active genre, when available.
    if (const auto genre = style::activeGenre())
        if (auto it = genre->balanceLu.find (role); it != genre->balanceLu.end())
            return it->second;

    switch (role)
    {
        case InstrumentRole::LeadVocal:      return 0.0f;
        case InstrumentRole::Kick:           return -1.0f;
        case InstrumentRole::Snare:          return -2.0f;
        case InstrumentRole::Bass:           return -2.5f;
        case InstrumentRole::DrumBus:        return 1.0f;
        case InstrumentRole::Toms:           return -6.0f;
        case InstrumentRole::Overheads:      return -7.0f;
        case InstrumentRole::HiHat:          return -11.0f;
        case InstrumentRole::Percussion:     return -10.0f;
        case InstrumentRole::BackingVocal:   return -6.0f;
        case InstrumentRole::ElectricGuitar: return -5.0f;
        case InstrumentRole::AcousticGuitar: return -5.0f;
        case InstrumentRole::Piano:          return -5.0f;
        case InstrumentRole::Keys:           return -7.0f;
        case InstrumentRole::Synth:          return -6.0f;
        case InstrumentRole::Pad:            return -10.0f;
        case InstrumentRole::Strings:        return -8.0f;
        case InstrumentRole::Brass:          return -6.0f;
        case InstrumentRole::FX:             return -12.0f;
        case InstrumentRole::MixBus:         return -2.0f;
        default:                             return -6.0f;
    }
}

void GainBalancer::setUserOffset (InstrumentRole role, float lu)
{
    for (auto& [r, v] : userOffsets)
        if (r == role) { v = lu; return; }
    userOffsets.emplace_back (role, lu);
}

float GainBalancer::userOffset (InstrumentRole role) const
{
    for (auto& [r, v] : userOffsets)
        if (r == role)
            return v;
    return 0.0f;
}

std::vector<MixAction> GainBalancer::balance (const MixSession& session, const std::string& rootId) const
{
    struct Entry { const ChannelState* channel; float preGainLufs; float target; };
    std::vector<Entry> entries;

    for (auto& id : session.scopeOf (rootId))
    {
        if (id == rootId)
            continue;
        const auto* c = session.find (id);
        if (c == nullptr || ! c->features.valid || c->kind == ChannelKind::Master)
            continue;
        // The analyser runs after the AI gain stage, so remove it to get the source loudness.
        const float preGain = c->features.shortTermLufs - c->aiGainDb;
        if (c->features.shortTermLufs < options.silenceLufs)
            continue;
        entries.push_back ({ c, preGain, targetOffsetLu (c->role) + userOffset (c->role) });
    }

    if (entries.size() < 2)
        return {};

    // Anchor: the lead vocal if present, otherwise the kick, otherwise the loudest-target channel.
    auto anchorIt = std::find_if (entries.begin(), entries.end(), [] (auto& e) { return e.channel->role == InstrumentRole::LeadVocal; });
    if (anchorIt == entries.end())
        anchorIt = std::find_if (entries.begin(), entries.end(), [] (auto& e) { return e.channel->role == InstrumentRole::Kick; });
    if (anchorIt == entries.end())
        anchorIt = std::max_element (entries.begin(), entries.end(), [] (auto& a, auto& b) { return a.target < b.target; });

    // Anchor reference loudness (what the anchor would be at its current gain).
    const float anchorLufs = anchorIt->preGainLufs + anchorIt->channel->aiGainDb - anchorIt->target;

    // Desired gains, then remove their mean so the overall bus level does not drift.
    std::vector<float> desired;
    for (auto& e : entries)
        desired.push_back (anchorLufs + e.target - e.preGainLufs);

    float mean = 0.0f;
    for (auto d : desired) mean += d;
    mean /= static_cast<float> (desired.size());

    std::vector<MixAction> actions;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto* c = entries[i].channel;
        if (c->gainLocked)
            continue;

        const float want = std::clamp (desired[i] - mean, options.minGainDb, options.maxGainDb);
        const float error = want - c->aiGainDb;
        if (std::abs (error) < options.deadbandDb)
            continue;

        MixAction a;
        a.type = ActionType::NudgeGain;
        a.channelId = c->id;
        a.delta = std::clamp (error, -options.maxStepDb, options.maxStepDb);
        a.unit = "dB";
        a.reason = "balance: " + toString (c->role) + " target " + std::to_string (static_cast<int> (std::lround (entries[i].target)))
                   + " LU vs anchor";
        actions.push_back (std::move (a));
    }
    return actions;
}

} // namespace smix
