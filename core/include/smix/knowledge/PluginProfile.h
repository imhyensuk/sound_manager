#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/AudioFeatures.h"
#include "smix/ParamSemantics.h"
#include "smix/PluginCatalog.h"

namespace smix::knowledge
{

/** What moving one parameter from low to high actually does to sound (measured). */
struct ParamEffect
{
    int index = -1;
    std::string name;
    ParamRole role = ParamRole::Unknown;
    std::string unit;
    std::string lowText, highText, defaultText;
    std::array<float, SpectrumBands::kNumBands> bandDeltaDb {};  // high minus low setting
    float levelDeltaDb = 0.0f;
    float crestDeltaDb = 0.0f;   // < 0: compresses more at the high setting
    float widthDelta = 0.0f;
    float tailDeltaDb = 0.0f;    // reverb/delay energy after an impulse
    float distortionDelta = 0.0f;
    float sensitivity = 0.0f;    // how audible the parameter is (0 = no measurable effect)
    std::string summary;

    nlohmann::json toJson() const;
    static ParamEffect fromJson (const nlohmann::json&);
};

/** Everything Sound Manager learned about one installed plugin. */
struct PluginProfile
{
    static constexpr int kVersion = 1;

    std::string uid, name, vendor, format;
    PluginCategory declaredCategory = PluginCategory::Unknown;  // from name / host category
    PluginCategory measuredCategory = PluginCategory::Unknown;  // from its behaviour
    int numParams = 0;
    int latencySamples = 0;
    double realtimeFactor = 0.0;  // processing time / audio time (CPU cost)
    double loadMs = 0.0;
    std::uint64_t memoryBytes = 0;  // resident growth when instantiated (estimate)
    std::vector<ParamEffect> effects;
    std::string summary;
    double profiledAt = 0.0;
    int version = kVersion;
    bool failed = false;
    std::string error;

    /** Text indexed for retrieval (Korean + English keywords). */
    std::string document() const;

    nlohmann::json toJson() const;
    static PluginProfile fromJson (const nlohmann::json&);
};

/**
    The plugin under test, as seen by the profiler. Implemented by the host side
    (a JUCE AudioPluginInstance in the out-of-process profiler) and by fakes in tests.
*/
class ProbeTarget
{
public:
    virtual ~ProbeTarget() = default;
    virtual double sampleRate() const = 0;
    virtual int numParams() const = 0;
    virtual std::string paramName (int index) const = 0;
    virtual std::string paramLabel (int index) const = 0;
    virtual float paramDefault (int index) const = 0;
    virtual std::string paramText (int index, float normalised) const = 0;
    virtual bool paramIsDiscrete (int index) const = 0;
    virtual void setParam (int index, float normalised) = 0;
    /** Clears internal state (reverb tails, envelopes). */
    virtual void reset() = 0;
    /** Processes stereo audio in place. */
    virtual void process (float* left, float* right, int numSamples) = 0;
    virtual int latencySamples() const { return 0; }
};

struct ProfilerOptions
{
    int maxParams = 48;            // most relevant parameters to measure
    double stimulusSeconds = 0.6;
    float lowSetting = 0.1f, highSetting = 0.9f;
};

/**
    Measures a plugin with test signals (pink noise, drum-like bursts, an impulse and a sine)
    at low/high settings of each parameter, then describes what each parameter does and what
    kind of processor the plugin really is. Offline; never runs on the audio thread.
*/
class PluginProfiler
{
public:
    using Progress = std::function<void (int done, int total, const std::string& what)>;

    PluginProfile profile (ProbeTarget&, const PluginInfo&, const ProfilerOptions& = {}, const Progress& = {}) const;

    struct Measurement
    {
        std::array<float, SpectrumBands::kNumBands> bandDb {};
        float levelDb = -120.0f, crestDb = 0.0f, width = 0.0f, tailDb = -120.0f, distortion = 0.0f;
    };

    /** One full measurement of the target at its current settings. */
    Measurement measure (ProbeTarget&, const ProfilerOptions&) const;
};

} // namespace smix::knowledge
