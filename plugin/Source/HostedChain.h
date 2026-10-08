#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <smix/ValueMapper.h>

#include <atomic>
#include <map>

/**
    The user's own plugins, hosted inside Sound Manager and run in series.

    Audio thread: process() only, never blocks (try-lock; passes audio through during a swap).
    Message thread: everything else.
*/
class HostedChain
{
public:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        std::string uid;
        std::atomic<bool> bypassed { false };
        bool prepared = false;  // message thread only
        std::map<int, smix::ValueMapper> mappers;  // learned display-text curves, per parameter index
    };
    using SlotList = std::vector<std::unique_ptr<Slot>>;

    void prepare (double sampleRate, int maximumBlockSize);
    void release();
    void process (juce::AudioBuffer<float>&) noexcept;

    /** One position of a new chain: either a slot that is already running (kept with its state) or a fresh one. */
    struct Entry
    {
        Slot* reuse = nullptr;
        std::unique_ptr<Slot> fresh;
    };

    /**
        Swaps in a new chain. Fresh instances are prepared first; re-used slots move over without
        interruption. Returns the slots that are no longer used so they can be destroyed outside the lock.
    */
    SlotList rebuild (std::vector<Entry> entries);
    void moveSlot (int from, int to);

    int size() const noexcept { return static_cast<int> (slots.size()); }
    Slot* slot (int index) const noexcept;
    int getLatencySamples() const;

    /** Samples every parameter's display text so real units can be converted to normalised values. */
    static void learnValueMaps (Slot&);

private:
    void prepareInstance (juce::AudioPluginInstance&) const;

    juce::SpinLock lock;
    SlotList slots;
    double sampleRate = 44100.0;
    int blockSize = 512;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
};
