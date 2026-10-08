#include <doctest/doctest.h>

#include <smix/MixHistory.h>
#include <smix/Progress.h>

#include "TestHelpers.h"

using namespace smix;

TEST_CASE ("history: record, change, restore params/gain; reload chains with saved states")
{
    mem::Runtime rt (16u << 20);
    MixHistory history (&rt, 10);
    auto session = test::makeSession();
    auto blobs = [] (const std::string& ch, int slot) { return std::vector<std::uint8_t> (2048, static_cast<std::uint8_t> (ch[0] + slot)); };

    const auto before = history.record (session, { "k", "s" }, "시작", "user", 1000.0, blobs);
    session.find ("k")->chain[0].params[4].value = 0.2f;
    session.find ("k")->aiGainDb = 3.0f;
    const auto after = history.record (session, { "k", "s" }, "킥 단단하게", "chat", 1001.0, blobs);
    CHECK (after > before);
    CHECK (history.blobCount() == 2);  // identical states are stored once

    auto plan = history.planRestore (before, session);
    CHECK (plan.reloads.empty());
    bool gain = false, param = false;
    for (auto& a : plan.actions)
    {
        gain |= a.type == ActionType::SetGain && *a.value == doctest::Approx (0.0);
        param |= a.type == ActionType::SetParam && a.paramIndex == 4 && *a.value == doctest::Approx (0.5);
    }
    CHECK (gain);
    CHECK (param);

    // Restoring goes through the executor with restore limits (any value may be set back).
    PluginCatalog catalog;
    test::FakeController ctl;
    ActionExecutor exec (session, catalog, ctl, ActionLimits::forRestore());
    for (auto& o : exec.executeAll (plan.actions, "m"))
        CHECK (o.ok);
    CHECK (session.find ("k")->chain[0].params[4].value == doctest::Approx (0.5f));

    // A different chain is reloaded with the recorded plugin states.
    session.find ("k")->chain.pop_back();
    plan = history.planRestore (after, session);
    REQUIRE (plan.reloads.size() == 1);
    CHECK (plan.reloads[0].pluginUids == std::vector<std::string> { "eq", "comp" });
    CHECK (plan.reloads[0].states[1] == blobs ("k", 1));

    // Protected channels are never rolled back by the AI.
    session.find ("s")->protectedChannel = true;
    session.find ("s")->aiGainDb = 5.0f;
    plan = history.planRestore (before, session);
    CHECK_FALSE (plan.notes.empty());

    // Per-channel export/import (what each plugin instance saves in the project).
    MixHistory loaded (&rt);
    loaded.importChannel (history.exportChannel ("k"));
    loaded.importChannel (history.exportChannel ("s"));
    REQUIRE (loaded.snapshots().size() == 2);
    CHECK (loaded.find (after)->label == "킥 단단하게");
    CHECK (loaded.find (after)->channels.size() == 2);
}

TEST_CASE ("progress estimate blends the prior with measured speed")
{
    ProgressEstimator p;
    p.start ("플러그인 분석", 10, 5.0, 0.0);
    CHECK (p.etaSeconds (0.0) == doctest::Approx (50.0));
    for (int i = 1; i <= 5; ++i)
        p.advance (1, i * 1.0);  // much faster than the prior: 1 s per unit
    CHECK (p.etaSeconds (5.0) < 25.0);
    CHECK (p.etaSeconds (5.0) > 5.0);
    CHECK (p.describe (5.0).find ("5/10") != std::string::npos);
    CHECK (formatDuration (125) == "약 2분 5초");
    CHECK (formatDuration (3700) == "약 1시간 1분");
}

TEST_CASE ("protection: AI changes are blocked and turned into requests to the user")
{
    auto session = test::makeSession();
    PluginCatalog catalog;
    test::FakeController ctl;
    ActionExecutor exec (session, catalog, ctl);

    session.find ("k")->chain[0].protectedSlot = true;
    MixAction eq;
    eq.type = ActionType::NudgeParam;
    eq.channelId = "k";
    eq.slot = 0;
    eq.param = "Band 2 Gain";
    eq.delta = -2.0;
    eq.unit = "dB";
    eq.reason = "탁함 줄이기";
    auto o = exec.execute (eq, "m");
    CHECK_FALSE (o.ok);
    CHECK (o.needsUser);
    CHECK (o.manualRequest.find ("Band 2 Gain") != std::string::npos);
    CHECK (o.manualRequest.find ("탁함 줄이기") != std::string::npos);
    CHECK (ctl.paramCalls == 0);

    // Other plugins on the channel are still fine.
    MixAction comp;
    comp.type = ActionType::NudgeParam;
    comp.channelId = "k";
    comp.slot = 1;
    comp.param = "Attack";
    comp.delta = 2.0;
    comp.unit = "ms";
    CHECK (exec.execute (comp, "m").ok);

    // A chain change that drops the protected plugin is refused.
    MixAction chain;
    chain.type = ActionType::SetChain;
    chain.channelId = "k";
    chain.plugins = { "comp" };
    CHECK (exec.execute (chain, "m").needsUser);

    session.find ("v")->protectedChannel = true;
    MixAction gain;
    gain.type = ActionType::NudgeGain;
    gain.channelId = "v";
    gain.delta = 1.5;
    o = exec.execute (gain, "m");
    CHECK (o.needsUser);
    CHECK (o.manualRequest.find ("볼륨") != std::string::npos);
}
