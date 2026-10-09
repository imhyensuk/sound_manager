#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include <smix/dsp/Builtin.h>

using namespace smix::dsp;

namespace
{
constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<std::vector<float>> sine (double hz, double seconds, float amp, int channels = 2)
{
    std::vector<std::vector<float>> x (static_cast<size_t> (channels), std::vector<float> (static_cast<size_t> (seconds * kSr)));
    for (auto& c : x)
        for (size_t i = 0; i < c.size(); ++i)
            c[i] = amp * static_cast<float> (std::sin (2.0 * kPi * hz * static_cast<double> (i) / kSr));
    return x;
}

double rmsDb (const std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    if (to == 0 || to > x.size()) to = x.size();
    double s = 0;
    for (size_t i = from; i < to; ++i) s += static_cast<double> (x[i]) * x[i];
    return 10.0 * std::log10 (s / static_cast<double> (to - from) + 1e-20);
}

double peakDb (const std::vector<float>& x, size_t from = 0)
{
    float p = 0;
    for (size_t i = from; i < x.size(); ++i) p = std::max (p, std::abs (x[i]));
    return 20.0 * std::log10 (p + 1e-12);
}

double gainAt (Kind k, std::initializer_list<std::pair<const char*, float>> params, double hz, float amp = 0.1f)
{
    auto p = create (k);
    for (auto& [id, v] : params) p->set (id, v);
    auto x = sine (hz, 1.0, amp);
    const double in = rmsDb (x[0], 24000);
    renderOffline (*p, x, kSr);
    return rmsDb (x[0], 24000, x[0].size() - 2000) - in;
}
} // namespace

TEST_CASE ("builtin specs: names, ranges, normalisation round trip")
{
    for (int i = 0; i < kNumKinds; ++i)
    {
        const auto k = static_cast<Kind> (i);
        CHECK ((kindFromString (uidFor (k)) == k));
        CHECK ((kindFromString (toString (k)) == k));
        for (auto& s : specsFor (k))
        {
            CHECK (s.min < s.max);
            CHECK (s.def >= s.min);
            CHECK (s.def <= s.max);
            CHECK (std::abs (fromNormalised (s, toNormalised (s, s.def)) - s.def) < 1e-3f * (s.max - s.min) + 1e-4f);
            if (s.centre > s.min && s.centre < s.max)
                CHECK (std::abs (toNormalised (s, s.centre) - 0.5f) < 1e-3f);
        }
    }
    CHECK (formatValue (specsFor (Kind::Compressor)[1], 4.0f) == "4.0:1");
    CHECK (formatValue (specsFor (Kind::EQ)[4], 2500.0f) == "2.50 kHz");
}

TEST_CASE ("builtin gain and EQ")
{
    CHECK (std::abs (gainAt (Kind::Gain, { { "gain", 6.0f } }, 1000.0) - 6.0) < 0.1);
    CHECK (std::abs (gainAt (Kind::EQ, { { "b3_freq", 1000.0f }, { "b3_gain", 6.0f } }, 1000.0) - 6.0) < 0.2);
    CHECK (std::abs (gainAt (Kind::EQ, { { "b3_freq", 1000.0f }, { "b3_gain", 6.0f }, { "b3_q", 4.0f } }, 4000.0)) < 0.5);
    CHECK (gainAt (Kind::EQ, { { "lowcut", 200.0f }, { "lowcut_slope", 1.0f } }, 50.0) < -40.0);
    CHECK (std::abs (gainAt (Kind::EQ, { { "lowcut", 200.0f } }, 2000.0)) < 0.2);
    CHECK (std::abs (gainAt (Kind::EQ, { { "b6_freq", 8000.0f }, { "b6_gain", -6.0f } }, 16000.0) + 6.0) < 1.0);
    CHECK (gainAt (Kind::EQ, { { "highcut", 2000.0f } }, 10000.0) < -20.0);
}

TEST_CASE ("builtin compressor follows its static curve")
{
    // -6 dBFS peak sine, threshold -18, 4:1, hard knee -> 12 dB over -> 9 dB reduction.
    auto p = create (Kind::Compressor);
    p->set ("threshold", -18.0f);
    p->set ("ratio", 4.0f);
    p->set ("knee", 0.0f);
    p->set ("attack", 1.0f);
    p->set ("release", 300.0f);
    auto x = sine (200.0, 1.0, 0.5f);
    renderOffline (*p, x, kSr);
    CHECK (std::abs (peakDb (x[0], 24000) - (-6.0 - 9.0)) < 1.0);
    CHECK (std::abs (p->reductionDb() - 9.0f) < 1.0f);

    // Mix 0 % = untouched
    auto q = create (Kind::Compressor);
    q->set ("mix", 0.0f);
    auto y = sine (200.0, 0.5, 0.5f);
    renderOffline (*q, y, kSr);
    CHECK (std::abs (peakDb (y[0], 12000) + 6.0) < 0.1);
}

TEST_CASE ("builtin gate closes on quiet material only")
{
    CHECK (gainAt (Kind::Gate, { { "threshold", -40.0f }, { "range", 30.0f } }, 300.0, 0.003f) < -25.0);  // -50 dB tone
    CHECK (std::abs (gainAt (Kind::Gate, { { "threshold", -40.0f }, { "range", 30.0f } }, 300.0, 0.3f)) < 0.2);
}

TEST_CASE ("builtin de-esser turns down only the sibilant band")
{
    const auto params = { std::pair<const char*, float> { "freq", 5000.0f }, { "threshold", -40.0f }, { "range", 10.0f } };
    CHECK (gainAt (Kind::DeEsser, params, 8000.0, 0.3f) < -5.0);
    CHECK (std::abs (gainAt (Kind::DeEsser, params, 300.0, 0.3f)) < 0.5);
}

TEST_CASE ("builtin saturation: harmonics on loud parts, unity on quiet parts, exact latency")
{
    CHECK (std::abs (gainAt (Kind::Saturation, { { "drive", 12.0f } }, 200.0, 0.003f)) < 0.3);

    auto p = create (Kind::Saturation);
    p->set ("drive", 18.0f);
    auto x = sine (200.0, 0.5, 0.7f, 1);
    renderOffline (*p, x, kSr);
    // 3rd harmonic present: correlate with 600 Hz
    double h1 = 0, h3 = 0, h1s = 0, h3s = 0;
    for (size_t i = 4800; i < x[0].size() - 4800; ++i)
    {
        const double t = static_cast<double> (i) / kSr;
        h1 += x[0][i] * std::sin (2 * kPi * 200 * t); h1s += x[0][i] * std::cos (2 * kPi * 200 * t);
        h3 += x[0][i] * std::sin (2 * kPi * 600 * t); h3s += x[0][i] * std::cos (2 * kPi * 600 * t);
    }
    CHECK (std::sqrt (h3 * h3 + h3s * h3s) / std::sqrt (h1 * h1 + h1s * h1s) > 0.05);

    // latency compensated: an impulse comes out at the same place
    auto q = create (Kind::Saturation);
    q->set ("drive", 0.0f);
    std::vector<std::vector<float>> imp (1, std::vector<float> (2000, 0.0f));
    imp[0][500] = 0.1f;
    renderOffline (*q, imp, kSr);
    size_t best = 0;
    for (size_t i = 0; i < imp[0].size(); ++i)
        if (std::abs (imp[0][i]) > std::abs (imp[0][best])) best = i;
    CHECK (best == 500);
}

TEST_CASE ("builtin enveloper shapes attack and sustain")
{
    // Decaying hits every 0.5 s
    auto hits = [] {
        std::vector<std::vector<float>> x (1, std::vector<float> (static_cast<size_t> (kSr * 2)));
        for (size_t i = 0; i < x[0].size(); ++i)
        {
            const double t = std::fmod (static_cast<double> (i) / kSr, 0.5);
            x[0][i] = static_cast<float> (0.5 * std::sin (2 * kPi * 150 * t) * std::exp (-t * 8));
        }
        return x;
    };
    auto tailDb = [] (const std::vector<float>& x) { return rmsDb (x, static_cast<size_t> (kSr * 0.5 + kSr * 0.25), static_cast<size_t> (kSr * 0.5 + kSr * 0.45)); };
    auto headDb = [] (const std::vector<float>& x) { return rmsDb (x, static_cast<size_t> (kSr * 0.5), static_cast<size_t> (kSr * 0.5 + kSr * 0.01)); };

    auto ref = hits();
    auto a = hits();
    auto p = create (Kind::Enveloper);
    p->set ("attack", 12.0f);
    renderOffline (*p, a, kSr);
    CHECK (headDb (a[0]) - headDb (ref[0]) > 3.0);
    CHECK (std::abs (tailDb (a[0]) - tailDb (ref[0])) < 2.0);

    auto s = hits();
    auto q = create (Kind::Enveloper);
    q->set ("sustain", -12.0f);
    renderOffline (*q, s, kSr);
    CHECK (tailDb (s[0]) - tailDb (ref[0]) < -4.0);
}

TEST_CASE ("builtin reverb: decay time and mix")
{
    auto p = create (Kind::Reverb);
    p->set ("decay", 2.0f);
    p->set ("mix", 100.0f);
    p->set ("predelay", 0.0f);
    p->set ("lowcut", 20.0f);
    p->set ("damping", 20000.0f);
    std::vector<std::vector<float>> x (2, std::vector<float> (static_cast<size_t> (kSr * 3), 0.0f));
    x[0][100] = x[1][100] = 1.0f;
    renderOffline (*p, x, kSr);
    // energy between 0.3-0.5 s vs 1.3-1.5 s: 1 s apart -> 60/RT60 = 30 dB for RT60 = 2 s
    const double early = rmsDb (x[0], static_cast<size_t> (0.3 * kSr), static_cast<size_t> (0.5 * kSr));
    const double late = rmsDb (x[0], static_cast<size_t> (1.3 * kSr), static_cast<size_t> (1.5 * kSr));
    CHECK (early - late > 22.0);
    CHECK (early - late < 38.0);

    CHECK (std::abs (gainAt (Kind::Reverb, { { "mix", 0.0f } }, 1000.0)) < 0.1);
}

TEST_CASE ("builtin limiter keeps the ceiling")
{
    auto p = create (Kind::Limiter);
    p->set ("input", 12.0f);
    p->set ("ceiling", -1.0f);
    std::mt19937 rng (3);
    std::normal_distribution<float> nd (0.0f, 0.3f);
    std::vector<std::vector<float>> x (2, std::vector<float> (static_cast<size_t> (kSr)));
    for (auto& c : x) for (auto& v : c) v = nd (rng);
    renderOffline (*p, x, kSr);
    CHECK (peakDb (x[0]) <= -0.99);
    CHECK (peakDb (x[1]) <= -0.99);
    CHECK (rmsDb (x[0]) > -15.0);  // limited, not crushed to silence
}
