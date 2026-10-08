#include "smix/AudioAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace smix
{
namespace
{
constexpr double kPi = 3.141592653589793;

inline float powerToDb (double p) noexcept
{
    return static_cast<float> (10.0 * std::log10 (std::max (p, 1.0e-12)));
}
} // namespace

//==============================================================================
float AudioAnalyzer::rtaBinFrequency (int bin) noexcept
{
    return static_cast<float> (20.0 * std::pow (1000.0, (bin + 0.5) / kRtaBins));
}

float SpectrumBands::centreHz (int band) noexcept
{
    return std::sqrt (kEdgesHz[static_cast<size_t> (band)] * kEdgesHz[static_cast<size_t> (band + 1)]);
}

int SpectrumBands::bandForFrequency (float hz) noexcept
{
    for (int b = 0; b < kNumBands; ++b)
        if (hz < kEdgesHz[static_cast<size_t> (b + 1)])
            return b;
    return kNumBands - 1;
}

int SpectrumBands::bandForName (const std::string& name) noexcept
{
    for (int b = 0; b < kNumBands; ++b)
        if (name == kNames[static_cast<size_t> (b)])
            return b;
    return -1;
}

nlohmann::json AudioFeatures::toJson() const
{
    auto round1 = [] (float v) { return std::round (v * 10.0f) / 10.0f; };
    auto round2 = [] (float v) { return std::round (v * 100.0f) / 100.0f; };

    nlohmann::json bands = nlohmann::json::object();
    for (int b = 0; b < SpectrumBands::kNumBands; ++b)
        bands[SpectrumBands::kNames[static_cast<size_t> (b)]] = round1 (bandLevelDb[static_cast<size_t> (b)]);

    return {
        { "valid", valid },
        { "seconds_analysed", round1 (secondsAnalysed) },
        { "band_level_db", bands },
        { "rms_db", round1 (rmsDb) },
        { "peak_db", round1 (peakDb) },
        { "crest_db", round1 (crestDb) },
        { "short_term_lufs", round1 (shortTermLufs) },
        { "integrated_lufs", round1 (integratedLufs) },
        { "loudness_range_lu", round1 (loudnessRangeLu) },
        { "spectral_centroid_hz", std::round (spectralCentroidHz) },
        { "spectral_flatness", round2 (spectralFlatness) },
        { "stereo_correlation", round2 (stereoCorrelation) },
        { "stereo_width", round2 (stereoWidth) },
        { "transient_density_per_s", round1 (transientDensity) },
        { "transient_strength", round2 (transientStrength) }
    };
}

AudioFeatures AudioFeatures::fromJson (const nlohmann::json& j)
{
    AudioFeatures f;
    f.valid = j.value ("valid", false);
    f.secondsAnalysed = j.value ("seconds_analysed", 0.0f);
    if (j.contains ("band_level_db"))
        for (int b = 0; b < SpectrumBands::kNumBands; ++b)
            f.bandLevelDb[static_cast<size_t> (b)] = j["band_level_db"].value (SpectrumBands::kNames[static_cast<size_t> (b)], -60.0f);
    f.rmsDb = j.value ("rms_db", -120.0f);
    f.peakDb = j.value ("peak_db", -120.0f);
    f.crestDb = j.value ("crest_db", 0.0f);
    f.shortTermLufs = j.value ("short_term_lufs", -120.0f);
    f.integratedLufs = j.value ("integrated_lufs", -120.0f);
    f.loudnessRangeLu = j.value ("loudness_range_lu", 0.0f);
    f.spectralCentroidHz = j.value ("spectral_centroid_hz", 0.0f);
    f.spectralFlatness = j.value ("spectral_flatness", 0.0f);
    f.stereoCorrelation = j.value ("stereo_correlation", 1.0f);
    f.stereoWidth = j.value ("stereo_width", 0.0f);
    f.transientDensity = j.value ("transient_density_per_s", 0.0f);
    f.transientStrength = j.value ("transient_strength", 0.0f);
    return f;
}

//==============================================================================
AudioAnalyzer::AudioAnalyzer()
{
    prepare (44100.0);
}

void AudioAnalyzer::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0 ? newSampleRate : 44100.0;

    window.resize (kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<size_t> (i)] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * i / (kFftSize - 1)));

    fifo.assign (kFftSize, 0.0f);
    fftBuffer.assign (kFftSize, {});
    prevMagnitude.assign (kFftSize / 2, 0.0f);
    binToBand.assign (kFftSize / 2, -1);
    for (int bin = 1; bin < kFftSize / 2; ++bin)
    {
        const float hz = static_cast<float> (bin * sampleRate / kFftSize);
        if (hz >= SpectrumBands::kEdgesHz.front() && hz < SpectrumBands::kEdgesHz.back())
            binToBand[static_cast<size_t> (bin)] = SpectrumBands::bandForFrequency (hz);
    }

    // ITU-R BS.1770 K-weighting, coefficients derived for any sample rate.
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (kPi * f0 / sampleRate);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        for (auto& f : preFilter)
        {
            f.b0 = (Vh + Vb * K / Q + K * K) / a0;
            f.b1 = 2.0 * (K * K - Vh) / a0;
            f.b2 = (Vh - Vb * K / Q + K * K) / a0;
            f.a1 = 2.0 * (K * K - 1.0) / a0;
            f.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (kPi * f0 / sampleRate);
        const double a0 = 1.0 + K / Q + K * K;
        for (auto& f : rlbFilter)
        {
            f.b0 = 1.0; f.b1 = -2.0; f.b2 = 1.0;
            f.a1 = 2.0 * (K * K - 1.0) / a0;
            f.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }

    blockLength = std::max (1, static_cast<int> (sampleRate * 0.1));

    double windowSum = 0;
    for (auto w : window) windowSum += w;
    fftNorm = static_cast<float> (2.0 / windowSum);
    rtaBinRanges.assign (kRtaBins, { 1, 1 });
    for (int k = 0; k < kRtaBins; ++k)
    {
        const double f0 = 20.0 * std::pow (1000.0, static_cast<double> (k) / kRtaBins);
        const double f1 = 20.0 * std::pow (1000.0, static_cast<double> (k + 1) / kRtaBins);
        const int b0 = std::clamp (static_cast<int> (f0 * kFftSize / sampleRate), 1, kFftSize / 2 - 1);
        const int b1 = std::clamp (static_cast<int> (f1 * kFftSize / sampleRate), b0, kFftSize / 2 - 1);
        rtaBinRanges[static_cast<size_t> (k)] = { b0, b1 };
    }
    reset();
}

void AudioAnalyzer::reset()
{
    std::fill (fifo.begin(), fifo.end(), 0.0f);
    std::fill (prevMagnitude.begin(), prevMagnitude.end(), 0.0f);
    fifoPos = samplesSinceHop = 0;
    bandPower.fill (0.0);
    centroidAcc = flatnessAcc = fluxMean = 0;
    onsetHoldFrames = 0;
    onsetCount = onsetFluxSum = 0;
    sumSquares = peak = 0;
    smoothedPower = peakHold = smoothedWidth = 0;
    smoothedCorrelation = 1;
    mm = ss = lr = ll = rr = 0;
    for (auto& f : preFilter) f.clear();
    for (auto& f : rlbFilter) f.clear();
    blockPower = 0;
    blockSamples = 0;
    stBlocks.fill (0.0);
    stIndex = stCount = 0;
    history.fill (0.0f);
    historyWrite = historyCount = 0;
    gatedPowerSum = 0;
    gatedBlockCount = 0;
    totalSeconds = 0;
    framesAnalysed = 0;
}

void AudioAnalyzer::process (const float* left, const float* right, int numSamples) noexcept
{
    const int numChannels = right != nullptr ? 2 : 1;

    for (int i = 0; i < numSamples; ++i)
    {
        const float l = left[i];
        const float r = right != nullptr ? right[i] : l;

        // Spectral path (mono sum)
        fifo[static_cast<size_t> (fifoPos)] = 0.5f * (l + r);
        fifoPos = (fifoPos + 1) & (kFftSize - 1);
        if (++samplesSinceHop >= kHop)
        {
            samplesSinceHop = 0;
            analyseFrame();
        }

        // Level / stereo path
        peakL = std::max (peakL, static_cast<double> (std::abs (l)));
        peakR = std::max (peakR, static_cast<double> (std::abs (r)));
        sumL += static_cast<double> (l) * l;
        sumR += static_cast<double> (r) * r;
        const double m = 0.5 * (l + r), s = 0.5 * (l - r);
        sumSquares += 0.5 * (static_cast<double> (l) * l + static_cast<double> (r) * r);
        peak = std::max (peak, static_cast<double> (std::max (std::abs (l), std::abs (r))));
        mm += m * m; ss += s * s;
        lr += static_cast<double> (l) * r; ll += static_cast<double> (l) * l; rr += static_cast<double> (r) * r;

        // Loudness path
        const float kl = rlbFilter[0].process (preFilter[0].process (l));
        blockPower += static_cast<double> (kl) * kl;
        if (numChannels == 2)
        {
            const float kr = rlbFilter[1].process (preFilter[1].process (r));
            blockPower += static_cast<double> (kr) * kr;
        }

        if (++blockSamples >= blockLength)
            finishLoudnessBlock();
    }
}

void AudioAnalyzer::analyseFrame() noexcept
{
    for (int i = 0; i < kFftSize; ++i)
    {
        const int idx = (fifoPos + i) & (kFftSize - 1);
        fftBuffer[static_cast<size_t> (i)] = { fifo[static_cast<size_t> (idx)] * window[static_cast<size_t> (i)], 0.0f };
    }
    fft.perform (fftBuffer.data());

    std::array<double, SpectrumBands::kNumBands> framePower {};
    double total = 0, weighted = 0, logSum = 0, flux = 0;
    int counted = 0;

    for (int bin = 1; bin < kFftSize / 2; ++bin)
    {
        const float mag = std::abs (fftBuffer[static_cast<size_t> (bin)]);
        const float prev = prevMagnitude[static_cast<size_t> (bin)];
        prevMagnitude[static_cast<size_t> (bin)] = mag;

        const int band = binToBand[static_cast<size_t> (bin)];
        if (band < 0)
            continue;

        const double p = static_cast<double> (mag) * mag;
        framePower[static_cast<size_t> (band)] += p;
        total += p;
        weighted += p * (bin * sampleRate / kFftSize);
        logSum += std::log (p + 1.0e-20);
        flux += std::max (0.0f, mag - prev);
        ++counted;
    }

    if (visualsEnabled.load (std::memory_order_relaxed))
    {
        RtaFrame frame;
        for (int k = 0; k < kRtaBins; ++k)
        {
            const auto [b0, b1] = rtaBinRanges[static_cast<size_t> (k)];
            float mag = 0.0f;
            for (int b = b0; b <= b1; ++b)
                mag = std::max (mag, std::abs (fftBuffer[static_cast<size_t> (b)]));
            frame.db[static_cast<size_t> (k)] = 20.0f * std::log10 (std::max (mag * fftNorm, 1.0e-7f));
        }
        rtaRing.push (frame);
    }

    // Ignore silence so that the noise floor does not pollute the picture.
    if (total < 1.0e-9 || counted == 0)
        return;

    const double framesPerSecond = sampleRate / kHop;
    const double alpha = framesAnalysed == 0 ? 1.0 : std::min (1.0, 1.0 / (framesPerSecond * 3.0));

    for (size_t b = 0; b < bandPower.size(); ++b)
        bandPower[b] += alpha * (framePower[b] - bandPower[b]);

    centroidAcc += alpha * (weighted / total - centroidAcc);
    const double flatness = std::exp (logSum / counted) / (total / counted);
    flatnessAcc += alpha * (flatness - flatnessAcc);

    // Onset detection on positive spectral flux.
    const bool isOnset = framesAnalysed > 4 && onsetHoldFrames == 0 && flux > 1.6 * fluxMean + 1.0e-6;
    if (isOnset)
    {
        onsetHoldFrames = static_cast<int> (framesPerSecond * 0.05) + 1;  // 50 ms refractory period
        const double strength = std::min (1.0, (flux / std::max (fluxMean, 1.0e-9) - 1.0) / 4.0);
        onsetFluxSum += alpha * 10.0 * (strength - onsetFluxSum);
    }
    else if (onsetHoldFrames > 0)
    {
        --onsetHoldFrames;
    }

    onsetCount += alpha * ((isOnset ? framesPerSecond : 0.0) - onsetCount);
    fluxMean += (framesAnalysed == 0 ? 1.0 : 0.05) * (flux - fluxMean);
    ++framesAnalysed;
}

void AudioAnalyzer::finishLoudnessBlock() noexcept
{
    const double ms = blockPower / blockSamples;
    stBlocks[static_cast<size_t> (stIndex)] = ms;
    stIndex = (stIndex + 1) % kShortTermBlocks;
    stCount = std::min (stCount + 1, kShortTermBlocks);
    totalSeconds += blockSamples / sampleRate;

    double stPower = 0;
    for (int i = 0; i < stCount; ++i)
        stPower += stBlocks[static_cast<size_t> (i)];
    stPower /= std::max (1, stCount);

    const float stLufs = static_cast<float> (-0.691 + 10.0 * std::log10 (std::max (stPower, 1.0e-12)));

    if (stLufs > -70.0f)
    {
        history[static_cast<size_t> (historyWrite)] = stLufs;
        historyWrite = (historyWrite + 1) % kHistory;
        historyCount = std::min (historyCount + 1, kHistory);

        // Two-stage gating (absolute -70 LUFS, relative -10 LU) on the block powers.
        const double currentIntegrated = gatedBlockCount > 0
            ? -0.691 + 10.0 * std::log10 (gatedPowerSum / gatedBlockCount) : -70.0;
        const double blockLufs = -0.691 + 10.0 * std::log10 (std::max (ms, 1.0e-12));
        if (blockLufs > -70.0 && blockLufs > currentIntegrated - 10.0)
        {
            gatedPowerSum += ms;
            ++gatedBlockCount;
        }
    }

    if (visualsEnabled.load (std::memory_order_relaxed))
    {
        MeterFrame mf;
        auto db = [] (double v) { return static_cast<float> (20.0 * std::log10 (std::max (v, 1.0e-6))); };
        mf.peakL = db (peakL);
        mf.peakR = db (peakR);
        mf.rmsL = db (std::sqrt (sumL / blockSamples));
        mf.rmsR = db (std::sqrt (sumR / blockSamples));
        double momentary = 0;
        const int count = std::min (4, stCount);
        for (int i = 1; i <= count; ++i)
            momentary += stBlocks[static_cast<size_t> ((stIndex - i + kShortTermBlocks) % kShortTermBlocks)];
        mf.momentaryLufs = static_cast<float> (-0.691 + 10.0 * std::log10 (std::max (momentary / std::max (1, count), 1.0e-12)));
        mf.shortTermLufs = stLufs;
        const double denom = std::sqrt (ll * rr);
        mf.correlation = denom > 1.0e-12 ? static_cast<float> (lr / denom) : 1.0f;
        meterRing.push (mf);
    }
    peakL = peakR = sumL = sumR = 0;

    blockPower = 0;
    blockSamples = 0;
    publish();
}

void AudioAnalyzer::publish() noexcept
{
    auto& slot = exchange.writeSlot();
    auto& f = slot.features;

    const double total = [this] { double t = 0; for (auto p : bandPower) t += p; return t; }();
    f.valid = framesAnalysed > 8;
    f.secondsAnalysed = static_cast<float> (totalSeconds);
    for (size_t b = 0; b < bandPower.size(); ++b)
        f.bandLevelDb[b] = total > 0 ? powerToDb (bandPower[b] / total) : -120.0f;

    // Levels: smoothed per 100 ms block with ~1 s memory, peak with slow release.
    // The smoothing state lives in members because each publish fills a different slot.
    static constexpr double kLevelAlpha = 0.1;
    const double blockMs = sumSquares / std::max (1, blockLength);
    smoothedPower += kLevelAlpha * (blockMs - smoothedPower);
    peakHold = std::max (peak, peakHold * 0.97);  // ~ -0.26 dB per 100 ms release
    f.rmsDb = powerToDb (smoothedPower);
    f.peakDb = static_cast<float> (20.0 * std::log10 (std::max (peakHold, 1.0e-6)));
    f.crestDb = f.rmsDb > -100.0f ? f.peakDb - f.rmsDb : 0.0f;

    const double denom = std::sqrt (ll * rr);
    if (denom > 1.0e-12)
    {
        smoothedCorrelation += kLevelAlpha * (lr / denom - smoothedCorrelation);
        smoothedWidth += kLevelAlpha * ((mm > 1.0e-12 ? ss / mm : 0.0) - smoothedWidth);
    }
    f.stereoCorrelation = static_cast<float> (smoothedCorrelation);
    f.stereoWidth = static_cast<float> (smoothedWidth);

    double stPower = 0;
    for (int i = 0; i < stCount; ++i)
        stPower += stBlocks[static_cast<size_t> (i)];
    stPower /= std::max (1, stCount);
    f.shortTermLufs = static_cast<float> (-0.691 + 10.0 * std::log10 (std::max (stPower, 1.0e-12)));
    f.integratedLufs = gatedBlockCount > 0
        ? static_cast<float> (-0.691 + 10.0 * std::log10 (gatedPowerSum / gatedBlockCount)) : -120.0f;

    f.spectralCentroidHz = static_cast<float> (centroidAcc);
    f.spectralFlatness = static_cast<float> (flatnessAcc);
    f.transientDensity = static_cast<float> (onsetCount);
    f.transientStrength = static_cast<float> (std::clamp (onsetFluxSum, 0.0, 1.0));

    slot.history = history;
    slot.historyCount = historyCount;
    slot.gatedPowerSum = gatedPowerSum;
    slot.gatedBlockCount = gatedBlockCount;

    exchange.publish();

    sumSquares = 0;
    peak = 0;
    mm = ss = lr = ll = rr = 0;
}

AudioFeatures AudioAnalyzer::snapshot()
{
    exchange.update();
    const auto& p = exchange.read();
    AudioFeatures f = p.features;

    if (p.historyCount > 10)
    {
        std::vector<float> values (p.history.begin(), p.history.begin() + p.historyCount);
        const float gate = f.integratedLufs - 20.0f;
        values.erase (std::remove_if (values.begin(), values.end(), [gate] (float v) { return v < gate; }), values.end());
        if (values.size() > 4)
        {
            std::sort (values.begin(), values.end());
            const auto at = [&values] (double q) { return values[static_cast<size_t> (q * (values.size() - 1))]; };
            f.loudnessRangeLu = at (0.95) - at (0.10);
        }
    }
    return f;
}

} // namespace smix
