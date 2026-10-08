#pragma once

#include <optional>
#include <string>
#include <vector>

namespace smix
{

/** A numeric value parsed from a plugin's display text ("1.2 kHz" -> 1200 Hz). */
struct ParsedValue
{
    bool ok = false;
    double value = 0.0;
    std::string unit;  // canonical: "dB", "Hz", "ms", "%", "ratio", "" (unitless)
};

ParsedValue parseValueText (const std::string& text, const std::string& labelHint = {});

/** Converts value/unit pairs to the canonical unit ("kHz" -> "Hz", "s" -> "ms"). */
ParsedValue canonicalise (double value, const std::string& unit);

/**
    Third-party plugins only expose normalised 0..1 parameter values. The mapper learns
    the plugin's own normalised -> real-world curve by sampling its display text, so the AI
    can say "set the band to 80 Hz / +3 dB" and we can find the matching normalised value.
*/
class ValueMapper
{
public:
    void addSample (float normalised, const std::string& text, const std::string& labelHint = {});

    /** True if enough monotonic numeric samples were collected. */
    bool isUsable() const;

    const std::string& unit() const noexcept { return unitName; }

    std::optional<float> toNormalised (double realValue, const std::string& unit = {}) const;
    std::optional<double> toReal (float normalised) const;

    /** Recommended normalised points to probe (dense enough for log-scaled frequency knobs). */
    static std::vector<float> probePoints (int count = 65);

private:
    struct Sample { float norm; double value; };
    std::vector<Sample> samples;
    std::string unitName;

    bool logDomain() const;
};

} // namespace smix
