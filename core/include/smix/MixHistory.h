#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "smix/MixAction.h"
#include "smix/MixSession.h"
#include "smix/mem/MemoryRuntime.h"

namespace smix
{

struct SlotSnapshot
{
    std::string pluginUid, pluginName;
    bool bypassed = false;
    std::vector<std::pair<int, float>> params;  // index -> normalised value
    std::uint64_t stateHash = 0;                // full plugin state blob (deduplicated, in the memory runtime)
};

struct ChannelSnapshot
{
    std::string channelId, channelName;
    float gainDb = 0.0f;
    std::vector<SlotSnapshot> chain;
};

struct Snapshot
{
    std::int64_t id = 0;
    double time = 0.0;          // seconds since epoch
    std::string label;          // "킥 단단하게", "자동 믹스", ...
    std::string source;         // "chat", "auto", "user", "restore", "plan"
    std::vector<ChannelSnapshot> channels;
};

/** What must happen to get back to a snapshot. */
struct RestorePlan
{
    std::vector<MixAction> actions;  // parameter, gain and bypass changes on unchanged chains

    struct ChainReload
    {
        std::string channelId;
        std::vector<std::string> pluginUids;
        std::vector<std::vector<std::uint8_t>> states;  // full plugin states, per position
        std::vector<bool> bypassed;
    };
    std::vector<ChainReload> reloads;  // channels whose chain differs: reload with saved states
    std::vector<std::string> notes;    // channels that no longer exist, protected channels...
};

/**
    Records the mix after every AI step so the user can go back to any point (requirement 11).

    Parameter values are kept as numbers; full plugin state blobs are deduplicated by hash and
    kept in the memopro runtime, where they are compressed when memory is short.
*/
class MixHistory
{
public:
    /** Returns the full state of the plugin in a slot (empty if unavailable). */
    using BlobProvider = std::function<std::vector<std::uint8_t> (const std::string& channelId, int slot)>;

    explicit MixHistory (mem::Runtime* runtime = nullptr, std::size_t maxSnapshots = 200);
    ~MixHistory();

    std::int64_t record (const MixSession&, const std::vector<std::string>& channelIds, const std::string& label,
                         const std::string& source, double timeSeconds, const BlobProvider& blobs = {});

    const std::vector<Snapshot>& snapshots() const noexcept { return list; }
    const Snapshot* find (std::int64_t id) const;

    RestorePlan planRestore (std::int64_t id, const MixSession& current) const;

    /** Per-channel slice for saving in that channel's plugin state (project file). */
    nlohmann::json exportChannel (const std::string& channelId, std::size_t maxSnapshots = 50) const;
    void importChannel (const nlohmann::json&);

    std::size_t blobCount() const noexcept { return blobs.size(); }
    std::uint64_t blobBytes() const;
    void clear();

    static std::uint64_t hashBytes (const std::vector<std::uint8_t>&);

private:
    std::uint64_t keepBlob (const std::vector<std::uint8_t>&);
    std::vector<std::uint8_t> blobData (std::uint64_t hash) const;
    void dropSnapshot (const Snapshot&);

    struct Blob
    {
        mem::BufferId buffer = mem::kNoBuffer;
        std::vector<std::uint8_t> inline_;  // used when there is no runtime (or it refused)
        std::size_t size = 0;
        int refs = 0;
    };

    mem::Runtime* runtime;
    std::size_t maxSnapshots;
    std::vector<Snapshot> list;
    std::map<std::uint64_t, Blob> blobs;
    std::int64_t lastId = 0;
};

} // namespace smix
