#include <doctest/doctest.h>

#include <smix/ChainPlanner.h>
#include <smix/GainBalancer.h>
#include <smix/RecipeEngine.h>
#include <smix/style/GenreProfile.h>

#include <algorithm>

#include "TestHelpers.h"

using namespace smix;

namespace
{
PluginCatalog makeCatalog (bool allowLimiter = true)
{
    PluginCatalog c;
    auto add = [&c] (std::string uid, std::string name, std::string cat, bool allowed) {
        PluginInfo p;
        p.uid = uid;
        p.name = name;
        p.manufacturer = "Vendor";
        p.hostCategory = cat;
        c.addOrUpdate (p);
        c.setAllowed (uid, allowed);
    };
    add ("proq", "Pro-Q 3", "Fx|EQ", true);
    add ("1176", "CLA-76", "Fx|Dynamics", true);
    add ("gate", "Noise Gate", "Fx|Dynamics", true);
    add ("deess", "DeEsser", "Fx|Dynamics", true);
    add ("verb", "Valhalla Room", "Fx|Reverb", true);
    add ("lim", "Pro-L 2", "Fx|Mastering", allowLimiter);
    add ("sat", "Decapitator", "Fx|Distortion", false);  // installed but NOT ticked by the user
    return c;
}
} // namespace

TEST_CASE ("plugin classification from names and host categories")
{
    CHECK ((PluginCatalog::classify ("FabFilter Pro-Q 3", "FabFilter", "Fx|EQ") == PluginCategory::EQ));
    CHECK ((PluginCatalog::classify ("CLA-76", "Waves", "") == PluginCategory::Compressor));
    CHECK ((PluginCatalog::classify ("Renaissance DeEsser", "Waves", "Fx|Dynamics") == PluginCategory::DeEsser));
    CHECK ((PluginCatalog::classify ("ValhallaVintageVerb", "Valhalla", "") == PluginCategory::Reverb));
    CHECK ((PluginCatalog::classify ("Pro-L 2", "FabFilter", "") == PluginCategory::Limiter));
    CHECK ((PluginCatalog::classify ("Something", "X", "Fx|Delay") == PluginCategory::Delay));
}

TEST_CASE ("kick chain: gate -> EQ -> compressor, only allowed plugins, no limiter")
{
    const auto catalog = makeCatalog();
    PerceptualProfile neutral;
    const auto plan = ChainPlanner().plan (ChannelKind::Track, InstrumentRole::Kick, neutral, catalog);
    const auto uids = plan.pluginUids();
    REQUIRE (uids.size() == 3);
    CHECK (uids[0] == "gate");
    CHECK (uids[1] == "proq");
    CHECK (uids[2] == "1176");
    for (auto& u : uids)
        CHECK (u != "sat");  // never use a plugin the user did not allow
}

TEST_CASE ("master chain ends with the limiter, or reports that one is missing")
{
    PerceptualProfile neutral;
    const auto plan = ChainPlanner().plan (ChannelKind::Master, InstrumentRole::Master, neutral, makeCatalog());
    REQUIRE_FALSE (plan.slots.empty());
    CHECK ((plan.slots.back().category == PluginCategory::Limiter));

    const auto noLimiter = ChainPlanner().plan (ChannelKind::Master, InstrumentRole::Master, neutral, makeCatalog (false));
    CHECK ((noLimiter.slots.back().category != PluginCategory::Limiter));
    CHECK_FALSE (noLimiter.notes.empty());
}

TEST_CASE ("scope rules: track / bus / master")
{
    const auto s = test::makeSession();
    CHECK (s.scopeOf ("k").size() == 1);
    const auto bus = s.scopeOf ("db");
    CHECK (bus.size() == 3);  // bus + kick + snare
    CHECK (s.inScope ("db", "k"));
    CHECK_FALSE (s.inScope ("db", "v"));
    CHECK (s.scopeOf ("m").size() == 6);
}

TEST_CASE ("gain balancer pulls a too-loud hihat down and keeps the mean")
{
    MixSession s;
    s.upsert (test::makeChannel ("m", "Master", InstrumentRole::Master, ChannelKind::Master));
    auto set = [&s] (std::string id, InstrumentRole r, float lufs) {
        auto c = test::makeChannel (id, id, r);
        c.features.valid = true;
        c.features.shortTermLufs = lufs;
        s.upsert (c);
    };
    set ("vox", InstrumentRole::LeadVocal, -18.0f);
    set ("hat", InstrumentRole::HiHat, -18.0f);  // target is 11 LU below the vocal

    const auto actions = GainBalancer().balance (s, "m");
    REQUIRE (actions.size() == 2);
    float hat = 0, vox = 0;
    for (auto& a : actions)
        (a.channelId == "hat" ? hat : vox) = static_cast<float> (a.delta);
    CHECK (hat < 0.0f);
    CHECK (vox > 0.0f);
    CHECK (std::abs (hat) <= 1.0f);  // rate limited
}

TEST_CASE ("genre profile with learned processing drives the chain and the starting settings")
{
    auto genre = std::make_shared<style::GenreProfile>();
    genre->name = "CCM";
    style::ProcessingSettings vocal;
    vocal.inputLevelDb = -20.0f;
    vocal.deesser.used = true;
    vocal.comp = { true, -6.0f, 3.0f, 12.0f, 90.0f, 6.0f, 4.0f };
    vocal.reverb.used = false;  // this engineer kept the vocal dry on the track
    genre->processing[InstrumentRole::LeadVocal] = vocal;
    style::ProcessingSettings kick;  // learned: no gate on the kick
    kick.comp.used = true;
    genre->processing[InstrumentRole::Kick] = kick;
    style::setActiveGenre (genre);

    const auto catalog = makeCatalog();
    PerceptualProfile neutral;
    const auto vox = ChainPlanner().plan (ChannelKind::Track, InstrumentRole::LeadVocal, neutral, catalog).pluginUids();
    CHECK (std::find (vox.begin(), vox.end(), "deess") != vox.end());  // learned, although nothing sounds sibilant
    CHECK (std::find (vox.begin(), vox.end(), "verb") == vox.end());   // learned: no insert reverb
    const auto k = ChainPlanner().plan (ChannelKind::Track, InstrumentRole::Kick, neutral, catalog).pluginUids();
    CHECK (std::find (k.begin(), k.end(), "gate") == k.end());

    // Starting settings of a third-party compressor follow the learned values, relative to this channel's level.
    auto c = test::makeChannel ("v", "Lead Vox", InstrumentRole::LeadVocal);
    c.chain.push_back (test::makeCompressor());
    c.features.valid = true;
    c.features.shortTermLufs = -14.0f;
    const auto res = RecipeEngine().initialSettings (c, 0);
    bool threshold = false, ratio = false;
    for (auto& a : res.actions)
    {
        if (a.param == "Threshold") threshold = a.value && std::abs (*a.value - (-20.0)) < 0.01;  // -14 + (-6)
        if (a.param == "Ratio") ratio = a.value && std::abs (*a.value - 3.0) < 0.01;
    }
    CHECK (threshold);
    CHECK (ratio);
    style::setActiveGenre (nullptr);
}
