#include "BuiltinFormat.h"

#include <nlohmann/json.hpp>

using smix::dsp::Kind;

namespace
{
constexpr const char* kIdentifierPrefix = "smix.";

juce::String identifierFor (Kind k)
{
    return juce::String (kIdentifierPrefix) + juce::String (smix::dsp::toString (k));
}

class BuiltinParameter : public juce::AudioPluginInstance::HostedParameter
{
public:
    BuiltinParameter (smix::dsp::Processor& p, int i) : proc (p), index (i), spec (p.specs()[static_cast<size_t> (i)]) {}

    juce::String getParameterID() const override { return spec.id; }
    float getValue() const override { return smix::dsp::toNormalised (spec, proc.get (index)); }
    void setValue (float v) override { proc.set (index, smix::dsp::fromNormalised (spec, v)); }
    float getDefaultValue() const override { return smix::dsp::toNormalised (spec, spec.def); }
    juce::String getName (int maximumLength) const override { return juce::String (spec.name).substring (0, maximumLength); }
    juce::String getLabel() const override { return {}; }  // the unit is part of the text ("-12.0 dB")
    int getNumSteps() const override
    {
        if (spec.toggle) return 2;
        if (! spec.choices.empty()) return static_cast<int> (spec.choices.size());
        return juce::AudioProcessor::getDefaultNumParameterSteps();
    }
    bool isDiscrete() const override { return spec.toggle || ! spec.choices.empty(); }
    bool isBoolean() const override { return spec.toggle; }
    juce::String getText (float v, int maximumLength) const override
    {
        return juce::String (smix::dsp::formatValue (spec, smix::dsp::fromNormalised (spec, v))).substring (0, maximumLength);
    }
    float getValueForText (const juce::String& text) const override
    {
        if (! spec.choices.empty())
        {
            for (size_t c = 0; c < spec.choices.size(); ++c)
                if (text.trim().equalsIgnoreCase (juce::String (spec.choices[c])))
                    return smix::dsp::toNormalised (spec, static_cast<float> (c));
        }
        if (spec.toggle)
            return text.trim().equalsIgnoreCase ("on") || text.getIntValue() != 0 ? 1.0f : 0.0f;
        float v = text.getFloatValue();
        if (spec.unit == "Hz" && text.containsIgnoreCase ("k"))
            v *= 1000.0f;
        if (spec.unit == "ms" && text.trim().endsWithIgnoreCase (" s"))
            v *= 1000.0f;
        return smix::dsp::toNormalised (spec, v);
    }

private:
    smix::dsp::Processor& proc;
    int index;
    const smix::dsp::ParamSpec& spec;
};

class BuiltinInstance : public juce::AudioPluginInstance
{
public:
    explicit BuiltinInstance (Kind k)
        : AudioPluginInstance (BusesProperties()
                                   .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          kind (k), proc (smix::dsp::create (k))
    {
        for (int i = 0; i < proc->numParams(); ++i)
            addHostedParameter (std::make_unique<BuiltinParameter> (*proc, i));
    }

    void fillInPluginDescription (juce::PluginDescription& d) const override { d = BuiltinFormat::describe (kind); }
    const juce::String getName() const override { return smix::dsp::displayName (kind); }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
        return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
    }

    void prepareToPlay (double sampleRate, int maximumBlockSize) override
    {
        proc->prepare (sampleRate, maximumBlockSize, juce::jmax (1, getTotalNumOutputChannels()));
        setLatencySamples (proc->latencySamples());
    }
    void releaseResources() override {}
    void reset() override { proc->reset(); }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        proc->process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
    }
    using AudioPluginInstance::processBlock;

    double getTailLengthSeconds() const override { return proc->tailSeconds(); }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        nlohmann::json j { { "builtin", smix::dsp::toString (kind) }, { "version", 1 } };
        for (int i = 0; i < proc->numParams(); ++i)
            j["params"][proc->specs()[static_cast<size_t> (i)].id] = proc->get (i);
        const auto text = j.dump();
        dest.replaceAll (text.data(), text.size());
    }

    void setStateInformation (const void* data, int size) override
    {
        const auto j = nlohmann::json::parse (static_cast<const char*> (data), static_cast<const char*> (data) + size, nullptr, false);
        if (j.is_discarded() || ! j.contains ("params") || ! j["params"].is_object())
            return;
        const auto params = j["params"];
        for (auto it = params.begin(); it != params.end(); ++it)
            if (it.value().is_number())
                proc->set (it.key(), it.value().get<float>());
    }

private:
    Kind kind;
    std::unique_ptr<smix::dsp::Processor> proc;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BuiltinInstance)
};

std::optional<Kind> kindForIdentifier (const juce::String& s)
{
    return smix::dsp::kindFromString (s.trim().toStdString());
}
} // namespace

juce::PluginDescription BuiltinFormat::describe (Kind k)
{
    juce::PluginDescription d;
    d.name = smix::dsp::displayName (k);
    d.descriptiveName = juce::String::fromUTF8 (smix::dsp::description (k).c_str());
    d.pluginFormatName = kFormatName;
    d.category = "Fx|" + juce::String (smix::toString (smix::dsp::categoryOf (k)));
    d.manufacturerName = "Sound Manager";
    d.version = "1.0";
    d.fileOrIdentifier = identifierFor (k);
    d.uniqueId = 0x534d0100 + static_cast<int> (k);
    d.deprecatedUid = d.uniqueId;
    d.isInstrument = false;
    d.numInputChannels = 2;
    d.numOutputChannels = 2;
    d.hasSharedContainer = false;
    return d;
}

std::optional<juce::PluginDescription> BuiltinFormat::descriptionFor (const juce::String& uidOrIdentifier)
{
    if (const auto k = kindForIdentifier (uidOrIdentifier))
        return describe (*k);
    return std::nullopt;
}

void BuiltinFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& id)
{
    if (const auto k = kindForIdentifier (id))
        results.add (new juce::PluginDescription (describe (*k)));
}

bool BuiltinFormat::fileMightContainThisPluginType (const juce::String& id)
{
    return id.startsWith (kIdentifierPrefix) && kindForIdentifier (id).has_value();
}

juce::String BuiltinFormat::getNameOfPluginFromIdentifier (const juce::String& id)
{
    if (const auto k = kindForIdentifier (id))
        return smix::dsp::displayName (*k);
    return id;
}

void BuiltinFormat::createPluginInstance (const juce::PluginDescription& d, double sampleRate, int blockSize,
                                          PluginCreationCallback callback)
{
    const auto k = kindForIdentifier (d.fileOrIdentifier);
    if (! k)
    {
        callback (nullptr, "Unknown Sound Manager processor: " + d.fileOrIdentifier);
        return;
    }
    auto instance = std::make_unique<BuiltinInstance> (*k);
    instance->setRateAndBufferSizeDetails (sampleRate, blockSize);
    callback (std::move (instance), {});
}
