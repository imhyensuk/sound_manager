#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <smix/PluginCatalog.h>
#include <smix/dsp/Builtin.h>

/**
    Process-wide inventory of the user's plugins, shared by every Sound Manager instance
    (juce::SharedResourcePointer). Owns the format manager, the scan results, the user's
    "AI may use this" choices and global settings such as the API key.
    All public methods are message-thread only unless stated otherwise.
*/
class PluginLibrary : public juce::ChangeBroadcaster
{
public:
    PluginLibrary();
    ~PluginLibrary() override;

    juce::AudioPluginFormatManager& formats() noexcept { return formatManager; }

    smix::PluginCatalog& catalog() noexcept { return pluginCatalog; }
    void saveCatalog();

    /** Scans the default VST3/AU folders on a background thread. */
    void startScan();
    bool isScanning() const;
    juce::String getScanStatus() const;

    /** Adds a single plugin file/bundle (e.g. one the default scan does not cover). Returns the uids found. */
    std::vector<std::string> addPluginFile (const juce::String& path);

    std::optional<juce::PluginDescription> descriptionFor (const std::string& uid) const;

    // Settings shared by all instances (stored locally, never sent anywhere)
    juce::String getSetting (const juce::String& key, const juce::String& fallback = {}) const;
    void setSetting (const juce::String& key, const juce::String& value);

    static juce::File dataDirectory();
    static juce::File knownPluginsFile() { return dataDirectory().getChildFile ("known-plugins.xml"); }
    static juce::File modelsDirectory();

private:
    class ScanThread;
    void syncCatalogFromKnownList();
    void registerBuiltins();
    void setStatus (const juce::String&);

    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    smix::PluginCatalog pluginCatalog;
    std::unique_ptr<ScanThread> scanThread;
    std::unique_ptr<juce::PropertiesFile> settings;
    mutable juce::CriticalSection statusLock;
    juce::String scanStatus;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginLibrary)
};
