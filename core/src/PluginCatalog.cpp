#include "smix/PluginCatalog.h"

#include <algorithm>

namespace smix
{

nlohmann::json PluginInfo::toJson() const
{
    return { { "uid", uid }, { "name", name }, { "manufacturer", manufacturer }, { "format", format },
             { "host_category", hostCategory }, { "category", toString (category) },
             { "category_overridden", categoryOverridden }, { "allowed", allowed }, { "priority", priority } };
}

PluginInfo PluginInfo::fromJson (const nlohmann::json& j)
{
    PluginInfo p;
    p.uid = j.value ("uid", "");
    p.name = j.value ("name", "");
    p.manufacturer = j.value ("manufacturer", "");
    p.format = j.value ("format", "");
    p.hostCategory = j.value ("host_category", "");
    p.category = categoryFromString (j.value ("category", "unknown")).value_or (PluginCategory::Unknown);
    p.categoryOverridden = j.value ("category_overridden", false);
    p.allowed = j.value ("allowed", false);
    p.priority = j.value ("priority", 100);
    return p;
}

PluginCategory PluginCatalog::classify (const std::string& name, const std::string& manufacturer, const std::string& hostCategory)
{
    const auto n = toLowerAscii (name);
    const auto m = toLowerAscii (manufacturer);
    const auto c = toLowerAscii (hostCategory);

    // Name first: it is the most specific signal. Order matters ("de-esser" before "ess", "limiter" before "lim").
    if (containsAny (n, { "de-ess", "deess", "de ess", "sibil" }))                         return PluginCategory::DeEsser;
    if (containsAny (n, { "gate", "expander", "noise sup" }))                              return PluginCategory::Gate;
    if (containsAny (n, { "limit", "maximi", "pro-l", " l1", " l2", "l3 " }))              return PluginCategory::Limiter;
    if (containsAny (n, { "transient", "envelope", "trans-x", "smack attack", "spiff" }))  return PluginCategory::TransientShaper;
    if (containsAny (n, { "tune", "melodyne", "pitch" }))                                  return PluginCategory::PitchCorrection;
    if (containsAny (n, { "eq", "equal", "pultec", "pro-q", "filter", "q10", "q6" }))      return PluginCategory::EQ;
    if (containsAny (n, { "comp", "1176", "cla-76", "cla-2a", "cla-3a", "la-2a", "la2a", "la-3a", "dbx", "fairchild", "glue", "pro-c",
                          "opto", "vca", "dynamics", "squeez", "leveler", "ssl g" }))       return PluginCategory::Compressor;
    if (containsAny (n, { "satur", "tape", "tube", "decapitator", "saturn", "drive", "distort",
                          "exciter", "fuzz", "crush", "warm", "console", "harmonic" }))      return PluginCategory::Saturation;
    if (containsAny (n, { "verb", "room", "plate", "hall", "space", "chamber", "ambien" }))  return PluginCategory::Reverb;
    if (containsAny (n, { "delay", "echo", "tape dly", "dly" }))                           return PluginCategory::Delay;
    if (containsAny (n, { "chorus", "flang", "phase", "tremolo", "vibrato", "rotary", "ensemble" }))
                                                                                            return PluginCategory::Modulation;
    if (containsAny (n, { "stereo", "imager", "width", "wider", "spread", "mid/side", "m/s" })) return PluginCategory::StereoImager;
    if (containsAny (n, { "utility", "gain", "trim", "meter" }))                           return PluginCategory::Utility;

    // Fall back to the format's own category tags (VST3 subcategories, AU types, ...).
    if (containsAny (c, { "eq" }))                                       return PluginCategory::EQ;
    if (containsAny (c, { "reverb" }))                                   return PluginCategory::Reverb;
    if (containsAny (c, { "delay" }))                                    return PluginCategory::Delay;
    if (containsAny (c, { "modulation" }))                               return PluginCategory::Modulation;
    if (containsAny (c, { "distortion" }))                               return PluginCategory::Saturation;
    if (containsAny (c, { "spatial", "surround" }))                      return PluginCategory::StereoImager;
    if (containsAny (c, { "mastering" }))                                return PluginCategory::Limiter;
    if (containsAny (c, { "dynamics" }))                                 return PluginCategory::Compressor;
    if (containsAny (c, { "tools", "analyzer" }))                        return PluginCategory::Utility;
    if (containsAny (c, { "pitch" }))                                    return PluginCategory::PitchCorrection;
    (void) m;
    return PluginCategory::Unknown;
}

void PluginCatalog::addOrUpdate (PluginInfo info)
{
    if (! info.categoryOverridden && info.category == PluginCategory::Unknown)
        info.category = classify (info.name, info.manufacturer, info.hostCategory);

    for (auto& existing : plugins)
    {
        if (existing.uid == info.uid)
        {
            info.allowed = existing.allowed;
            info.priority = existing.priority;
            if (existing.categoryOverridden)
            {
                info.category = existing.category;
                info.categoryOverridden = true;
            }
            existing = std::move (info);
            return;
        }
    }
    plugins.push_back (std::move (info));
}

void PluginCatalog::remove (const std::string& uid)
{
    plugins.erase (std::remove_if (plugins.begin(), plugins.end(), [&] (auto& p) { return p.uid == uid; }), plugins.end());
}

bool PluginCatalog::setAllowed (const std::string& uid, bool allowed)
{
    for (auto& p : plugins)
        if (p.uid == uid) { p.allowed = allowed; return true; }
    return false;
}

bool PluginCatalog::setCategory (const std::string& uid, PluginCategory category)
{
    for (auto& p : plugins)
        if (p.uid == uid) { p.category = category; p.categoryOverridden = true; return true; }
    return false;
}

bool PluginCatalog::setPriority (const std::string& uid, int priority)
{
    for (auto& p : plugins)
        if (p.uid == uid) { p.priority = priority; return true; }
    return false;
}

const PluginInfo* PluginCatalog::find (const std::string& uid) const
{
    for (auto& p : plugins)
        if (p.uid == uid)
            return &p;
    return nullptr;
}

std::vector<PluginInfo> PluginCatalog::allowedPlugins() const
{
    std::vector<PluginInfo> out;
    for (auto& p : plugins)
        if (p.allowed)
            out.push_back (p);
    std::stable_sort (out.begin(), out.end(), [] (auto& a, auto& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.name < b.name;
    });
    return out;
}

std::vector<PluginInfo> PluginCatalog::allowedIn (PluginCategory category) const
{
    auto all = allowedPlugins();
    all.erase (std::remove_if (all.begin(), all.end(), [category] (auto& p) { return p.category != category; }), all.end());
    return all;
}

nlohmann::json PluginCatalog::toJson() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (auto& p : plugins)
        arr.push_back (p.toJson());
    return { { "version", 1 }, { "plugins", arr } };
}

PluginCatalog PluginCatalog::fromJson (const nlohmann::json& j)
{
    PluginCatalog c;
    if (j.contains ("plugins") && j["plugins"].is_array())
        for (auto& p : j["plugins"])
            c.plugins.push_back (PluginInfo::fromJson (p));
    return c;
}

} // namespace smix
