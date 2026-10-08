#include "smix/PerceptualProfile.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "smix/style/GenreProfile.h"

namespace smix
{
namespace
{
using Curve = std::array<float, SpectrumBands::kNumBands>;

//                         sub    bass  lowmid  mud    mid  upmid  pres   bite   sib    air
constexpr Curve kFullMix { -11.f, -7.f, -7.5f, -8.5f, -9.5f, -11.f, -13.f, -16.f, -18.f, -23.f };

Curve add (Curve c, std::initializer_list<float> offsets)
{
    size_t i = 0;
    for (auto o : offsets)
        c[i++] += o;
    return c;
}

float clampScore (float v) { return std::clamp (v, -2.0f, 2.0f); }
} // namespace

std::string toString (Descriptor d)
{
    switch (d)
    {
        case Descriptor::Boomy:      return "boomy";
        case Descriptor::Muddy:      return "muddy";
        case Descriptor::Boxy:       return "boxy";
        case Descriptor::Harsh:      return "harsh";
        case Descriptor::Sibilant:   return "sibilant";
        case Descriptor::Bright:     return "bright";
        case Descriptor::Thin:       return "thin";
        case Descriptor::Punchy:     return "punchy";
        case Descriptor::Squashed:   return "squashed";
        case Descriptor::Wide:       return "wide";
        case Descriptor::PhaseIssue: return "phase_issue";
        default:                     return "unknown";
    }
}

std::string koreanLabel (Descriptor d, bool positive)
{
    switch (d)
    {
        case Descriptor::Boomy:      return positive ? "저음이 부밍함" : "저음이 부족함";
        case Descriptor::Muddy:      return positive ? "탁함(머디)" : "저중역이 비어 있음";
        case Descriptor::Boxy:       return positive ? "박스톤/답답함" : "중역이 빠짐";
        case Descriptor::Harsh:      return positive ? "쏘고 거칠음" : "존재감이 약함";
        case Descriptor::Sibilant:   return positive ? "치찰음이 강함" : "치찰음이 약함";
        case Descriptor::Bright:     return positive ? "너무 밝음" : "어둡고 먹먹함";
        case Descriptor::Thin:       return positive ? "얇음(바디 부족)" : "두꺼움";
        case Descriptor::Punchy:     return positive ? "펀치감 있음" : "어택이 약하고 흐림";
        case Descriptor::Squashed:   return positive ? "과하게 눌림" : "다이내믹이 큼";
        case Descriptor::Wide:       return positive ? "스테레오가 넓음" : "좁음/모노에 가까움";
        case Descriptor::PhaseIssue: return positive ? "위상 문제" : "위상 양호";
        default:                     return "?";
    }
}

Curve referenceCurve (InstrumentRole role)
{
    // A genre profile learned from the user's own mixes replaces the rule-of-thumb curves.
    if (const auto genre = style::activeGenre())
        if (auto it = genre->curves.find (role); it != genre->curves.end())
            return it->second;

    switch (role)
    {
        //                                     sub   bass lowmid mud   mid  upmid pres  bite  sib   air
        case InstrumentRole::Kick:     return add (kFullMix, { +6.f, +5.f, +1.f, -3.f, -4.f, -3.f, -1.f, -1.f, -2.f, -2.f });
        case InstrumentRole::Snare:    return add (kFullMix, { -8.f, -2.f, +2.f, +1.f, +1.f, +1.f, +1.f, +1.f, +1.f, 0.f });
        case InstrumentRole::HiHat:    return add (kFullMix, { -20.f, -18.f, -14.f, -10.f, -5.f, 0.f, +4.f, +7.f, +9.f, +10.f });
        case InstrumentRole::Toms:     return add (kFullMix, { 0.f, +4.f, +3.f, 0.f, -2.f, -3.f, -2.f, -3.f, -4.f, -5.f });
        case InstrumentRole::Overheads:return add (kFullMix, { -10.f, -6.f, -3.f, -1.f, 0.f, +1.f, +3.f, +4.f, +5.f, +5.f });
        case InstrumentRole::Bass:     return add (kFullMix, { +4.f, +5.f, +2.f, -1.f, -4.f, -6.f, -8.f, -10.f, -12.f, -14.f });
        case InstrumentRole::LeadVocal:
        case InstrumentRole::BackingVocal:
                                       return add (kFullMix, { -14.f, -6.f, -1.f, +1.f, +2.f, +3.f, +3.f, +2.f, +2.f, +2.f });
        case InstrumentRole::AcousticGuitar:
        case InstrumentRole::ElectricGuitar:
                                       return add (kFullMix, { -14.f, -5.f, 0.f, +1.f, +2.f, +2.f, +2.f, +1.f, 0.f, -1.f });
        case InstrumentRole::Piano: case InstrumentRole::Keys: case InstrumentRole::Synth:
        case InstrumentRole::Pad: case InstrumentRole::Strings: case InstrumentRole::Brass:
                                       return add (kFullMix, { -10.f, -3.f, 0.f, +1.f, +1.f, +1.f, 0.f, 0.f, 0.f, 0.f });
        default:                       return kFullMix;
    }
}

PerceptualProfile PerceptualProfile::analyse (const AudioFeatures& f, InstrumentRole role)
{
    PerceptualProfile p;
    if (! f.valid)
        return p;

    const auto ref = referenceCurve (role);
    for (size_t b = 0; b < ref.size(); ++b)
        p.deviation[b] = f.bandLevelDb[b] - ref[b];

    const auto& d = p.deviation;
    auto set = [&p] (Descriptor desc, float v) { p.scores[static_cast<size_t> (desc)] = clampScore (v); };

    // Roughly: 6 dB deviation from the reference ~ score 1.
    set (Descriptor::Boomy,    (0.6f * d[0] + 0.4f * d[1]) / 6.0f);
    set (Descriptor::Muddy,    (0.4f * d[2] + 0.6f * d[3]) / 5.0f);
    set (Descriptor::Boxy,     d[4] / 5.0f);
    set (Descriptor::Harsh,    (0.3f * d[5] + 0.5f * d[6] + 0.2f * d[7]) / 5.0f);
    set (Descriptor::Sibilant, d[8] / 5.0f);
    set (Descriptor::Bright,   (0.4f * d[8] + 0.6f * d[9]) / 6.0f);
    set (Descriptor::Thin,     -(0.5f * d[1] + 0.5f * d[2]) / 6.0f);

    // Punch: strong transients plus healthy crest factor. Sustained material is never "punchy".
    const float crestTerm = (f.crestDb - 12.0f) / 6.0f;
    const float transientTerm = (f.transientStrength - 0.35f) * 3.0f;
    const bool percussive = isDrumRole (role) || role == InstrumentRole::Bass || role == InstrumentRole::Master
                            || role == InstrumentRole::MixBus || role == InstrumentRole::Unknown;
    set (Descriptor::Punchy, percussive ? 0.5f * crestTerm + 0.5f * transientTerm : 0.0f);

    const float lraTerm = f.loudnessRangeLu > 0.0f ? (4.0f - f.loudnessRangeLu) / 4.0f : 0.0f;
    set (Descriptor::Squashed, 0.6f * ((9.0f - f.crestDb) / 4.0f) + 0.4f * lraTerm);

    set (Descriptor::Wide, (f.stereoWidth - 0.25f) / 0.25f);
    set (Descriptor::PhaseIssue, f.stereoCorrelation < 0.2f ? (0.2f - f.stereoCorrelation) / 0.4f : 0.0f);
    return p;
}

std::vector<DescriptorScore> PerceptualProfile::salient (float threshold) const
{
    std::vector<DescriptorScore> out;
    for (size_t i = 0; i < scores.size(); ++i)
        if (std::abs (scores[i]) >= threshold)
            out.push_back ({ static_cast<Descriptor> (i), scores[i] });
    std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return std::abs (a.score) > std::abs (b.score); });
    return out;
}

std::string PerceptualProfile::summary() const
{
    std::ostringstream os;
    os.precision (2);
    bool first = true;
    for (auto& s : salient())
    {
        os << (first ? "" : ", ") << koreanLabel (s.descriptor, s.score > 0) << " (" << toString (s.descriptor)
           << (s.score > 0 ? " +" : " ") << s.score << ")";
        first = false;
    }
    return first ? "특이사항 없음 (balanced)" : os.str();
}

nlohmann::json PerceptualProfile::toJson() const
{
    nlohmann::json scoresJson = nlohmann::json::object();
    for (size_t i = 0; i < scores.size(); ++i)
        scoresJson[toString (static_cast<Descriptor> (i))] = std::round (scores[i] * 100.0f) / 100.0f;

    nlohmann::json dev = nlohmann::json::object();
    for (int b = 0; b < SpectrumBands::kNumBands; ++b)
        dev[SpectrumBands::kNames[static_cast<size_t> (b)]] = std::round (deviation[static_cast<size_t> (b)] * 10.0f) / 10.0f;

    return { { "descriptor_scores", scoresJson }, { "band_deviation_from_reference_db", dev }, { "summary", summary() } };
}

} // namespace smix
