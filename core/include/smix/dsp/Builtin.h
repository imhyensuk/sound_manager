#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "smix/Types.h"

namespace smix::knowledge
{
struct PluginProfile;
}

namespace smix::dsp
{

/**
    Sound Manager's own processors. They are always available, need no third-party plugin and
    are fully understood by the AI (exact parameter meaning, units and ranges). In Logic Pro they
    are essential: Logic's stock plugins cannot be hosted or controlled by another plugin.

    Plain C++ (no JUCE): the plugin wraps them as hostable plugins, the learner uses the very same
    code to model what an engineer did, and the tests render known settings through them.
*/
enum class Kind
{
    Gain, Gate, EQ, DeEsser, Compressor, Saturation, Enveloper, Reverb, Limiter
};

constexpr int kNumKinds = 9;

struct ParamSpec
{
    std::string id;     // stable id ("threshold")
    std::string name;   // display name, chosen so the parameter semantics are recognised ("Threshold")
    std::string unit;   // "dB", "Hz", "ms", "s", "%", "ratio", "" (choice / toggle)
    float min = 0.0f, max = 1.0f, def = 0.0f;
    float centre = 0.0f;               // skew: value shown at the middle of the knob (0 = linear)
    std::vector<std::string> choices;  // a choice parameter (index stored as the value)
    bool toggle = false;
    std::string help;                  // Korean explanation (knowledge base / tooltips)
};

std::string toString (Kind);               // "compressor"
std::string displayName (Kind);            // "SM Compressor"
std::string koreanName (Kind);             // "컴프레서"
std::string description (Kind);            // Korean, what it is for (knowledge base)
std::string uidFor (Kind);                 // "SoundManager-smix.compressor"
std::optional<Kind> kindFromString (const std::string&);  // "compressor" / "smix.compressor" / uid
PluginCategory categoryOf (Kind);
const std::vector<ParamSpec>& specsFor (Kind);

float toNormalised (const ParamSpec&, float value);
float fromNormalised (const ParamSpec&, float normalised);
std::string formatValue (const ParamSpec&, float value);  // "-12.0 dB", "4.0:1", "1.20 kHz", "35 %"

class Processor
{
public:
    explicit Processor (Kind);
    virtual ~Processor() = default;

    Kind kind() const noexcept { return processorKind; }
    const std::vector<ParamSpec>& specs() const noexcept { return specsFor (processorKind); }
    int numParams() const noexcept { return static_cast<int> (specs().size()); }
    int indexOf (const std::string& id) const;

    /** Thread-safe (message thread sets, audio thread reads at the next block). Values in real units. */
    void set (int index, float value);
    void set (const std::string& id, float value);
    float get (int index) const;
    float get (const std::string& id) const;
    void resetToDefaults();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();
    /** In-place, any number of channels up to the prepared count. Real-time safe. */
    void process (float* const* channels, int numChannels, int numSamples) noexcept;

    virtual int latencySamples() const { return 0; }
    virtual double tailSeconds() const { return 0.0; }
    /** Current gain reduction (dB, >= 0) for meters and learning. */
    float reductionDb() const noexcept { return meter.load (std::memory_order_relaxed); }

protected:
    virtual void onPrepare() {}
    virtual void onReset() {}
    virtual void onParams() {}
    virtual void onProcess (float* const* channels, int numChannels, int numSamples) noexcept = 0;

    float p (int index) const { return cached[static_cast<size_t> (index)]; }

    double sampleRate = 48000.0;
    int maxBlock = 512;
    int channelCount = 2;
    std::atomic<float> meter { 0.0f };

private:
    Kind processorKind;
    std::unique_ptr<std::atomic<float>[]> values;
    std::vector<float> cached;
    std::atomic<bool> dirty { true };
};

std::unique_ptr<Processor> create (Kind);

/**
    Adds what Sound Manager knows by construction to a measured profile of a built-in processor:
    the Korean description, every parameter's meaning, and the true category. Returns false for
    other plugins.
*/
bool annotateProfile (knowledge::PluginProfile&);

/** Renders a whole signal through a processor (offline; used by the learner and tests). */
void renderOffline (Processor&, std::vector<std::vector<float>>& channels, double sampleRate, int blockSize = 512);

} // namespace smix::dsp
