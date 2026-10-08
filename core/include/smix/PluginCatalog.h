#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/Types.h"

namespace smix
{

/** A plugin installed on the user's machine, as seen by the scanner. */
struct PluginInfo
{
    std::string uid;            // stable identifier (e.g. JUCE PluginDescription::createIdentifierString())
    std::string name;
    std::string manufacturer;
    std::string format;         // VST3, AudioUnit, AAX, ...
    std::string hostCategory;   // category string reported by the plugin format (e.g. "Fx|EQ")
    PluginCategory category = PluginCategory::Unknown;
    bool categoryOverridden = false;  // user changed the category by hand
    bool allowed = false;             // the user ticked "AI may use this plugin"
    int priority = 100;               // lower = preferred when several allowed plugins share a category

    nlohmann::json toJson() const;
    static PluginInfo fromJson (const nlohmann::json&);
};

/**
    The user's plugin inventory plus their choices. Only `allowed` plugins are ever
    inserted by the AI (requirement: "use only the plugins the user ticked").
*/
class PluginCatalog
{
public:
    /** Best-effort classification from name / vendor / format category. */
    static PluginCategory classify (const std::string& name, const std::string& manufacturer, const std::string& hostCategory);

    /** Adds a scanned plugin; keeps the user's allow/category/priority choices if it is already known. */
    void addOrUpdate (PluginInfo);
    void remove (const std::string& uid);

    bool setAllowed (const std::string& uid, bool allowed);
    bool setCategory (const std::string& uid, PluginCategory);
    bool setPriority (const std::string& uid, int priority);

    const PluginInfo* find (const std::string& uid) const;
    const std::vector<PluginInfo>& all() const noexcept { return plugins; }

    /** Allowed plugins sorted by (priority, name). */
    std::vector<PluginInfo> allowedPlugins() const;
    std::vector<PluginInfo> allowedIn (PluginCategory) const;

    nlohmann::json toJson() const;
    static PluginCatalog fromJson (const nlohmann::json&);

private:
    std::vector<PluginInfo> plugins;
};

} // namespace smix
