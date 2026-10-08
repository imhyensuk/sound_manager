#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class SoundManagerProcessor;
class AnalysisView;

/** Tabs: 믹스 / 체인 / 채팅 / 분석 / 기록 / 레퍼런스 / 플러그인 / 모듈·설정 */
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
    AnalysisView* analysis = nullptr;
    int analysisTabIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundManagerEditor)
};
