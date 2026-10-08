#include "AgentWorker.h"

#include "PluginProcessor.h"
#include "Text.h"

#include <optional>

namespace
{
/**
    Runs f on the message thread and waits for the result. The `alive` flag is checked on the
    message thread, so the lambda never touches a processor that has been destroyed meanwhile.
*/
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

/** smix::SessionAccess implemented on top of the hub, marshalled to the message thread. */
class MarshalledAccess : public smix::SessionAccess
{
public:
    MarshalledAccess (SoundManagerProcessor& p, std::shared_ptr<std::atomic<bool>> a, juce::Thread& t)
        : owner (p), alive (std::move (a)), worker (t) {}

    nlohmann::json snapshot() override
    {
        return call ([this] {
            auto& hub = owner.getHub();
            hub.refresh();
            return hub.getSession().snapshotJson (owner.getInstanceId(), true);
        });
    }

    nlohmann::json pluginParameters (const std::string& channelId, int slot) override
    {
        return call ([this, channelId, slot]() -> nlohmann::json {
            auto& hub = owner.getHub();
            hub.refresh();
            if (! hub.getSession().inScope (owner.getInstanceId(), channelId))
                return { { "error", "channel outside scope" } };
            const auto* c = hub.getSession().find (channelId);
            if (c == nullptr || slot < 0 || slot >= static_cast<int> (c->chain.size()))
                return { { "error", "no such channel/slot" } };
            return c->chain[static_cast<size_t> (slot)].toJson (true, 256);
        });
    }

    nlohmann::json channelAnalysis (const std::string& channelId) override
    {
        return call ([this, channelId]() -> nlohmann::json {
            auto& hub = owner.getHub();
            hub.refresh();
            const auto* c = hub.getSession().find (channelId);
            if (c == nullptr || ! hub.getSession().inScope (owner.getInstanceId(), channelId))
                return { { "error", "channel outside scope" } };
            return c->toJson (false);
        });
    }

    nlohmann::json allowedPlugins() override
    {
        return call ([this] {
            nlohmann::json arr = nlohmann::json::array();
            for (auto& p : owner.getLibrary().catalog().allowedPlugins())
                arr.push_back ({ { "uid", p.uid }, { "name", p.name }, { "vendor", p.manufacturer },
                                 { "category", smix::toString (p.category) } });
            return nlohmann::json { { "allowed_plugins", arr } };
        });
    }

    nlohmann::json suggestChain (const std::string& channelId) override
    {
        return call ([this, channelId]() -> nlohmann::json {
            if (! owner.getHub().getSession().inScope (owner.getInstanceId(), channelId))
                return { { "error", "channel outside scope" } };
            return owner.getHub().planChainFor (channelId).toJson();
        });
    }

    std::vector<smix::ActionOutcome> apply (const std::vector<smix::MixAction>& actions) override
    {
        auto result = onMessageThread<std::vector<smix::ActionOutcome>> (alive, worker, [this, actions] {
            return owner.getHub().apply (actions, owner.getInstanceId());
        });
        return result.value_or (std::vector<smix::ActionOutcome> {});
    }

private:
    template <typename Fn>
    nlohmann::json call (Fn&& fn)
    {
        auto r = onMessageThread<nlohmann::json> (alive, worker, std::forward<Fn> (fn));
        return r.value_or (nlohmann::json { { "error", "plugin is shutting down" } });
    }

    SoundManagerProcessor& owner;
    std::shared_ptr<std::atomic<bool>> alive;
    juce::Thread& worker;
};

juce::String describeOutcomes (const std::vector<smix::ActionOutcome>& outcomes)
{
    juce::String s;
    for (auto& o : outcomes)
        s << (o.ok ? juce::String::fromUTF8 ("\xE2\x9C\x93 ") : juce::String::fromUTF8 ("\xE2\x9C\x97 ")) << juce::String (o.message) << "\n";
    return s.trimEnd();
}
} // namespace

//==============================================================================
AgentWorker::AgentWorker (SoundManagerProcessor& o) : juce::Thread ("Sound Manager AI"), owner (o)
{
}

AgentWorker::~AgentWorker()
{
    shutdown();
}

void AgentWorker::shutdown()
{
    alive->store (false);
    signalThreadShouldExit();
    wake.signal();
    stopThread (4000);
}

void AgentWorker::submit (const juce::String& userText)
{
    {
        const juce::ScopedLock sl (queueLock);
        queue.add (userText);
    }
    if (! isThreadRunning())
        startThread();
    wake.signal();
}

void AgentWorker::resetConversation()
{
    resetRequested = true;
}

void AgentWorker::run()
{
    while (! threadShouldExit())
    {
        juce::String next;
        {
            const juce::ScopedLock sl (queueLock);
            if (! queue.isEmpty())
            {
                next = queue[0];
                queue.remove (0);
            }
        }

        if (next.isEmpty())
        {
            wake.wait (500);
            continue;
        }

        busy = true;
        handle (next);
        busy = false;
    }
}

void AgentWorker::handle (const juce::String& text)
{
    if (resetRequested.exchange (false))
        agent.resetConversation();

    // Settings live in a PropertiesFile owned by the message thread.
    struct Settings { std::string key, model, effort; };
    const auto settings = onMessageThread<Settings> (alive, *this, [this] {
        auto& lib = owner.getLibrary();
        return Settings { lib.getApiKey().toStdString(), lib.getModel().toStdString(), lib.getEffort().toStdString() };
    });
    if (! settings)
        return;

    juce::String replyText;

    if (settings->key.empty())
    {
        // Offline fallback: rule-based interpretation + recipes.
        const auto result = onMessageThread<juce::String> (alive, *this, [this, text] {
            auto& hub = owner.getHub();
            hub.refresh();
            const auto r = local.interpret (text.toStdString(), hub.getSession(), owner.getInstanceId());
            juce::String out = juce::String (r.reply);
            if (r.understood)
            {
                const auto outcomes = hub.apply (r.actions, owner.getInstanceId());
                if (! outcomes.empty())
                    out << "\n" << describeOutcomes (outcomes);
            }
            for (auto& n : r.notes)
                out << "\n- " << juce::String (n);
            out << "\n" << ko ("(오프라인 모드: 설정 탭에서 Anthropic API 키를 입력하면 Claude가 직접 듣고 판단합니다)");
            return out;
        });
        replyText = result.value_or (juce::String());
    }
    else
    {
        smix::AgentConfig config = agent.getConfig();
        config.apiKey = settings->key;
        config.model = settings->model;
        config.effort = settings->effort;
        agent.setConfig (config);

        MarshalledAccess access (owner, alive, *this);
        const auto reply = agent.send (text.toStdString(), access);
        if (reply.ok)
            replyText = juce::String (reply.text);
        else
            replyText = ko ("오류: ") + juce::String (reply.error);
        if (! reply.outcomes.empty())
            replyText << "\n\n" << describeOutcomes (reply.outcomes);
    }

    juce::MessageManager::callAsync ([alive = alive, &o = owner, replyText] {
        if (alive->load())
            o.addLog ("AI", replyText);
    });
}
