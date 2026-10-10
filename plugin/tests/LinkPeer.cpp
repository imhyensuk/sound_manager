// A second process for the integration test: publishes one channel through SessionLink and
// prints the first call it receives, then exits.
//   SmxLinkPeer --key smx-test --port 47999 --seconds 40

#include <juce_events/juce_events.h>

#include <iostream>

#include "../Source/SessionLink.h"

namespace
{
class Peer : private juce::Timer
{
public:
    Peer (const juce::String& key, int port, int seconds)
        : link ([] (const nlohmann::json& call) {
              std::cout << "CALL " << call.dump() << std::endl;
              done = true;
          }),
          deadline (juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0)
    {
        link.start (key, port);
        startTimer (200);
    }

private:
    void timerCallback() override
    {
        smix::ChannelState c;
        c.id = "remote-snare";
        c.name = "Remote Snare";
        c.kind = smix::ChannelKind::Track;
        c.role = smix::InstrumentRole::Snare;
        c.features.valid = true;
        c.features.secondsAnalysed = 10.0f;
        c.features.shortTermLufs = -20.0f;
        c.features.integratedLufs = -20.0f;
        c.features.rmsDb = -22.0f;
        c.features.peakDb = -6.0f;
        link.publish ({ c }, { { c.id, false } });
        if (juce::Time::getMillisecondCounterHiRes() > deadline)
            done = true;
    }

public:
    static inline bool done = false;

private:

    SessionLink link;
    double deadline;
};
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::String key = "smx-test";
    int port = 47999, seconds = 40;
    for (int i = 1; i + 1 < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--key") key = argv[i + 1];
        if (a == "--port") port = juce::String (argv[i + 1]).getIntValue();
        if (a == "--seconds") seconds = juce::String (argv[i + 1]).getIntValue();
    }
    {
        Peer peer (key, port, seconds);
        while (! Peer::done)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    }
    return 0;
}
