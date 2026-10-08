#include "HostedChain.h"

void HostedChain::prepareInstance (juce::AudioPluginInstance& p) const
{
    p.disableNonMainBuses();
    p.setChannelLayoutOfBus (true, 0, juce::AudioChannelSet::stereo());
    p.setChannelLayoutOfBus (false, 0, juce::AudioChannelSet::stereo());
    p.setRateAndBufferSizeDetails (sampleRate, blockSize);
    p.prepareToPlay (sampleRate, blockSize);
}

void HostedChain::prepare (double newSampleRate, int maximumBlockSize)
{
    sampleRate = newSampleRate;
    blockSize = maximumBlockSize;
    scratch.setSize (8, maximumBlockSize, false, true, true);

    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& s : slots)
    {
        prepareInstance (*s->instance);
        s->prepared = true;
    }
}

void HostedChain::release()
{
    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& s : slots)
        s->instance->releaseResources();
}

void HostedChain::process (juce::AudioBuffer<float>& buffer) noexcept
{
    const juce::SpinLock::ScopedTryLockType tl (lock);
    if (! tl.isLocked())
        return;  // chain is being swapped right now: pass audio through for this block

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    for (auto& s : slots)
    {
        auto& p = *s->instance;
        const int needed = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
        if (needed == 0)
            continue;

        midi.clear();
        const juce::ScopedLock callbackLock (p.getCallbackLock());

        if (needed <= numChannels)
        {
            juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), needed, numSamples);
            if (s->bypassed.load (std::memory_order_relaxed))
                p.processBlockBypassed (view, midi);
            else
                p.processBlock (view, midi);
        }
        else if (needed <= scratch.getNumChannels() && numSamples <= scratch.getNumSamples())
        {
            juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), needed, numSamples);
            for (int ch = 0; ch < needed; ++ch)
                view.copyFrom (ch, 0, buffer, juce::jmin (ch, numChannels - 1), 0, numSamples);
            if (s->bypassed.load (std::memory_order_relaxed))
                p.processBlockBypassed (view, midi);
            else
                p.processBlock (view, midi);
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.copyFrom (ch, 0, view, ch, 0, numSamples);
        }
    }
}

HostedChain::SlotList HostedChain::rebuild (std::vector<Entry> entries)
{
    // Only fresh instances: re-used ones are still running in the current chain.
    for (auto& e : entries)
    {
        if (e.fresh != nullptr && ! e.fresh->prepared)
        {
            prepareInstance (*e.fresh->instance);
            e.fresh->prepared = true;
        }
    }

    SlotList newSlots;
    newSlots.reserve (entries.size());

    const juce::SpinLock::ScopedLockType sl (lock);
    for (auto& e : entries)
    {
        if (e.fresh != nullptr)
        {
            newSlots.push_back (std::move (e.fresh));
        }
        else if (e.reuse != nullptr)
        {
            for (auto& existing : slots)
            {
                if (existing.get() == e.reuse)
                {
                    newSlots.push_back (std::move (existing));
                    break;
                }
            }
        }
    }
    std::swap (slots, newSlots);
    return newSlots;  // the old list (moved-from entries are null)
}

void HostedChain::moveSlot (int from, int to)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    if (from < 0 || to < 0 || from >= size() || to >= size() || from == to)
        return;
    auto moving = std::move (slots[static_cast<size_t> (from)]);
    slots.erase (slots.begin() + from);
    slots.insert (slots.begin() + to, std::move (moving));
}

HostedChain::Slot* HostedChain::slot (int index) const noexcept
{
    return juce::isPositiveAndBelow (index, size()) ? slots[static_cast<size_t> (index)].get() : nullptr;
}

int HostedChain::getLatencySamples() const
{
    int total = 0;
    for (auto& s : slots)
        total += s->instance->getLatencySamples();
    return total;
}

void HostedChain::learnValueMaps (Slot& s)
{
    s.mappers.clear();
    const auto points = smix::ValueMapper::probePoints (33);
    const auto& params = s.instance->getParameters();
    const int limit = juce::jmin (params.size(), 256);  // some plugins expose thousands of (MIDI CC) parameters

    for (int i = 0; i < limit; ++i)
    {
        auto* p = params[i];
        if (p->isBoolean())
            continue;
        smix::ValueMapper m;
        const auto label = p->getLabel().toStdString();
        for (auto v : points)
            m.addSample (v, p->getText (v, 64).toStdString(), label);
        if (m.isUsable())
            s.mappers[p->getParameterIndex()] = std::move (m);
    }
}
