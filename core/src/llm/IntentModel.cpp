#include "smix/llm/IntentModel.h"

#include <algorithm>

#include "smix/PerceptualProfile.h"

namespace smix::llm
{
namespace
{
std::optional<SoundGoal> goalFromString (const std::string& s)
{
    for (int g = 0; g <= static_cast<int> (SoundGoal::Smoother); ++g)
        if (toString (static_cast<SoundGoal> (g)) == s)
            return static_cast<SoundGoal> (g);
    return std::nullopt;
}

bool has (const std::string& lower, std::initializer_list<const char*> words) { return containsAny (lower, words); }
} // namespace

nlohmann::json IntentResult::toJson() const
{
    nlohmann::json goalsJson = nlohmann::json::array();
    for (auto& g : goals)
        goalsJson.push_back ({ { "goal", toString (g.goal) }, { "amount", g.amount } });
    return { { "understood", understood }, { "targets", targets }, { "goals", goalsJson }, { "question", question },
             { "options", options }, { "reply", reply }, { "view", view }, { "command", command }, { "source", source } };
}

std::string RuleIntentModel::detectView (const std::string& textIn)
{
    const auto t = toLowerAscii (textIn);
    if (! has (t, { "보여", "띄워", "열어", "켜 줘", "켜줘", "show", "open", "display", "그래프", "graph", "볼래" }))
        return {};
    if (has (t, { "waterfall", "워터폴", "스펙트로그램", "spectrogram", "폭포" }))        return "waterfall";
    if (has (t, { "lufs", "라우드니스", "loudness", "음량 변화" }))                       return "loudness";
    if (has (t, { "위상", "상관", "correlation", "고니오", "vectorscope", "벡터스코프", "phase" })) return "correlation";
    if (has (t, { "미터", "meter", "볼륨", "레벨", "level", "피크", "peak" }))           return "meters";
    if (has (t, { "rta", "스펙트럼", "spectrum", "주파수", "frequency", "eq 그래프", "분석" })) return "rta";
    return {};
}

std::string RuleIntentModel::detectCommand (const std::string& textIn, bool feedbackExpected)
{
    const auto t = toLowerAscii (textIn);
    if (has (t, { "되돌려", "원래대로", "취소해", "undo", "revert", "이전 상태" }))                        return "undo";
    if (has (t, { "자동 믹스 꺼", "자동믹스 꺼", "멈춰", "그만", "stop mixing", "automix off" }))          return "automix_stop";
    if (has (t, { "전체 믹스", "전체를 믹스", "믹싱 시작", "믹스 시작", "믹스해", "믹싱해", "자동 믹스 켜", "mix everything",
                  "start mixing", "mix the song", "mix it" }))
        return "automix_start";
    if (has (t, { "레퍼런스", "reference", "참고 곡", "이 곡처럼", "업로드한" }) && has (t, { "맞춰", "처럼", "like", "match", "스타일" }))
        return "match_reference";
    if (has (t, { "체인", "chain", "플러그인 순서" }) && has (t, { "짜", "계획", "plan", "추천", "만들어" }))
        return "plan_chain";
    if (has (t, { "상태", "status", "메모리", "memory", "모듈" }) && has (t, { "알려", "보여", "어때", "show" }))
        return "status";
    if (feedbackExpected)
    {
        if (has (t, { "좋아", "괜찮", "완벽", "딱 좋", "됐어", "good", "great", "perfect", "ok" }) && ! has (t, { "더 ", "덜" }))
            return "feedback_ok";
        if (has (t, { "덜", "너무 과", "과해", "줄여", "less", "too much" }))
            return "feedback_less";
        if (has (t, { "더", "조금 더", "more", "stronger" }) && t.size() < 40)
            return "feedback_more";
    }
    return {};
}

IntentResult RuleIntentModel::interpret (const IntentRequest& r)
{
    IntentResult out;
    out.source = "rules";
    out.view = detectView (r.userText);
    out.command = detectCommand (r.userText, ! r.pendingQuestion.empty());
    if (! out.view.empty() || ! out.command.empty())
    {
        out.understood = true;
        return out;
    }
    if (r.session == nullptr)
        return out;

    const auto intent = interpreter.parse (r.userText, *r.session, r.scopeRootId);
    out.targets = intent.targetChannelIds;
    out.goals = intent.goals;
    out.amount = intent.amount;
    out.understood = ! intent.goals.empty() && ! intent.targetChannelIds.empty();

    if (! intent.goals.empty() && intent.targetChannelIds.empty())
    {
        out.question = "어느 채널을 말씀하시는 건가요?";
        for (auto& id : r.session->scopeOf (r.scopeRootId))
            if (auto* c = r.session->find (id))
                out.options.push_back (c->name.empty() ? c->id : c->name);
    }
    else if (intent.goals.empty())
    {
        out.question = "어떤 느낌으로 바꾸고 싶으세요?";
        out.options = { "더 단단하게", "더 밝게", "더 따뜻하게", "덜 탁하게", "더 크게" };
    }
    return out;
}

//==============================================================================
IntentResult parseModelJson (const nlohmann::json& j, const IntentRequest& r)
{
    IntentResult out;
    out.source = "llm";
    if (! j.is_object())
        return out;

    out.reply = j.value ("reply", std::string {});
    out.view = j.value ("view", std::string {});
    out.command = j.value ("command", std::string {});
    out.question = j.value ("question", std::string {});
    if (j.contains ("options") && j["options"].is_array())
        for (auto& o : j["options"])
            if (o.is_string())
                out.options.push_back (o.get<std::string>());

    if (j.contains ("goals") && j["goals"].is_array())
        for (auto& g : j["goals"])
            if (g.is_object())
                if (auto goal = goalFromString (g.value ("goal", std::string {})))
                    out.goals.push_back ({ *goal, std::clamp (g.value ("amount", 1.0f), 0.1f, 2.0f) });

    if (r.session != nullptr && j.contains ("targets") && j["targets"].is_array())
        for (auto& t : j["targets"])
            if (t.is_string())
                for (auto& id : r.session->resolveTarget (t.get<std::string>(), r.scopeRootId))
                    if (std::find (out.targets.begin(), out.targets.end(), id) == out.targets.end())
                        out.targets.push_back (id);

    out.understood = (! out.goals.empty() && ! out.targets.empty()) || ! out.view.empty() || ! out.command.empty();
    return out;
}

const std::string& intentGrammar()
{
    // Strict JSON object; goal/view/command values limited to the vocabulary the assistant can execute.
    static const std::string g = R"(root ::= "{" ws "\"reply\":" ws string "," ws "\"targets\":" ws strings "," ws "\"goals\":" ws goals "," ws "\"question\":" ws string "," ws "\"options\":" ws strings "," ws "\"view\":" ws view "," ws "\"command\":" ws command ws "}"
goals ::= "[" ws ( goal ( "," ws goal ){0,5} )? ws "]"
goal ::= "{" ws "\"goal\":" ws goalname "," ws "\"amount\":" ws amount ws "}"
goalname ::= "\"tighter\"" | "\"punchier\"" | "\"more_attack\"" | "\"warmer\"" | "\"brighter\"" | "\"darker\"" | "\"less_muddy\"" | "\"less_harsh\"" | "\"less_boomy\"" | "\"more_body\"" | "\"thinner\"" | "\"more_air\"" | "\"less_sibilance\"" | "\"more_controlled\"" | "\"more_dynamic\"" | "\"wider\"" | "\"narrower\"" | "\"more_space\"" | "\"drier\"" | "\"louder\"" | "\"quieter\"" | "\"forward\"" | "\"back\"" | "\"smoother\""
amount ::= "0.5" | "1.0" | "1.5"
view ::= "\"\"" | "\"rta\"" | "\"waterfall\"" | "\"meters\"" | "\"loudness\"" | "\"correlation\""
command ::= "\"\"" | "\"undo\"" | "\"automix_start\"" | "\"automix_stop\"" | "\"match_reference\"" | "\"plan_chain\"" | "\"feedback_ok\"" | "\"feedback_more\"" | "\"feedback_less\"" | "\"status\""
strings ::= "[" ws ( string ( "," ws string ){0,7} )? ws "]"
string ::= "\"" char{0,160} "\""
char ::= [^"\\\x7F\x00-\x1F] | "\\" ["\\/bfnrt]
ws ::= [ \t\n]{0,2}
)";
    return g;
}

std::string intentSystemPrompt()
{
    return R"(너는 DAW 플러그인 안에서 동작하는 믹싱 엔지니어 보조 AI다. 사용자의 요청과 문맥을 이해해서 JSON 하나로만 답한다.
- targets: 대상 채널 이름이나 id (세션 목록에 있는 것만). 대상이 분명하지 않으면 비워 두고 question으로 물어본다.
- goals: 소리 목표 (tighter, punchier, more_attack, warmer, brighter, darker, less_muddy, less_harsh, less_boomy, more_body, thinner, more_air, less_sibilance, more_controlled, more_dynamic, wider, narrower, more_space, drier, louder, quieter, forward, back, smoother). amount는 "조금"=0.5, 보통=1.0, "많이"=1.5.
- 요청이 모호하면 goals를 비우고 question과 options(2~5개)로 되묻는다.
- view: 사용자가 그래프를 보고 싶어하면 rta, waterfall, meters, loudness, correlation 중 하나.
- command: undo(되돌리기), automix_start(전체 믹싱 시작), automix_stop, match_reference(레퍼런스에 맞추기), plan_chain(플러그인 순서 계획), feedback_ok/feedback_more/feedback_less(방금 변경에 대한 반응), status.
- reply: 한국어로 짧게, 무엇을 이해했고 무엇을 할지.
Example: 요청 "드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어" -> {"reply":"킥을 조금 더 단단하게 만들게요.","targets":["kick"],"goals":[{"goal":"tighter","amount":0.5}],"question":"","options":[],"view":"","command":""})";
}

std::string intentUserPrompt (const IntentRequest& r)
{
    std::string s = "[세션 채널]\n";
    if (r.session != nullptr)
    {
        for (auto& id : r.session->scopeOf (r.scopeRootId))
        {
            const auto* c = r.session->find (id);
            if (c == nullptr)
                continue;
            const auto profile = PerceptualProfile::analyse (c->features, c->role);
            s += "- " + (c->name.empty() ? c->id : c->name) + " (id " + c->id + ", " + toString (c->role) + ")";
            if (c->protectedChannel) s += " [보호됨]";
            if (! c->style.empty()) s += " 스타일: " + c->style;
            if (c->features.valid) s += " 들리는 상태: " + profile.summary();
            s += "\n";
        }
    }
    if (! r.referenceStyle.empty())
        s += "[레퍼런스 곡] " + r.referenceStyle + "\n";
    if (! r.knowledge.empty())
        s += "[관련 플러그인 지식]\n" + r.knowledge + "\n";
    if (! r.recentTurns.empty())
    {
        s += "[최근 대화]\n";
        for (auto& [role, text] : r.recentTurns)
            s += role + ": " + text + "\n";
    }
    if (! r.pendingQuestion.empty())
        s += "[AI가 방금 한 질문] " + r.pendingQuestion + "\n";
    s += "[요청] " + r.userText;
    return s;
}

} // namespace smix::llm
