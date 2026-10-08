#include "smix/mem/MemoryRuntime.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#if SMIX_HAVE_MEMOPRO
 #include <memopro.h>
#endif

namespace smix::mem
{

std::string formatBytes (std::uint64_t bytes)
{
    const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = static_cast<double> (bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4)
    {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf (buf, sizeof (buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

nlohmann::json Stats::toJson() const
{
    return { { "backend", memopro ? "memopro" : "fallback" }, { "budget", budget }, { "used", used },
             { "peak_used", peakUsed }, { "buffers", buffers }, { "resident_bytes", residentBytes },
             { "compressed_bytes", compressedBytes }, { "pinned_bytes", pinnedBytes }, { "rereads", rereads },
             { "recomputes", recomputes }, { "compressions", compressions }, { "refusals", refusals },
             { "written_bytes", writtenBytes } };
}

//==============================================================================
Pin::~Pin() { release(); }

Pin::Pin (Pin&& o) noexcept { *this = std::move (o); }

Pin& Pin::operator= (Pin&& o) noexcept
{
    if (this != &o)
    {
        release();
        owner = o.owner; handle = o.handle; data_ = o.data_; size_ = o.size_;
        o.owner = nullptr; o.handle = nullptr; o.data_ = nullptr; o.size_ = 0;
    }
    return *this;
}

void Pin::release()
{
    if (owner != nullptr && handle != nullptr)
        owner->unpin (*this);
    owner = nullptr;
    handle = nullptr;
    data_ = nullptr;
    size_ = 0;
}

//==============================================================================
bool Runtime::memoproCompiledIn() noexcept
{
#if SMIX_HAVE_MEMOPRO
    return true;
#else
    return false;
#endif
}

std::string Runtime::memoproVersion()
{
#if SMIX_HAVE_MEMOPRO
    return mp_version();
#else
    return {};
#endif
}

Runtime::Runtime (std::uint64_t budget) : budgetBytes (budget)
{
#if SMIX_HAVE_MEMOPRO
    mp_config cfg;
    mp_config_default (&cfg, budget);
    cfg.policy = MP_POLICY_LRU;  // access is driven by the user, not by repeating passes
    cfg.prefetch = 0;
    mp_runtime* r = nullptr;
    if (mp_runtime_new (&cfg, &r) == MP_OK)
        rt = r;
    else
        error = mp_last_error();
#endif
}

Runtime::~Runtime()
{
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
        mp_runtime_free (static_cast<mp_runtime*> (rt));
#endif
}

bool Runtime::reserve (std::size_t bytes)
{
    if (used + bytes > budgetBytes)
    {
        // Fallback: drop unpinned file-backed buffers (they can be re-read) before refusing.
        for (auto& [id, e] : entries)
        {
            if (used + bytes <= budgetBytes)
                break;
            if (! e.path.empty() && e.loaded && e.pins == 0)
            {
                used -= e.size;
                e.bytes.clear();
                e.bytes.shrink_to_fit();
                e.loaded = false;
            }
        }
    }
    if (used + bytes > budgetBytes)
    {
        ++refusals;
        error = "budget exceeded: " + formatBytes (bytes) + " requested, " + formatBytes (budgetBytes - used) + " free";
        return false;
    }
    used += bytes;
    peak = std::max (peak, used);
    return true;
}

BufferId Runtime::allocate (std::size_t bytes)
{
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
    {
        mp_buffer b = 0;
        if (mp_alloc (static_cast<mp_runtime*> (rt), bytes, 1, &b) != MP_OK)
        {
            error = mp_last_error();
            return kNoBuffer;
        }
        return b;
    }
#endif
    std::lock_guard<std::mutex> g (lock);
    if (! reserve (bytes))
        return kNoBuffer;
    Entry e;
    e.bytes.assign (bytes, 0);
    e.size = bytes;
    e.loaded = true;
    const auto id = nextId++;
    entries.emplace (id, std::move (e));
    return id;
}

BufferId Runtime::store (const void* data, std::size_t bytes)
{
    if (bytes == 0)
        return kNoBuffer;  // empty data needs no buffer; reading kNoBuffer yields nothing
    const auto id = allocate (bytes);
    if (id == kNoBuffer)
        return kNoBuffer;
    auto p = pin (id, true);
    if (! p)
    {
        free (id);
        return kNoBuffer;
    }
    if (bytes > 0)
        std::memcpy (p.data(), data, bytes);
    return id;
}

BufferId Runtime::addFile (const std::string& path, std::uint64_t offset, std::size_t bytes)
{
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
    {
        mp_buffer b = 0;
        if (mp_add_file (static_cast<mp_runtime*> (rt), path.c_str(), offset, bytes, 1, &b) != MP_OK)
        {
            error = mp_last_error();
            return kNoBuffer;
        }
        return b;
    }
#endif
    std::ifstream f (path, std::ios::binary | std::ios::ate);
    if (! f || static_cast<std::uint64_t> (f.tellg()) < offset + bytes)
    {
        error = "cannot read " + path;
        return kNoBuffer;
    }
    std::lock_guard<std::mutex> g (lock);
    Entry e;
    e.path = path;
    e.offset = offset;
    e.size = bytes;
    const auto id = nextId++;
    entries.emplace (id, std::move (e));
    return id;
}

Pin Runtime::pin (BufferId id, bool write)
{
    Pin p;
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
    {
        mp_pin* mp = nullptr;
        if (mp_pin_acquire (static_cast<mp_runtime*> (rt), id, write ? 1 : 0, &mp) != MP_OK)
        {
            error = mp_last_error();
            return p;
        }
        p.owner = this;
        p.handle = mp;
        p.data_ = mp_pin_data (mp);
        p.size_ = mp_pin_size (mp);
        return p;
    }
#endif
    std::lock_guard<std::mutex> g (lock);
    auto it = entries.find (id);
    if (it == entries.end())
    {
        error = "unknown buffer";
        return p;
    }
    auto& e = it->second;
    if (write && (e.pins > 0 || ! e.path.empty()))
    {
        error = "a writable pin must be the only pin of a memory buffer";
        return p;
    }
    if (! e.loaded)
    {
        if (! reserve (e.size))
            return p;
        std::ifstream f (e.path, std::ios::binary);
        e.bytes.resize (e.size);
        f.seekg (static_cast<std::streamoff> (e.offset));
        f.read (reinterpret_cast<char*> (e.bytes.data()), static_cast<std::streamsize> (e.size));
        if (! f)
        {
            used -= e.size;
            e.bytes.clear();
            error = "read failed: " + e.path;
            return p;
        }
        e.loaded = true;
        ++rereads;
    }
    ++e.pins;
    p.owner = this;
    p.handle = &e;
    p.data_ = e.bytes.data();
    p.size_ = e.size;
    return p;
}

void Runtime::unpin (Pin& p)
{
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
    {
        mp_unpin (static_cast<mp_pin*> (p.handle));
        return;
    }
#endif
    std::lock_guard<std::mutex> g (lock);
    static_cast<Entry*> (p.handle)->pins--;
}

std::vector<std::uint8_t> Runtime::read (BufferId id)
{
    auto p = pin (id);
    if (! p)
        return {};
    const auto* b = p.as<std::uint8_t>();
    return { b, b + p.size() };
}

std::string Runtime::readText (BufferId id)
{
    auto p = pin (id);
    if (! p)
        return {};
    return { p.as<char>(), p.size() };
}

void Runtime::free (BufferId id)
{
    if (id == kNoBuffer)
        return;
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
    {
        mp_free (static_cast<mp_runtime*> (rt), id);
        return;
    }
#endif
    std::lock_guard<std::mutex> g (lock);
    auto it = entries.find (id);
    if (it == entries.end())
        return;
    if (it->second.loaded)
        used -= it->second.size;
    entries.erase (it);
}

bool Runtime::evict (BufferId id)
{
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
    {
        int evicted = 0;
        return mp_evict (static_cast<mp_runtime*> (rt), id, &evicted) == MP_OK && evicted != 0;
    }
#endif
    std::lock_guard<std::mutex> g (lock);
    auto it = entries.find (id);
    if (it == entries.end() || it->second.path.empty() || ! it->second.loaded || it->second.pins > 0)
        return false;
    used -= it->second.size;
    it->second.bytes.clear();
    it->second.bytes.shrink_to_fit();
    it->second.loaded = false;
    return true;
}

void Runtime::prefetch (BufferId id)
{
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
        mp_prefetch (static_cast<mp_runtime*> (rt), id);
#else
    (void) id;
#endif
}

std::size_t Runtime::sizeOf (BufferId id) const
{
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
    {
        std::size_t n = 0;
        mp_nbytes (static_cast<const mp_runtime*> (rt), id, &n);
        return n;
    }
#endif
    std::lock_guard<std::mutex> g (lock);
    auto it = entries.find (id);
    return it == entries.end() ? 0 : it->second.size;
}

Stats Runtime::stats() const
{
    Stats s;
    s.budget = budgetBytes;
#if SMIX_HAVE_MEMOPRO
    if (rt != nullptr)
    {
        mp_stats m;
        std::memset (&m, 0, sizeof (m));
        m.size = sizeof (m);
        if (mp_stats_get (static_cast<const mp_runtime*> (rt), &m) == MP_OK)
        {
            s.memopro = true;
            s.budget = m.budget;
            s.used = m.used;
            s.peakUsed = m.peak_used;
            s.buffers = m.buffers;
            s.residentBytes = m.resident_bytes;
            s.compressedBytes = m.compressed_bytes;
            s.pinnedBytes = m.pinned_bytes;
            s.rereads = m.rereads;
            s.recomputes = m.recomputes;
            s.compressions = m.compressions;
            s.refusals = m.refusals;
            s.writtenBytes = m.written_bytes;
        }
        return s;
    }
#endif
    std::lock_guard<std::mutex> g (lock);
    s.used = used;
    s.peakUsed = peak;
    s.buffers = entries.size();
    s.residentBytes = used;
    s.refusals = refusals;
    s.rereads = rereads;
    for (auto& [id, e] : entries)
        if (e.pins > 0)
            s.pinnedBytes += e.size;
    return s;
}

} // namespace smix::mem
