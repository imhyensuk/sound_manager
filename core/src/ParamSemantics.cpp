#include "smix/ParamSemantics.h"

#include <algorithm>
#include <cctype>

namespace smix
{
namespace
{
int extractNumber (const std::string& s)
{
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (std::isdigit (static_cast<unsigned char> (s[i])))
        {
            int v = 0;
            while (i < s.size() && std::isdigit (static_cast<unsigned char> (s[i])))
                v = v * 10 + (s[i++] - '0');
            return v;
        }
    }
    return -1;
}

int namedBand (const std::string& n)
{
    if (containsAny (n, { "low mid", "lowmid", "lo mid", "lmf", "lm " }))  return 1;
    if (containsAny (n, { "high mid", "highmid", "hi mid", "hmf", "hm " })) return 2;
    if (containsAny (n, { "low", "lf", "bass" }))                           return 0;
    if (containsAny (n, { "high", "hf", "treble", "air" }))                 return 3;
    if (containsAny (n, { "mid", "mf" }))                                   return 1;
    return -1;
}
} // namespace

std::string toString (ParamRole r)
{
    switch (r)
    {
        case ParamRole::Bypass:           return "bypass";
        case ParamRole::InputGain:        return "input_gain";
        case ParamRole::OutputGain:       return "output_gain";
        case ParamRole::Mix:              return "mix";
        case ParamRole::Threshold:        return "threshold";
        case ParamRole::Ratio:            return "ratio";
        case ParamRole::Attack:           return "attack";
        case ParamRole::Release:          return "release";
        case ParamRole::Knee:             return "knee";
        case ParamRole::Makeup:           return "makeup";
        case ParamRole::Range:            return "range";
        case ParamRole::BandFreq:         return "band_freq";
        case ParamRole::BandGain:         return "band_gain";
        case ParamRole::BandQ:            return "band_q";
        case ParamRole::BandShape:        return "band_shape";
        case ParamRole::BandEnable:       return "band_enable";
        case ParamRole::LowCutFreq:       return "low_cut_freq";
        case ParamRole::HighCutFreq:      return "high_cut_freq";
        case ParamRole::Drive:            return "drive";
        case ParamRole::TransientAttack:  return "transient_attack";
        case ParamRole::TransientSustain: return "transient_sustain";
        case ParamRole::Decay:            return "decay";
        case ParamRole::PreDelay:         return "pre_delay";
        case ParamRole::Size:             return "size";
        case ParamRole::Damping:          return "damping";
        case ParamRole::DelayTime:        return "delay_time";
        case ParamRole::Feedback:         return "feedback";
        case ParamRole::Width:            return "width";
        case ParamRole::Ceiling:          return "ceiling";
        default:                          return "unknown";
    }
}

ParamSemantic classifyParameter (const std::string& rawName, PluginCategory category)
{
    const auto n = toLowerAscii (rawName);
    ParamSemantic s;

    if (containsAny (n, { "bypass" }))                                      { s.role = ParamRole::Bypass; return s; }
    if (containsAny (n, { "midi cc", "program", "zoom", "meter", "gui" }))   return s;

    if (containsAny (n, { "low cut", "lowcut", "lo cut", "hpf", "high pass", "highpass", "hp freq", "hi pass" }))
        { s.role = ParamRole::LowCutFreq; return s; }
    if (containsAny (n, { "high cut", "highcut", "hi cut", "lpf", "low pass", "lowpass", "lp freq" }))
        { s.role = ParamRole::HighCutFreq; return s; }

    if (containsAny (n, { "input", "in gain", "drive in" }) && ! containsAny (n, { "freq" }))
        { s.role = category == PluginCategory::Saturation ? ParamRole::Drive : ParamRole::InputGain; return s; }
    if (containsAny (n, { "output", "out gain", "volume", "level out" }))   { s.role = ParamRole::OutputGain; return s; }
    if (containsAny (n, { "mix", "dry/wet", "wet", "blend", "parallel" }))  { s.role = ParamRole::Mix; return s; }
    if (containsAny (n, { "ceiling", "out ceiling", "true peak" }))         { s.role = ParamRole::Ceiling; return s; }

    if (category == PluginCategory::TransientShaper)
    {
        if (containsAny (n, { "attack", "punch" }))  { s.role = ParamRole::TransientAttack; return s; }
        if (containsAny (n, { "sustain", "body" }))  { s.role = ParamRole::TransientSustain; return s; }
    }

    if (containsAny (n, { "thresh" }))                    { s.role = ParamRole::Threshold; return s; }
    if (containsAny (n, { "ratio" }))                     { s.role = ParamRole::Ratio; return s; }
    if (containsAny (n, { "attack" }))                    { s.role = ParamRole::Attack; return s; }
    if (containsAny (n, { "release", "recovery" }))       { s.role = ParamRole::Release; return s; }
    if (containsAny (n, { "knee" }))                      { s.role = ParamRole::Knee; return s; }
    if (containsAny (n, { "makeup", "make-up", "make up" })) { s.role = ParamRole::Makeup; return s; }
    if (containsAny (n, { "range", "depth" }) && category != PluginCategory::Modulation) { s.role = ParamRole::Range; return s; }
    if (containsAny (n, { "drive", "saturation", "amount", "heat" }) && category == PluginCategory::Saturation)
        { s.role = ParamRole::Drive; return s; }

    if (containsAny (n, { "pre-delay", "predelay", "pre delay" })) { s.role = ParamRole::PreDelay; return s; }
    if (category == PluginCategory::Reverb)
    {
        if (containsAny (n, { "decay", "time", "rt60", "length" })) { s.role = ParamRole::Decay; return s; }
        if (containsAny (n, { "size", "room" }))                    { s.role = ParamRole::Size; return s; }
        if (containsAny (n, { "damp" }))                            { s.role = ParamRole::Damping; return s; }
    }
    if (category == PluginCategory::Delay)
    {
        if (containsAny (n, { "time", "delay" }))     { s.role = ParamRole::DelayTime; return s; }
        if (containsAny (n, { "feedback", "repeat" })) { s.role = ParamRole::Feedback; return s; }
    }
    if (containsAny (n, { "width", "spread", "stereo" })) { s.role = ParamRole::Width; return s; }

    if (category == PluginCategory::EQ || containsAny (n, { "band", "eq " }))
    {
        const int number = extractNumber (n);
        const int band = number >= 0 ? std::max (0, number - 1) : namedBand (n);
        if (containsAny (n, { "freq", "frq", "hz" }))                          { s.role = ParamRole::BandFreq; s.band = band; return s; }
        if (containsAny (n, { " q", "q ", "bandwidth", "bw", "width", "res" }) || n == "q")
                                                                               { s.role = ParamRole::BandQ; s.band = band; return s; }
        if (containsAny (n, { "shape", "type", "slope", "mode" }))            { s.role = ParamRole::BandShape; s.band = band; return s; }
        if (containsAny (n, { "enable", "active", "on", "used" }))            { s.role = ParamRole::BandEnable; s.band = band; return s; }
        if (containsAny (n, { "gain", "boost", "cut", "level", "db" }))       { s.role = ParamRole::BandGain; s.band = band; return s; }
    }

    if (containsAny (n, { "gain" }))
        s.role = category == PluginCategory::Compressor ? ParamRole::Makeup : ParamRole::OutputGain;
    return s;
}

nlohmann::json ParamInfo::toJson (bool verbose) const
{
    nlohmann::json j { { "index", index }, { "name", name }, { "value", std::round (value * 1000.0f) / 1000.0f },
                       { "text", valueText } };
    if (semantic.role != ParamRole::Unknown)
    {
        j["role"] = toString (semantic.role);
        if (semantic.band >= 0)
            j["band"] = semantic.band + 1;
    }
    if (verbose)
    {
        j["id"] = id;
        j["label"] = label;
        j["default"] = defaultValue;
        j["steps"] = numSteps;
        j["boolean"] = isBoolean;
    }
    return j;
}

ParamInfo ParamInfo::fromJson (const nlohmann::json& j)
{
    ParamInfo p;
    p.index = j.value ("index", -1);
    p.id = j.value ("id", "");
    p.name = j.value ("name", "");
    p.label = j.value ("label", "");
    p.value = j.value ("value", 0.0f);
    p.defaultValue = j.value ("default", 0.0f);
    p.valueText = j.value ("text", "");
    p.numSteps = j.value ("steps", 0);
    p.isBoolean = j.value ("boolean", false);
    return p;
}

std::vector<const ParamInfo*> mixRelevantParameters (const std::vector<ParamInfo>& params, size_t maxCount)
{
    std::vector<const ParamInfo*> known, unknown;
    for (auto& p : params)
    {
        if (p.semantic.role == ParamRole::Bypass)
            continue;
        (p.semantic.role != ParamRole::Unknown ? known : unknown).push_back (&p);
    }
    for (auto* p : unknown)
    {
        const auto n = toLowerAscii (p->name);
        if (! containsAny (n, { "midi", "cc ", "program", "zoom", "meter", "gui", "analyzer", "display", "scale" }))
            known.push_back (p);
    }
    if (known.size() > maxCount)
        known.resize (maxCount);
    return known;
}

} // namespace smix
