#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include <smix/GainBalancer.h>
#include <smix/PerceptualProfile.h>
#include <smix/style/GenreProfile.h>

using namespace smix;
using namespace smix::style;

namespace
{
constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/** A 'song' with sections: every track has its own on/off and level pattern over time. */
struct Song
{
    std::vector<std::vector<float>> tracks;
    std::vector<std::string> names;
    std::vector<InstrumentRole> roles;
};

Song makeSong (unsigned seed, double seconds)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (0.3f, 1.0f);
    std::normal_distribution<float> n (0.0f, 1.0f);
    const auto N = static_cast<size_t> (kSr * seconds);
    Song s;
    s.names = { "Kick.wav", "Vox F.wav", "Bass.wav", "Pad.wav", "Click.wav" };
    s.roles = { InstrumentRole::Kick, InstrumentRole::LeadVocal, InstrumentRole::Bass, InstrumentRole::Pad, InstrumentRole::Unknown };
    s.tracks.assign (5, std::vector<float> (N));
    std::vector<float> section (5, 1.0f);
    double ph = 0, lp = 0;
    for (size_t i = 0; i < N; ++i)
    {
        if (i % static_cast<size_t> (kSr * 2.0) == 0)  // new section every 2 s
            for (auto& v : section) v = u (rng) > 0.4f ? u (rng) : 0.0f;
        const double t = std::fmod (i / kSr, 0.5);
        s.tracks[0][i] = section[0] * static_cast<float> (std::sin (2 * kPi * 55 * t) * std::exp (-t * 8));
        ph += 2 * kPi * (220 + 20 * std::sin (i / kSr)) / kSr;
        float v = 0;
        for (int h = 1; h < 10; ++h) v += static_cast<float> (std::sin (ph * h) / h);
        s.tracks[1][i] = section[1] * 0.3f * v;
        s.tracks[2][i] = section[2] * 0.5f * static_cast<float> (std::sin (2 * kPi * 70 * i / kSr) + 0.3 * std::sin (2 * kPi * 140 * i / kSr));
        lp = 0.9 * lp + 0.1 * n (rng);
        s.tracks[3][i] = section[3] * (0.2f * n (rng) + 0.5f * static_cast<float> (lp));
        s.tracks[4][i] = (i % 24000 < 200) ? 0.5f : 0.0f;  // click track: never in the mix
    }
    return s;
}

/** One-pole low-pass: the 'engineer' darkens the pad. */
std::vector<float> darken (const std::vector<float>& x)
{
    std::vector<float> y (x.size());
    float z = 0;
    for (size_t i = 0; i < x.size(); ++i) y[i] = z = 0.85f * z + 0.15f * x[i];
    return y;
}

double loudnessProxy (const BandFrames& f)
{
    static const double k[] = { -4.0, -1.0, 0.0, 0.0, 0.0, 0.5, 2.5, 3.8, 4.0, 4.0 };
    double s = 0;
    for (auto& fr : f)
        for (size_t b = 0; b < fr.size(); ++b)
            s += fr[b] * std::pow (10.0, k[b] / 10.0);
    return s;
}
} // namespace

TEST_CASE ("learning how raw tracks sit in the final mix (balance and EQ)")
{
    const float gains[] = { 0.7f, 1.0f, 0.5f, 0.6f, 0.0f };
    std::vector<LearnedSong> learned;
    for (unsigned seed : { 1u, 2u, 3u })
    {
        const auto song = makeSong (seed, 24.0);
        std::vector<float> mix (song.tracks[0].size(), 0.0f);
        const auto pad = darken (song.tracks[3]);
        for (size_t i = 0; i < mix.size(); ++i)
            mix[i] = gains[0] * song.tracks[0][i] + gains[1] * song.tracks[1][i] + gains[2] * song.tracks[2][i] + gains[3] * pad[i];

        // The session's click track is recognised by name and left out (as smix_learn_mix does).
        std::vector<LearnTrack> tracks;
        for (size_t t = 0; t < song.tracks.size(); ++t)
            if (! isAuxiliaryTrackName (song.names[t]))
                tracks.push_back ({ song.names[t], song.names[t], song.roles[t], bandFrames (song.tracks[t].data(), song.tracks[t].size(), kSr) });
        const auto result = learnSong ("song", tracks, bandFrames (mix.data(), mix.size(), kSr));
        REQUIRE (result.tracks.size() == 4);
        CHECK (result.anchor == "Vox F.wav");

        // Expected levels from the true gains (same loudness proxy).
        std::vector<float> scaledKick (song.tracks[0]), scaledVox (song.tracks[1]), scaledBass (song.tracks[2]);
        for (auto& v : scaledKick) v *= gains[0];
        for (auto& v : scaledBass) v *= gains[2];
        const double vox = loudnessProxy (bandFrames (scaledVox.data(), scaledVox.size(), kSr));
        const double kickLu = 10 * std::log10 (loudnessProxy (bandFrames (scaledKick.data(), scaledKick.size(), kSr)) / vox);
        const double bassLu = 10 * std::log10 (loudnessProxy (bandFrames (scaledBass.data(), scaledBass.size(), kSr)) / vox);
        INFO ("kick " << result.tracks[0].levelLu << " vs " << kickLu << ", bass " << result.tracks[2].levelLu << " vs " << bassLu);
        CHECK (std::abs (result.tracks[0].levelLu - kickLu) < 1.0);
        CHECK (std::abs (result.tracks[2].levelLu - bassLu) < 1.0);
        for (auto& t : result.tracks)
            CHECK (t.present);

        // The pad was darkened: its high bands come out attenuated relative to its lows.
        const auto& padTrack = result.tracks[3];
        CHECK (padTrack.bandGainDb[8] < padTrack.bandGainDb[3] - 6.0f);
        CHECK (result.fit[4] > 0.9f);
        learned.push_back (result);
    }

    StyleProfile m1, m2;
    m1.integratedLufs = -10.0f;
    m2.integratedLufs = -12.0f;
    const auto genre = combineSongs ("CCM", learned, { m1, m2 });
    CHECK (genre.songs == 3);
    REQUIRE (genre.balanceLu.count (InstrumentRole::Kick));
    CHECK (genre.balanceLu.at (InstrumentRole::LeadVocal) == 0.0f);
    CHECK (genre.master.integratedLufs == doctest::Approx (-11.0f));
    CHECK (genre.curves.count (InstrumentRole::Unknown) == 0);
    CHECK (isAuxiliaryTrackName ("08_Click.wav"));
    CHECK (isAuxiliaryTrackName ("가이드 보컬.wav"));
    CHECK_FALSE (isAuxiliaryTrackName ("Kick In.wav"));

    // Round trip and activation: the balancer and the perception use the learned values.
    const auto loaded = GenreProfile::fromJson (genre.toJson());
    CHECK (loaded.balanceLu.at (InstrumentRole::Kick) == doctest::Approx (genre.balanceLu.at (InstrumentRole::Kick)).epsilon (0.02));
    const float before = GainBalancer::targetOffsetLu (InstrumentRole::Kick);
    setActiveGenre (std::make_shared<GenreProfile> (loaded));
    CHECK (GainBalancer::targetOffsetLu (InstrumentRole::Kick) == doctest::Approx (loaded.balanceLu.at (InstrumentRole::Kick)));
    CHECK (referenceCurve (InstrumentRole::Pad)[9] == doctest::Approx (loaded.curves.at (InstrumentRole::Pad)[9]));
    setActiveGenre (nullptr);
    CHECK (GainBalancer::targetOffsetLu (InstrumentRole::Kick) == doctest::Approx (before));
}
