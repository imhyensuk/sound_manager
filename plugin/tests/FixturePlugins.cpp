// Minimal third-party-style plugins used only by the integration test:
// a 4-band parametric EQ and a compressor with descriptive parameter names and unit text,
// built as real VST3s so Sound Manager hosts them exactly like a user's own plugins.

#include <juce_audio_processors/juce_audio_processors.h>

namespace
{
juce::String hzText (float v, int)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (v, 1) + " Hz";
}

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[2] {}, z2[2] {};

    void setPeak (double sr, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = 2.0 * juce::MathConstants<double>::pi * f / sr;
        const double alpha = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1 + alpha / A;
        b0 = (1 + alpha * A) / a0; b1 = -2 * c / a0; b2 = (1 - alpha * A) / a0;
        a1 = -2 * c / a0; a2 = (1 - alpha / A) / a0;
    }
    float process (float x, int ch)
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return static_cast<float> (y);
    }
};
} // namespace

class FixtureProcessor : public juce::AudioProcessor
{
public:
    FixtureProcessor()
        : AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo()).withOutput ("Out", juce::AudioChannelSet::stereo())),
          state (*this, nullptr, "STATE", layout())
    {
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout layout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout l;
       #if SMX_FIXTURE_EQ
        const float defaults[] = { 80.0f, 400.0f, 2500.0f, 10000.0f };
        for (int b = 1; b <= 4; ++b)
        {
            const auto n = String (b);
            NormalisableRange<float> freq (20.0f, 20000.0f);
            freq.setSkewForCentre (632.0f);
            l.add (std::make_unique<AudioParameterFloat> (ParameterID { "f" + n, 1 }, "Band " + n + " Frequency", freq, defaults[b - 1],
                                                          AudioParameterFloatAttributes().withStringFromValueFunction (hzText)));
            l.add (std::make_unique<AudioParameterFloat> (ParameterID { "g" + n, 1 }, "Band " + n + " Gain", NormalisableRange<float> (-18.0f, 18.0f), 0.0f,
                                                          AudioParameterFloatAttributes().withLabel ("dB")));
            l.add (std::make_unique<AudioParameterFloat> (ParameterID { "q" + n, 1 }, "Band " + n + " Q", NormalisableRange<float> (0.1f, 10.0f), 1.0f));
        }
       #else
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "thr", 1 }, "Threshold", NormalisableRange<float> (-60.0f, 0.0f), -10.0f,
                                                      AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "ratio", 1 }, "Ratio", NormalisableRange<float> (1.0f, 20.0f), 2.0f,
                                                      AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + ":1"; })));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "att", 1 }, "Attack", NormalisableRange<float> (0.1f, 100.0f), 5.0f,
                                                      AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "rel", 1 }, "Release", NormalisableRange<float> (10.0f, 1000.0f), 100.0f,
                                                      AudioParameterFloatAttributes().withLabel ("ms")));
       #endif
        return l;
    }

    void prepareToPlay (double sr, int) override { sampleRate = sr; env = 0; }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
       #if SMX_FIXTURE_EQ
        for (int b = 0; b < 4; ++b)
        {
            const auto n = juce::String (b + 1);
            bands[b].setPeak (sampleRate, *state.getRawParameterValue ("f" + n), *state.getRawParameterValue ("q" + n),
                              *state.getRawParameterValue ("g" + n));
        }
        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
            for (auto& s : juce::Span<float> (buffer.getWritePointer (ch), (size_t) buffer.getNumSamples()))
                for (auto& band : bands)
                    s = band.process (s, ch);
       #else
        const float thr = *state.getRawParameterValue ("thr"), ratio = *state.getRawParameterValue ("ratio");
        const float att = std::exp (-1.0f / (0.001f * *state.getRawParameterValue ("att") * (float) sampleRate));
        const float rel = std::exp (-1.0f / (0.001f * *state.getRawParameterValue ("rel") * (float) sampleRate));
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            float peak = 0;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                peak = juce::jmax (peak, std::abs (buffer.getSample (ch, i)));
            const float levelDb = juce::Decibels::gainToDecibels (peak, -120.0f);
            const float target = levelDb > thr ? (levelDb - thr) * (1.0f - 1.0f / ratio) : 0.0f;
            env = target > env ? att * env + (1 - att) * target : rel * env + (1 - rel) * target;
            const float g = juce::Decibels::decibelsToGain (-env);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
        }
       #endif
    }

    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& d) override
    {
        if (auto xml = state.copyState().createXml())
            copyXmlToBinary (*xml, d);
    }
    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            state.replaceState (juce::ValueTree::fromXml (*xml));
    }

private:
    juce::AudioProcessorValueTreeState state;
    double sampleRate = 44100;
    float env = 0;
    Biquad bands[4];
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FixtureProcessor();
}
