#pragma once

#include <string>
#include <vector>

#include "smix/MixAction.h"
#include "smix/MixSession.h"

namespace smix
{

/**
    Level balancing between the channels in an instance's scope (requirement 6).

    Every role has a target loudness relative to the "anchor" of the mix (lead vocal, or
    kick when there is no vocal). The balancer measures each channel's short-term loudness
    (post chain, pre AI gain), computes the gain that would hit the target and moves
    towards it in small, smoothed steps.
*/
class GainBalancer
{
public:
    struct Options
    {
        float maxStepDb = 1.0f;       // per update, keeps changes inaudible
        float deadbandDb = 0.75f;     // ignore smaller errors (hysteresis)
        float silenceLufs = -55.0f;   // channels quieter than this are not playing
        float minGainDb = -18.0f;
        float maxGainDb = 9.0f;
    };

    GainBalancer() = default;
    explicit GainBalancer (Options o) : options (o) {}

    /** Target loudness of a role relative to the anchor, in LU. */
    static float targetOffsetLu (InstrumentRole);

    /** User preference ("make the vocal louder") shifts a role's target. */
    void setUserOffset (InstrumentRole, float lu);
    float userOffset (InstrumentRole) const;

    /** Returns nudge_gain actions for the children of rootId (the root itself is never balanced). */
    std::vector<MixAction> balance (const MixSession&, const std::string& rootId) const;

private:
    Options options;
    std::vector<std::pair<InstrumentRole, float>> userOffsets;
};

} // namespace smix
