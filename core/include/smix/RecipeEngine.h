#pragma once

#include <string>
#include <vector>

#include "smix/MixAction.h"
#include "smix/MixSession.h"

namespace smix
{

/** Sound goals in the vocabulary of a mixing engineer. */
enum class SoundGoal
{
    Tighter,        // 단단하게 / tight / solid
    Punchier,       // 펀치감 / punchy
    MoreAttack,     // 어택, 클릭 / click
    Warmer,
    Brighter,
    Darker,
    LessMuddy,
    LessHarsh,
    LessBoomy,
    MoreBody,       // 두껍게 / full / fat
    Thinner,
    MoreAir,
    LessSibilance,
    MoreControlled, // 더 압축 / consistent
    MoreDynamic,
    Wider,
    Narrower,
    MoreSpace,
    Drier,
    Louder,
    Quieter,
    Forward,        // 앞으로 / 존재감
    Back,
    Smoother
};

std::string toString (SoundGoal);
std::string koreanLabel (SoundGoal);

struct Goal
{
    SoundGoal goal;
    float amount = 1.0f;  // 0.5 = "조금", 1 = normal, 1.5 = "많이"
};

/**
    Turns sound goals into concrete moves on whatever plugins the channel actually has.
    It understands plugins only through ParamSemantics (+ learned ValueMappers), so it works
    with any vendor's EQ/compressor as long as parameter names are reasonably descriptive.
*/
class RecipeEngine
{
public:
    struct Result
    {
        std::vector<MixAction> actions;
        std::vector<std::string> notes;  // e.g. "no compressor on this channel - allow one to shape attack"
        std::vector<std::pair<int, int>> claimedBands;  // (slot, gain param index) already used by this result
    };

    Result actionsFor (const ChannelState&, const Goal&) const;

    /** Sensible starting settings for a freshly inserted plugin (role-aware). */
    Result initialSettings (const ChannelState&, int slotIndex) const;

    /** Single building blocks, for other planners (style matching, the language model). */
    Result eq (const ChannelState& c, double freqHz, double gainDb) const { Result r; eqMove (c, { freqHz, gainDb }, r); return r; }
    Result nudge (const ChannelState& c, PluginCategory cat, ParamRole role, double delta, const std::string& unit,
                  const std::string& reason) const
    {
        Result r;
        paramNudge (c, cat, role, delta, unit, r, reason);
        return r;
    }

private:
    struct EqMove { double freqHz; double gainDb; };

    void eqMove (const ChannelState&, EqMove, Result&) const;
    void paramNudge (const ChannelState&, PluginCategory, ParamRole, double delta, const std::string& unit, Result&,
                     const std::string& reason) const;
    void paramSet (const ChannelState&, int slot, ParamRole, double value, const std::string& unit, Result&,
                   const std::string& reason) const;
    void gainNudge (const ChannelState&, double deltaDb, Result&, const std::string& reason) const;
};

} // namespace smix
