#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "smix/AutoMixer.h"
#include "smix/MixHistory.h"
#include "smix/PerceptualProfile.h"
#include "smix/dialogue/Dialogue.h"
#include "smix/llm/IntentModel.h"
#include "smix/style/StyleProfile.h"

namespace smix
{

/** The host side of the assistant (implemented by the plugin's hub, and by fakes in tests). */
class AssistantHost
{
public:
    virtual ~AssistantHost() = default;
    virtual const MixSession& session() = 0;  // fresh state of every channel
    /** Validated execution; records a history snapshot labelled `label`. */
    virtual std::vector<ActionOutcome> apply (const std::vector<MixAction>&, const std::string& label) = 0;
    virtual bool undoLast() = 0;
    virtual void renameChannel (const std::string& channelId, const std::string& name) = 0;
    virtual void setChannelStyle (const std::string& channelId, const std::string& style) = 0;
    virtual void showView (const std::string& view) = 0;
    virtual void setAutoMix (bool on) = 0;
    virtual std::string knowledgeFor (const std::string& query) { (void) query; return {}; }
    virtual const style::StyleProfile* referenceStyle() { return nullptr; }
    virtual std::vector<std::string> nameSuggestions (const std::string& channelId) { (void) channelId; return {}; }
    virtual std::string statusText() { return {}; }
};

/**
    The active mixing assistant (requirements 5, 10, 12, 13, 14):

      user text -> reasoning module (local LLM, or rules) -> clarify if ambiguous
                -> ask the style before mixing a channel the first time (skip/timeout = AI default)
                -> apply bounded moves (history snapshot) -> protected parts become requests to the user
                -> listen again after a few seconds: not there yet -> one more step by itself,
                   otherwise ask how it sounds (more / less / good / undo)

    It also asks for a name for every channel whose content it cannot tell.
    Single-threaded: call from the message thread (the LLM call itself may be slow; the plugin
    runs handleUser() on a worker that holds the session lock only for apply()).
*/
class MixAssistant
{
public:
    struct Message
    {
        std::string text;
        bool isQuestion = false;
        int questionId = 0;
    };

    struct Options
    {
        double styleQuestionTimeout = 25.0;    // seconds; then the AI default style is used
        double feedbackQuestionTimeout = 40.0; // then the change is kept
        double verifyAfterSeconds = 4.0;       // listening time before judging a change
        int maxSelfCorrections = 1;
        bool askForNames = true;
        bool askStyleBeforeMixing = true;
    };

    MixAssistant (std::string scopeRootId, llm::IntentModel& rules);

    void setReasoningModel (llm::IntentModel* model) { reasoning = model; }
    void setRootId (std::string id) { rootId = std::move (id); }
    const std::string& getRootId() const noexcept { return rootId; }
    Options& options() noexcept { return opts; }
    dialogue::DialogueManager& dialogue() noexcept { return questions; }

    /** A chat message from the user. Returns what to show in the chat. */
    std::vector<Message> handleUser (const std::string& text, AssistantHost&, double now);

    /** The user clicked an option or "건너뛰기" (skip = option -1 with skip true). */
    std::vector<Message> handleAnswer (int questionId, int option, bool skip, const std::string& text, AssistantHost&, double now);

    /** Timeouts, naming requests and the listen-again checks. Call every ~0.5 s. */
    std::vector<Message> tick (AssistantHost&, double now);

    /** Starts mixing every channel in scope (asks styles first). */
    std::vector<Message> startMixing (AssistantHost&, double now);

    const std::vector<std::pair<std::string, std::string>>& transcript() const noexcept { return turns; }

    /** Generic DAW names ("Audio 1", "Track 3", "Channel ab12") that say nothing about the content. */
    static bool isUninformativeName (const std::string&);

private:
    struct PendingChange
    {
        std::vector<std::string> channels;
        std::vector<Goal> goals;
        std::string label;
        double appliedAt = 0.0;
        std::map<std::string, PerceptualProfile> before;
        std::map<std::string, float> loudnessBefore;
        int corrections = 0;
        bool verified = false;
    };

    std::vector<Message> applyGoals (const std::vector<std::string>& channels, const std::vector<Goal>& goals,
                                     const std::string& label, AssistantHost&, double now, bool askFeedback);
    std::vector<Message> runCommand (const llm::IntentResult&, AssistantHost&, double now);
    std::vector<Message> askStyle (const ChannelState&, AssistantHost&, double now);
    std::vector<Message> resolveAnswer (const dialogue::Question&, const dialogue::Answer&, AssistantHost&, double now);
    std::vector<Message> verify (AssistantHost&, double now);
    std::vector<Message> continueMixing (AssistantHost&, double now);
    std::vector<Goal> goalsForStyle (const ChannelState&, const std::string& styleLabel, AssistantHost&) const;
    Message ask (dialogue::Question, double now);
    void remember (const std::string& role, const std::string& text);

    std::string rootId;
    llm::IntentModel& rules;
    llm::IntentModel* reasoning = nullptr;
    Options opts;
    dialogue::DialogueManager questions;
    RecipeEngine recipes;

    std::vector<PendingChange> changes;
    std::set<std::string> namingAsked;
    std::vector<std::string> mixQueue;  // channels waiting to be mixed after their style is known
    bool mixing = false;
    std::vector<std::pair<std::string, std::string>> turns;
};

/** Descriptor the goal is expected to move, and in which direction (+1 = score should rise). */
std::optional<std::pair<Descriptor, int>> expectedEffect (SoundGoal);

} // namespace smix
