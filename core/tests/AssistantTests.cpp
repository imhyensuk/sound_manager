#include <doctest/doctest.h>

#include <smix/MixAssistant.h>

#include "TestHelpers.h"

using namespace smix;

namespace
{
/** In-memory host: the shared test session, the real executor and history. */
struct FakeHost : AssistantHost
{
    MixSession s = test::makeSession();
    PluginCatalog catalog;
    test::FakeController ctl;
    MixHistory history;
    std::vector<std::string> views, labels;
    std::map<std::string, std::string> renamed, styles;
    bool autoMix = false;
    int undos = 0;

    FakeHost()
    {
        for (auto& c : s.channels())
        {
            auto copy = c;
            copy.features.valid = true;
            copy.features.secondsAnalysed = 10;
            copy.features.shortTermLufs = -20;
            copy.features.crestDb = 12;
            copy.features.bandLevelDb = referenceCurve (c.role);
            s.upsert (copy);
        }
        // The vocal and bass get an EQ too so that style moves have something to act on.
        s.find ("v")->chain.push_back (test::makeEq ("eq"));
        s.find ("b")->chain.push_back (test::makeEq ("eq"));
    }

    const MixSession& session() override { return s; }
    std::vector<ActionOutcome> apply (const std::vector<MixAction>& actions, const std::string& label) override
    {
        labels.push_back (label);
        ActionExecutor exec (s, catalog, ctl);
        auto out = exec.executeAll (actions, "m");
        history.record (s, s.scopeOf ("m"), label, "chat", static_cast<double> (labels.size()));
        return out;
    }
    bool undoLast() override { ++undos; return true; }
    void renameChannel (const std::string& id, const std::string& name) override
    {
        renamed[id] = name;
        s.find (id)->name = name;
        s.find (id)->role = guessRoleFromTrackName (name);
    }
    void setChannelStyle (const std::string& id, const std::string& style) override { styles[id] = style; s.find (id)->style = style; }
    void showView (const std::string& v) override { views.push_back (v); }
    void setAutoMix (bool on) override { autoMix = on; }
};

bool anyContains (const std::vector<MixAssistant::Message>& msgs, const std::string& text)
{
    for (auto& m : msgs)
        if (m.text.find (text) != std::string::npos)
            return true;
    return false;
}

const MixAssistant::Message* firstQuestion (const std::vector<MixAssistant::Message>& msgs)
{
    for (auto& m : msgs)
        if (m.isQuestion)
            return &m;
    return nullptr;
}
} // namespace

TEST_CASE ("chat request -> moves -> listen again -> feedback question -> 'more'")
{
    FakeHost host;
    llm::RuleIntentModel rules;
    MixAssistant a ("m", rules);
    a.options().askForNames = false;

    auto out = a.handleUser ("드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어", host, 0.0);
    REQUIRE_FALSE (out.empty());
    CHECK (anyContains (out, "단단하게"));
    CHECK (host.ctl.paramCalls > 0);
    const int callsAfterFirst = host.ctl.paramCalls;

    // Too early to judge: nothing happens.
    CHECK (a.tick (host, 2.0).empty());

    // The kick now measures less muddy: the assistant asks how it sounds.
    host.s.find ("k")->features.bandLevelDb[3] -= 3.0f;
    out = a.tick (host, 5.0);
    const auto* q = firstQuestion (out);
    REQUIRE (q != nullptr);
    CHECK (q->text.find ("Kick In") != std::string::npos);

    out = a.handleUser ("조금 더", host, 6.0);
    CHECK (host.ctl.paramCalls > callsAfterFirst);
    CHECK (host.labels.back().find ("(더)") != std::string::npos);
}

TEST_CASE ("no measurable improvement -> one more step by itself")
{
    FakeHost host;
    llm::RuleIntentModel rules;
    MixAssistant a ("m", rules);
    a.options().askForNames = false;
    a.handleUser ("킥이 너무 탁해", host, 0.0);
    const int calls = host.ctl.paramCalls;
    const auto out = a.tick (host, 5.0);  // features unchanged: not better
    CHECK (anyContains (out, "한 단계 더"));
    CHECK (host.ctl.paramCalls > calls);
    CHECK (firstQuestion (a.tick (host, 10.0)) != nullptr);  // after the correction it asks
}

TEST_CASE ("start mixing asks the style first; skip or timeout uses the AI default")
{
    FakeHost host;
    llm::RuleIntentModel rules;
    MixAssistant a ("m", rules);
    a.options().askForNames = false;

    auto out = a.handleUser ("전체 믹스 시작해줘", host, 0.0);
    auto* q = firstQuestion (out);
    REQUIRE (q != nullptr);
    CHECK (q->text.find ("스타일") != std::string::npos);
    const auto first = a.dialogue().pending().front();
    CHECK (first.options.front() == dialogue::defaultStyleLabel());

    // Skip: default style, and the next channel is asked.
    out = a.handleAnswer (first.id, -1, true, {}, host, 1.0);
    CHECK (host.styles[first.channelId] == dialogue::defaultStyleLabel());
    REQUIRE_FALSE (a.dialogue().pending().empty());

    // Pick an explicit style for the next one.
    auto second = a.dialogue().pending().front();
    out = a.handleAnswer (second.id, 1, false, {}, host, 2.0);
    CHECK (host.styles[second.channelId] == second.options[1]);

    // Let every remaining question time out.
    double t = 3.0;
    for (int i = 0; i < 20 && ! a.dialogue().pending().empty(); ++i)
    {
        t += 30.0;
        a.tick (host, t);
    }
    CHECK (a.dialogue().pending().empty());
    CHECK (host.autoMix);
    CHECK (host.styles.size() >= 4);
}

TEST_CASE ("asks for the name of a channel it cannot identify")
{
    FakeHost host;
    host.s.upsert (test::makeChannel ("x", "Audio 3", InstrumentRole::Unknown));
    llm::RuleIntentModel rules;
    MixAssistant a ("m", rules);
    auto out = a.tick (host, 0.0);
    auto* q = firstQuestion (out);
    REQUIRE (q != nullptr);
    CHECK (q->text.find ("Audio 3") != std::string::npos);
    CHECK (a.tick (host, 1.0).empty());  // one question at a time, never repeated

    a.handleUser ("오버헤드 마이크야", host, 2.0);
    CHECK (host.renamed["x"] == "오버헤드 마이크야");
    CHECK ((host.s.find ("x")->role == InstrumentRole::Overheads));

    CHECK (MixAssistant::isUninformativeName ("Track 12"));
    CHECK (MixAssistant::isUninformativeName ("오디오 4"));
    CHECK_FALSE (MixAssistant::isUninformativeName ("Kick In"));
}

TEST_CASE ("protected plugin -> the user is asked to make the change")
{
    FakeHost host;
    host.s.find ("k")->chain[0].protectedSlot = true;
    llm::RuleIntentModel rules;
    MixAssistant a ("m", rules);
    a.options().askForNames = false;
    const auto out = a.handleUser ("킥이 너무 탁해", host, 0.0);
    const auto* q = firstQuestion (out);
    REQUIRE (q != nullptr);
    CHECK (q->text.find ("보호") != std::string::npos);
    CHECK ((a.dialogue().pending().front().kind == dialogue::QuestionKind::ManualChange));
}

TEST_CASE ("views, undo and clarifying questions")
{
    FakeHost host;
    llm::RuleIntentModel rules;
    MixAssistant a ("m", rules);
    a.options().askForNames = false;

    a.handleUser ("waterfall 그래프 보여줘", host, 0.0);
    CHECK (host.views == std::vector<std::string> { "waterfall" });
    a.handleUser ("볼륨 미터 띄워줘", host, 0.0);
    CHECK (host.views.back() == "meters");

    a.handleUser ("방금 거 되돌려줘", host, 0.0);
    CHECK (host.undos == 1);

    auto out = a.handleUser ("더 단단하게 해줘", host, 0.0);  // which channel?
    const auto* q = firstQuestion (out);
    REQUIRE (q != nullptr);
    const auto pending = a.dialogue().pending().front();
    const auto it = std::find (pending.options.begin(), pending.options.end(), "Bass DI");
    REQUIRE (it != pending.options.end());
    a.handleAnswer (pending.id, static_cast<int> (it - pending.options.begin()), false, {}, host, 1.0);
    CHECK (host.labels.back().find ("Bass DI") != std::string::npos);
}
