#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "smix/ChainPlanner.h"
#include "smix/GainBalancer.h"
#include "smix/RecipeEngine.h"

namespace smix
{

/**
    The continuous "auto mix" loop (requirement 5): every few seconds it listens to every
    channel in scope and makes small, bounded corrections:
      - plans a chain for channels that have none (if enabled),
      - gives freshly inserted plugins role-aware starting settings,
      - fixes clearly audible tonal problems with gentle EQ moves,
      - balances levels between channels.
    All moves are rate-limited and capped so the mix converges instead of drifting.
*/
class AutoMixer
{
public:
    struct Options
    {
        bool planChains = true;
        bool initialiseNewPlugins = true;
        bool correctTone = true;
        bool balanceLevels = true;
        float correctionThreshold = 0.6f;  // descriptor score that triggers a correction
        float correctionAmount = 0.35f;    // fraction of a full recipe per correction
        int maxCorrectionsPerGoal = 4;     // per channel, until reset()
        double secondsBetweenCorrections = 6.0;
        double minAnalysisSeconds = 4.0;   // need this much audio before judging a channel
    };

    struct Tick
    {
        std::vector<MixAction> actions;
        std::vector<std::string> log;
    };

    AutoMixer() = default;
    explicit AutoMixer (Options o) : options (o) {}

    Options& getOptions() noexcept { return options; }
    GainBalancer& balancer() noexcept { return gainBalancer; }

    Tick tick (const MixSession&, const std::string& rootId, const PluginCatalog&, double nowSeconds);

    /** Forget correction history (e.g. after the user changed something by hand). */
    void reset();

    /**
        The user asked for a change on this channel (chat or by hand): leave its tone alone for a
        while so the auto mixer never "corrects" a deliberate choice.
    */
    void holdChannel (const std::string& channelId, double nowSeconds, double holdSeconds = 600.0);
    bool isHeld (const std::string& channelId, double nowSeconds) const;

    /**
        Plugins just inserted on a channel should receive role-aware starting settings once loaded.
        The caller marks this after a successful set_chain (including the ones tick() itself plans).
    */
    void markForInitialisation (const std::string& channelId, const std::vector<std::string>& pluginUids);

private:
    Options options;
    ChainPlanner planner;
    RecipeEngine recipes;
    GainBalancer gainBalancer;

    std::set<std::string> plannedChannels;
    std::set<std::string> pendingInit;       // channelId|uid of freshly inserted plugins
    std::map<std::string, double> heldUntil; // channelId -> time
    std::map<std::string, int> correctionCounts;  // channelId|goal
    std::map<std::string, double> lastCorrection; // channelId
};

} // namespace smix
