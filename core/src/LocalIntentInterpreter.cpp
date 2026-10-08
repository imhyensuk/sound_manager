#include "smix/LocalIntentInterpreter.h"

#include <algorithm>

namespace smix
{
namespace
{
struct RoleAlias { InstrumentRole role; std::vector<const char*> words; };

const std::vector<RoleAlias>& roleAliases()
{
    static const std::vector<RoleAlias> aliases {
        { InstrumentRole::Kick,           { "kick", "킥", "bass drum", "베이스 드럼", "베이스드럼" } },
        { InstrumentRole::Snare,          { "snare", "스네어" } },
        { InstrumentRole::HiHat,          { "hihat", "hi-hat", "hi hat", "hats", "하이햇", "햇" } },
        { InstrumentRole::Toms,           { "tom", "toms", "탐" } },
        { InstrumentRole::Overheads,      { "overhead", "cymbal", "오버헤드", "심벌" } },
        { InstrumentRole::Percussion,     { "perc", "percussion", "퍼커션", "클랩", "clap", "shaker", "쉐이커" } },
        { InstrumentRole::DrumBus,        { "drums", "drum", "드럼" } },
        { InstrumentRole::Bass,           { "bass", "베이스", "808" } },
        { InstrumentRole::BackingVocal,   { "backing vocal", "backing", "bgv", "chorus vocal", "코러스", "화음", "백보컬" } },
        { InstrumentRole::LeadVocal,      { "lead vocal", "vocal", "vox", "voice", "보컬", "목소리", "노래" } },
        { InstrumentRole::AcousticGuitar, { "acoustic", "어쿠스틱", "통기타" } },
        { InstrumentRole::ElectricGuitar, { "guitar", "gtr", "기타" } },
        { InstrumentRole::Piano,          { "piano", "피아노" } },
        { InstrumentRole::Keys,           { "keys", "rhodes", "organ", "건반", "오르간" } },
        { InstrumentRole::Synth,          { "synth", "신스", "신디" } },
        { InstrumentRole::Pad,            { "pad", "패드" } },
        { InstrumentRole::Strings,        { "strings", "string", "스트링", "현악" } },
        { InstrumentRole::Brass,          { "brass", "horn", "브라스", "관악" } },
        { InstrumentRole::FX,             { "fx", "effect", "효과음" } },
        { InstrumentRole::Master,         { "master", "whole mix", "전체", "마스터", "믹스 전체" } },
    };
    return aliases;
}

/** A goal word plus its opposite, so "너무 X" / "too X" / "덜 X" can be inverted. */
struct GoalWord { std::vector<const char*> words; SoundGoal goal; SoundGoal opposite; bool isProblemWord; };

const std::vector<GoalWord>& goalWords()
{
    // isProblemWord: the word names a problem ("muddy", "탁해"), so mentioning it means "less of it".
    static const std::vector<GoalWord> words {
        { { "단단", "타이트", "탄탄", "tight", "tighter", "solid", "firm", "firmer" },                       SoundGoal::Tighter,       SoundGoal::MoreBody,       false },
        { { "펀치", "때리", "치는 맛", "타격감", "punch", "punchy", "punchier", "impact", "hit harder" },       SoundGoal::Punchier,      SoundGoal::Smoother,       false },
        { { "어택", "클릭", "찰지", "attack", "click", "clicky" },                                 SoundGoal::MoreAttack,    SoundGoal::Smoother,       false },
        { { "따뜻", "포근", "warm", "warmer", "warmth" },                                                    SoundGoal::Warmer,        SoundGoal::Brighter,       false },
        { { "밝", "선명", "화사", "반짝", "bright", "brighter", "crisp", "crisper", "shiny" },                    SoundGoal::Brighter,      SoundGoal::Darker,         false },
        { { "어둡", "차분", "dark", "darker" },                                                    SoundGoal::Darker,        SoundGoal::Brighter,       false },
        { { "탁하", "탁해", "탁한", "탁함", "먹먹", "머디", "뭉개", "답답", "박스", "muddy", "mud", "boxy", "cloudy" },         SoundGoal::LessMuddy,     SoundGoal::LessMuddy,      true },
        { { "쏘", "날카", "거칠", "귀가 아", "귀 아", "쨍", "harsh", "harshness", "piercing", "shrill" },          SoundGoal::LessHarsh,     SoundGoal::LessHarsh,      true },
        { { "붕붕", "부밍", "웅웅", "울렁", "boomy", "boom", "rumble", "woofy" },                           SoundGoal::LessBoomy,     SoundGoal::LessBoomy,      true },
        { { "두껍", "두툼", "풍성", "무게감", "바디", "묵직", "full", "fuller", "fat", "fatter", "thick", "thicker", "body" }, SoundGoal::MoreBody,      SoundGoal::Thinner,        false },
        { { "얇", "가볍", "thin" },                                                      SoundGoal::MoreBody,      SoundGoal::MoreBody,       true },
        { { "공기", "시원", "air", "airy", "airier" },                                             SoundGoal::MoreAir,       SoundGoal::Darker,         false },
        { { "치찰", "스 소리", "ㅅ 소리", "sibilance", "sibilant", "esses", "essy" },                                SoundGoal::LessSibilance, SoundGoal::LessSibilance,  true },
        { { "압축", "눌러", "일정", "컨트롤", "compress", "compressed", "controlled", "consistent", "more even" }, SoundGoal::MoreControlled, SoundGoal::MoreDynamic,   false },
        { { "다이내믹", "다이나믹", "숨쉬", "살아", "dynamic", "dynamics", "breathe", "open up" },        SoundGoal::MoreDynamic,   SoundGoal::MoreControlled, false },
        { { "넓", "스테레오감", "퍼지", "wide", "wider", "width" },                                  SoundGoal::Wider,         SoundGoal::Narrower,       false },
        { { "좁", "모노", "가운데로", "narrow", "narrower", "mono" },                                   SoundGoal::Narrower,      SoundGoal::Wider,          false },
        { { "공간", "울림", "리버브", "잔향", "reverb", "space", "spacious", "ambience", "ambient" },                 SoundGoal::MoreSpace,     SoundGoal::Drier,          false },
        { { "건조", "드라이", "dry", "drier", "dryer" },                                           SoundGoal::Drier,         SoundGoal::MoreSpace,      false },
        { { "크게", "키워", "올려", "볼륨 업", "더 크", "louder", "turn up", "raise" },               SoundGoal::Louder,        SoundGoal::Quieter,        false },
        { { "작게", "낮춰", "볼륨 다운", "시끄", "볼륨 줄", "소리 줄", "quieter", "turn down", "lower" },            SoundGoal::Quieter,       SoundGoal::Louder,         false },
        { { "앞으로 나", "앞으로 빼", "앞으로 와", "존재감", "또렷", "잘 들리", "들리게", "묻혀", "묻히", "forward", "upfront", "presence", "buried", "cut through" },
                                                                                          SoundGoal::Forward,       SoundGoal::Back,           false },
        { { "뒤로", "뒤에", "튀어", "push back", "further back", "sit back", "behind" },                                    SoundGoal::Back,          SoundGoal::Forward,        false },
        { { "부드럽", "매끄럽", "smooth", "smoother", "softer", "silky" },                              SoundGoal::Smoother,      SoundGoal::Punchier,       false },
    };
    return words;
}

bool isAsciiWordChar (char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/**
    Finds a term in lower-cased text. Korean stems are matched as substrings (they inflect:
    단단한/단단하게), ASCII words need word boundaries so that "thin" does not match "thing".
*/
size_t findTerm (const std::string& text, const char* term)
{
    const std::string t (term);
    const bool ascii = std::all_of (t.begin(), t.end(), [] (char c) { return static_cast<unsigned char> (c) < 128; });
    size_t pos = text.find (t);
    while (ascii && pos != std::string::npos)
    {
        const bool startOk = pos == 0 || ! isAsciiWordChar (text[pos - 1]);
        const size_t end = pos + t.size();
        const bool endOk = end >= text.size() || ! isAsciiWordChar (text[end]);
        if (startOk && endOk)
            return pos;
        pos = text.find (t, pos + 1);
    }
    return pos;
}

bool precededByAny (const std::string& text, size_t pos, std::initializer_list<const char*> words, size_t window)
{
    const size_t from = pos > window ? pos - window : 0;
    const auto before = text.substr (from, pos - from);
    for (auto* w : words)
        if (before.find (w) != std::string::npos)
            return true;
    return false;
}
} // namespace

ParsedIntent LocalIntentInterpreter::parse (const std::string& rawText, const MixSession& session, const std::string& rootId) const
{
    ParsedIntent intent;
    const auto text = toLowerAscii (rawText);

    // --- amount -------------------------------------------------------------------
    if (containsAny (text, { "조금", "약간", "살짝", "slightly", "a bit", "a little", "a touch", "tiny" }))
        intent.amount = 0.5f;
    if (containsAny (text, { "많이", "훨씬", "확 ", "아주", "엄청", "a lot", "much more", "way more", "really", "significantly" }))
        intent.amount = 1.5f;

    // --- targets ------------------------------------------------------------------
    struct Mention { size_t pos; InstrumentRole role; };
    std::vector<Mention> mentions;
    for (auto& alias : roleAliases())
    {
        for (auto* w : alias.words)
        {
            const auto pos = findTerm (text, w);
            if (pos == std::string::npos)
                continue;
            // "bass drum" must not also count as "bass"; "backing vocal" not as "vocal".
            const bool shadowed = std::any_of (mentions.begin(), mentions.end(), [&] (const Mention& m) {
                return m.pos <= pos && pos < m.pos + 12 && m.role != alias.role
                       && ((alias.role == InstrumentRole::Bass && m.role == InstrumentRole::Kick)
                           || (alias.role == InstrumentRole::LeadVocal && m.role == InstrumentRole::BackingVocal)
                           || (alias.role == InstrumentRole::DrumBus && m.role == InstrumentRole::Kick));
            });
            if (! shadowed)
                mentions.push_back ({ pos, alias.role });
            break;
        }
    }
    std::sort (mentions.begin(), mentions.end(), [] (auto& a, auto& b) { return a.pos < b.pos; });

    // The most specific mention wins ("드럼의 킥" -> kick, not the drum bus).
    for (auto& m : mentions)
        intent.mentionedRoles.push_back (m.role);

    std::vector<InstrumentRole> specific;
    for (auto r : intent.mentionedRoles)
        if (r != InstrumentRole::DrumBus && r != InstrumentRole::Master)
            specific.push_back (r);

    auto resolveRole = [&] (InstrumentRole r) {
        for (auto& id : session.resolveTarget (toString (r), rootId))
            if (std::find (intent.targetChannelIds.begin(), intent.targetChannelIds.end(), id) == intent.targetChannelIds.end())
                intent.targetChannelIds.push_back (id);
    };

    // Also accept literal track names typed by the user ("Kick In", "Vox Dbl").
    for (auto& id : session.scopeOf (rootId))
    {
        const auto* c = session.find (id);
        if (c != nullptr && c->name.size() >= 2 && text.find (toLowerAscii (c->name)) != std::string::npos)
            intent.targetChannelIds.push_back (id);
    }

    if (intent.targetChannelIds.empty())
    {
        if (! specific.empty())
            resolveRole (specific.front());
        else if (! intent.mentionedRoles.empty())
            resolveRole (intent.mentionedRoles.front());
    }

    // Nothing named, or "전체/master": act on the instance's own channel.
    if (intent.targetChannelIds.empty() && (intent.mentionedRoles.empty() || intent.mentionedRoles.front() == InstrumentRole::Master))
        intent.targetChannelIds.push_back (rootId);

    // --- goals --------------------------------------------------------------------
    // Neutralise common words that contain goal stems ("부탁해" contains "탁해" = muddy).
    std::string goalText = text;
    for (auto* polite : { "부탁" })
        for (auto pos = goalText.find (polite); pos != std::string::npos; pos = goalText.find (polite, pos + 1))
            goalText.replace (pos, std::string (polite).size(), std::string (std::string (polite).size(), '_'));

    for (auto& gw : goalWords())
    {
        for (auto* w : gw.words)
        {
            const auto pos = findTerm (goalText, w);
            if (pos == std::string::npos)
                continue;

            const bool tooMuch = precededByAny (goalText, pos, { "너무", "too ", "지나치게", "과하게" }, 12)
                                 || containsAny (goalText.substr (pos), { "과해", "지나쳐", "too much" });
            const bool less = precededByAny (goalText, pos, { "덜 ", "less ", "not so" }, 8);

            SoundGoal g = gw.goal;
            if (! gw.isProblemWord && (tooMuch || less))
                g = gw.opposite;

            const bool exists = std::any_of (intent.goals.begin(), intent.goals.end(), [g] (auto& x) { return x.goal == g; });
            if (! exists)
                intent.goals.push_back ({ g, intent.amount });
            break;
        }
    }
    return intent;
}

LocalIntentInterpreter::Result LocalIntentInterpreter::interpret (const std::string& text, const MixSession& session,
                                                                 const std::string& rootId) const
{
    Result r;
    r.intent = parse (text, session, rootId);

    if (r.intent.goals.empty())
    {
        r.reply = "요청을 이해하지 못했어요. 예: \"킥을 조금 더 단단하게\", \"보컬이 너무 쏴요\", \"베이스를 살짝 키워줘\"";
        return r;
    }
    if (r.intent.targetChannelIds.empty())
    {
        r.reply = "대상 채널을 찾지 못했어요. 해당 트랙에 플러그인이 삽입되어 있고 이 인스턴스의 범위(버스/마스터) 안에 있는지 확인해 주세요.";
        return r;
    }

    r.understood = true;
    std::string targets, goals;
    for (auto& id : r.intent.targetChannelIds)
    {
        const auto* c = session.find (id);
        if (c == nullptr)
            continue;
        targets += (targets.empty() ? "" : ", ") + (c->name.empty() ? c->id : c->name);
        for (auto& g : r.intent.goals)
        {
            auto res = recipes.actionsFor (*c, g);
            r.actions.insert (r.actions.end(), res.actions.begin(), res.actions.end());
            r.notes.insert (r.notes.end(), res.notes.begin(), res.notes.end());
        }
    }
    for (auto& g : r.intent.goals)
        goals += (goals.empty() ? "" : ", ") + koreanLabel (g.goal);

    r.reply = targets + " → " + goals + (r.intent.amount < 1.0f ? " (조금)" : r.intent.amount > 1.0f ? " (많이)" : "");
    if (r.actions.empty())
        r.reply += "\n적용할 수 있는 플러그인이 체인에 없어요. 필요한 종류의 플러그인을 허용 목록에 체크해 주세요.";
    return r;
}

} // namespace smix
