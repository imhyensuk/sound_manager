#include "smix/ClaudeMixAgent.h"

namespace smix
{

ClaudeMixAgent::ClaudeMixAgent (AgentConfig c, HttpTransport& t) : config (std::move (c)), transport (t)
{
}

void ClaudeMixAgent::resetConversation()
{
    messages = nlohmann::json::array();
}

std::string ClaudeMixAgent::systemPrompt()
{
    return R"(You are the mixing engineer inside "Sound Manager", an audio plugin inserted on DAW mixer channels.

How you hear: you cannot listen to audio directly. Every user turn starts with a <session_snapshot> holding, for each channel in your scope, measured features (short-term/integrated LUFS, peak, crest factor, loudness range, ten band levels from "sub" to "air", spectral centroid, transient density/strength, stereo width/correlation) and a perceptual profile: descriptor scores (boomy, muddy, boxy, harsh, sibilant, bright, thin, punchy, squashed, wide, phase_issue; positive = too much, negative = too little, |1| = clearly audible) and band deviations from a reference curve for the channel's role. Treat these as your ears. After you change something, call get_channel_analysis to listen again before claiming an improvement; measurements need a few seconds of playback to settle, so say so if the audio may not have been playing.

Scope: a track instance mixes only its own channel; a bus instance mixes the bus and every channel routed into it; the master instance mixes everything. Never act on channels outside the snapshot.

What you control, only through apply_mix_actions:
- parameters of the plugins already in each channel's chain (set_param / nudge_param), using real units ("dB", "Hz", "kHz", "ms", "%", "ratio") when the parameter has a role/text that shows the unit, or "normalized" 0..1 otherwise;
- each channel's AI gain stage for level balance (set_gain / nudge_gain, dB);
- bypass (set_bypass), slot order (move_slot) and the whole chain (set_chain) - set_chain may only use plugin uids from list_allowed_plugins; the user chose these and nothing else may be inserted.

Mixing practice: prefer small moves (EQ 1-3 dB, gain 0.5-2 dB per step) and cuts before boosts; fix the cause on the right channel (a "muddy mix" is often one or two channels); respect the processing order subtractive EQ -> dynamics -> colour -> tonal EQ -> time-based effects, with the limiter last on the master. Use get_plugin_parameters before touching a plugin whose parameters were not listed. Translate subjective words the way an engineer would, e.g. a "tighter/단단한 kick" usually means less 250-400 Hz, a controlled low end around 50-70 Hz, a slightly slower compressor attack or more transient attack, and a little 3-5 kHz click.

Reply in the user's language (usually Korean), briefly: what you heard, what you changed and why, and what to listen for. If a request needs a plugin type the user has not allowed, say which category to allow instead of improvising.)";
}

nlohmann::json ClaudeMixAgent::toolDefinitions()
{
    using nlohmann::json;

    const json actionSchema = {
        { "type", "object" },
        { "properties", {
            { "type", { { "type", "string" }, { "enum", { "set_param", "nudge_param", "set_gain", "nudge_gain", "set_bypass", "set_chain", "move_slot" } } } },
            { "channel", { { "type", "string" }, { "description", "Channel id from the snapshot." } } },
            { "slot", { { "type", "integer" }, { "description", "Chain slot index (set_param, nudge_param, set_bypass, move_slot)." } } },
            { "param", { { "type", "string" }, { "description", "Parameter name exactly as listed." } } },
            { "param_index", { { "type", "integer" }, { "description", "Parameter index; preferred over param when known." } } },
            { "value", { { "type", "number" }, { "description", "set_param target, in 'unit'." } } },
            { "delta", { { "type", "number" }, { "description", "nudge_param relative change, in 'unit'." } } },
            { "unit", { { "type", "string" }, { "enum", { "normalized", "dB", "Hz", "kHz", "ms", "s", "%", "ratio" } } } },
            { "value_db", { { "type", "number" }, { "description", "set_gain absolute AI gain in dB." } } },
            { "delta_db", { { "type", "number" }, { "description", "nudge_gain relative change in dB." } } },
            { "bypass", { { "type", "boolean" } } },
            { "plugins", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "set_chain: ordered allowed plugin uids." } } },
            { "to_slot", { { "type", "integer" } } },
            { "reason", { { "type", "string" }, { "description", "Short engineering reason, shown to the user." } } }
        } },
        { "required", { "type", "channel" } }
    };

    return json::array ({
        { { "name", "apply_mix_actions" },
          { "description", "Apply one or more mixing moves. Every move is validated (scope, allowed plugins, safe ranges) and the "
                           "result of each one is returned, including the resulting value in real units when known." },
          { "input_schema", { { "type", "object" },
                              { "properties", { { "actions", { { "type", "array" }, { "items", actionSchema } } },
                                                { "explanation", { { "type", "string" } } } } },
                              { "required", { "actions" } } } } },
        { { "name", "get_plugin_parameters" },
          { "description", "Full parameter list (index, name, normalized value, display text, inferred role) of one plugin slot." },
          { "input_schema", { { "type", "object" },
                              { "properties", { { "channel", { { "type", "string" } } }, { "slot", { { "type", "integer" } } } } },
                              { "required", { "channel", "slot" } } } } },
        { { "name", "get_channel_analysis" },
          { "description", "Listen again: the latest measured features and perceptual profile of one channel." },
          { "input_schema", { { "type", "object" },
                              { "properties", { { "channel", { { "type", "string" } } } } },
                              { "required", { "channel" } } } } },
        { { "name", "list_allowed_plugins" },
          { "description", "Plugins the user allowed the AI to use (uid, name, vendor, category)." },
          { "input_schema", { { "type", "object" }, { "properties", json::object() } } } },
        { { "name", "suggest_chain" },
          { "description", "The built-in planner's recommended plugin order for a channel, with the purpose of each slot." },
          { "input_schema", { { "type", "object" },
                              { "properties", { { "channel", { { "type", "string" } } } } },
                              { "required", { "channel" } } } } }
    });
}

nlohmann::json ClaudeMixAgent::buildRequest() const
{
    nlohmann::json body {
        { "model", config.model },
        { "max_tokens", config.maxTokens },
        { "system", nlohmann::json::array ({ { { "type", "text" }, { "text", systemPrompt() },
                                               { "cache_control", { { "type", "ephemeral" } } } } }) },
        { "tools", toolDefinitions() },
        { "messages", messages },
        { "thinking", { { "type", "adaptive" } } },
        { "output_config", { { "effort", config.effort } } }
    };
    if (config.serverSideFallbacks)
        body["fallbacks"] = "default";
    return body;
}

nlohmann::json ClaudeMixAgent::runTool (const std::string& name, const nlohmann::json& input, SessionAccess& access, Reply& reply,
                                        bool& isError)
{
    isError = false;
    try
    {
        if (name == "apply_mix_actions")
        {
            const auto parsed = parseActions (input.value ("actions", nlohmann::json::array()));
            auto outcomes = access.apply (parsed.actions);
            nlohmann::json results = nlohmann::json::array();
            for (auto& o : outcomes)
                results.push_back (o.toJson());
            reply.outcomes.insert (reply.outcomes.end(), outcomes.begin(), outcomes.end());
            return { { "results", results }, { "parse_errors", parsed.errors } };
        }
        if (name == "get_plugin_parameters")
            return access.pluginParameters (input.at ("channel").get<std::string>(), input.at ("slot").get<int>());
        if (name == "get_channel_analysis")
            return access.channelAnalysis (input.at ("channel").get<std::string>());
        if (name == "list_allowed_plugins")
            return access.allowedPlugins();
        if (name == "suggest_chain")
            return access.suggestChain (input.at ("channel").get<std::string>());
    }
    catch (const std::exception& e)
    {
        isError = true;
        return { { "error", std::string ("invalid tool input: ") + e.what() } };
    }

    isError = true;
    return { { "error", "unknown tool " + name } };
}

ClaudeMixAgent::Reply ClaudeMixAgent::send (const std::string& userText, SessionAccess& access)
{
    Reply reply;

    if (config.apiKey.empty())
    {
        reply.error = "Anthropic API key is not set";
        return reply;
    }

    const auto snapshot = access.snapshot();
    const nlohmann::json userBlock { { "type", "text" },
                                     { "text", "<session_snapshot>\n" + snapshot.dump() + "\n</session_snapshot>\n\n" + userText } };

    // After a failed request or an exhausted tool loop the history already ends with a user turn;
    // extend it instead of creating two consecutive user turns. History is otherwise append-only.
    if (! messages.empty() && messages.back().value ("role", std::string {}) == "user")
        messages.back()["content"].push_back (userBlock);
    else
        messages.push_back ({ { "role", "user" }, { "content", nlohmann::json::array ({ userBlock }) } });

    std::vector<std::pair<std::string, std::string>> headers {
        { "content-type", "application/json" },
        { "x-api-key", config.apiKey },
        { "anthropic-version", "2023-06-01" }
    };
    if (config.serverSideFallbacks)
        headers.emplace_back ("anthropic-beta", "server-side-fallback-2026-07-01");

    for (int round = 0; round < config.maxToolRounds; ++round)
    {
        const auto response = transport.post (config.baseUrl + "/v1/messages", headers, buildRequest().dump());
        if (response.status == 0)
        {
            reply.error = "network error: " + response.error;
            return reply;
        }

        const auto json = nlohmann::json::parse (response.body, nullptr, false);
        if (json.is_discarded())
        {
            reply.error = "invalid JSON from API (HTTP " + std::to_string (response.status) + ")";
            return reply;
        }
        if (response.status != 200)
        {
            const auto message = json.contains ("error") ? json["error"].value ("message", std::string {}) : std::string {};
            reply.error = "API error " + std::to_string (response.status) + ": " + message;
            return reply;
        }

        const auto stopReason = json.value ("stop_reason", std::string {});
        const auto content = json.value ("content", nlohmann::json::array());

        if (stopReason == "refusal")
        {
            // A refused turn is not appended, so the conversation stays valid for the next request.
            reply.error = "the model declined this request";
            return reply;
        }

        // Append the assistant turn unchanged (thinking blocks must be passed back as-is).
        messages.push_back ({ { "role", "assistant" }, { "content", content } });

        std::string text;
        for (auto& block : content)
            if (block.value ("type", std::string {}) == "text")
                text += block.value ("text", std::string {});

        if (stopReason != "tool_use")
        {
            reply.ok = true;
            reply.text = text;
            if (stopReason == "max_tokens")
                reply.text += "\n(응답이 길이 제한으로 잘렸습니다)";
            return reply;
        }

        nlohmann::json results = nlohmann::json::array();
        for (auto& block : content)
        {
            if (block.value ("type", std::string {}) != "tool_use")
                continue;
            bool isError = false;
            const auto output = runTool (block.value ("name", std::string {}), block.value ("input", nlohmann::json::object()),
                                         access, reply, isError);
            nlohmann::json result { { "type", "tool_result" }, { "tool_use_id", block.value ("id", std::string {}) },
                                    { "content", output.dump() } };
            if (isError)
                result["is_error"] = true;
            results.push_back (result);
        }
        messages.push_back ({ { "role", "user" }, { "content", results } });
    }

    reply.ok = ! reply.outcomes.empty();
    reply.text = "(도구 호출 횟수 한도에 도달했습니다. 적용된 변경 사항을 확인해 주세요.)";
    return reply;
}

} // namespace smix
