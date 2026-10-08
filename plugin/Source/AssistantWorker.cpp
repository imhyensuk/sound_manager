#include "AssistantWorker.h"

#include <smix/style/GenreProfile.h>

#include "PluginProcessor.h"
#include "Text.h"

using namespace smix;

namespace
{
template <typename Result, typename Fn>
std::optional<Result> onMessageThread (const std::shared_ptr<std::atomic<bool>>& alive, juce::Thread& worker, Fn&& fn)
{
    struct State
    {
        std::optional<Result> result;
        juce::WaitableEvent done;
    };
    auto state = std::make_shared<State>();
    juce::MessageManager::callAsync ([state, alive, fn = std::forward<Fn> (fn)]() mutable {
        if (alive->load())
            state->result = fn();
        state->done.signal();
    });
    while (! state->done.wait (100))
        if (worker.threadShouldExit())
            return std::nullopt;
    return state->result;
}
} // namespace

//==============================================================================
/** AssistantHost on top of the hub; every call runs on the message thread. */
class AssistantWorker::Host : public AssistantHost
{
public:
    Host (SoundManagerProcessor& p, std::shared_ptr<std::atomic<bool>> a, juce::Thread& t) : owner (p), alive (std::move (a)), worker (t) {}

    const MixSession& session() override
    {
        // A copy, refreshed at most every 200 ms: the hub mutates its own session on the message thread.
        const double now = Engine::now();
        if (now - copiedAt > 0.2)
        {
            if (auto s = call<MixSession> ([this] {
                    owner.getHub().refresh();
                    return owner.getHub().getSession();
                }))
                copy = std::move (*s);
            copiedAt = now;
        }
        return copy;
    }

    std::vector<ActionOutcome> apply (const std::vector<MixAction>& actions, const std::string& label) override
    {
        copiedAt = 0;
        return call<std::vector<ActionOutcome>> ([this, actions, label] {
                   return owner.getHub().apply (actions, owner.getInstanceId(), label, "chat");
               }).value_or (std::vector<ActionOutcome> {});
    }

    bool undoLast() override
    {
        copiedAt = 0;
        return call<bool> ([this] {
                   juce::String report;
                   const bool ok = owner.getHub().undo (owner.getInstanceId(), &report);
                   if (ok)
                       owner.addLog ("system", report);
                   return ok;
               }).value_or (false);
    }

    void renameChannel (const std::string& id, const std::string& name) override
    {
        copiedAt = 0;
        call<bool> ([this, id, name] {
            if (auto* p = owner.getHub().findInstance (id))
                p->setUserLabel (juce::String::fromUTF8 (name.c_str()));
            return true;
        });
    }

    void setChannelStyle (const std::string& id, const std::string& s) override
    {
        copiedAt = 0;
        call<bool> ([this, id, s] {
            if (auto* p = owner.getHub().findInstance (id))
                p->setStyle (juce::String::fromUTF8 (s.c_str()));
            return true;
        });
    }

    void showView (const std::string& view) override
    {
        call<bool> ([this, view] {
            owner.requestView (juce::String (view));
            return true;
        });
    }

    void setAutoMix (bool on) override
    {
        call<bool> ([this, on] {
            owner.setAutoMixEnabled (on);
            return true;
        });
    }

    std::string knowledgeFor (const std::string& query) override
    {
        return call<std::string> ([this, query] {
                   auto& e = owner.getEngine();
                   auto lease = e.modules().acquire (modules::ModuleId::Knowledge, Engine::now());
                   return lease ? e.knowledge().contextFor (query, 3, 1800) : std::string {};
               }).value_or (std::string {});
    }

    const style::StyleProfile* referenceStyle() override
    {
        reference = call<std::optional<style::StyleProfile>> ([this]() -> std::optional<style::StyleProfile> {
                        // The reference chosen on this instance, or on the master.
                        juce::String name = owner.getReferenceName();
                        for (auto* p : owner.getHub().getInstances())
                            if (name.isEmpty() && p->getEffectiveKind() == ChannelKind::Master)
                                name = p->getReferenceName();
                        if (const auto* r = owner.getEngine().findReference (name.toStdString()))
                            return *r;
                        // No reference chosen: the learned genre's average finished mix.
                        if (const auto g = style::activeGenre(); g && g->songs > 0)
                            return g->master;
                        return std::nullopt;
                    }).value_or (std::nullopt);
        return reference ? &*reference : nullptr;
    }

    std::vector<std::string> nameSuggestions (const std::string& id) override
    {
        auto guesses = call<std::vector<std::string>> ([this, id] {
                           auto* p = owner.getHub().findInstance (id);
                           if (p == nullptr)
                               return std::vector<std::string> {};
                           auto s = p->nameSuggestions();
                           if (s.empty())
                               p->startEarCapture();  // suggestions appear with the next question
                           return s;
                       }).value_or (std::vector<std::string> {});
        for (auto* common : { "킥", "스네어", "베이스", "리드 보컬", "일렉 기타", "피아노" })
            if (std::find (guesses.begin(), guesses.end(), common) == guesses.end() && guesses.size() < 6)
                guesses.push_back (common);
        return guesses;
    }

    std::string rootId()
    {
        return call<std::string> ([this] { return owner.getInstanceId(); }).value_or (std::string {});
    }

    std::string statusText() override
    {
        return call<std::string> ([this] { return owner.getEngine().statusText().toStdString(); }).value_or (std::string {});
    }

private:
    template <typename R, typename Fn>
    std::optional<R> call (Fn&& fn) { return onMessageThread<R> (alive, worker, std::forward<Fn> (fn)); }

    SoundManagerProcessor& owner;
    std::shared_ptr<std::atomic<bool>> alive;
    juce::Thread& worker;
    MixSession copy;
    double copiedAt = 0;
    std::optional<style::StyleProfile> reference;
};

//==============================================================================
AssistantWorker::AssistantWorker (SoundManagerProcessor& o) : juce::Thread ("Sound Manager assistant"), owner (o)
{
    host = std::make_unique<Host> (owner, alive, static_cast<juce::Thread&> (*this));
    startThread();
}

AssistantWorker::~AssistantWorker()
{
    shutdown();
}

void AssistantWorker::shutdown()
{
    alive->store (false);
    signalThreadShouldExit();
    wake.signal();
    stopThread (6000);
}

void AssistantWorker::submit (const juce::String& text)
{
    {
        const juce::ScopedLock sl (lock);
        Task t;
        t.text = text;
        queue.push_back (t);
    }
    wake.signal();
}

void AssistantWorker::answer (int questionId, int option, bool skip, const juce::String& text)
{
    {
        const juce::ScopedLock sl (lock);
        Task t;
        t.type = Task::Answer;
        t.questionId = questionId;
        t.option = option;
        t.skip = skip;
        t.text = text;
        queue.push_back (t);
        questions.erase (std::remove_if (questions.begin(), questions.end(), [questionId] (auto& q) { return q.id == questionId; }), questions.end());
    }
    wake.signal();
}

void AssistantWorker::resetConversation()
{
    {
        const juce::ScopedLock sl (lock);
        Task t;
        t.type = Task::Reset;
        queue.push_back (t);
    }
    wake.signal();
}

std::vector<AssistantWorker::PendingQuestion> AssistantWorker::pendingQuestions() const
{
    const juce::ScopedLock sl (lock);
    return questions;
}

juce::String AssistantWorker::status() const
{
    const juce::ScopedLock sl (lock);
    return currentStatus;
}

juce::String AssistantWorker::reasoningEngine() const
{
    const juce::ScopedLock sl (lock);
    return engineName;
}

void AssistantWorker::setStatus (const juce::String& s)
{
    const juce::ScopedLock sl (lock);
    currentStatus = s;
}

void AssistantWorker::publish (const std::vector<MixAssistant::Message>& messages)
{
    std::vector<PendingQuestion> open;
    if (assistant != nullptr)
        for (auto& q : assistant->dialogue().pending())
        {
            PendingQuestion p;
            p.id = q.id;
            p.text = juce::String::fromUTF8 (q.text.c_str());
            for (auto& o : q.options)
                p.options.add (juce::String::fromUTF8 (o.c_str()));
            p.defaultOption = q.defaultOption;
            p.freeText = q.freeText;
            p.remainingSeconds = q.remaining (Engine::now());
            open.push_back (p);
        }
    {
        const juce::ScopedLock sl (lock);
        questions = open;
    }
    if (messages.empty())
        return;
    juce::StringArray texts;
    for (auto& m : messages)
        texts.add (juce::String::fromUTF8 (m.text.c_str()));
    juce::MessageManager::callAsync ([alive = alive, &o = owner, texts] {
        if (alive->load())
            for (auto& t : texts)
                o.addLog ("AI", t);
    });
}

void AssistantWorker::run()
{
    assistant = std::make_unique<MixAssistant> (host->rootId(), rules);
    double lastTick = 0;
    while (! threadShouldExit())
    {
        std::optional<Task> task;
        {
            const juce::ScopedLock sl (lock);
            if (! queue.empty())
            {
                task = queue.front();
                queue.pop_front();
            }
        }
        if (task)
        {
            busy = true;
            handle (*task);
            busy = false;
            setStatus ({});
            continue;
        }

        // Questions time out, channels need names, changes need to be listened to again.
        const double now = Engine::now();
        if (now - lastTick > 0.5)
        {
            lastTick = now;
            assistant->setRootId (host->rootId());  // the id changes when a project is loaded
            publish (assistant->tick (*host, now));
        }
        wake.wait (250);
    }
    assistant.reset();
}

void AssistantWorker::handle (const Task& t)
{
    const double now = Engine::now();
    assistant->setRootId (host->rootId());
    if (t.type == Task::Reset)
    {
        assistant = std::make_unique<MixAssistant> (host->rootId(), rules);
        publish ({ { "새 대화를 시작했어요." } });
        return;
    }
    if (t.type == Task::Answer)
    {
        publish (assistant->handleAnswer (t.questionId, t.option, t.skip, t.text.toStdString(), *host, now));
        return;
    }

    // The reasoning module (local LLM) is loaded on demand; the ETA covers loading and generating.
    auto& engine = owner.getEngine();
    std::string error;
    llm::IntentModel* reasoning = nullptr;
    modules::ModuleManager::Lease lease;
    if (engine.modules().isEnabled (modules::ModuleId::Intent) && engine.llmModelFile().existsAsFile())
    {
        if (! engine.modules().isLoaded (modules::ModuleId::Intent))
            setStatus (ko ("언어 모델 불러오는 중 · ") + juce::String (formatDuration (engine.llmLoadSecondsEstimate())) + ko (" 남음"));
        lease = engine.modules().acquire (modules::ModuleId::Intent, now, &error);
        if (lease)
            reasoning = &engine.llmIntent();
        else
            publish ({ { "언어 모델을 쓸 수 없어 규칙 기반으로 이해할게요: " + error } });
    }
    {
        const juce::ScopedLock sl (lock);
        engineName = reasoning != nullptr ? "local-llm" : "rules";
    }
    if (reasoning != nullptr)
    {
        // ~120 answer tokens + ~600 prompt tokens (most of the system prompt is cached).
        const double eta = 120.0 / engine.llm().tokensPerSecond() + 300.0 / engine.llm().promptTokensPerSecond();
        setStatus (ko ("요청 이해 중 (로컬 LLM) · ") + juce::String (formatDuration (eta)) + ko (" 남음"));
    }
    else
    {
        setStatus (ko ("요청 이해 중"));
    }
    assistant->setReasoningModel (reasoning);
    publish (assistant->handleUser (t.text.toStdString(), *host, now));
}
