#include <doctest/doctest.h>

#include <algorithm>

#include <smix/AutoMixer.h>
#include <smix/LocalIntentInterpreter.h>

#include "TestHelpers.h"

using namespace smix;

namespace
{
bool hasGoal (const ParsedIntent& i, SoundGoal g)
{
    return std::any_of (i.goals.begin(), i.goals.end(), [g] (auto& x) { return x.goal == g; });
}
} // namespace

TEST_CASE ("'드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어' targets the kick with a small 'tighter' goal")
{
    const auto session = test::makeSession();
    LocalIntentInterpreter li;
    const auto i = li.parse ("드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어", session, "m");
    REQUIRE (i.targetChannelIds.size() == 1);
    CHECK (i.targetChannelIds[0] == "k");
    REQUIRE ((hasGoal (i, SoundGoal::Tighter)));
    CHECK (i.amount == doctest::Approx (0.5f));
}

TEST_CASE ("the tighter-kick recipe cuts mud, tightens the low end and slows the attack")
{
    auto session = test::makeSession();
    LocalIntentInterpreter li;
    const auto r = li.interpret ("드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어", session, "m");
    REQUIRE (r.understood);
    REQUIRE_FALSE (r.actions.empty());

    PluginCatalog catalog;
    test::FakeController controller;
    ActionExecutor exec (session, catalog, controller);
    const auto outcomes = exec.executeAll (r.actions, "m");

    bool mudCut = false, attackSlower = false;
    for (auto& o : outcomes)
    {
        INFO (o.message);
        CHECK (o.ok);
        if (o.action.param.find ("Gain") != std::string::npos && o.action.delta < 0)
            mudCut = true;
        if (o.action.param == "Attack" && o.action.delta > 0)
            attackSlower = true;
    }
    CHECK (mudCut);
    CHECK (attackSlower);

    // The 400 Hz band was moved down, i.e. the band nearest to the kick's mud region got the cut.
    const auto& eq = session.find ("k")->chain[0];
    const auto* band2Gain = eq.findParam ("Band 2 Gain");
    CHECK (*eq.mappers.at (band2Gain->index).toReal (band2Gain->value) < -1.0);
}

TEST_CASE ("negation and problem words")
{
    const auto session = test::makeSession();
    LocalIntentInterpreter li;

    auto i = li.parse ("보컬이 너무 밝아요", session, "m");
    CHECK ((hasGoal (i, SoundGoal::Darker)));
    CHECK (i.targetChannelIds == std::vector<std::string> { "v" });

    i = li.parse("the vocal sounds harsh", session, "m");
    CHECK ((hasGoal (i, SoundGoal::LessHarsh)));

    i = li.parse ("베이스가 너무 얇아", session, "m");
    CHECK ((hasGoal (i, SoundGoal::MoreBody)));
    CHECK (i.targetChannelIds == std::vector<std::string> { "b" });

    i = li.parse ("make the bass drum a lot punchier", session, "m");
    CHECK (i.targetChannelIds == std::vector<std::string> { "k" });
    CHECK ((hasGoal (i, SoundGoal::Punchier)));
    CHECK (i.amount == doctest::Approx (1.5f));

    // "부탁해요" (please) must not be read as "탁" (muddy).
    i = li.parse ("보컬 조금 키워주세요 부탁해요", session, "m");
    CHECK ((hasGoal (i, SoundGoal::Louder)));
    CHECK_FALSE ((hasGoal (i, SoundGoal::LessMuddy)));
}

TEST_CASE ("a track instance with an unknown name applies requests to itself")
{
    auto session = test::makeSession();
    session.upsert (test::makeChannel ("u", "Audio 1", InstrumentRole::Unknown));
    LocalIntentInterpreter li;
    const auto i = li.parse ("make the kick tighter", session, "u");
    CHECK (i.targetChannelIds == std::vector<std::string> { "u" });
}

TEST_CASE ("drum-bus instance cannot be steered onto the vocal")
{
    const auto session = test::makeSession();
    LocalIntentInterpreter li;
    const auto r = li.interpret ("보컬을 키워줘", session, "db");
    CHECK (r.intent.targetChannelIds.empty());
    CHECK_FALSE (r.understood);
}

TEST_CASE ("auto mixer plans a chain for an empty channel and then corrects a muddy one")
{
    auto session = test::makeSession();
    PluginCatalog catalog;
    PluginInfo eq { "eq", "Test EQ", "V", "VST3", "Fx|EQ" };
    PluginInfo comp { "comp", "Test Comp", "V", "VST3", "Fx|Dynamics" };
    catalog.addOrUpdate (eq);
    catalog.addOrUpdate (comp);
    catalog.setAllowed ("eq", true);
    catalog.setAllowed ("comp", true);

    // Snare: playing, no plugins yet.
    auto* snare = session.find ("s");
    snare->features.valid = true;
    snare->features.secondsAnalysed = 10;
    snare->features.shortTermLufs = -20;
    snare->features.bandLevelDb = referenceCurve (InstrumentRole::Snare);

    // Kick: playing and muddy.
    auto* kick = session.find ("k");
    kick->features.valid = true;
    kick->features.secondsAnalysed = 10;
    kick->features.shortTermLufs = -20;
    kick->features.crestDb = 14;
    kick->features.bandLevelDb = referenceCurve (InstrumentRole::Kick);
    kick->features.bandLevelDb[3] += 8.0f;  // 250-500 Hz build-up

    AutoMixer mixer;
    mixer.getOptions().initialiseNewPlugins = false;
    const auto t = mixer.tick (session, "db", catalog, 100.0);

    bool plannedSnare = false, kickCut = false;
    for (auto& a : t.actions)
    {
        if (a.type == ActionType::SetChain && a.channelId == "s")
            plannedSnare = true;
        if (a.channelId == "k" && a.type == ActionType::NudgeParam && a.delta < 0)
            kickCut = true;
    }
    CHECK (plannedSnare);
    CHECK (kickCut);
}

TEST_CASE ("auto mixer leaves channels alone that the user just changed")
{
    auto session = test::makeSession();
    PluginCatalog catalog;
    auto* kick = session.find ("k");
    kick->features.valid = true;
    kick->features.secondsAnalysed = 10;
    kick->features.shortTermLufs = -20;
    kick->features.bandLevelDb = referenceCurve (InstrumentRole::Kick);
    kick->features.bandLevelDb[3] += 8.0f;  // muddy

    AutoMixer mixer;
    mixer.getOptions().balanceLevels = false;
    mixer.holdChannel ("k", 100.0, 60.0);
    CHECK (mixer.tick (session, "db", catalog, 120.0).actions.empty());
    CHECK_FALSE (mixer.tick (session, "db", catalog, 200.0).actions.empty());

    // Plugins that the AI did not insert are never re-initialised.
    AutoMixer fresh;
    fresh.getOptions().correctTone = false;
    fresh.getOptions().balanceLevels = false;
    CHECK (fresh.tick (session, "db", catalog, 0.0).actions.empty());
    fresh.markForInitialisation ("k", { "comp" });
    const auto t = fresh.tick (session, "db", catalog, 1.0);
    CHECK_FALSE (t.actions.empty());
    for (auto& a : t.actions)
        CHECK (a.slot == 1);
}
