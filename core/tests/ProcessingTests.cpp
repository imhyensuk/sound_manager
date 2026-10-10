#include <doctest/doctest.h>

#include <cmath>
#include <iostream>
#include <random>

#include <smix/style/ProcessingLearner.h>

using namespace smix::style;

namespace
{
constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/** Sung phrases: harmonic tone with vibrato, syllable envelope, sibilant noise bursts, quiet room noise. */
Audio vocal (double seconds)
{
    std::mt19937 rng (7);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    const size_t n = static_cast<size_t> (seconds * kSr);
    Audio a (1, std::vector<float> (n));
    double phase = 0, hp = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / kSr;
        const double phrase = std::fmod (t, 3.0);
        const double f0 = 220.0 * (1.0 + 0.01 * std::sin (2 * kPi * 5.5 * t)) * (phrase < 1.5 ? 1.0 : 1.26);
        phase += 2 * kPi * f0 / kSr;
        double v = 0;
        for (int h = 1; h <= 12; ++h) v += std::sin (phase * h) / (h * 0.8);
        // syllables: 4 per second, loud and soft ones
        const double syl = std::fmod (t, 0.25) / 0.25;
        const double env = (phrase < 2.6 ? 1.0 : 0.0) * std::sin (kPi * syl) * (0.35 + 0.65 * (0.5 + 0.5 * std::sin (2 * kPi * 0.37 * t)));
        // sibilance burst at the start of every 3rd syllable
        const double sylIndex = std::floor (t / 0.25);
        float s = 0;
        if (std::fmod (sylIndex, 3.0) == 0 && syl < 0.25 && phrase < 2.6)
        {
            const float w = nd (rng);
            hp = 0.2 * hp + 0.8 * w;  // crude high-pass-ish noise
            s = static_cast<float> ((w - hp) * 0.6);
        }
        a[0][i] = static_cast<float> (0.25 * v * env) + s + 0.0005f * nd (rng);
    }
    return a;
}

/** Kick drum with bleed from the rest of the kit between the hits. */
Audio kick (double seconds)
{
    std::mt19937 rng (11);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    const size_t n = static_cast<size_t> (seconds * kSr);
    Audio a (1, std::vector<float> (n));
    double lp = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / kSr;
        const double tb = std::fmod (t, 0.5);
        const double accent = std::fmod (std::floor (t / 0.5), 4.0) == 0 ? 1.0 : 0.7;
        const double body = std::sin (2 * kPi * (50.0 * tb + 60.0 * (1 - std::exp (-tb * 40)) / 40)) * std::exp (-tb * 14) * accent;
        const double click = tb < 0.004 ? (1 - tb / 0.004) * 0.6 * accent : 0.0;
        lp = 0.9 * lp + 0.1 * nd (rng);
        a[0][i] = static_cast<float> (0.6 * (body + click)) + 0.01f * static_cast<float> (lp);  // bleed ~ -45 dB
    }
    return a;
}
} // namespace

TEST_CASE ("EQ fitting recovers low cut, bells and shelves from a curve")
{
    EqSettings truth;
    truth.lowCutHz = 120.0f;
    truth.bands[0] = { 100.0f, 0.0f, 0.707f };
    truth.bands[1] = { 400.0f, -4.0f, 1.4f };
    truth.bands[2] = { 3000.0f, 3.0f, 1.0f };
    truth.bands[3] = { 2500.0f, 0.0f, 1.0f };
    truth.bands[4] = { 6000.0f, 0.0f, 1.0f };
    truth.bands[5] = { 9000.0f, 2.5f, 0.707f };
    std::array<float, kEqCurveBands> curve {};
    std::array<bool, kEqCurveBands> valid {};
    for (int b = 0; b < kEqCurveBands; ++b)
    {
        curve[static_cast<size_t> (b)] = eqResponseDb (truth, eqCurveCentres()[static_cast<size_t> (b)]);
        valid[static_cast<size_t> (b)] = true;
    }
    const auto fit = fitEq (curve, valid);
    CHECK (fit.used);
    CHECK (fit.errorDb < 0.6f);
    CHECK (fit.lowCutHz > 80.0f);
    CHECK (fit.lowCutHz < 180.0f);
    for (float hz : { 60.0f, 400.0f, 3000.0f, 10000.0f })
        CHECK (std::abs (eqResponseDb (fit, hz) - eqResponseDb (truth, hz)) < 1.5f);
}

TEST_CASE ("processing learner: vocal chain (EQ, de-esser, compressor, saturation)")
{
    const auto raw = vocal (14.0);
    ProcessingSettings truth;
    truth.inputLevelDb = activeLoudness (raw, kSr);
    truth.eq.used = true;
    truth.eq.lowCutHz = 100.0f;
    truth.eq.bands[0] = { 100.0f, 0.0f, 0.707f };
    truth.eq.bands[1] = { 400.0f, -4.0f, 1.4f };
    truth.eq.bands[2] = { 800.0f, 0.0f, 1.0f };
    truth.eq.bands[3] = { 2500.0f, 0.0f, 1.0f };
    truth.eq.bands[4] = { 6000.0f, 0.0f, 1.0f };
    truth.eq.bands[5] = { 8000.0f, 3.0f, 0.707f };
    truth.comp = { true, -8.0f, 4.0f, 10.0f, 120.0f, 6.0f, 0.0f };
    truth.deesser = { true, 6500.0f, -22.0f, 8.0f };
    truth.saturation = { true, 12.0f, 0 };
    auto processed = raw;
    renderChain (truth, processed, kSr);

    LearnOptions opt;
    opt.maxAnalysisSeconds = 10.0;
    opt.learnEnvelope = false;
    const auto got = learnProcessing (raw, processed, kSr, opt);
    std::cout << "    vocal learned: " << got.describe() << "  (fit " << got.fitDb << " dB, input " << got.inputLevelDb << " LUFS)\n";

    CHECK (std::abs (got.latencySamples) <= 1);
    CHECK (got.eq.used);
    for (float hz : { 400.0f, 1000.0f, 8000.0f })
        CHECK (std::abs (eqResponseDb (got.eq, hz) - eqResponseDb (truth.eq, hz)) < 2.0f);
    CHECK (got.comp.used);
    CHECK (got.comp.ratio >= 2.0f);
    CHECK (got.comp.ratio <= 8.0f);
    CHECK (std::abs (got.comp.thresholdRel - truth.comp.thresholdRel) < 6.0f);
    CHECK (got.saturation.used);
    CHECK (got.saturation.driveDb >= 6.0f);
    CHECK (got.saturation.driveDb <= 24.0f);
    CHECK (! got.reverb.used);
    CHECK (got.fitDb < 2.0f);
}

TEST_CASE ("processing learner: kick chain (gate, EQ, compressor, enveloper)")
{
    const auto raw = kick (12.0);
    ProcessingSettings truth;
    truth.inputLevelDb = activeLoudness (raw, kSr);
    truth.gate = { true, -12.0f, 40.0f, 0.5f, 40.0f, 100.0f };  // closes on the tail and the bleed between hits
    truth.eq.used = true;
    truth.eq.lowCutHz = 0.0f;
    truth.eq.bands[0] = { 100.0f, 0.0f, 0.707f };
    truth.eq.bands[1] = { 350.0f, -5.0f, 2.0f };
    truth.eq.bands[2] = { 800.0f, 0.0f, 1.0f };
    truth.eq.bands[3] = { 4000.0f, 4.0f, 1.0f };
    truth.eq.bands[4] = { 6000.0f, 0.0f, 1.0f };
    truth.eq.bands[5] = { 10000.0f, 0.0f, 0.707f };
    truth.envelope = { true, 6.0f, -6.0f };
    auto processed = raw;
    renderChain (truth, processed, kSr);

    LearnOptions opt;
    opt.maxAnalysisSeconds = 10.0;
    opt.learnSaturation = false;
    const auto got = learnProcessing (raw, processed, kSr, opt);
    std::cout << "    kick learned: " << got.describe() << "  (fit " << got.fitDb << " dB)\n";

    CHECK (got.gate.used);
    CHECK (got.gate.rangeDb >= 15.0f);
    CHECK (got.eq.used);
    CHECK (std::abs (eqResponseDb (got.eq, 350.0) - eqResponseDb (truth.eq, 350.0)) < 2.5f);
    CHECK (got.envelope.used);
    CHECK (got.envelope.attackDb > 0.0f);
    CHECK (got.fitDb < 3.0f);
}

TEST_CASE ("processing learner: insert reverb and a reverb return")
{
    const auto raw = vocal (14.0);
    ProcessingSettings truth;
    truth.inputLevelDb = activeLoudness (raw, kSr);
    truth.reverb.used = true;
    truth.reverb.decayS = 2.2f;
    truth.reverb.mixPct = 25.0f;
    truth.reverb.lowCutHz = 20.0f;
    truth.reverb.dampingHz = 12000.0f;
    auto processed = raw;
    renderChain (truth, processed, kSr);
    LearnOptions opt;
    opt.maxAnalysisSeconds = 10.0;
    opt.learnSaturation = false;
    opt.learnEnvelope = false;
    const auto got = learnProcessing (raw, processed, kSr, opt);
    std::cout << "    reverb learned: " << got.describe() << "\n";
    CHECK (got.reverb.used);
    CHECK (got.reverb.decayS >= 1.0f);
    CHECK (got.reverb.decayS <= 4.5f);
    CHECK (got.reverb.mixPct >= 10.0f);
    CHECK (got.reverb.mixPct <= 40.0f);

    // Return: 100 % wet reverb of the dry vocal at -6 dB, in a mix where the dry track sits at 0 dB
    Audio ret = raw;
    for (auto& v : ret[0]) v *= 0.5f;
    {
        ProcessingSettings wet;
        wet.reverb.used = true;
        wet.reverb.decayS = 1.6f;
        wet.reverb.mixPct = 100.0f;
        wet.reverb.lowCutHz = 250.0f;
        wet.reverb.dampingHz = 12000.0f;
        renderChain (wet, ret, kSr);
    }
    ProcessingSettings track;
    std::vector<ProcessingSettings*> out { &track };
    learnReturn (ret, { makeSendSource (raw, kSr) }, kSr, { 0.0f }, 0.0f, out);
    std::cout << "    from return: " << track.describe() << "\n";
    CHECK (track.reverb.used);
    CHECK (track.reverb.fromReturn);
    CHECK (track.reverb.decayS >= 0.8f);
    CHECK (track.reverb.decayS <= 3.0f);
    // wet:dry = 0.5 -> an insert mix around 1/3
    CHECK (track.reverb.mixPct > 15.0f);
    CHECK (track.reverb.mixPct < 55.0f);
}

TEST_CASE ("processing settings: JSON round trip and combining")
{
    ProcessingSettings a;
    a.comp = { true, -10.0f, 4.0f, 10.0f, 100.0f, 6.0f, 3.0f };
    a.eqCurveValid.fill (true);
    a.eqCurveDb[10] = -3.0f;
    ProcessingSettings b = a;
    b.comp.ratio = 2.0f;
    ProcessingSettings c = a;
    c.comp.used = false;
    const auto back = ProcessingSettings::fromJson (a.toJson());
    CHECK (back.comp.used);
    CHECK (std::abs (back.comp.ratio - 4.0f) < 0.01f);
    CHECK (std::abs (back.eqCurveDb[10] + 3.0f) < 0.01f);
    const auto m = combineProcessing ({ a, b, c });
    CHECK (m.comp.used);
    CHECK (m.comp.ratio > 2.0f);
    CHECK (m.comp.ratio < 4.0f);
    CHECK (m.tracks == 3);
}
