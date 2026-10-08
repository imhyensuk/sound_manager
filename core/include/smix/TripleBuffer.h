#pragma once

#include <array>
#include <atomic>

namespace smix
{

/**
    Lock-free single-producer / single-consumer "latest value" exchange.
    The audio thread publishes, a UI/worker thread reads the newest snapshot.
    Neither side ever blocks or allocates.
*/
template <typename T>
class TripleBuffer
{
public:
    /** Producer: the slot to fill before calling publish(). */
    T& writeSlot() noexcept { return slots[static_cast<size_t> (backIndex)]; }

    void publish() noexcept
    {
        const int prev = middle.exchange (backIndex | kDirty, std::memory_order_acq_rel);
        backIndex = prev & kIndexMask;
    }

    /** Consumer: returns true if a new value was received. read() is valid either way. */
    bool update() noexcept
    {
        if ((middle.load (std::memory_order_relaxed) & kDirty) == 0)
            return false;
        const int prev = middle.exchange (frontIndex, std::memory_order_acq_rel);
        frontIndex = prev & kIndexMask;
        return true;
    }

    const T& read() const noexcept { return slots[static_cast<size_t> (frontIndex)]; }

private:
    static constexpr int kDirty = 4;
    static constexpr int kIndexMask = 3;

    std::array<T, 3> slots {};
    int backIndex = 0;
    std::atomic<int> middle { 1 };
    int frontIndex = 2;
};

} // namespace smix
