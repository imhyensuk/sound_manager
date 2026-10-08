#pragma once

#include <array>
#include <string>

#include <nlohmann/json.hpp>

namespace smix
{

/** Perceptually motivated frequency regions used everywhere in the engine. */
struct SpectrumBands
{
    static constexpr int kNumBands = 10;
    static constexpr std::array<float, kNumBands + 1> kEdgesHz { 20.f, 60.f, 120.f, 250.f, 500.f, 1000.f,
                                                                 2000.f, 4000.f, 6000.f, 10000.f, 20000.f };
    static constexpr std::array<const char*, kNumBands> kNames { "sub", "bass", "low_mid", "mud", "mid",
                                                                 "upper_mid", "presence", "bite", "sibilance", "air" };

    static float centreHz (int band) noexcept;
    static int bandForFrequency (float hz) noexcept;
    static int bandForName (const std::string& name) noexcept;
};

/** One analysis snapshot of a channel ("what the ear hears right now"). */
struct AudioFeatures
{
    bool valid = false;
    float secondsAnalysed = 0.0f;

    /** Band energies relative to the total spectral energy, in dB (sum of linear shares == 1). */
    std::array<float, SpectrumBands::kNumBands> bandLevelDb {};

    float rmsDb = -120.0f;
    float peakDb = -120.0f;
    float crestDb = 0.0f;              // peak - rms: low = squashed, high = spiky/dynamic
    float shortTermLufs = -120.0f;     // ITU-R BS.1770 K-weighted, 3 s window
    float integratedLufs = -120.0f;    // gated over everything analysed so far
    float loudnessRangeLu = 0.0f;      // spread (p95 - p10) of short-term loudness
    float spectralCentroidHz = 0.0f;   // brightness
    float spectralFlatness = 0.0f;     // 0 = tonal, 1 = noise-like
    float stereoCorrelation = 1.0f;    // -1 .. +1
    float stereoWidth = 0.0f;          // side/mid energy ratio, 0 = mono
    float transientDensity = 0.0f;     // onsets per second
    float transientStrength = 0.0f;    // mean onset flux relative to the average flux (0..~1)

    nlohmann::json toJson() const;
    static AudioFeatures fromJson (const nlohmann::json&);
};

} // namespace smix
