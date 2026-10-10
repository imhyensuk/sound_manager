#include "smix/ValueMapper.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "smix/Types.h"

namespace smix
{

ParsedValue canonicalise (double value, const std::string& unitIn)
{
    const auto u = toLowerAscii (unitIn);
    if (u == "khz")                              return { true, value * 1000.0, "Hz" };
    if (u == "hz")                               return { true, value, "Hz" };
    if (u == "s" || u == "sec" || u == "secs")   return { true, value * 1000.0, "ms" };
    if (u == "ms" || u == "msec")                return { true, value, "ms" };
    if (u == "us" || u == "µs")                  return { true, value / 1000.0, "ms" };
    if (u == "db" || u == "dbfs" || u == "lu")   return { true, value, "dB" };
    if (u == "%" || u == "percent")              return { true, value, "%" };
    if (u == "ratio" || u == ":1")               return { true, value, "ratio" };
    return { true, value, "" };
}

ParsedValue parseValueText (const std::string& textIn, const std::string& labelHint)
{
    const auto text = toLowerAscii (textIn);
    if (text.empty())
        return {};

    if (text.find ("-inf") != std::string::npos || text.find ("-∞") != std::string::npos)
        return { true, -150.0, "dB" };

    // Find the first number (allowing sign and decimal point).
    size_t start = std::string::npos;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        const bool digit = std::isdigit (static_cast<unsigned char> (c)) != 0;
        const bool signedNumber = (c == '-' || c == '+' || c == '.') && i + 1 < text.size()
                                  && std::isdigit (static_cast<unsigned char> (text[i + 1]));
        if (digit || signedNumber)
        {
            start = i;
            break;
        }
    }
    if (start == std::string::npos)
        return {};

    char* end = nullptr;
    const double value = std::strtod (text.c_str() + start, &end);
    std::string rest (end);
    rest.erase (0, rest.find_first_not_of (' '));

    // Ratio "4:1" or "4.0 : 1"
    if (! rest.empty() && rest[0] == ':')
        return { true, value, "ratio" };

    // Number directly followed by 'k' means thousands ("1.2k", "1.2 kHz")
    if (! rest.empty() && rest[0] == 'k')
        return { true, value * 1000.0, "Hz" };

    std::string unit;
    for (char c : rest)
    {
        if (c == ' ' || std::isdigit (static_cast<unsigned char> (c)))
            break;
        unit.push_back (c);
    }
    if (unit.empty())
        unit = toLowerAscii (labelHint);

    if (unit.rfind ("khz", 0) == 0) return { true, value * 1000.0, "Hz" };
    if (unit.rfind ("hz", 0) == 0)  return { true, value, "Hz" };
    if (unit.rfind ("ms", 0) == 0)  return { true, value, "ms" };
    if (unit == "s" || unit.rfind ("sec", 0) == 0) return { true, value * 1000.0, "ms" };
    if (unit.rfind ("db", 0) == 0)  return { true, value, "dB" };
    if (unit.rfind ("%", 0) == 0)   return { true, value, "%" };
    return { true, value, "" };
}

void ValueMapper::addSample (float normalised, const std::string& text, const std::string& labelHint)
{
    const auto parsed = parseValueText (text, labelHint);
    if (! parsed.ok)
        return;
    if (unitName.empty())
        unitName = parsed.unit;
    else if (! parsed.unit.empty() && parsed.unit != unitName)
        return;  // e.g. "Off" style outliers with different units

    auto it = std::lower_bound (samples.begin(), samples.end(), normalised, [] (const Sample& s, float n) { return s.norm < n; });
    samples.insert (it, { normalised, parsed.value });
}

bool ValueMapper::isUsable() const
{
    if (samples.size() < 3)
        return false;

    bool increasing = true, decreasing = true;
    for (size_t i = 1; i < samples.size(); ++i)
    {
        if (samples[i].value < samples[i - 1].value) increasing = false;
        if (samples[i].value > samples[i - 1].value) decreasing = false;
    }
    return (increasing || decreasing) && samples.front().value != samples.back().value;
}

bool ValueMapper::logDomain() const
{
    if (unitName != "Hz" && unitName != "ms")
        return false;
    return std::all_of (samples.begin(), samples.end(), [] (const Sample& s) { return s.value > 0.0; });
}

std::optional<float> ValueMapper::toNormalised (double realValue, const std::string& unit) const
{
    if (! isUsable())
        return std::nullopt;

    if (! unit.empty())
    {
        const auto c = canonicalise (realValue, unit);
        if (! unitName.empty() && ! c.unit.empty() && c.unit != unitName)
            return std::nullopt;
        realValue = c.value;
    }

    const bool useLog = logDomain() && realValue > 0.0;
    auto tx = [useLog] (double v) { return useLog ? std::log (v) : v; };
    const double target = tx (realValue);

    // Samples are sorted by normalised position and values are monotonic.
    const bool increasing = samples.back().value > samples.front().value;
    if (increasing ? realValue <= samples.front().value : realValue >= samples.front().value)
        return samples.front().norm;
    if (increasing ? realValue >= samples.back().value : realValue <= samples.back().value)
        return samples.back().norm;

    for (size_t i = 1; i < samples.size(); ++i)
    {
        const double a = tx (samples[i - 1].value), b = tx (samples[i].value);
        const bool inside = increasing ? (target >= a && target <= b) : (target <= a && target >= b);
        if (inside)
        {
            const double t = (b != a) ? (target - a) / (b - a) : 0.0;
            return static_cast<float> (samples[i - 1].norm + t * (samples[i].norm - samples[i - 1].norm));
        }
    }
    return std::nullopt;
}

std::optional<double> ValueMapper::toReal (float normalised) const
{
    if (! isUsable())
        return std::nullopt;
    if (normalised <= samples.front().norm) return samples.front().value;
    if (normalised >= samples.back().norm)  return samples.back().value;

    const bool useLog = logDomain();
    for (size_t i = 1; i < samples.size(); ++i)
    {
        if (normalised <= samples[i].norm)
        {
            const auto& s0 = samples[i - 1];
            const auto& s1 = samples[i];
            const double t = (normalised - s0.norm) / std::max (1.0e-9f, s1.norm - s0.norm);
            if (useLog)
                return std::exp (std::log (s0.value) + t * (std::log (s1.value) - std::log (s0.value)));
            return s0.value + t * (s1.value - s0.value);
        }
    }
    return samples.back().value;
}

std::vector<float> ValueMapper::probePoints (int count)
{
    std::vector<float> pts;
    pts.reserve (static_cast<size_t> (count));
    for (int i = 0; i < count; ++i)
        pts.push_back (static_cast<float> (i) / static_cast<float> (count - 1));
    return pts;
}

nlohmann::json ValueMapper::toJson() const
{
    nlohmann::json pts = nlohmann::json::array();
    for (auto& smp : samples)
        pts.push_back ({ std::round (smp.norm * 10000.0f) / 10000.0f, smp.value });
    return { { "unit", unitName }, { "points", pts } };
}

ValueMapper ValueMapper::fromJson (const nlohmann::json& j)
{
    ValueMapper m;
    m.unitName = j.value ("unit", std::string {});
    if (j.contains ("points") && j["points"].is_array())
        for (auto& pt : j["points"])
            if (pt.is_array() && pt.size() == 2 && pt[0].is_number() && pt[1].is_number())
                m.samples.push_back ({ pt[0].get<float>(), pt[1].get<double>() });
    std::sort (m.samples.begin(), m.samples.end(), [] (const Sample& a, const Sample& b) { return a.norm < b.norm; });
    return m;
}

} // namespace smix
