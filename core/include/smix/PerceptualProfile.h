#pragma once

#include <array>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/AudioFeatures.h"
#include "smix/Types.h"

namespace smix
{

/**
    Perceptual descriptors a mixing engineer would use. Scores are signed:
    > 0 means "too much of it", < 0 means "too little", |score| ~ 1 is clearly audible.
*/
enum class Descriptor
{
    Boomy,      // + excess sub/bass
    Muddy,      // + excess low-mid build-up
    Boxy,       // + excess 500 Hz - 1 kHz
    Harsh,      // + excess 2 - 6 kHz
    Sibilant,   // + excess 6 - 10 kHz
    Bright,     // + excess air / high end, - dull
    Thin,       // + lacks body (bass/low-mid deficit)
    Punchy,     // + strong clear transients, - soft/flat attack
    Squashed,   // + over-compressed (low crest / small loudness range)
    Wide,       // + wide stereo image, - narrow/mono
    PhaseIssue, // + negative correlation
    NumDescriptors
};

struct DescriptorScore
{
    Descriptor descriptor;
    float score;
};

std::string toString (Descriptor);
std::string koreanLabel (Descriptor, bool positive);

/** Target tonal balance (dB per band, relative to total energy) for a role. */
std::array<float, SpectrumBands::kNumBands> referenceCurve (InstrumentRole);

class PerceptualProfile
{
public:
    static PerceptualProfile analyse (const AudioFeatures&, InstrumentRole);

    float score (Descriptor d) const noexcept { return scores[static_cast<size_t> (d)]; }

    /** Per-band deviation from the role's reference curve, dB. */
    const std::array<float, SpectrumBands::kNumBands>& bandDeviationDb() const noexcept { return deviation; }

    /** Descriptors whose |score| exceeds threshold, strongest first. */
    std::vector<DescriptorScore> salient (float threshold = 0.35f) const;

    /** Short human summary, e.g. "약간 탁함(muddy +0.6), 어택이 약함(punchy -0.5)". */
    std::string summary() const;

    nlohmann::json toJson() const;

private:
    std::array<float, static_cast<size_t> (Descriptor::NumDescriptors)> scores {};
    std::array<float, SpectrumBands::kNumBands> deviation {};
};

} // namespace smix
