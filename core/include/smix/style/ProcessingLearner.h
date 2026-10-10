#pragma once

#include <array>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace smix::style
{

/**
    How an engineer processed one track, expressed as settings of Sound Manager's built-in
    processors (smix::dsp) - learned by comparing the raw track with the same track after its
    insert chain ("processed" stem).

    Method: every module is fitted by simulation. Candidate settings are rendered through the very
    processors the plugin uses, on the user's own raw audio, and compared with what the engineer's
    chain produced. The settings that reproduce the processed track best are kept.

    Thresholds are stored relative to the raw input's active loudness (median short-term LUFS of
    the parts that play), so they transfer to tracks recorded at another level.
*/
constexpr int kEqCurveBands = 31;  // 1/3 octave, 20 Hz .. 20 kHz
const std::array<float, kEqCurveBands>& eqCurveCentres();

struct EqSettings
{
    bool used = false;
    float lowCutHz = 0.0f;  // 0 = off
    bool steep = false;     // 24 dB/oct
    struct Band { float freq = 1000.0f, gain = 0.0f, q = 1.0f; };
    std::array<Band, 6> bands {};  // [0] low shelf, [1..4] peaks, [5] high shelf
    float highCutHz = 0.0f;  // 0 = off
    float errorDb = 0.0f;    // RMS distance of the fitted EQ from the measured curve
};

struct GateSettings    { bool used = false; float thresholdRel = -30, rangeDb = 20, attackMs = 0.5f, holdMs = 40, releaseMs = 150; };
struct DeEsserSettings { bool used = false; float freqHz = 6500, thresholdRel = -20, rangeDb = 6; };
struct CompSettings
{
    bool used = false;
    float thresholdRel = -10, ratio = 3, attackMs = 10, releaseMs = 120, kneeDb = 6;
    float avgReductionDb = 0;  // typical gain reduction on the active parts
};
struct SaturationSettings { bool used = false; float driveDb = 0; int type = 0; };
struct EnvelopeSettings   { bool used = false; float attackDb = 0, sustainDb = 0; };
struct ReverbSettings
{
    bool used = false;
    float predelayMs = 20, decayS = 1.6f, sizePct = 60, dampingHz = 7000, lowCutHz = 150, mixPct = 15;
    bool fromReturn = false;  // learned from a reverb return (send) rather than an insert
};

struct ProcessingSettings
{
    float inputLevelDb = -20.0f;  // active loudness of the raw input (reference for thresholds)
    float gainDb = 0.0f;          // static level change of the chain (trim / makeup / fader)
    std::array<float, kEqCurveBands> eqCurveDb {};  // measured tonal change (raw -> processed), 0 dB around 1 kHz
    std::array<bool, kEqCurveBands> eqCurveValid {};
    EqSettings eq;
    GateSettings gate;
    DeEsserSettings deesser;
    CompSettings comp;
    SaturationSettings saturation;
    EnvelopeSettings envelope;
    ReverbSettings reverb;
    float pan = 0.0f;         // -100..100 (stereo processed tracks only)
    float widthPct = 100.0f;
    float fitDb = 0.0f;       // RMS level error (10 ms frames) of the re-created chain vs the processed track
    int latencySamples = 0;   // the engineer's chain latency (found by alignment)
    int tracks = 1;           // how many tracks were combined into this (genre profiles)

    nlohmann::json toJson() const;
    static ProcessingSettings fromJson (const nlohmann::json&);
    /** Korean one-liner per module ("컴프 3.2:1, 어택 12 ms ..."). */
    std::string describe() const;
};

struct LearnOptions
{
    double maxAnalysisSeconds = 24.0;  // simulations run on the most active excerpt of this length
    bool learnSaturation = true;
    bool learnEnvelope = true;
    bool learnReverb = true;
};

using Audio = std::vector<std::vector<float>>;  // channels x samples

/** Learns the processing that turned `raw` into `processed` (same take, any channel counts). */
ProcessingSettings learnProcessing (const Audio& raw, const Audio& processed, double sampleRate, const LearnOptions& = {});

/** Renders `audio` through the built-in processors with these settings (thresholds use inputLevelDb). */
void renderChain (const ProcessingSettings&, Audio& audio, double sampleRate);

/** Fits low cut / shelves / 4 peaks / high cut to a measured 1/3-octave curve. */
EqSettings fitEq (const std::array<float, kEqCurveBands>& curveDb, const std::array<bool, kEqCurveBands>& valid);

/** Magnitude (dB) of the built-in EQ with these settings at a frequency. */
float eqResponseDb (const EqSettings&, double freqHz, double sampleRate = 48000.0);

/** Active loudness: median short-term LUFS of the sounding parts (same meter as the plugin). */
float activeLoudness (const Audio&, double sampleRate);

/** Median over several tracks of one role (EQ refitted on the median curve). */
ProcessingSettings combineProcessing (const std::vector<ProcessingSettings>&);

/** What learnReturn needs from one (processed) track: compact, so whole songs fit in memory. */
struct SendSource
{
    std::vector<double> energy;  // 400 Hz - 6 kHz energy per 50 ms
    std::vector<std::array<double, 10>> bands;  // SpectrumBands energies per 0.4 s frame
};
SendSource makeSendSource (const Audio&, double sampleRate);

/**
    A reverb return (aux) and the processed tracks that may feed it: estimates the return's decay
    (from how it falls while the sources are silent) and tone, and every track's send amount,
    expressed as the mix of an insert reverb with the same decay. Results are written into each
    track's settings.reverb (fromReturn = true) unless the track already has an insert reverb.
    `dryMixGainDb[i]` / `returnMixGainDb` are the gains of the track / return files into the final mix: 0 dB when
    both were exported post-fader (the recommended export, see training/DATA.md). A source must explain at
    least 5 % of the return to count as sent.
*/
void learnReturn (const Audio& ret, const std::vector<SendSource>& sources, double sampleRate,
                  const std::vector<float>& dryMixGainDb, float returnMixGainDb, std::vector<ProcessingSettings*>& out);

} // namespace smix::style
