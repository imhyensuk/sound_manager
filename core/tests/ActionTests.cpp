#include <doctest/doctest.h>

#include "TestHelpers.h"

using namespace smix;

TEST_CASE ("parseActions validates and reports errors without throwing")
{
    const auto j = nlohmann::json::parse (R"([
        {"type":"set_param","channel":"k","slot":0,"param":"Band 2 Gain","value":-3,"unit":"dB"},
        {"type":"nudge_gain","channel":"v","delta_db":1.5},
        {"type":"set_param","channel":"k","slot":0,"param":"Band 2 Gain"},
        {"type":"explode","channel":"k"},
        {"type":"set_chain","channel":"k","plugins":["eq","comp"]},
        42
    ])");
    const auto r = parseActions (j);
    CHECK (r.actions.size() == 3);
    CHECK (r.errors.size() == 3);
}

TEST_CASE ("set_param in dB is converted through the learned value map")
{
    auto session = test::makeSession();
    PluginCatalog catalog;
    test::FakeController controller;
    ActionExecutor exec (session, catalog, controller);

    MixAction a;
    a.type = ActionType::SetParam;
    a.channelId = "k";
    a.slot = 0;
    a.param = "Band 2 Gain";
    a.value = -3.0;
    a.unit = "dB";

    const auto out = exec.execute (a, "m");
    INFO (out.message);
    REQUIRE (out.ok);
    // -18..+18 dB over 0..1  ->  -3 dB = 0.4167
    CHECK (controller.lastValue == doctest::Approx (15.0 / 36.0).epsilon (0.01));
    CHECK (out.message.find ("-3.00 dB") != std::string::npos);
}

TEST_CASE ("nudge in ms on a compressor attack")
{
    auto session = test::makeSession();
    PluginCatalog catalog;
    test::FakeController controller;
    ActionExecutor exec (session, catalog, controller);

    const auto* attack = session.find ("k")->chain[1].findByRole (ParamRole::Attack);
    REQUIRE (attack != nullptr);
    const double before = *session.find ("k")->chain[1].mappers.at (attack->index).toReal (attack->value);

    MixAction a;
    a.type = ActionType::NudgeParam;
    a.channelId = "k";
    a.slot = 1;
    a.paramIndex = attack->index;
    a.delta = 5.0;
    a.unit = "ms";
    REQUIRE (exec.execute (a, "m").ok);

    const auto* after = session.find ("k")->chain[1].findByRole (ParamRole::Attack);
    const double now = *session.find ("k")->chain[1].mappers.at (after->index).toReal (after->value);
    CHECK (now == doctest::Approx (before + 5.0).epsilon (0.02));
}

TEST_CASE ("scope, allow-list and gain limits are enforced")
{
    auto session = test::makeSession();
    PluginCatalog catalog;
    PluginInfo allowed { "eq", "Test EQ", "V", "VST3", "Fx|EQ" };
    PluginInfo forbidden { "sat", "Saturator", "V", "VST3", "Fx|Distortion" };
    catalog.addOrUpdate (allowed);
    catalog.addOrUpdate (forbidden);
    catalog.setAllowed ("eq", true);

    test::FakeController controller;
    ActionExecutor exec (session, catalog, controller);

    MixAction outOfScope;
    outOfScope.type = ActionType::NudgeGain;
    outOfScope.channelId = "v";
    outOfScope.delta = 1.0;
    CHECK_FALSE (exec.execute (outOfScope, "db").ok);  // the drum bus may not touch the vocal

    MixAction chain;
    chain.type = ActionType::SetChain;
    chain.channelId = "s";
    chain.plugins = { "eq", "sat" };
    const auto res = exec.execute (chain, "db");
    CHECK_FALSE (res.ok);
    CHECK (res.message.find ("not in the user's allowed list") != std::string::npos);

    MixAction huge;
    huge.type = ActionType::NudgeGain;
    huge.channelId = "s";
    huge.delta = 30.0;
    REQUIRE (exec.execute (huge, "db").ok);
    CHECK (controller.lastGain == doctest::Approx (6.0));  // clamped to maxGainStepDb

    session.find ("s")->gainLocked = true;
    CHECK_FALSE (exec.execute (huge, "db").ok);
}

TEST_CASE ("set_chain keeps settings of plugins that stay in the chain")
{
    auto session = test::makeSession();
    PluginCatalog catalog;
    for (auto uid : { "eq", "comp" })
    {
        PluginInfo p;
        p.uid = uid;
        p.name = uid;
        catalog.addOrUpdate (p);
        catalog.setAllowed (uid, true);
    }
    session.find ("k")->chain[0].params[1].value = 0.75f;  // a tweaked EQ gain

    test::FakeController controller;
    ActionExecutor exec (session, catalog, controller);
    MixAction a;
    a.type = ActionType::SetChain;
    a.channelId = "k";
    a.plugins = { "comp", "eq" };
    REQUIRE (exec.execute (a, "k").ok);

    const auto& chain = session.find ("k")->chain;
    REQUIRE (chain.size() == 2);
    CHECK (chain[0].pluginUid == "comp");
    CHECK (chain[1].params[1].value == doctest::Approx (0.75f));
}
