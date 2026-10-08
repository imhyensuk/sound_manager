#include "smix/MixAssistant.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace smix
{
namespace
{
using dialogue::Question;
using dialogue::QuestionKind;

SoundGoal opposite (SoundGoal g)
{
    switch (g)
    {
        case SoundGoal::Tighter:        return SoundGoal::MoreBody;
        case SoundGoal::Punchier:       return SoundGoal::Smoother;
        case SoundGoal::MoreAttack:     return SoundGoal::Smoother;
        case SoundGoal::Warmer:         return SoundGoal::Brighter;
        case SoundGoal::Brighter:       return SoundGoal::Darker;
        case SoundGoal::Darker:         return SoundGoal::Brighter;
        case SoundGoal::LessMuddy:      return SoundGoal::MoreBody;
        case SoundGoal::LessHarsh:      return SoundGoal::Brighter;
        case SoundGoal::LessBoomy:      return SoundGoal::MoreBody;
        case SoundGoal::MoreBody:       return SoundGoal::Thinner;
        case SoundGoal::Thinner:        return SoundGoal::MoreBody;
        case SoundGoal::MoreAir:        return SoundGoal::Darker;
        case SoundGoal::LessSibilance:  return SoundGoal::Brighter;
        case SoundGoal::MoreControlled: return SoundGoal::MoreDynamic;
        case SoundGoal::MoreDynamic:    return SoundGoal::MoreControlled;
        case SoundGoal::Wider:          return SoundGoal::Narrower;
        case SoundGoal::Narrower:       return SoundGoal::Wider;
        case SoundGoal::MoreSpace:      return SoundGoal::Drier;
        case SoundGoal::Drier:          return SoundGoal::MoreSpace;
        case SoundGoal::Louder:         return SoundGoal::Quieter;
        case SoundGoal::Quieter:        return SoundGoal::Louder;
        case SoundGoal::Forward:        return SoundGoal::Back;
        case SoundGoal::Back:           return SoundGoal::Forward;
        case SoundGoal::Smoother:       return SoundGoal::Punchier;
    }
    return g;
}

std::string channelLabel (const ChannelState& c)
{
    return c.name.empty() ? c.id : c.name;
}

std::string goalsText (const std::vector<Goal>& goals)
{
    std::string s;
    for (auto& g : goals)
        s += (s.empty() ? "" : ", ") + koreanLabel (g.goal);
    return s;
}
} // namespace

std::optional<std::pair<Descriptor, int>> expectedEffect (SoundGoal g)
{
    switch (g)
    {
        case SoundGoal::Tighter:        return std::make_pair (Descriptor::Muddy, -1);
        case SoundGoal::Punchier:
        case SoundGoal::MoreAttack:     return std::make_pair (Descriptor::Punchy, +1);
        case SoundGoal::Warmer:
        case SoundGoal::Darker:         return std::make_pair (Descriptor::Bright, -1);
        case SoundGoal::Brighter:
        case SoundGoal::MoreAir:        return std::make_pair (Descriptor::Bright, +1);
        case SoundGoal::LessMuddy:      return std::make_pair (Descriptor::Muddy, -1);
        case SoundGoal::LessHarsh:
        case SoundGoal::Smoother:       return std::make_pair (Descriptor::Harsh, -1);
        case SoundGoal::LessBoomy:      return std::make_pair (Descriptor::Boomy, -1);
        case SoundGoal::MoreBody:       return std::make_pair (Descriptor::Thin, -1);
        case SoundGoal::Thinner:        return std::make_pair (Descriptor::Thin, +1);
        case SoundGoal::LessSibilance:  return std::make_pair (Descriptor::Sibilant, -1);
        case SoundGoal::MoreControlled: return std::make_pair (Descriptor::Squashed, +1);
        case SoundGoal::MoreDynamic:    return std::make_pair (Descriptor::Squashed, -1);
        case SoundGoal::Wider:          return std::make_pair (Descriptor::Wide, +1);
        case SoundGoal::Narrower:       return std::make_pair (Descriptor::Wide, -1);
        default:                        return std::nullopt;  // loudness / space are judged by level or by the user
    }
}

bool MixAssistant::isUninformativeName (const std::string& nameIn)
{
    const auto n = toLowerAscii (nameIn);
    if (n.empty())
        return true;
    std::string letters;
    for (char c : n)
        if (! std::isdigit (static_cast<unsigned char> (c)) && c != ' ' && c != '-' && c != '_' && c != '.' && c != '#')
            letters += c;
    static const char* generic[] = { "audio", "track", "channel", "inst", "instrument", "midi", "bus", "aux", "new track",
                                     "오디오", "트랙", "채널", "untitled", "mono", "stereo" };
    for (auto* g : generic)
        if (letters == g || n.rfind (std::string ("channel "), 0) == 0)
            return true;
    return false;
}

MixAssistant::MixAssistant (std::string scopeRootId, llm::IntentModel& r) : rootId (std::move (scopeRootId)), rules (r) {}

void MixAssistant::remember (const std::string& role, const std::string& text)
{
    turns.emplace_back (role, text);
    if (turns.size() > 12)
        turns.erase (turns.begin());
}

MixAssistant::Message MixAssistant::ask (Question q, double now)
{
    const auto id = questions.ask (q, now);
    remember ("assistant", q.text);
    return { q.text, true, id };
}

//==============================================================================
std::vector<MixAssistant::Message> MixAssistant::handleUser (const std::string& text, AssistantHost& host, double now)
{
    remember ("user", text);
    const auto& session = host.session();

    llm::IntentRequest req;
    req.userText = text;
    req.session = &session;
    req.scopeRootId = rootId;
    req.recentTurns = turns;
    if (auto open = questions.pending(); ! open.empty())
        req.pendingQuestion = open.back().text;
    req.knowledge = host.knowledgeFor (text);
    if (const auto* ref = host.referenceStyle())
    {
        for (auto& d : ref->descriptors())
            req.referenceStyle += d + " ";
    }

    llm::IntentResult result;
    if (reasoning != nullptr)
        result = reasoning->interpret (req);
    if (! result.understood && result.question.empty())
    {
        auto byRules = rules.interpret (req);
        if (byRules.understood || result.source.empty() || reasoning == nullptr)
            result = byRules;
    }

    // Feedback on the last change ("더", "좋아요") answers the open feedback question.
    if (result.command.rfind ("feedback_", 0) == 0)
    {
        for (auto& q : questions.pending())
        {
            if (q.kind != QuestionKind::Feedback)
                continue;
            const int option = result.command == "feedback_ok" ? 0 : result.command == "feedback_more" ? 1 : 2;
            if (auto qa = questions.answer (q.id, option))
                return resolveAnswer (qa->first, qa->second, host, now);
        }
    }

    // A typed answer to an open question (e.g. the channel name, or "2"). Text that asks for a
    // sound ("make the kick tighter") is a new request, never an answer such as a name.
    if (! result.understood && result.view.empty() && result.command.empty() && result.goals.empty())
        if (auto qa = questions.answerFromText (text))
            return resolveAnswer (qa->first, qa->second, host, now);

    std::vector<Message> out;
    if (! result.view.empty())
    {
        host.showView (result.view);
        out.push_back ({ result.reply.empty() ? "'" + result.view + "' 그래프를 열었어요." : result.reply });
        if (result.goals.empty() && result.command.empty())
            return out;
    }

    if (! result.command.empty())
    {
        auto more = runCommand (result, host, now);
        out.insert (out.end(), more.begin(), more.end());
        return out;
    }

    if (result.understood && ! result.goals.empty() && ! result.targets.empty())
    {
        const auto& goals = result.goals;
        if (! result.reply.empty())
            out.push_back ({ result.reply });
        auto applied = applyGoals (result.targets, goals, text, host, now, true);
        out.insert (out.end(), applied.begin(), applied.end());
        return out;
    }

    if (! result.question.empty())
    {
        Question q;
        q.kind = QuestionKind::Clarify;
        q.text = result.question;
        q.options = result.options;
        q.freeText = true;
        q.context = { { "original", text } };
        out.push_back (ask (q, now));
        return out;
    }

    out.push_back ({ result.reply.empty()
                         ? "요청을 이해하지 못했어요. 예: \"킥을 조금 더 단단하게\", \"보컬이 너무 쏴요\", \"전체 믹스 시작해줘\", \"waterfall 보여줘\""
                         : result.reply });
    return out;
}

std::vector<MixAssistant::Message> MixAssistant::runCommand (const llm::IntentResult& r, AssistantHost& host, double now)
{
    std::vector<Message> out;
    if (r.command == "undo")
        out.push_back ({ host.undoLast() ? "직전 변경을 되돌렸어요." : "되돌릴 기록이 없어요." });
    else if (r.command == "automix_start")
        return startMixing (host, now);
    else if (r.command == "automix_stop")
    {
        mixing = false;
        mixQueue.clear();
        host.setAutoMix (false);
        out.push_back ({ "자동 믹스를 멈췄어요. 지금 상태는 그대로 유지돼요." });
    }
    else if (r.command == "match_reference")
    {
        const auto* ref = host.referenceStyle();
        if (ref == nullptr)
            out.push_back ({ "먼저 '레퍼런스' 탭에 원하는 스타일의 음원이나 영상을 올려 주세요." });
        else
        {
            const auto* root = host.session().find (rootId);
            if (root == nullptr || root->kind == ChannelKind::Track)
                out.push_back ({ "레퍼런스 매칭은 마스터나 버스 인스턴스에서 할 수 있어요." });
            else
            {
                const auto match = style::matchStyle (*ref, *root);
                const auto outcomes = host.apply (match.actions, "레퍼런스 '" + ref->name + "'에 맞춤");
                std::string text = "레퍼런스 '" + ref->name + "'에 맞췄어요.";
                for (auto& n : match.notes) text += "\n- " + n;
                out.push_back ({ text });
                for (auto& o : outcomes)
                    if (o.needsUser)
                        out.push_back ({ o.manualRequest });
            }
        }
    }
    else if (r.command == "plan_chain")
        out.push_back ({ "체인 탭의 'AI 체인 계획 적용'을 누르거나 '전체 믹스 시작해줘'라고 말씀해 주세요. 허용한 플러그인으로 순서를 짤게요." });
    else if (r.command == "status")
        out.push_back ({ host.statusText() });
    else if (r.command.rfind ("feedback_", 0) == 0)
        out.push_back ({ "방금 바꾼 것이 없어요. 원하는 소리를 말씀해 주세요." });
    return out;
}

//==============================================================================
std::vector<MixAssistant::Message> MixAssistant::applyGoals (const std::vector<std::string>& channelIds, const std::vector<Goal>& goals,
                                                              const std::string& label, AssistantHost& host, double now, bool askFeedback)
{
    std::vector<Message> out;
    const auto& session = host.session();

    PendingChange change;
    change.goals = goals;
    change.label = label;
    change.appliedAt = now;
    std::vector<MixAction> actions;
    std::vector<std::string> notes;
    std::string names;

    for (auto& id : channelIds)
    {
        const auto* c = session.find (id);
        if (c == nullptr)
            continue;
        names += (names.empty() ? "" : ", ") + channelLabel (*c);
        change.channels.push_back (id);
        change.before[id] = PerceptualProfile::analyse (c->features, c->role);
        change.loudnessBefore[id] = c->features.shortTermLufs;
        for (auto& g : goals)
        {
            auto r = recipes.actionsFor (*c, g);
            actions.insert (actions.end(), r.actions.begin(), r.actions.end());
            notes.insert (notes.end(), r.notes.begin(), r.notes.end());
        }
    }

    if (actions.empty())
    {
        std::string text = names + ": 적용할 수 있는 플러그인이 없어요.";
        for (auto& n : notes) text += "\n- " + n;
        out.push_back ({ text });
        return out;
    }

    const auto outcomes = host.apply (actions, label);
    std::string text = names + " → " + goalsText (goals);
    int ok = 0;
    std::set<std::string> requests;
    for (auto& o : outcomes)
    {
        if (o.ok)
        {
            ++ok;
            text += "\n✓ " + o.message;
        }
        else if (o.needsUser)
        {
            requests.insert (o.manualRequest);
        }
    }
    for (auto& n : notes)
        text += "\n- " + n;
    out.push_back ({ text });

    for (auto& r : requests)
    {
        Question q;
        q.kind = QuestionKind::ManualChange;
        q.text = r;
        q.options = { "직접 바꿨어요", "그대로 둘게요" };
        q.defaultOption = 1;
        out.push_back (ask (q, now));
    }

    if (ok > 0)
    {
        if (! askFeedback)
            change.verified = true;  // style passes are judged by the auto mixer, not by asking
        changes.push_back (std::move (change));
    }
    remember ("assistant", text);
    return out;
}

std::vector<MixAssistant::Message> MixAssistant::verify (AssistantHost& host, double now)
{
    std::vector<Message> out;
    for (auto& change : changes)
    {
        if (change.verified || now - change.appliedAt < opts.verifyAfterSeconds)
            continue;

        const auto& session = host.session();
        bool listened = true;
        int improved = 0, judged = 0;
        for (auto& id : change.channels)
        {
            const auto* c = session.find (id);
            if (c == nullptr)
                continue;
            if (! c->features.valid)
            {
                listened = false;
                continue;
            }
            const auto after = PerceptualProfile::analyse (c->features, c->role);
            for (auto& g : change.goals)
            {
                if (g.goal == SoundGoal::Louder || g.goal == SoundGoal::Quieter || g.goal == SoundGoal::Forward || g.goal == SoundGoal::Back)
                {
                    const float d = c->features.shortTermLufs - change.loudnessBefore[id];
                    const int sign = (g.goal == SoundGoal::Louder || g.goal == SoundGoal::Forward) ? 1 : -1;
                    ++judged;
                    improved += d * static_cast<float> (sign) > 0.3f ? 1 : 0;
                    continue;
                }
                if (auto e = expectedEffect (g.goal))
                {
                    const float d = after.score (e->first) - change.before[id].score (e->first);
                    ++judged;
                    improved += d * static_cast<float> (e->second) > 0.05f ? 1 : 0;
                }
            }
        }

        if (! listened && now - change.appliedAt < 30.0)
            continue;  // wait for playback

        const bool good = judged == 0 || improved * 2 >= judged;
        if (! good && listened && change.corrections < opts.maxSelfCorrections)
        {
            ++change.corrections;
            change.appliedAt = now;
            std::vector<Goal> step = change.goals;
            for (auto& g : step) g.amount *= 0.5f;
            const auto& s = host.session();
            std::vector<MixAction> actions;
            for (auto& id : change.channels)
                if (const auto* c = s.find (id))
                {
                    change.before[id] = PerceptualProfile::analyse (c->features, c->role);
                    change.loudnessBefore[id] = c->features.shortTermLufs;
                    for (auto& g : step)
                    {
                        auto r = recipes.actionsFor (*c, g);
                        actions.insert (actions.end(), r.actions.begin(), r.actions.end());
                    }
                }
            host.apply (actions, change.label + " (보정)");
            out.push_back ({ "다시 들어보니 아직 충분히 " + goalsText (change.goals) + " 되지 않아서 한 단계 더 조정했어요." });
            continue;
        }

        change.verified = true;
        Question q;
        q.kind = QuestionKind::Feedback;
        std::string who;
        for (auto& id : change.channels)
            if (const auto* c = session.find (id))
                who += (who.empty() ? "" : ", ") + channelLabel (*c);
        q.text = ! listened ? "재생이 멈춰 있어 직접 들어보지 못했어요. " + who + "이(가) 원하시는 대로 " + goalsText (change.goals) + " 됐나요?"
                 : good    ? "다시 들어보니 " + who + "이(가) " + goalsText (change.goals) + " 바뀌었어요. 어떠세요?"
                           : "조정했지만 측정상 변화가 크지 않아요. " + who + " 소리가 어떠세요?";
        q.options = { "좋아요", "더", "덜", "되돌리기" };
        q.defaultOption = 0;
        q.timeoutSeconds = opts.feedbackQuestionTimeout;
        q.context = { { "change", static_cast<int> (&change - changes.data()) } };
        out.push_back (ask (q, now));
    }
    return out;
}

std::vector<MixAssistant::Message> MixAssistant::resolveAnswer (const Question& q, const dialogue::Answer& a, AssistantHost& host, double now)
{
    std::vector<Message> out;
    const auto value = a.value (q);

    switch (q.kind)
    {
        case QuestionKind::ChannelName:
        {
            if (value.empty())
            {
                out.push_back ({ "알겠어요. 이름 없이 소리만 듣고 판단할게요." });
                break;
            }
            host.renameChannel (q.channelId, value);
            const auto role = guessRoleFromTrackName (value);
            out.push_back ({ "'" + value + "'(으)로 기억할게요" + (role != InstrumentRole::Unknown ? " → " + koreanName (role) + " 채널로 다룰게요." : ".") });
            break;
        }

        case QuestionKind::Style:
        {
            const auto label = value.empty() ? dialogue::defaultStyleLabel() : value;
            host.setChannelStyle (q.channelId, label);
            const auto* c = host.session().find (q.channelId);
            if (c != nullptr)
            {
                if (a.timedOut || a.skipped)
                    out.push_back ({ "'" + channelLabel (*c) + "'은(는) " + label + "로 진행할게요." });
                const auto goals = goalsForStyle (*c, label, host);
                if (! goals.empty())
                {
                    auto applied = applyGoals ({ c->id }, goals, "스타일: " + label, host, now, false);
                    out.insert (out.end(), applied.begin(), applied.end());
                }
            }
            if (mixing)
            {
                auto next = continueMixing (host, now);
                out.insert (out.end(), next.begin(), next.end());
            }
            break;
        }

        case QuestionKind::Clarify:
        {
            if (value.empty())
            {
                out.push_back ({ "알겠어요. 필요하면 다시 말씀해 주세요." });
                break;
            }
            const auto original = q.context.value ("original", std::string {});
            if (q.context.value ("depth", 0) > 1)
                break;
            auto again = handleUser (original + " " + value, host, now);
            out.insert (out.end(), again.begin(), again.end());
            break;
        }

        case QuestionKind::Feedback:
        {
            const int idx = q.context.value ("change", -1);
            if (idx < 0 || idx >= static_cast<int> (changes.size()))
                break;
            const auto change = changes[static_cast<size_t> (idx)];
            if (a.option == 0 || a.timedOut || a.skipped)
            {
                out.push_back ({ "좋아요, 그대로 둘게요." });
                break;
            }
            if (a.option == 1)
            {
                auto goals = change.goals;
                for (auto& g : goals) g.amount = std::max (0.25f, g.amount * 0.5f);
                return applyGoals (change.channels, goals, change.label + " (더)", host, now, true);
            }
            if (a.option == 2)
            {
                std::vector<Goal> goals;
                for (auto& g : change.goals)
                    goals.push_back ({ opposite (g.goal), std::max (0.25f, g.amount * 0.5f) });
                return applyGoals (change.channels, goals, change.label + " (덜)", host, now, true);
            }
            out.push_back ({ host.undoLast() ? "변경 전 상태로 되돌렸어요." : "되돌릴 기록이 없어요." });
            break;
        }

        case QuestionKind::ManualChange:
            out.push_back ({ a.option == 0 ? "고마워요. 바뀐 소리를 다시 들어볼게요." : "알겠어요. 보호된 부분은 그대로 둘게요." });
            break;

        case QuestionKind::Confirm:
            break;
    }
    for (auto& m : out)
        remember ("assistant", m.text);
    return out;
}

std::vector<MixAssistant::Message> MixAssistant::handleAnswer (int id, int option, bool skip, const std::string& text, AssistantHost& host,
                                                                double now)
{
    if (const auto* q = questions.find (id))
        remember ("user", skip ? "(건너뛰기)" : (option >= 0 && option < static_cast<int> (q->options.size()) ? q->options[static_cast<size_t> (option)] : text));
    auto qa = skip ? questions.skip (id) : questions.answer (id, option, text);
    if (! qa)
        return {};
    return resolveAnswer (qa->first, qa->second, host, now);
}

//==============================================================================
std::vector<Goal> MixAssistant::goalsForStyle (const ChannelState& c, const std::string& label, AssistantHost& host) const
{
    for (auto& o : dialogue::styleOptionsFor (c.role))
        if (o.label == label && ! o.goals.empty())
            return o.goals;

    if (label == "레퍼런스 스타일" || label == dialogue::defaultStyleLabel())
    {
        if (const auto* ref = host.referenceStyle())
        {
            const auto byRole = style::styleGoalsFor (*ref);
            if (auto it = byRole.find (c.role); it != byRole.end())
                return it->second;
        }
        if (label == dialogue::defaultStyleLabel())
        {
            // AI default: fix what is clearly audible, gently.
            std::vector<Goal> goals;
            const auto p = PerceptualProfile::analyse (c.features, c.role);
            if (p.score (Descriptor::Muddy) > 0.5f)     goals.push_back ({ SoundGoal::LessMuddy, 0.5f });
            if (p.score (Descriptor::Harsh) > 0.5f)     goals.push_back ({ SoundGoal::LessHarsh, 0.5f });
            if (p.score (Descriptor::Boomy) > 0.5f)     goals.push_back ({ SoundGoal::LessBoomy, 0.5f });
            if (p.score (Descriptor::Thin) > 0.5f)      goals.push_back ({ SoundGoal::MoreBody, 0.4f });
            if (p.score (Descriptor::Sibilant) > 0.5f && isVocalRole (c.role)) goals.push_back ({ SoundGoal::LessSibilance, 0.5f });
            return goals;
        }
        return {};
    }

    // A typed style ("좀 더 몽환적으로"): understand it like a request on this channel.
    llm::IntentRequest req;
    req.userText = (c.name.empty() ? c.id : c.name) + " " + label;
    req.session = &host.session();
    req.scopeRootId = rootId;
    auto r = rules.interpret (req);
    for (auto& g : r.goals) g.amount = std::min (g.amount, 0.7f);
    return r.goals;
}

std::vector<MixAssistant::Message> MixAssistant::askStyle (const ChannelState& c, AssistantHost& host, double now)
{
    Question q;
    q.kind = QuestionKind::Style;
    q.channelId = c.id;
    for (auto& o : dialogue::styleOptionsFor (c.role))
        q.options.push_back (o.label);
    if (host.referenceStyle() != nullptr)
        q.options.insert (q.options.begin() + 1, "레퍼런스 스타일");
    q.defaultOption = 0;
    q.freeText = true;
    q.timeoutSeconds = opts.styleQuestionTimeout;
    q.text = "'" + channelLabel (c) + "'(" + koreanName (c.role) + ")을(를) 어떤 스타일로 믹싱할까요? 직접 적어 주셔도 돼요. "
             + std::to_string (static_cast<int> (opts.styleQuestionTimeout)) + "초 안에 답이 없거나 건너뛰면 " + dialogue::defaultStyleLabel()
             + "로 진행해요.";
    return { ask (q, now) };
}

std::vector<MixAssistant::Message> MixAssistant::startMixing (AssistantHost& host, double now)
{
    mixing = true;
    mixQueue.clear();
    const auto& s = host.session();
    // Sources first, then buses, the master last: the master hears the finished channels.
    for (auto kind : { ChannelKind::Track, ChannelKind::Bus, ChannelKind::Master })
        for (auto& id : s.scopeOf (rootId))
            if (const auto* c = s.find (id); c != nullptr && c->kind == kind && ! c->protectedChannel)
                mixQueue.push_back (id);

    std::vector<Message> out { { "전체 믹싱을 시작할게요. 채널 " + std::to_string (mixQueue.size()) + "개를 차례로 듣고, 먼저 원하는 스타일을 여쭤볼게요." } };
    auto next = continueMixing (host, now);
    out.insert (out.end(), next.begin(), next.end());
    return out;
}

std::vector<MixAssistant::Message> MixAssistant::continueMixing (AssistantHost& host, double now)
{
    std::vector<Message> out;
    while (! mixQueue.empty())
    {
        const auto id = mixQueue.front();
        mixQueue.erase (mixQueue.begin());
        const auto* c = host.session().find (id);
        if (c == nullptr)
            continue;
        if (c->style.empty() && opts.askStyleBeforeMixing)
        {
            auto q = askStyle (*c, host, now);
            out.insert (out.end(), q.begin(), q.end());
            return out;  // continue when answered / timed out
        }
        const auto goals = goalsForStyle (*c, c->style.empty() ? dialogue::defaultStyleLabel() : c->style, host);
        if (! goals.empty())
        {
            auto applied = applyGoals ({ id }, goals, "스타일: " + c->style, host, now, false);
            out.insert (out.end(), applied.begin(), applied.end());
        }
    }
    if (mixing)
    {
        mixing = false;
        host.setAutoMix (true);
        out.push_back ({ "채널별 스타일 적용을 마쳤어요. 이제 자동 믹스가 레벨 밸런스와 톤을 계속 다듬어요. '되돌려'라고 하면 언제든 이전 상태로 돌아가요." });
    }
    return out;
}

std::vector<MixAssistant::Message> MixAssistant::tick (AssistantHost& host, double now)
{
    std::vector<Message> out;
    for (auto& [q, a] : questions.expire (now))
    {
        auto r = resolveAnswer (q, a, host, now);
        out.insert (out.end(), r.begin(), r.end());
    }

    if (opts.askForNames && ! questions.hasPending (QuestionKind::ChannelName))
    {
        const auto& s = host.session();
        for (auto& id : s.scopeOf (rootId))
        {
            const auto* c = s.find (id);
            if (c == nullptr || namingAsked.count (id) != 0 || c->kind == ChannelKind::Master)
                continue;
            if (! isUninformativeName (c->name) || c->role != InstrumentRole::Unknown)
                continue;
            namingAsked.insert (id);
            Question q;
            q.kind = QuestionKind::ChannelName;
            q.channelId = id;
            q.freeText = true;
            q.options = host.nameSuggestions (id);
            q.text = "'" + channelLabel (*c) + "' 채널에는 어떤 소리가 들어 있나요? 이름을 적어 주세요 (예: 킥, 리드 보컬, 일렉 기타). 이름을 알면 훨씬 빠르고 정확하게 믹싱할 수 있어요.";
            out.push_back (ask (q, now));
            break;  // one at a time
        }
    }

    auto checked = verify (host, now);
    out.insert (out.end(), checked.begin(), checked.end());
    return out;
}

} // namespace smix
