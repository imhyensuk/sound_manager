// SoundManagerProfiler: measures installed plugins in a separate process, so a plugin that
// crashes or hangs never takes the DAW down. Started by the plugin (Engine::ProfilerJob):
//
//   SoundManagerProfiler --data <SoundManagerAI data dir> --queue <file with one plugin uid per line>
//
// Output (stdout, one line each):  START <name> | PROGRESS <done> <total> | RESULT <profile json> | DONE

#include <juce_audio_processors/juce_audio_processors.h>

#include <smix/PluginCatalog.h>
#include <smix/knowledge/PluginProfile.h>

#include "../Source/BuiltinFormat.h"

#include <chrono>
#include <fstream>
#include <iostream>

#if JUCE_LINUX
 #include <unistd.h>
#elif JUCE_MAC
 #include <mach/mach.h>
#elif JUCE_WINDOWS
 #include <windows.h>
 #include <psapi.h>
#endif

namespace
{
std::uint64_t residentBytes()
{
#if JUCE_LINUX
    long pages = 0, resident = 0;
    if (std::ifstream f ("/proc/self/statm"); f >> pages >> resident)
        return static_cast<std::uint64_t> (resident) * static_cast<std::uint64_t> (sysconf (_SC_PAGESIZE));
#elif JUCE_MAC
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info (mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t> (&info), &count) == KERN_SUCCESS)
        return info.resident_size;
#elif JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo (GetCurrentProcess(), &pmc, sizeof (pmc)))
        return pmc.WorkingSetSize;
#endif
    return 0;
}

/** A hosted plugin seen through the profiler's ProbeTarget interface. */
class JuceProbeTarget : public smix::knowledge::ProbeTarget
{
public:
    explicit JuceProbeTarget (juce::AudioPluginInstance& p) : plugin (p)
    {
        plugin.disableNonMainBuses();
        plugin.setChannelLayoutOfBus (true, 0, juce::AudioChannelSet::stereo());
        plugin.setChannelLayoutOfBus (false, 0, juce::AudioChannelSet::stereo());
        plugin.setRateAndBufferSizeDetails (kRate, kBlock);
        plugin.prepareToPlay (kRate, kBlock);
        const int channels = juce::jmax (2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels());
        buffer.setSize (channels, kBlock);
    }

    ~JuceProbeTarget() override { plugin.releaseResources(); }

    double sampleRate() const override { return kRate; }
    int numParams() const override { return plugin.getParameters().size(); }
    std::string paramName (int i) const override { return param (i)->getName (64).toStdString(); }
    std::string paramLabel (int i) const override { return param (i)->getLabel().toStdString(); }
    float paramDefault (int i) const override { return param (i)->getDefaultValue(); }
    std::string paramText (int i, float v) const override { return param (i)->getText (v, 64).toStdString(); }
    bool paramIsDiscrete (int i) const override { return param (i)->isDiscrete(); }
    void setParam (int i, float v) override { param (i)->setValue (v); }
    void reset() override { plugin.reset(); }
    int latencySamples() const override { return plugin.getLatencySamples(); }

    void process (float* left, float* right, int n) override
    {
        for (int offset = 0; offset < n; offset += kBlock)
        {
            const int count = juce::jmin (kBlock, n - offset);
            buffer.clear();
            buffer.copyFrom (0, 0, left + offset, count);
            buffer.copyFrom (1, 0, right + offset, count);
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), count);
            midi.clear();
            plugin.processBlock (view, midi);
            std::memcpy (left + offset, view.getReadPointer (0), sizeof (float) * static_cast<size_t> (count));
            std::memcpy (right + offset, view.getReadPointer (1), sizeof (float) * static_cast<size_t> (count));
        }
    }

private:
    juce::AudioProcessorParameter* param (int i) const { return plugin.getParameters()[i]; }

    static constexpr double kRate = 48000.0;
    static constexpr int kBlock = 512;
    juce::AudioPluginInstance& plugin;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
};

void emit (const std::string& line)
{
    std::cout << line << "\n" << std::flush;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;  // some plugins expect a message manager
    juce::File dataDir, queueFile;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::string (argv[i]) == "--data")  dataDir = juce::File (juce::String::fromUTF8 (argv[i + 1]));
        if (std::string (argv[i]) == "--queue") queueFile = juce::File (juce::String::fromUTF8 (argv[i + 1]));
    }
    if (! dataDir.isDirectory() || ! queueFile.existsAsFile())
    {
        std::cerr << "usage: SoundManagerProfiler --data <dir> --queue <file>\n";
        return 2;
    }

    juce::KnownPluginList known;
    if (auto xml = juce::parseXML (dataDir.getChildFile ("known-plugins.xml")))
        known.recreateFromXml (*xml);
    smix::PluginCatalog catalog;
    {
        std::ifstream in (dataDir.getChildFile ("catalog.json").getFullPathName().toStdString());
        const auto j = nlohmann::json::parse (in, nullptr, false);
        if (! j.is_discarded())
            catalog = smix::PluginCatalog::fromJson (j);
    }

    juce::AudioPluginFormatManager formats;
    formats.addDefaultFormats();
    formats.addFormat (new BuiltinFormat());

    juce::StringArray uids;
    uids.addLines (queueFile.loadFileAsString());
    uids.removeEmptyStrings();

    for (auto& uid : uids)
    {
        const auto id = uid.toStdString();
        std::optional<juce::PluginDescription> desc = BuiltinFormat::descriptionFor (uid);
        for (auto& d : known.getTypes())
            if (d.createIdentifierString() == uid)
                desc = d;

        smix::PluginInfo info;
        if (const auto* c = catalog.find (id))
            info = *c;
        info.uid = id;

        smix::knowledge::PluginProfile profile;
        profile.uid = id;
        profile.version = smix::knowledge::PluginProfile::kVersion;
        if (! desc)
        {
            profile.failed = true;
            profile.error = "not in the scanned plugin list";
            emit ("RESULT " + profile.toJson().dump());
            continue;
        }
        if (info.name.empty())
        {
            info.name = desc->name.toStdString();
            info.manufacturer = desc->manufacturerName.toStdString();
            info.format = desc->pluginFormatName.toStdString();
            info.hostCategory = desc->category.toStdString();
            info.category = smix::PluginCatalog::classify (info.name, info.manufacturer, info.hostCategory);
        }
        emit ("START " + info.name);

        const auto before = residentBytes();
        const auto t0 = std::chrono::steady_clock::now();
        juce::String error;
        auto instance = formats.createPluginInstance (*desc, 48000.0, 512, error);
        const double loadMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        if (instance == nullptr)
        {
            profile.name = info.name;
            profile.failed = true;
            profile.error = error.toStdString();
            emit ("RESULT " + profile.toJson().dump());
            continue;
        }
        const auto after = residentBytes();

        {
            JuceProbeTarget target (*instance);
            profile = smix::knowledge::PluginProfiler().profile (target, info, {}, [] (int done, int total, const std::string&) {
                emit ("PROGRESS " + std::to_string (done) + " " + std::to_string (total));
            });
        }
        smix::dsp::annotateProfile (profile);  // built-ins: exact meaning of every parameter
        profile.loadMs = loadMs;
        profile.memoryBytes = after > before ? after - before : 0;
        instance.reset();
        emit ("RESULT " + profile.toJson().dump());
    }
    emit ("DONE");
    return 0;
}
