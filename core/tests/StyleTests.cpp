#include <doctest/doctest.h>

#include <random>

#include <smix/style/StyleProfile.h>

#include "TestHelpers.h"

using namespace smix;

namespace
{
/** White noise is bright; integrating it (one-pole) makes it dark. */
style::StyleProfile analyse (bool bright, double gain, int chunk)
{
    const double sr = 48000.0;
    style::StyleAnalyzer a (sr);
    std::mt19937 rng (7);
    std::normal_distribution<float> n (0.0f, 1.0f);
    std::vector<float> l (static_cast<size_t> (sr * 8)), r (l.size());
    float lp = 0;
    for (size_t i = 0; i < l.size(); ++i)
    {
        const float w = n (rng);
        lp = 0.98f * lp + 0.02f * w;
        const float s = static_cast<float> (gain) * (bright ? 0.1f * w : 2.0f * lp);
        l[i] = s;
        r[i] = s * 0.9f + 0.1f * static_cast<float> (gain) * 0.1f * n (rng);
    }
    for (size_t i = 0; i < l.size(); i += static_cast<size_t> (chunk))
        a.process (l.data() + i, r.data() + i, static_cast<int> (std::min<size_t> (static_cast<size_t> (chunk), l.size() - i)));
    return a.finish (bright ? "bright" : "dark", "test.wav");
}
} // namespace

TEST_CASE ("reference analysis: tone, loudness and stability across chunk sizes")
{
    const auto bright = analyse (true, 1.0, 4096);
    const auto dark = analyse (false, 1.0, 4096);
    CHECK (bright.seconds == doctest::Approx (8.0).epsilon (0.01));
    CHECK (bright.tiltDbPerOctave > dark.tiltDbPerOctave + 3.0f);
    CHECK (bright.centroidHz > dark.centroidHz * 2.0f);
    CHECK (bright.integratedLufs > -40.0f);
    CHECK (bright.correlation > 0.8f);

    const auto again = analyse (true, 1.0, 333);
    CHECK (again.integratedLufs == doctest::Approx (bright.integratedLufs).epsilon (0.001));
    CHECK (again.tiltDbPerOctave == doctest::Approx (bright.tiltDbPerOctave).epsilon (0.01));

    const auto j = bright.toJson();
    const auto back = style::StyleProfile::fromJson (j);
    CHECK (back.integratedLufs == doctest::Approx (bright.integratedLufs));
    CHECK (back.thirdOctaveDb[20] == doctest::Approx (bright.thirdOctaveDb[20]).epsilon (0.01));
}

TEST_CASE ("matching a bright reference brightens a dull master; style goals per instrument")
{
    const auto ref = analyse (true, 1.0, 4096);

    auto master = test::makeChannel ("m", "Master", InstrumentRole::Master, ChannelKind::Master);
    master.chain.push_back (test::makeEq());
    master.features.valid = true;
    master.features.bandLevelDb = ref.bandLevelDb;
    master.features.bandLevelDb[9] -= 8.0f;  // missing air
    master.features.bandLevelDb[8] -= 6.0f;
    master.features.crestDb = ref.crestDb;
    master.features.integratedLufs = ref.integratedLufs;
    master.features.stereoWidth = ref.widthMid;

    const auto m = style::matchStyle (ref, master);
    bool boost = false;
    for (auto& a : m.actions)
        if (a.type == ActionType::NudgeParam && a.delta > 0)
            boost = true;
    CHECK (boost);
    CHECK_FALSE (m.notes.empty());

    const auto goals = style::styleGoalsFor (ref);
    REQUIRE ((goals.count (InstrumentRole::LeadVocal) == 1));
    bool brighter = false;
    for (auto& g : goals.at (InstrumentRole::LeadVocal))
        brighter |= g.goal == SoundGoal::Brighter;
    CHECK (brighter);
}
