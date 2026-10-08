#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include <smix/ear/EarModel.h>

using namespace smix;
using namespace smix::ear;

namespace
{
std::vector<float> kickLike (double sr, double seconds)
{
    std::vector<float> x (static_cast<size_t> (sr * seconds));
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = std::fmod (i / sr, 0.5);
        x[i] = static_cast<float> (0.8 * std::exp (-t * 12) * std::sin (2 * M_PI * 55 * t));
    }
    return x;
}

std::vector<float> hatLike (double sr, double seconds)
{
    std::mt19937 rng (3);
    std::normal_distribution<float> n (0, 1);
    std::vector<float> x (static_cast<size_t> (sr * seconds));
    float prev = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = std::fmod (i / sr, 0.125);
        const float w = n (rng);
        x[i] = static_cast<float> (0.3 * std::exp (-t * 60)) * (w - prev);  // high-passed noise bursts
        prev = w;
    }
    return x;
}
} // namespace

TEST_CASE ("ear features separate a kick from a hi-hat; heuristic guesses")
{
    const auto k = EarFeatures::extract (kickLike (48000, 3).data(), 48000 * 3, 48000);
    const auto h = EarFeatures::extract (hatLike (48000, 3).data(), 48000 * 3, 48000);
    REQUIRE (k.size() == static_cast<size_t> (EarFeatures::kDim));
    CHECK (k[2 * EarFeatures::kMelBands] < h[2 * EarFeatures::kMelBands]);  // log centroid
    CHECK ((heuristicGuess (k).front().role == InstrumentRole::Kick));
    CHECK ((heuristicGuess (h).front().role == InstrumentRole::HiHat));
}

TEST_CASE ("MLP ear model: file format, memopro file-backed weights, classification")
{
    const int dim = EarFeatures::kDim;
    // A linear "model" that looks only at the log centroid: low -> kick, high -> hihat.
    std::vector<float> mean (static_cast<size_t> (dim), 0.0f), stdev (static_cast<size_t> (dim), 1.0f);
    std::vector<float> w1 (static_cast<size_t> (2 * dim), 0.0f), b1 { 0.0f, 0.0f };
    w1[static_cast<size_t> (2 * EarFeatures::kMelBands)] = -10.0f;      // kick logit
    w1[static_cast<size_t> (dim + 2 * EarFeatures::kMelBands)] = 10.0f; // hihat logit
    b1 = { 30.0f, -30.0f };
    REQUIRE (EarModel::write ("smix-ear-test.bin", { "kick", "hihat" }, { "kick", "hihat" }, { dim, 2 }, mean, stdev, { w1 }, { b1 }));

    mem::Runtime rt (1u << 20);
    EarModel model (&rt);
    std::string err;
    REQUIRE (model.load ("smix-ear-test.bin", err));
    const auto k = model.classify (EarFeatures::extract (kickLike (48000, 3).data(), 48000 * 3, 48000));
    const auto h = model.classify (EarFeatures::extract (hatLike (48000, 3).data(), 48000 * 3, 48000));
    CHECK (k.front().label == "kick");
    CHECK ((k.front().role == InstrumentRole::Kick));
    CHECK (h.front().label == "hihat");
    CHECK (k.front().probability > 0.9f);
    CHECK (rt.stats().buffers >= 1);
    std::remove ("smix-ear-test.bin");

    EarModel missing;
    CHECK_FALSE (missing.load ("nope.bin", err));
}
