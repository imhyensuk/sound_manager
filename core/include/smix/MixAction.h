#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/MixSession.h"
#include "smix/PluginCatalog.h"

namespace smix
{

enum class ActionType
{
    SetParam,    // absolute value (in a unit, or normalised)
    NudgeParam,  // relative change (in a unit, or normalised)
    SetGain,     // channel AI gain stage, absolute dB
    NudgeGain,   // channel AI gain stage, relative dB
    SetBypass,   // bypass/enable a slot
    SetChain,    // replace a channel's chain with an ordered list of allowed plugins
    MoveSlot     // reorder one slot
};

std::string toString (ActionType);

/** A single mixing move, as produced by the AI, the local interpreter or the auto mixer. */
struct MixAction
{
    ActionType type = ActionType::SetParam;
    std::string channelId;
    int slot = -1;
    std::string param;            // parameter name (alternative to paramIndex)
    int paramIndex = -1;
    std::optional<double> value;  // SetParam / SetGain
    double delta = 0.0;           // NudgeParam / NudgeGain
    std::string unit;             // "", "normalized", "dB", "Hz", "kHz", "ms", "s", "%", "ratio"
    bool bypass = false;
    std::vector<std::string> plugins;  // SetChain: plugin uids in order
    int toSlot = -1;
    std::string reason;

    nlohmann::json toJson() const;
};

struct ParseResult
{
    std::vector<MixAction> actions;
    std::vector<std::string> errors;
};

/** Parses the "actions" array produced by the language model. Never throws. */
ParseResult parseActions (const nlohmann::json& actions);

/** Safety rails applied to every action, whoever produced it. */
struct ActionLimits
{
    float minGainDb = -24.0f;
    float maxGainDb = 12.0f;
    float maxGainStepDb = 6.0f;     // per action
    float maxNormalisedStep = 0.5f; // per action, for parameter moves
    size_t maxChainLength = 8;

    /** Limits for restoring a recorded state (user-initiated): any value may be set back. */
    static ActionLimits forRestore() { ActionLimits l; l.maxGainStepDb = 48.0f; l.maxNormalisedStep = 1.0f; l.maxChainLength = 64; return l; }
};

/** The host side that actually turns resolved actions into sound. */
class MixController
{
public:
    virtual ~MixController() = default;
    virtual bool setParameter (const std::string& channelId, int slot, int paramIndex, float normalised) = 0;
    virtual bool setGain (const std::string& channelId, float gainDb) = 0;
    virtual bool setBypass (const std::string& channelId, int slot, bool bypassed) = 0;
    virtual bool setChain (const std::string& channelId, const std::vector<std::string>& pluginUids) = 0;
    virtual bool moveSlot (const std::string& channelId, int fromSlot, int toSlot) = 0;
};

struct ActionOutcome
{
    ActionOutcome() = default;
    ActionOutcome (MixAction a, bool success, std::string text) : action (std::move (a)), ok (success), message (std::move (text)) {}

    MixAction action;
    bool ok = false;
    std::string message;  // human readable result or error ("Band 1 Gain: 0.0 dB -> +3.0 dB")

    /** Blocked because the user protected the channel/plugin: the AI asks the user to do it by hand. */
    bool needsUser = false;
    std::string manualRequest;

    nlohmann::json toJson() const;
};

/**
    Validates actions against scope, the user's allowed plugin list and limits,
    converts real-world units to normalised values, then drives the MixController.
*/
class ActionExecutor
{
public:
    ActionExecutor (MixSession&, const PluginCatalog&, MixController&, ActionLimits = {});

    ActionOutcome execute (const MixAction&, const std::string& scopeRootId);
    std::vector<ActionOutcome> executeAll (const std::vector<MixAction>&, const std::string& scopeRootId);

private:
    ActionOutcome executeParam (const MixAction&, ChannelState&);
    ActionOutcome executeGain (const MixAction&, ChannelState&);
    ActionOutcome executeChain (const MixAction&, ChannelState&);
    std::optional<ActionOutcome> checkProtection (const MixAction&, const ChannelState&) const;

    MixSession& session;
    const PluginCatalog& catalog;
    MixController& controller;
    ActionLimits limits;
};

} // namespace smix
