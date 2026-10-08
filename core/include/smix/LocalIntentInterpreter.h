#pragma once

#include <string>
#include <vector>

#include "smix/MixSession.h"
#include "smix/RecipeEngine.h"

namespace smix
{

/** What the user asked for, in structured form. */
struct ParsedIntent
{
    std::vector<std::string> targetChannelIds;  // empty = could not resolve
    std::vector<InstrumentRole> mentionedRoles;
    std::vector<Goal> goals;
    float amount = 1.0f;
};

/**
    Offline, rule-based understanding of mixing requests in Korean and English
    ("드럼의 킥이 조금 더 단단한 소리가 나면 좋겠어", "vocals are too harsh").

    It is the fallback when no API key / network is available and also a fast path
    for simple requests. Anything it cannot handle is left to the language model.
*/
class LocalIntentInterpreter
{
public:
    ParsedIntent parse (const std::string& text, const MixSession&, const std::string& scopeRootId) const;

    struct Result
    {
        ParsedIntent intent;
        std::vector<MixAction> actions;
        std::vector<std::string> notes;
        std::string reply;  // Korean explanation for the chat window
        bool understood = false;
    };

    Result interpret (const std::string& text, const MixSession&, const std::string& scopeRootId) const;

private:
    RecipeEngine recipes;
};

} // namespace smix
