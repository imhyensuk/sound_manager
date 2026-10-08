#pragma once

#include <array>
#include <complex>
#include <vector>

#include "smix/AudioFeatures.h"
#include "smix/FFT.h"
#include "smix/SpscRing.h"
#include "smix/TripleBuffer.h"

namespace smix
{

/**
    Real-time safe "ear" of the plugin.

    process() is called on the audio thread and never allocates or locks.
    snapshot() may be called from any single other thread and returns the
    latest analysis (published every 100 ms).
*/
class AudioAnalyzer
{
public:
    AudioAnalyzer();

    /** Must be called before process(); allocates. Not real-time safe. */
    void prepare (double sampleRate);
    void reset();

    /** right may be nullptr for mono. */
    void process (const float* left, const float* right, int numSamples) noexcept;

    /** Consumer side; returns the newest published features. */
    AudioFeatures snapshot();

    //==============================================================================
    // Visualisation stream (RTA, waterfall, meters). Only computed while a view is open.
    static constexpr int kRtaBins = 128;  // log-spaced 20 Hz .. 20 kHz

    struct RtaFrame
    {
        std::array<float, kRtaBins> db {};  // dBFS per bin
    };

    struct MeterFrame
    {
        float peakL = -120, peakR = -120, rmsL = -120, rmsR = -120;
        float momentaryLufs = -120, shortTermLufs = -120, correlation = 1;
    };

    void setVisualsEnabled (bool on) noexcept { visualsEnabled.store (on, std::memory_order_relaxed); }
    bool visualsAreEnabled() const noexcept { return visualsEnabled.load (std::memory_order_relaxed); }
    bool popRta (RtaFrame& f) noexcept { return rtaRing.pop (f); }
    bool popMeter (MeterFrame& m) noexcept { return meterRing.pop (m); }
    static float rtaBinFrequency (int bin) noexcept;

private:
    static constexpr int kFftOrder = 11;
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kHop = kFftSize / 2;
    static constexpr int kShortTermBlocks = 30;   // 30 x 100 ms = 3 s
    static constexpr int kHistory = 600;          // 60 s of short-term loudness

    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        float process (float x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return static_cast<float> (y);
        }
        void clear() noexcept { z1 = z2 = 0; }
    };

    struct Published
    {
        AudioFeatures features;
        std::array<float, kHistory> history {};
        int historyCount = 0;
        double gatedPowerSum = 0;
        long gatedBlockCount = 0;
    };

    void analyseFrame() noexcept;
    void finishLoudnessBlock() noexcept;
    void publish() noexcept;

    double sampleRate = 44100.0;
    FFT fft { kFftOrder };
    std::vector<float> window, fifo;
    std::vector<std::complex<float>> fftBuffer;
    std::vector<float> prevMagnitude;
    std::vector<int> binToBand;
    int fifoPos = 0;
    int samplesSinceHop = 0;

    // Smoothed spectral state
    std::array<double, SpectrumBands::kNumBands> bandPower {};
    double centroidAcc = 0, flatnessAcc = 0;
    double fluxMean = 0;
    int onsetHoldFrames = 0;
    double onsetCount = 0, onsetFluxSum = 0;

    // Level / stereo state
    double sumSquares = 0, peak = 0;
    double mm = 0, ss = 0, lr = 0, ll = 0, rr = 0;
    double smoothedPower = 0, peakHold = 0, smoothedCorrelation = 1, smoothedWidth = 0;

    // Loudness state
    std::array<Biquad, 2> preFilter, rlbFilter;
    double blockPower = 0;
    int blockSamples = 0, blockLength = 4410;
    std::array<double, kShortTermBlocks> stBlocks {};
    int stIndex = 0, stCount = 0;
    std::array<float, kHistory> history {};
    int historyWrite = 0, historyCount = 0;
    double gatedPowerSum = 0;
    long gatedBlockCount = 0;
    double totalSeconds = 0;
    long framesAnalysed = 0;

    TripleBuffer<Published> exchange;

    std::atomic<bool> visualsEnabled { false };
    SpscRing<RtaFrame, 64> rtaRing;
    SpscRing<MeterFrame, 64> meterRing;
    std::vector<std::pair<int, int>> rtaBinRanges;  // FFT bin range per RTA bin
    float fftNorm = 1.0f;
    double peakL = 0, peakR = 0, sumL = 0, sumR = 0;
};

} // namespace smix
