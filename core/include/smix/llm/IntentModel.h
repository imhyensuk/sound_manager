#pragma once

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/LocalIntentInterpreter.h"
#include "smix/MixSession.h"

namespace smix::llm
{

/** Everything the reasoning model may look at for one user message. */
struct IntentRequest
{
    std::string userText;
    const MixSession* session = nullptr;
    std::string scopeRootId;
    std::string knowledge;                                        // retrieved plugin knowledge (RAG)
    std::string referenceStyle;                                   // short description of the reference track, if any
    std::vector<std::pair<std::string, std::string>> recentTurns;  // (role, text), oldest first
    std::string pendingQuestion;                                  // the question the user may be answering
};

/** What the user wants, in a structure the assistant can act on. */
struct IntentResult
{
    bool understood = false;
    std::vector<std::string> targets;  // channel ids
    std::vector<Goal> goals;
    float amount = 1.0f;
    std::string question;              // clarifying question when the request is ambiguous
    std::vector<std::string> options;
    std::string reply;                 // natural-language answer for the chat
    std::string view;                  // rta | waterfall | meters | loudness | correlation
    std::string command;               // undo | automix_start | automix_stop | match_reference | plan_chain |
                                       // feedback_ok | feedback_more | feedback_less | status
    std::string source;                // "rules" | "llm"

    nlohmann::json toJson() const;
};

/** A module that understands requests and context (requirement 8: reasoning model as a module). */
class IntentModel
{
public:
    virtual ~IntentModel() = default;
    virtual std::string name() const = 0;
    virtual IntentResult interpret (const IntentRequest&) = 0;
};

/** Always-available rule model (Korean/English vocabulary + recipes). */
class RuleIntentModel : public IntentModel
{
public:
    std::string name() const override { return "rules"; }
    IntentResult interpret (const IntentRequest&) override;

    /** UI view requested by the text ("waterfall 보여줘"), or "". */
    static std::string detectView (const std::string& text);
    /** Assistant command in the text, or "". */
    static std::string detectCommand (const std::string& text, bool feedbackExpected);

private:
    LocalIntentInterpreter interpreter;
};

/** Parses the JSON object the language model produces (see IntentGrammar) into an IntentResult. */
IntentResult parseModelJson (const nlohmann::json&, const IntentRequest&);

/** GBNF grammar that forces the language model to answer with that JSON object. */
const std::string& intentGrammar();

/** System prompt for the local language model. */
std::string intentSystemPrompt();

/** User-turn content: compact session state, knowledge and the request. */
std::string intentUserPrompt (const IntentRequest&);

} // namespace smix::llm
