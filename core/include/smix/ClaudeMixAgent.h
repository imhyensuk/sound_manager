#pragma once

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/MixAction.h"

namespace smix
{

struct HttpResponse
{
    int status = 0;          // 0 = transport failure
    std::string body;
    std::string error;
};

/** Minimal HTTPS POST abstraction so the agent can be unit-tested and hosted by JUCE, curl, etc. */
class HttpTransport
{
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse post (const std::string& url,
                               const std::vector<std::pair<std::string, std::string>>& headers,
                               const std::string& body) = 0;
};

/** The agent's window onto the mix. The host implements it (thread marshalling is the host's job). */
class SessionAccess
{
public:
    virtual ~SessionAccess() = default;
    virtual nlohmann::json snapshot() = 0;                                              // channels in scope, chains, ears
    virtual nlohmann::json pluginParameters (const std::string& channelId, int slot) = 0;
    virtual nlohmann::json channelAnalysis (const std::string& channelId) = 0;          // fresh measurement
    virtual nlohmann::json allowedPlugins() = 0;
    virtual nlohmann::json suggestChain (const std::string& channelId) = 0;
    virtual std::vector<ActionOutcome> apply (const std::vector<MixAction>&) = 0;
};

struct AgentConfig
{
    std::string apiKey;
    std::string model = "claude-opus-5-5";
    std::string effort = "medium";
    int maxTokens = 16000;
    int maxToolRounds = 8;
    std::string baseUrl = "https://api.anthropic.com";
    bool serverSideFallbacks = true;
};

/**
    Chat-driven mixing engineer backed by the Claude Messages API (requirement 7).

    The model "hears" through the analysis snapshot (loudness, spectrum, transients,
    stereo image and perceptual descriptors per channel), inspects plugin parameters
    through tools and changes the mix only through validated MixActions.

    send() blocks; call it from a worker thread.
*/
class ClaudeMixAgent
{
public:
    ClaudeMixAgent (AgentConfig, HttpTransport&);

    struct Reply
    {
        bool ok = false;
        std::string text;                  // the model's answer for the chat window
        std::vector<ActionOutcome> outcomes;
        std::string error;
    };

    Reply send (const std::string& userText, SessionAccess&);

    void resetConversation();
    const nlohmann::json& conversation() const noexcept { return messages; }

    void setConfig (AgentConfig c) { config = std::move (c); }
    const AgentConfig& getConfig() const noexcept { return config; }

    static std::string systemPrompt();
    static nlohmann::json toolDefinitions();

    /** Builds the request body; exposed for tests. */
    nlohmann::json buildRequest() const;

private:
    nlohmann::json runTool (const std::string& name, const nlohmann::json& input, SessionAccess&, Reply&, bool& isError);

    AgentConfig config;
    HttpTransport& transport;
    nlohmann::json messages = nlohmann::json::array();
};

} // namespace smix
