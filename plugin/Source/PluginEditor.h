#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class SoundManagerProcessor;

/** Tabs: Mix (scope & balance) / Chain / Plugins (allow-list) / Chat / Settings. */
class SoundManagerEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit SoundManagerEditor (SoundManagerProcessor&);
    ~SoundManagerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    SoundManagerProcessor& processor;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundManagerEditor)
};
