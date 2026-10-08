#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

#include <smix/AudioAnalyzer.h>
#include <smix/PerceptualProfile.h>
#include <smix/ValueMapper.h>

using namespace smix;

namespace
{
AudioFeatures runSine (double freq, double amplitude, double seconds, double sr = 48000.0)
{
    AudioAnalyzer a;
    a.prepare (sr);
    std::vector<float> l (512), r (512);
    double phase = 0.0;
    const int blocks = static_cast<int> (seconds * sr / 512);
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < 512; ++i)
        {
            l[static_cast<size_t> (i)] = r[static_cast<size_t> (i)] = static_cast<float> (amplitude * std::sin (phase));
            phase += 2.0 * 3.14159265358979323846 * freq / sr;
        }
        a.process (l.data(), r.data(), 512);
    }
    return a.snapshot();
}
} // namespace

TEST_CASE ("1 kHz sine at -20 dBFS reads about -20 LUFS and lands in the right band")
{
    const auto f = runSine (1000.0, std::pow (10.0, -20.0 / 20.0), 4.0);
    REQUIRE (f.valid);
    // BS.1770: a stereo 1 kHz sine at -20 dBFS peak gives about -20.0 LUFS (+3 dB for two channels, -3 dB sine RMS).
    CHECK (f.shortTermLufs == doctest::Approx (-20.0).epsilon (0.03));
    CHECK (f.peakDb == doctest::Approx (-20.0).epsilon (0.02));
    CHECK (f.crestDb == doctest::Approx (3.0).epsilon (0.1));
    CHECK (f.stereoCorrelation > 0.99f);
    CHECK (f.stereoWidth < 0.01f);

    const int strongest = static_cast<int> (std::max_element (f.bandLevelDb.begin(), f.bandLevelDb.end()) - f.bandLevelDb.begin());
    CHECK (std::string (SpectrumBands::kNames[static_cast<size_t> (strongest)]) == "upper_mid");
}

TEST_CASE ("low sine on a vocal channel is perceived as boomy")
{
    const auto f = runSine (55.0, 0.3, 4.0);
    const auto p = PerceptualProfile::analyse (f, InstrumentRole::LeadVocal);
    CHECK ((p.score (Descriptor::Boomy) > 1.0f));
    CHECK ((p.salient().front().descriptor == Descriptor::Boomy));
}

TEST_CASE ("periodic clicks are detected as transients")
{
    AudioAnalyzer a;
    const double sr = 48000.0;
    a.prepare (sr);
    std::vector<float> buf (static_cast<size_t> (sr * 6), 0.0f);
    std::mt19937 rng (1);
    std::normal_distribution<float> noise (0.0f, 0.001f);
    for (size_t i = 0; i < buf.size(); ++i)
    {
        buf[i] = noise (rng);
        const size_t pos = i % static_cast<size_t> (sr / 4);  // 4 hits per second
        if (pos < 2000)
            buf[i] += 0.8f * std::exp (-static_cast<float> (pos) / 300.0f) * std::sin (static_cast<float> (pos) * 0.05f);
    }
    for (size_t i = 0; i + 256 <= buf.size(); i += 256)
        a.process (buf.data() + i, buf.data() + i, 256);
    const auto f = a.snapshot();
    CHECK (f.transientDensity > 2.0f);
    CHECK (f.transientDensity < 6.0f);
    CHECK (f.crestDb > 10.0f);
}

TEST_CASE ("value text parsing")
{
    CHECK (parseValueText ("1.2 kHz").value == doctest::Approx (1200.0));
    CHECK (parseValueText ("1.2 kHz").unit == "Hz");
    CHECK (parseValueText ("-3.5 dB").value == doctest::Approx (-3.5));
    CHECK (parseValueText ("4.0:1").unit == "ratio");
    CHECK (parseValueText ("1.5 s").value == doctest::Approx (1500.0));
    CHECK (parseValueText ("25 %").unit == "%");
    CHECK (parseValueText ("-inf dB").value < -100.0);
    CHECK_FALSE (parseValueText ("Off").ok);
}

TEST_CASE ("value mapper inverts a log frequency knob")
{
    ValueMapper m;
    for (auto n : ValueMapper::probePoints (65))
    {
        const double hz = 20.0 * std::pow (1000.0, n);
        char buf[32];
        std::snprintf (buf, sizeof (buf), hz >= 1000 ? "%.2f kHz" : "%.1f Hz", hz >= 1000 ? hz / 1000.0 : hz);
        m.addSample (n, buf);
    }
    REQUIRE (m.isUsable());
    const auto n = m.toNormalised (632.0, "Hz");
    REQUIRE (n.has_value());
    CHECK (*n == doctest::Approx (0.5).epsilon (0.01));
    CHECK (*m.toNormalised (2.0, "kHz") == doctest::Approx (std::log (100.0) / std::log (1000.0)).epsilon (0.01));
    CHECK (*m.toReal (0.5f) == doctest::Approx (632.0).epsilon (0.01));
}
