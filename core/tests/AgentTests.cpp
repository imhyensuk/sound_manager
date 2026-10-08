#include <doctest/doctest.h>

#include <deque>

#include <smix/ClaudeMixAgent.h>

#include "TestHelpers.h"

using namespace smix;

namespace
{
struct ScriptedTransport : HttpTransport
{
    std::deque<std::string> responses;
    std::vector<nlohmann::json> requests;
    std::vector<std::pair<std::string, std::string>> lastHeaders;

    HttpResponse post (const std::string&, const std::vector<std::pair<std::string, std::string>>& headers,
                       const std::string& body) override
    {
        requests.push_back (nlohmann::json::parse (body));
        lastHeaders = headers;
        if (responses.empty())
            return { 500, R"({"type":"error","error":{"message":"no more scripted responses"}})", {} };
        auto r = responses.front();
        responses.pop_front();
        return { 200, r, {} };
    }
};

struct TestAccess : SessionAccess
{
    MixSession session = test::makeSession();
    PluginCatalog catalog;
    test::FakeController controller;

    nlohmann::json snapshot() override { return session.snapshotJson ("m", true); }
    nlohmann::json pluginParameters (const std::string& ch, int slot) override
    {
        return session.find (ch)->chain[static_cast<size_t> (slot)].toJson (true, 200);
    }
    nlohmann::json channelAnalysis (const std::string& ch) override { return session.find (ch)->toJson (false); }
    nlohmann::json allowedPlugins() override { return catalog.toJson(); }
    nlohmann::json suggestChain (const std::string&) override { return nlohmann::json::object(); }
    std::vector<ActionOutcome> apply (const std::vector<MixAction>& actions) override
    {
        ActionExecutor exec (session, catalog, controller);
        return exec.executeAll (actions, "m");
    }
};
} // namespace

TEST_CASE ("request shape: model, adaptive thinking, effort, fallbacks, cached system prompt, tools")
{
    ScriptedTransport t;
    ClaudeMixAgent agent ({ "test-key" }, t);
    const auto body = agent.buildRequest();
    CHECK (body["model"] == "claude-opus-5-5");
    CHECK (body["thinking"]["type"] == "adaptive");
    CHECK (body["output_config"]["effort"] == "medium");
    CHECK (body["fallbacks"] == "default");
    CHECK (body["system"][0]["cache_control"]["type"] == "ephemeral");
    CHECK (body["tools"].size() == 5);
    CHECK_FALSE (body.contains ("temperature"));
    CHECK_FALSE (body.contains ("tool_choice"));
}

TEST_CASE ("tool loop: model applies an EQ cut, then answers")
{
    ScriptedTransport t;
    t.responses.push_back (R"({
        "id":"msg_1","type":"message","role":"assistant","stop_reason":"tool_use",
        "content":[
            {"type":"thinking","thinking":"","signature":"sig"},
            {"type":"tool_use","id":"tu_1","name":"apply_mix_actions","input":{
                "actions":[{"type":"nudge_param","channel":"k","slot":0,"param":"Band 2 Gain","delta":-2.5,"unit":"dB",
                            "reason":"less 300-400 Hz boxiness"}]}}
        ]})");
    t.responses.push_back (R"({
        "id":"msg_2","type":"message","role":"assistant","stop_reason":"end_turn",
        "content":[{"type":"text","text":"킥의 350Hz 부근을 2.5dB 줄여 더 단단하게 만들었어요."}]})");

    TestAccess access;
    ClaudeMixAgent agent ({ "test-key" }, t);
    const auto reply = agent.send ("드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어", access);

    INFO (reply.error);
    REQUIRE (reply.ok);
    CHECK (reply.text.find ("단단") != std::string::npos);
    REQUIRE (reply.outcomes.size() == 1);
    CHECK (reply.outcomes[0].ok);
    CHECK (access.controller.paramCalls == 1);

    // Second request carries the assistant turn unchanged (incl. thinking) plus the tool result.
    REQUIRE (t.requests.size() == 2);
    const auto& msgs = t.requests[1]["messages"];
    REQUIRE (msgs.size() == 3);
    CHECK (msgs[1]["content"][0]["type"] == "thinking");
    CHECK (msgs[2]["content"][0]["type"] == "tool_result");
    CHECK (msgs[2]["content"][0]["tool_use_id"] == "tu_1");
    CHECK (msgs[0]["content"][0]["text"].get<std::string>().find ("<session_snapshot>") == 0);

    bool hasBeta = false;
    for (auto& [k, v] : t.lastHeaders)
        if (k == "anthropic-beta" && v == "server-side-fallback-2026-07-01")
            hasBeta = true;
    CHECK (hasBeta);
}

TEST_CASE ("refusal and API errors are reported, not crashed on")
{
    ScriptedTransport t;
    t.responses.push_back (R"({"type":"message","role":"assistant","stop_reason":"refusal","content":[]})");
    TestAccess access;
    ClaudeMixAgent agent ({ "test-key" }, t);
    auto reply = agent.send ("hello", access);
    CHECK_FALSE (reply.ok);
    CHECK_FALSE (reply.error.empty());

    // Next request goes out with a single (merged) user turn, never two consecutive user turns.
    t.responses.push_back (R"({"type":"message","role":"assistant","stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]})");
    reply = agent.send ("again", access);
    CHECK (reply.ok);
    CHECK (t.requests.back()["messages"].size() == 1);

    ClaudeMixAgent noKey ({}, t);
    CHECK_FALSE (noKey.send ("x", access).ok);
}
