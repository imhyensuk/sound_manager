#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace smix::modules
{

/** Every function of Sound Manager is a module that only runs when it is needed. */
enum class ModuleId
{
    Ear,          // analysis + perception ("hearing")
    EarModel,     // neural instrument recogniser (channel naming)
    Intent,       // local language model: requests and context
    Knowledge,    // plugin knowledge base (RAG)
    Style,        // reference-track style analysis and matching
    AutoMix,      // continuous auto-mix loop
    Visualizer,   // RTA / waterfall / meters (DSP side only runs while a view is open)
    History,      // mix snapshots
    Profiler,     // plugin behaviour profiling (out of process)
    Hibernation   // unloads idle hosted plugins
};

std::string toString (ModuleId);
std::string koreanName (ModuleId);

enum class ModuleState { Off, Unloaded, Loading, Active, Failed };
std::string toString (ModuleState);

/** A loadable unit. load()/unload() are called by the manager, never concurrently. */
class Module
{
public:
    virtual ~Module() = default;
    virtual ModuleId id() const = 0;
    virtual bool load (std::string& error) = 0;
    virtual void unload() = 0;
    /** Memory the module holds while loaded (estimate, bytes). */
    virtual std::uint64_t memoryEstimate() const { return 0; }
    /** Expected load time in seconds (for the ETA display). */
    virtual double loadSecondsEstimate() const { return 0.0; }
};

/** A module defined by lambdas (handy for small modules and tests). */
class LambdaModule : public Module
{
public:
    LambdaModule (ModuleId i, std::function<bool (std::string&)> l, std::function<void()> u, std::uint64_t bytes = 0)
        : moduleId (i), loader (std::move (l)), unloader (std::move (u)), estimate (bytes) {}
    ModuleId id() const override { return moduleId; }
    bool load (std::string& e) override { return loader ? loader (e) : true; }
    void unload() override { if (unloader) unloader(); }
    std::uint64_t memoryEstimate() const override { return estimate; }

private:
    ModuleId moduleId;
    std::function<bool (std::string&)> loader;
    std::function<void()> unloader;
    std::uint64_t estimate;
};

struct ModulePolicy
{
    double idleUnloadSeconds = 120.0;  // unload when unused this long (0 = never)
    bool enabled = true;               // user switch; disabled modules never load
    bool pinned = false;               // never unload automatically (e.g. the ear)
};

/**
    Turns modules on when they are needed and off when they are not (requirement: modular,
    on-demand operation). Use acquire() around any use; the returned Lease keeps the module
    loaded. tick() unloads idle modules and, under memory pressure, the least recently used ones.
    Thread-safe.
*/
class ModuleManager
{
public:
    class Lease
    {
    public:
        Lease() = default;
        Lease (ModuleManager*, ModuleId);
        ~Lease();
        Lease (Lease&&) noexcept;
        Lease& operator= (Lease&&) noexcept;
        Lease (const Lease&) = delete;
        Lease& operator= (const Lease&) = delete;
        explicit operator bool() const noexcept { return owner != nullptr; }

    private:
        ModuleManager* owner = nullptr;
        ModuleId moduleId {};
    };

    explicit ModuleManager (std::uint64_t memoryBudgetBytes = 0) : budget (memoryBudgetBytes) {}
    ~ModuleManager();

    void add (std::unique_ptr<Module>, ModulePolicy = {});

    /** Loads the module if needed. Empty Lease (false) if disabled or loading failed. */
    Lease acquire (ModuleId, double nowSeconds, std::string* error = nullptr);

    /** Marks the module as used without holding it (e.g. a view is still visible). */
    void touch (ModuleId, double nowSeconds);

    void setEnabled (ModuleId, bool);
    bool isEnabled (ModuleId) const;
    bool isLoaded (ModuleId) const;
    ModuleState state (ModuleId) const;
    void setPolicy (ModuleId, ModulePolicy);
    void setMemoryBudget (std::uint64_t bytes) { budget = bytes; }

    /** Unloads idle modules; returns the ones it unloaded. */
    std::vector<ModuleId> tick (double nowSeconds);

    void unloadAll();

    std::uint64_t loadedMemoryEstimate() const;
    nlohmann::json status (double nowSeconds) const;

private:
    struct Slot
    {
        std::unique_ptr<Module> module;
        ModulePolicy policy;
        ModuleState state = ModuleState::Unloaded;
        int leases = 0;
        double lastUsed = 0.0;
        std::string lastError;
        int loads = 0;
    };

    void release (ModuleId);
    void unloadLocked (Slot&);

    mutable std::recursive_mutex lock;
    std::map<ModuleId, Slot> slots;
    std::uint64_t budget;
};

} // namespace smix::modules
