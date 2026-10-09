#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <smix/dsp/Builtin.h>

#include <optional>

/**
    Sound Manager's built-in processors (smix::dsp) as a plugin format, so they live in the hosted
    chain exactly like the user's own plugins: same planning, parameter control, history,
    hibernation, protection and project state. Always available - in Logic Pro they are what the
    AI can actually move, because Logic's stock plugins cannot be hosted by another plugin.
*/
class BuiltinFormat : public juce::AudioPluginFormat
{
public:
    static constexpr const char* kFormatName = "SoundManager";

    static juce::PluginDescription describe (smix::dsp::Kind);
    /** Built-in description for a catalog uid ("SoundManager-smix.compressor") or identifier ("smix.compressor"). */
    static std::optional<juce::PluginDescription> descriptionFor (const juce::String& uidOrIdentifier);

    juce::String getName() const override { return kFormatName; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }

private:
    void createPluginInstance (const juce::PluginDescription&, double initialSampleRate, int initialBufferSize,
                               PluginCreationCallback) override;
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }
};
