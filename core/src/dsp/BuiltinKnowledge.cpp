#include "smix/dsp/Builtin.h"
#include "smix/knowledge/PluginProfile.h"

namespace smix::dsp
{

bool annotateProfile (knowledge::PluginProfile& profile)
{
    const auto k = kindFromString (profile.uid);
    if (! k)
        return false;
    profile.declaredCategory = categoryOf (*k);
    profile.measuredCategory = categoryOf (*k);  // known by construction, not guessed
    profile.vendor = "Sound Manager";
    const std::string intro = "Sound Manager 내장 " + koreanName (*k) + " (" + displayName (*k) + "). " + description (*k);
    profile.summary = profile.summary.empty() ? intro : intro + " " + profile.summary;
    for (auto& e : profile.effects)
        for (auto& spec : specsFor (*k))
            if (spec.name == e.name)
            {
                std::string range = formatValue (spec, spec.min) + " ~ " + formatValue (spec, spec.max) + ", 기본 " + formatValue (spec, spec.def);
                e.summary = spec.help + " (" + range + ")" + (e.summary.empty() ? "" : " " + e.summary);
            }
    return true;
}

} // namespace smix::dsp
