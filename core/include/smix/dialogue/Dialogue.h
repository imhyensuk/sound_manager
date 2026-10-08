#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/RecipeEngine.h"

namespace smix::dialogue
{

enum class QuestionKind
{
    ChannelName,   // "이 채널에는 어떤 소리가 들어 있나요?"
    Style,         // "보컬을 어떤 스타일로 믹싱할까요?"
    Clarify,       // ambiguous request
    Feedback,      // "단단해졌나요?"
    ManualChange,  // protected channel/plugin: the user has to make the change
    Confirm
};

std::string toString (QuestionKind);

struct Question
{
    int id = 0;
    QuestionKind kind = QuestionKind::Confirm;
    std::string channelId;
    std::string text;
    std::vector<std::string> options;
    int defaultOption = -1;        // used when skipped or timed out (-1: nothing happens)
    bool freeText = false;         // a typed answer is welcome
    double askedAt = 0.0;
    double timeoutSeconds = 0.0;   // 0 = waits until answered or skipped
    nlohmann::json context;        // what to do with the answer

    double remaining (double now) const { return timeoutSeconds > 0 ? std::max (0.0, askedAt + timeoutSeconds - now) : -1.0; }
    nlohmann::json toJson (double now) const;
};

struct Answer
{
    int questionId = 0;
    int option = -1;
    std::string text;
    bool skipped = false;
    bool timedOut = false;

    /** The chosen option text, the typed text, or "" when skipped without a default. */
    std::string value (const Question& q) const;
};

/** Questions the assistant is waiting on. Message-thread / single-thread use. */
class DialogueManager
{
public:
    int ask (Question, double now);
    std::vector<Question> pending() const { return open; }
    const Question* find (int id) const;
    bool hasPending (QuestionKind, const std::string& channelId = {}) const;

    /** option >= 0 picks an option; otherwise text is a free answer. */
    std::optional<std::pair<Question, Answer>> answer (int id, int option, const std::string& text = {});
    std::optional<std::pair<Question, Answer>> skip (int id);

    /** Questions whose time ran out, resolved with their default option. */
    std::vector<std::pair<Question, Answer>> expire (double now);

    /** If the user types instead of clicking, match the text to the newest open question's options. */
    std::optional<std::pair<Question, Answer>> answerFromText (const std::string& text);

private:
    std::vector<Question> open;
    int nextId = 1;
};

/** Style choices offered per instrument before the AI mixes it. */
struct StyleOption
{
    std::string label;
    std::vector<Goal> goals;
};

std::vector<StyleOption> styleOptionsFor (InstrumentRole);

/** Label of the AI default style (used on skip / timeout). */
const std::string& defaultStyleLabel();

} // namespace smix::dialogue
