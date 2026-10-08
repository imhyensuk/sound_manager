#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/Types.h"
#include "smix/mem/MemoryRuntime.h"

namespace smix::ear
{

/**
    Features the instrument recogniser hears: 64 log-mel bands (mean and spread over time)
    plus 8 timbre/dynamics descriptors. The same code produces training data
    (tools/smix_ear_features) and runs inside the plugin, so they always match.
*/
struct EarFeatures
{
    static constexpr int kMelBands = 64;
    static constexpr int kExtra = 8;
    static constexpr int kDim = 2 * kMelBands + kExtra;
    /** Minimum fraction of sounding frames (last feature) for a window to say anything.
        Low on purpose: a kick or tom track rings only a quarter of the time. */
    static constexpr float kMinActive = 0.15f;

    static std::vector<float> extract (const float* mono, std::size_t numSamples, double sampleRate);
};

/**
    A small neural network (MLP) that names what a channel contains (kick, vocal, ...).

    File format "SMXEAR01": magic, uint32 header length, JSON header
    {classes, roles, dims:[in,h1,...,out]}, then float32 feature mean/std and each layer's
    weights [out x in] and biases. Weights are registered as file-backed memopro buffers:
    they are read only while classifying and dropped (and re-read) when memory is short.
*/
class EarModel
{
public:
    explicit EarModel (mem::Runtime* r = nullptr) : runtime (r) {}
    ~EarModel();

    bool load (const std::string& path, std::string& error);
    void unload();
    bool isLoaded() const noexcept { return loaded; }

    struct Guess
    {
        std::string label;    // class label from the model ("kick", "male_vocal", ...)
        std::string display;  // name shown to the user ("남성 보컬"), empty if the model has none
        InstrumentRole role = InstrumentRole::Unknown;
        float probability = 0.0f;
    };

    std::vector<Guess> classify (const std::vector<float>& features, std::size_t topK = 3) const;

    std::uint64_t weightBytes() const noexcept { return bytes; }
    const std::vector<std::string>& classes() const noexcept { return labels; }

    /** Writes a model file (used by tests; training scripts write the same layout). */
    static bool write (const std::string& path, const std::vector<std::string>& classes, const std::vector<std::string>& roles,
                       const std::vector<int>& dims, const std::vector<float>& mean, const std::vector<float>& stdev,
                       const std::vector<std::vector<float>>& weights, const std::vector<std::vector<float>>& biases);

private:
    mem::Runtime* runtime;
    std::vector<std::string> labels;
    std::vector<std::string> displayNames;
    std::vector<InstrumentRole> roles;
    std::vector<int> dims;
    std::string filePath;
    std::uint64_t dataOffset = 0, bytes = 0;
    mem::BufferId weights = mem::kNoBuffer;
    std::vector<float> inlineWeights;  // when there is no runtime
    bool loaded = false;
};

/** Fallback when no trained model is installed: a few spectral/temporal rules. */
std::vector<EarModel::Guess> heuristicGuess (const std::vector<float>& features);

} // namespace smix::ear
