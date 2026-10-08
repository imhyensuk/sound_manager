#include "smix/dialogue/Dialogue.h"

#include <algorithm>

namespace smix::dialogue
{

std::string toString (QuestionKind k)
{
    switch (k)
    {
        case QuestionKind::ChannelName:  return "channel_name";
        case QuestionKind::Style:        return "style";
        case QuestionKind::Clarify:      return "clarify";
        case QuestionKind::Feedback:     return "feedback";
        case QuestionKind::ManualChange: return "manual_change";
        case QuestionKind::Confirm:      return "confirm";
    }
    return "unknown";
}

nlohmann::json Question::toJson (double now) const
{
    return { { "id", id }, { "kind", toString (kind) }, { "channel", channelId }, { "text", text }, { "options", options },
             { "default_option", defaultOption }, { "free_text", freeText }, { "remaining_seconds", remaining (now) } };
}

std::string Answer::value (const Question& q) const
{
    if (option >= 0 && option < static_cast<int> (q.options.size()))
        return q.options[static_cast<size_t> (option)];
    return text;
}

int DialogueManager::ask (Question q, double now)
{
    q.id = nextId++;
    q.askedAt = now;
    open.push_back (std::move (q));
    return open.back().id;
}

const Question* DialogueManager::find (int id) const
{
    for (auto& q : open)
        if (q.id == id)
            return &q;
    return nullptr;
}

bool DialogueManager::hasPending (QuestionKind kind, const std::string& channelId) const
{
    return std::any_of (open.begin(), open.end(), [&] (auto& q) { return q.kind == kind && (channelId.empty() || q.channelId == channelId); });
}

std::optional<std::pair<Question, Answer>> DialogueManager::answer (int id, int option, const std::string& text)
{
    auto it = std::find_if (open.begin(), open.end(), [id] (auto& q) { return q.id == id; });
    if (it == open.end())
        return std::nullopt;
    Question q = *it;
    open.erase (it);
    Answer a;
    a.questionId = id;
    a.option = option >= 0 && option < static_cast<int> (q.options.size()) ? option : -1;
    a.text = text;
    return std::make_pair (q, a);
}

std::optional<std::pair<Question, Answer>> DialogueManager::skip (int id)
{
    auto it = std::find_if (open.begin(), open.end(), [id] (auto& q) { return q.id == id; });
    if (it == open.end())
        return std::nullopt;
    Question q = *it;
    open.erase (it);
    Answer a;
    a.questionId = id;
    a.skipped = true;
    a.option = q.defaultOption;
    return std::make_pair (q, a);
}

std::vector<std::pair<Question, Answer>> DialogueManager::expire (double now)
{
    std::vector<std::pair<Question, Answer>> out;
    for (auto it = open.begin(); it != open.end();)
    {
        if (it->timeoutSeconds > 0 && now >= it->askedAt + it->timeoutSeconds)
        {
            Answer a;
            a.questionId = it->id;
            a.timedOut = true;
            a.option = it->defaultOption;
            out.emplace_back (*it, a);
            it = open.erase (it);
        }
        else
        {
            ++it;
        }
    }
    return out;
}

std::optional<std::pair<Question, Answer>> DialogueManager::answerFromText (const std::string& textIn)
{
    if (open.empty())
        return std::nullopt;
    const auto text = toLowerAscii (textIn);
    for (auto it = open.rbegin(); it != open.rend(); ++it)
    {
        for (size_t i = 0; i < it->options.size(); ++i)
        {
            const auto option = toLowerAscii (it->options[i]);
            // "1", "2"... or the option's leading words.
            const bool byNumber = text == std::to_string (i + 1) || text == std::to_string (i + 1) + "번";
            const auto head = option.substr (0, std::min<size_t> (option.size(), 9));
            if (byNumber || (! head.empty() && text.find (head) != std::string::npos))
                return answer (it->id, static_cast<int> (i));
        }
        if (it->freeText)
            return answer (it->id, -1, textIn);
    }
    return std::nullopt;
}

//==============================================================================
const std::string& defaultStyleLabel()
{
    static const std::string label = "AI 기본 스타일";
    return label;
}

std::vector<StyleOption> styleOptionsFor (InstrumentRole role)
{
    using G = SoundGoal;
    std::vector<StyleOption> o { { defaultStyleLabel(), {} } };
    switch (role)
    {
        case InstrumentRole::Kick:
            o.push_back ({ "단단하고 타이트하게", { { G::Tighter, 0.8f }, { G::MoreAttack, 0.5f } } });
            o.push_back ({ "묵직한 서브(힙합/EDM)", { { G::MoreBody, 0.8f }, { G::LessMuddy, 0.5f } } });
            o.push_back ({ "자연스럽고 어쿠스틱하게", { { G::LessMuddy, 0.4f }, { G::MoreDynamic, 0.4f } } });
            break;
        case InstrumentRole::Snare:
            o.push_back ({ "크랙 강조(팝/록)", { { G::Punchier, 0.7f }, { G::Brighter, 0.4f } } });
            o.push_back ({ "두껍고 묵직하게", { { G::MoreBody, 0.7f }, { G::MoreSpace, 0.3f } } });
            o.push_back ({ "빈티지/드라이", { { G::Warmer, 0.5f }, { G::Drier, 0.5f } } });
            break;
        case InstrumentRole::Bass:
            o.push_back ({ "단단하고 일정하게", { { G::MoreControlled, 0.7f }, { G::Tighter, 0.5f } } });
            o.push_back ({ "따뜻하고 둥글게", { { G::Warmer, 0.7f }, { G::Smoother, 0.4f } } });
            o.push_back ({ "그릿/디스토션", { { G::MoreAttack, 0.5f }, { G::MoreBody, 0.4f } } });
            break;
        case InstrumentRole::LeadVocal:
        case InstrumentRole::BackingVocal:
            o.push_back ({ "밝고 앞으로(팝)", { { G::Brighter, 0.6f }, { G::Forward, 0.7f }, { G::MoreControlled, 0.5f } } });
            o.push_back ({ "따뜻하고 부드럽게(발라드/R&B)", { { G::Warmer, 0.6f }, { G::Smoother, 0.5f }, { G::MoreSpace, 0.4f } } });
            o.push_back ({ "빈티지/로파이", { { G::Darker, 0.6f }, { G::MoreControlled, 0.7f } } });
            o.push_back ({ "공간감 크게(몽환)", { { G::MoreSpace, 0.8f }, { G::MoreAir, 0.5f } } });
            break;
        case InstrumentRole::ElectricGuitar:
        case InstrumentRole::AcousticGuitar:
            o.push_back ({ "선명하게", { { G::Brighter, 0.5f }, { G::LessMuddy, 0.5f } } });
            o.push_back ({ "두껍게(월 오브 사운드)", { { G::MoreBody, 0.6f }, { G::Wider, 0.5f } } });
            o.push_back ({ "따뜻하게", { { G::Warmer, 0.6f } } });
            break;
        case InstrumentRole::Pad:
        case InstrumentRole::Synth:
        case InstrumentRole::Keys:
        case InstrumentRole::Piano:
        case InstrumentRole::Strings:
            o.push_back ({ "넓고 배경으로", { { G::Wider, 0.6f }, { G::Back, 0.5f } } });
            o.push_back ({ "선명하게 앞으로", { { G::Brighter, 0.5f }, { G::Forward, 0.5f } } });
            o.push_back ({ "따뜻하게", { { G::Warmer, 0.6f } } });
            break;
        case InstrumentRole::Master:
        case InstrumentRole::MixBus:
        case InstrumentRole::DrumBus:
            o.push_back ({ "크고 촘촘하게(상업 마스터)", { { G::MoreControlled, 0.7f }, { G::Brighter, 0.3f } } });
            o.push_back ({ "다이내믹하게(어쿠스틱/재즈)", { { G::MoreDynamic, 0.6f } } });
            o.push_back ({ "따뜻한 아날로그 느낌", { { G::Warmer, 0.6f } } });
            break;
        default:
            o.push_back ({ "선명하게", { { G::LessMuddy, 0.5f }, { G::Brighter, 0.4f } } });
            o.push_back ({ "따뜻하게", { { G::Warmer, 0.5f } } });
            break;
    }
    return o;
}

} // namespace smix::dialogue
