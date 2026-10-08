#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace smix
{

/** Lock-free single-producer/single-consumer ring (audio thread -> UI). Drops when full. */
template <typename T, std::size_t Capacity>
class SpscRing
{
public:
    bool push (const T& v) noexcept
    {
        const auto w = write.load (std::memory_order_relaxed);
        const auto next = (w + 1) % Capacity;
        if (next == read.load (std::memory_order_acquire))
            return false;
        items[w] = v;
        write.store (next, std::memory_order_release);
        return true;
    }

    bool pop (T& out) noexcept
    {
        const auto r = read.load (std::memory_order_relaxed);
        if (r == write.load (std::memory_order_acquire))
            return false;
        out = items[r];
        read.store ((r + 1) % Capacity, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> items {};
    std::atomic<std::size_t> write { 0 }, read { 0 };
};

} // namespace smix
