#include "smix/ChainPlanner.h"

#include <functional>

namespace smix
{

std::vector<std::string> ChainPlan::pluginUids() const
{
    std::vector<std::string> uids;
    for (auto& s : slots)
        uids.push_back (s.pluginUid);
    return uids;
}

nlohmann::json ChainPlan::toJson() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (auto& s : slots)
        arr.push_back ({ { "plugin_uid", s.pluginUid }, { "plugin", s.pluginName },
                         { "category", toString (s.category) }, { "purpose", s.purpose } });
    return { { "slots", arr }, { "notes", notes } };
}

namespace
{
struct Step
{
    PluginCategory category;
    const char* purpose;
    std::function<bool()> needed;
    bool essential;  // report as a note if the user has no allowed plugin for it
};
} // namespace

ChainPlan ChainPlanner::plan (ChannelKind kind, InstrumentRole role, const PerceptualProfile& p, const PluginCatalog& catalog) const
{
    using C = PluginCategory;
    using D = Descriptor;

    const bool drums = isDrumRole (role);
    const bool vocal = isVocalRole (role);
    const bool shell = role == InstrumentRole::Kick || role == InstrumentRole::Snare || role == InstrumentRole::Toms;
    const bool dynamicSource = drums || vocal || role == InstrumentRole::Bass || role == InstrumentRole::AcousticGuitar
                               || role == InstrumentRole::Piano || role == InstrumentRole::Unknown;

    auto always = [] { return true; };
    auto score = [&p] (D d) { return p.score (d); };

    std::vector<Step> steps;

    if (kind == ChannelKind::Track)
    {
        steps = {
            { C::PitchCorrection, "pitch correction must see the raw performance", [&] { return role == InstrumentRole::LeadVocal; }, false },
            { C::Gate, "remove bleed between hits before anything adds gain", [&] { return shell; }, false },
            { C::EQ, "subtractive EQ: low cut and resonance/mud cleanup before dynamics", always, true },
            { C::DeEsser, "tame sibilance before compression exaggerates it", [&] { return vocal && score (D::Sibilant) > 0.2f; }, false },
            { C::Compressor, "control dynamics and shape the envelope", [&] { return dynamicSource || score (D::Squashed) < -0.3f; }, true },
            { C::TransientShaper, "restore attack/punch after compression", [&] { return drums && score (D::Punchy) < -0.2f; }, false },
            { C::Saturation, "add harmonics for density and audibility on small speakers",
              [&] { return score (D::Thin) > 0.3f || score (D::Bright) < -0.3f || role == InstrumentRole::Bass; }, false },
            { C::EQ, "additive/tonal EQ after colouration", [&] { return score (D::Bright) < -0.3f || score (D::Harsh) < -0.3f; }, false },
            { C::Modulation, "movement and width for static sources",
              [&] { return (role == InstrumentRole::Synth || role == InstrumentRole::Pad) && score (D::Wide) < -0.4f; }, false },
            { C::Delay, "depth", [&] { return vocal; }, false },
            { C::Reverb, "space, always last so it is not compressed", [&] { return vocal || role == InstrumentRole::Snare; }, false },
        };
    }
    else if (kind == ChannelKind::Bus)
    {
        steps = {
            { C::EQ, "bus cleanup / tonal balance of the group", always, true },
            { C::Compressor, "glue compression for the group", always, true },
            { C::TransientShaper, "group punch", [&] { return role == InstrumentRole::DrumBus && score (D::Punchy) < -0.2f; }, false },
            { C::Saturation, "cohesion through gentle harmonics", [&] { return role == InstrumentRole::DrumBus || score (D::Thin) > 0.3f; }, false },
            { C::StereoImager, "group width", [&] { return std::abs (score (D::Wide)) > 0.5f || score (D::PhaseIssue) > 0.3f; }, false },
        };
    }
    else
    {
        steps = {
            { C::EQ, "corrective master EQ (broad, gentle moves only)", always, true },
            { C::Compressor, "mix-bus glue (low ratio, slow attack)", always, true },
            { C::Saturation, "analogue-style density", [&] { return score (D::Thin) > 0.3f || score (D::Bright) < -0.4f; }, false },
            { C::StereoImager, "low-end mono and width control", [&] { return std::abs (score (D::Wide)) > 0.4f || score (D::PhaseIssue) > 0.2f; }, false },
            { C::EQ, "final tonal tilt", [&] { return std::abs (score (D::Bright)) > 0.5f; }, false },
            { C::Limiter, "loudness and true-peak ceiling, always last", always, true },
        };
    }

    ChainPlan result;
    int eqUsed = 0;
    for (auto& step : steps)
    {
        if (! step.needed())
            continue;

        const auto candidates = catalog.allowedIn (step.category);
        if (candidates.empty())
        {
            if (step.essential)
                result.notes.push_back ("no allowed " + toString (step.category) + " plugin - tick one in the plugin list ("
                                        + step.purpose + ")");
            continue;
        }

        // A second EQ slot prefers a different EQ (e.g. surgical first, musical second) if the user allowed one.
        const auto& chosen = (step.category == C::EQ && eqUsed > 0 && candidates.size() > 1) ? candidates[1] : candidates[0];
        if (step.category == C::EQ)
            ++eqUsed;

        result.slots.push_back ({ chosen.uid, chosen.name, chosen.category, step.purpose });
    }
    return result;
}

} // namespace smix
