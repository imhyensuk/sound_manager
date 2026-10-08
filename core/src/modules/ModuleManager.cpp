#include "smix/modules/ModuleManager.h"

#include <algorithm>

#include "smix/mem/MemoryRuntime.h"

namespace smix::modules
{

std::string toString (ModuleId id)
{
    switch (id)
    {
        case ModuleId::Ear:         return "ear";
        case ModuleId::EarModel:    return "ear_model";
        case ModuleId::Intent:      return "intent";
        case ModuleId::Knowledge:   return "knowledge";
        case ModuleId::Style:       return "style";
        case ModuleId::AutoMix:     return "auto_mix";
        case ModuleId::Visualizer:  return "visualizer";
        case ModuleId::History:     return "history";
        case ModuleId::Profiler:    return "profiler";
        case ModuleId::Hibernation: return "hibernation";
    }
    return "unknown";
}

std::string koreanName (ModuleId id)
{
    switch (id)
    {
        case ModuleId::Ear:         return "청음(분석·지각)";
        case ModuleId::EarModel:    return "악기 인식 모델";
        case ModuleId::Intent:      return "요구·문맥 추론 모델(로컬 LLM)";
        case ModuleId::Knowledge:   return "플러그인 지식(RAG)";
        case ModuleId::Style:       return "레퍼런스 스타일";
        case ModuleId::AutoMix:     return "자동 믹스";
        case ModuleId::Visualizer:  return "분석 그래프";
        case ModuleId::History:     return "믹싱 기록";
        case ModuleId::Profiler:    return "플러그인 분석기";
        case ModuleId::Hibernation: return "플러그인 절전";
    }
    return "?";
}

std::string toString (ModuleState s)
{
    switch (s)
    {
        case ModuleState::Off:      return "off";
        case ModuleState::Unloaded: return "unloaded";
        case ModuleState::Loading:  return "loading";
        case ModuleState::Active:   return "active";
        case ModuleState::Failed:   return "failed";
    }
    return "unknown";
}

//==============================================================================
ModuleManager::Lease::Lease (ModuleManager* m, ModuleId i) : owner (m), moduleId (i) {}

ModuleManager::Lease::~Lease()
{
    if (owner != nullptr)
        owner->release (moduleId);
}

ModuleManager::Lease::Lease (Lease&& o) noexcept { *this = std::move (o); }

ModuleManager::Lease& ModuleManager::Lease::operator= (Lease&& o) noexcept
{
    if (this != &o)
    {
        if (owner != nullptr)
            owner->release (moduleId);
        owner = o.owner;
        moduleId = o.moduleId;
        o.owner = nullptr;
    }
    return *this;
}

//==============================================================================
ModuleManager::~ModuleManager()
{
    unloadAll();
}

void ModuleManager::add (std::unique_ptr<Module> m, ModulePolicy policy)
{
    std::lock_guard<std::recursive_mutex> g (lock);
    const auto id = m->id();
    Slot s;
    s.module = std::move (m);
    s.policy = policy;
    s.state = policy.enabled ? ModuleState::Unloaded : ModuleState::Off;
    slots[id] = std::move (s);
}

ModuleManager::Lease ModuleManager::acquire (ModuleId id, double now, std::string* error)
{
    std::lock_guard<std::recursive_mutex> g (lock);
    auto it = slots.find (id);
    if (it == slots.end())
    {
        if (error) *error = "module not installed: " + toString (id);
        return {};
    }
    auto& s = it->second;
    if (! s.policy.enabled)
    {
        if (error) *error = "module switched off: " + toString (id);
        return {};
    }

    if (s.state != ModuleState::Active)
    {
        // Make room first: unload idle modules if loading this one would exceed the budget.
        if (budget > 0)
        {
            const auto need = s.module->memoryEstimate();
            std::vector<std::pair<double, ModuleId>> idle;
            for (auto& [otherId, o] : slots)
                if (otherId != id && o.state == ModuleState::Active && o.leases == 0 && ! o.policy.pinned)
                    idle.emplace_back (o.lastUsed, otherId);
            std::sort (idle.begin(), idle.end());
            for (auto& [t, otherId] : idle)
            {
                if (loadedMemoryEstimate() + need <= budget)
                    break;
                unloadLocked (slots[otherId]);
            }
        }

        s.state = ModuleState::Loading;
        std::string err;
        if (! s.module->load (err))
        {
            s.state = ModuleState::Failed;
            s.lastError = err;
            if (error) *error = err;
            return {};
        }
        s.state = ModuleState::Active;
        s.lastError.clear();
        ++s.loads;
    }

    ++s.leases;
    s.lastUsed = now;
    return Lease (this, id);
}

void ModuleManager::touch (ModuleId id, double now)
{
    std::lock_guard<std::recursive_mutex> g (lock);
    if (auto it = slots.find (id); it != slots.end())
        it->second.lastUsed = std::max (it->second.lastUsed, now);
}

void ModuleManager::release (ModuleId id)
{
    std::lock_guard<std::recursive_mutex> g (lock);
    if (auto it = slots.find (id); it != slots.end() && it->second.leases > 0)
        --it->second.leases;
}

void ModuleManager::unloadLocked (Slot& s)
{
    if (s.state == ModuleState::Active)
        s.module->unload();
    s.state = s.policy.enabled ? ModuleState::Unloaded : ModuleState::Off;
}

void ModuleManager::setEnabled (ModuleId id, bool enabled)
{
    std::lock_guard<std::recursive_mutex> g (lock);
    auto it = slots.find (id);
    if (it == slots.end())
        return;
    it->second.policy.enabled = enabled;
    if (! enabled && it->second.leases == 0)
        unloadLocked (it->second);
    else if (enabled && it->second.state == ModuleState::Off)
        it->second.state = ModuleState::Unloaded;
}

bool ModuleManager::isEnabled (ModuleId id) const
{
    std::lock_guard<std::recursive_mutex> g (lock);
    auto it = slots.find (id);
    return it != slots.end() && it->second.policy.enabled;
}

bool ModuleManager::isLoaded (ModuleId id) const
{
    return state (id) == ModuleState::Active;
}

ModuleState ModuleManager::state (ModuleId id) const
{
    std::lock_guard<std::recursive_mutex> g (lock);
    auto it = slots.find (id);
    return it == slots.end() ? ModuleState::Off : it->second.state;
}

void ModuleManager::setPolicy (ModuleId id, ModulePolicy p)
{
    std::lock_guard<std::recursive_mutex> g (lock);
    if (auto it = slots.find (id); it != slots.end())
    {
        it->second.policy = p;
        if (! p.enabled && it->second.leases == 0)
            unloadLocked (it->second);
    }
}

std::vector<ModuleId> ModuleManager::tick (double now)
{
    std::lock_guard<std::recursive_mutex> g (lock);
    std::vector<ModuleId> unloaded;
    for (auto& [id, s] : slots)
    {
        if (s.state != ModuleState::Active || s.leases > 0 || s.policy.pinned || s.policy.idleUnloadSeconds <= 0.0)
            continue;
        if (now - s.lastUsed >= s.policy.idleUnloadSeconds)
        {
            unloadLocked (s);
            unloaded.push_back (id);
        }
    }
    return unloaded;
}

void ModuleManager::unloadAll()
{
    std::lock_guard<std::recursive_mutex> g (lock);
    for (auto& [id, s] : slots)
        unloadLocked (s);
}

std::uint64_t ModuleManager::loadedMemoryEstimate() const
{
    std::lock_guard<std::recursive_mutex> g (lock);
    std::uint64_t total = 0;
    for (auto& [id, s] : slots)
        if (s.state == ModuleState::Active)
            total += s.module->memoryEstimate();
    return total;
}

nlohmann::json ModuleManager::status (double now) const
{
    std::lock_guard<std::recursive_mutex> g (lock);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& [id, s] : slots)
        arr.push_back ({ { "id", toString (id) }, { "name", koreanName (id) }, { "state", toString (s.state) },
                         { "enabled", s.policy.enabled }, { "in_use", s.leases },
                         { "idle_seconds", s.state == ModuleState::Active ? now - s.lastUsed : 0.0 },
                         { "memory", mem::formatBytes (s.module->memoryEstimate()) }, { "loads", s.loads },
                         { "error", s.lastError } });
    return { { "modules", arr }, { "loaded_memory", mem::formatBytes (loadedMemoryEstimate()) } };
}

} // namespace smix::modules
