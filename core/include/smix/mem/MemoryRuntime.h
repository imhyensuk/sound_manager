#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace smix::mem
{

using BufferId = std::uint64_t;
constexpr BufferId kNoBuffer = 0;

struct Stats
{
    bool memopro = false;           // true: backed by the memopro runtime
    std::uint64_t budget = 0;       // bytes the buffers may occupy
    std::uint64_t used = 0;
    std::uint64_t peakUsed = 0;
    std::uint64_t buffers = 0;
    std::uint64_t residentBytes = 0;
    std::uint64_t compressedBytes = 0;
    std::uint64_t pinnedBytes = 0;
    std::uint64_t rereads = 0;      // file-backed buffers dropped and read again
    std::uint64_t recomputes = 0;
    std::uint64_t compressions = 0;
    std::uint64_t refusals = 0;     // requests the budget could not hold
    std::uint64_t writtenBytes = 0; // always 0: nothing is ever swapped to disk

    nlohmann::json toJson() const;
};

class Runtime;

/**
    A buffer in use. While a Pin is alive the data stays in memory at the same address;
    afterwards the runtime may compress it, drop it (file-backed) or keep it.
*/
class Pin
{
public:
    Pin() = default;
    ~Pin();
    Pin (Pin&&) noexcept;
    Pin& operator= (Pin&&) noexcept;
    Pin (const Pin&) = delete;
    Pin& operator= (const Pin&) = delete;

    explicit operator bool() const noexcept { return data_ != nullptr; }
    void* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    template <typename T> T* as() const noexcept { return static_cast<T*> (data_); }

    void release();

private:
    friend class Runtime;
    Runtime* owner = nullptr;
    void* handle = nullptr;  // mp_pin* (memopro) or the fallback entry
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

/**
    Budgeted, lossless store for Sound Manager's large data. One Runtime is shared by every
    plugin instance in the process, so a project with hundreds of instances still has one ceiling.

    - store()/allocate(): data made in memory (compressed by memopro when memory is short)
    - addFile(): a region of a file (dropped and re-read, digest-verified, when memory is short)
    A request the budget cannot hold fails (and is counted), it is never swapped. Thread-safe.
*/
class Runtime
{
public:
    explicit Runtime (std::uint64_t budgetBytes);
    ~Runtime();

    Runtime (const Runtime&) = delete;
    Runtime& operator= (const Runtime&) = delete;

    static bool memoproCompiledIn() noexcept;
    static std::string memoproVersion();

    BufferId allocate (std::size_t bytes);
    BufferId store (const void* data, std::size_t bytes);
    BufferId store (const std::string& text) { return store (text.data(), text.size()); }
    BufferId addFile (const std::string& path, std::uint64_t offset, std::size_t bytes);

    /** write = true: exclusive, writable pin (memory buffers only). Invalid Pin on failure. */
    Pin pin (BufferId, bool write = false);

    /** Copies a whole buffer out (convenience for small blobs such as plugin states). */
    std::vector<std::uint8_t> read (BufferId);
    std::string readText (BufferId);

    void free (BufferId);
    bool evict (BufferId);
    void prefetch (BufferId);
    std::size_t sizeOf (BufferId) const;

    Stats stats() const;
    std::uint64_t budget() const noexcept { return budgetBytes; }
    const std::string& lastError() const noexcept { return error; }

private:
    friend class Pin;
    void unpin (Pin&);

    std::uint64_t budgetBytes;
    std::string error;

    // memopro backend
    void* rt = nullptr;  // mp_runtime*

    // fallback backend
    struct Entry
    {
        std::vector<std::uint8_t> bytes;
        std::string path;
        std::uint64_t offset = 0;
        std::size_t size = 0;
        bool loaded = false;
        int pins = 0;
    };
    mutable std::mutex lock;
    std::unordered_map<BufferId, Entry> entries;
    BufferId nextId = 1;
    std::uint64_t used = 0, peak = 0, refusals = 0, rereads = 0;
    bool reserve (std::size_t bytes);
};

/** Formats bytes as "12.3 MiB". */
std::string formatBytes (std::uint64_t bytes);

} // namespace smix::mem
