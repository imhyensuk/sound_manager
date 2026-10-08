#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/Types.h"

namespace smix
{

/** What a hosted plugin parameter does, inferred from its name. */
enum class ParamRole
{
    Unknown,
    Bypass,
    InputGain, OutputGain, Mix,
    Threshold, Ratio, Attack, Release, Knee, Makeup, Range,
    BandFreq, BandGain, BandQ, BandShape, BandEnable,
    LowCutFreq, HighCutFreq,
    Drive,
    TransientAttack, TransientSustain,
    Decay, PreDelay, Size, Damping,
    DelayTime, Feedback,
    Width,
    Ceiling
};

std::string toString (ParamRole);

struct ParamSemantic
{
    ParamRole role = ParamRole::Unknown;
    int band = -1;  // EQ band index when the parameter belongs to a band (0-based)
};

/** Infers the role of a parameter from its display name and the plugin's category. */
ParamSemantic classifyParameter (const std::string& name, PluginCategory category);

/** One parameter of a hosted plugin, as exposed to the AI. */
struct ParamInfo
{
    int index = -1;
    std::string id;
    std::string name;
    std::string label;          // unit label as reported by the plugin
    float value = 0.0f;         // normalised 0..1
    float defaultValue = 0.0f;  // normalised 0..1
    std::string valueText;      // e.g. "-3.2 dB"
    int numSteps = 0;           // 0 = continuous
    bool isBoolean = false;
    ParamSemantic semantic;

    nlohmann::json toJson (bool verbose) const;
    static ParamInfo fromJson (const nlohmann::json&);
};

/** Parameters that matter for mixing (filters out MIDI CCs, meters, UI zoom...). */
std::vector<const ParamInfo*> mixRelevantParameters (const std::vector<ParamInfo>&, size_t maxCount);

} // namespace smix
