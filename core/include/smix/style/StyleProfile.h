#pragma once

#include <array>
#include <complex>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/AudioFeatures.h"
#include "smix/FFT.h"
#include "smix/MixAction.h"
#include "smix/RecipeEngine.h"

namespace smix::style
{

constexpr int kThirdOctaveBands = 31;  // 20 Hz .. 20 kHz

float thirdOctaveCentreHz (int band);

/** The sound of a reference track, reduced to what a mixing engineer would match. */
struct StyleProfile
{
    std::string name;
    std::string sourceFile;
    double seconds = 0.0;

    std::array<float, kThirdOctaveBands> thirdOctaveDb {};      // relative to total energy
    std::array<float, SpectrumBands::kNumBands> bandLevelDb {};  // same scale as AudioFeatures

    float integratedLufs = -70.0f;
    float loudnessRangeLu = 0.0f;
    float peakDb = -120.0f;
    float plrDb = 0.0f;              // peak to loudness ratio: low = heavily limited
    float crestDb = 0.0f;
    float widthLow = 0.0f, widthMid = 0.0f, widthHigh = 0.0f;  // side/mid energy per region
    float correlation = 1.0f;
    float transientDensity = 0.0f;  // onsets per second
    float tiltDbPerOctave = 0.0f;   // spectral slope 100 Hz - 10 kHz
    float centroidHz = 0.0f;

    /** Engineer words for the reference ("밝음", "넓음", "강하게 압축됨"...). */
    std::vector<std::string> descriptors() const;

    nlohmann::json toJson() const;
    static StyleProfile fromJson (const nlohmann::json&);
};

/**
    Streaming analysis of a whole reference track (any length, constant memory):
    feed it chunk by chunk while decoding, then call finish().
*/
class StyleAnalyzer
{
public:
    explicit StyleAnalyzer (double sampleRate);

    void process (const float* left, const float* right, int numSamples);
    StyleProfile finish (const std::string& name, const std::string& sourceFile);
    double secondsProcessed() const noexcept { return samplesTotal / sampleRate; }

private:
    void analyseFrame();
    void finishBlock();

    static constexpr int kOrder = 12, kSize = 1 << kOrder, kHop = kSize / 2;
    double sampleRate;
    FFT fft { kOrder };
    std::vector<float> window, fifoL, fifoR;
    std::vector<std::complex<float>> bufMid, bufSide;
    std::vector<float> prevMag;
    std::vector<int> binThird, binBand;
    int fifoPos = 0, sinceHop = 0;

    std::array<double, kThirdOctaveBands> thirdPower {};
    std::array<double, SpectrumBands::kNumBands> bandPower {};
    double midLow = 0, sideLow = 0, midMid = 0, sideMid = 0, midHigh = 0, sideHigh = 0;
    double lr = 0, ll = 0, rr = 0;
    double centroidNum = 0, centroidDen = 0;
    double fluxMean = 0;
    long onsets = 0, frames = 0, hold = 0;

    // Loudness (BS.1770): 100 ms blocks -> 400 ms momentary for gating, 3 s short-term for LRA.
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };
    Biquad pre[2], rlb[2];
    double blockPower = 0;
    long blockSamples = 0, blockLength = 4800;
    std::vector<double> blocks;     // 100 ms mean squares (8 bytes per 100 ms: ~5 KB for 10 minutes)
    double peak = 0, sumSquares = 0;
    double samplesTotal = 0;
};

/** What matching a reference means for the mix right now. */
struct StyleMatch
{
    std::vector<MixAction> actions;                       // master/bus moves towards the reference
    std::vector<std::string> notes;                       // explanations shown to the user
    std::map<InstrumentRole, std::vector<Goal>> roleGoals;  // default per-instrument style derived from it
};

/**
    Compares a bus/master channel with the reference and proposes bounded moves:
    broad tonal balance (EQ), density (compression), loudness (limiter) and width.
*/
StyleMatch matchStyle (const StyleProfile& reference, const ChannelState& target, float amount = 1.0f);

/** Per-instrument goals that reproduce the character of a reference. */
std::map<InstrumentRole, std::vector<Goal>> styleGoalsFor (const StyleProfile& reference);

} // namespace smix::style
