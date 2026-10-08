#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <smix/AudioAnalyzer.h>

#include <deque>

class SoundManagerProcessor;

/**
    Live analysis graphs chosen by the user or requested in chat (requirement 6):
      rta         - spectrum with peak hold
      waterfall   - spectrogram history (frequency x time, colour = level)
      meters      - peak/RMS per channel, momentary/short-term LUFS
      loudness    - LUFS history
      correlation - phase correlation history
    The analyser only computes these frames while this view is visible (module on/off).
*/
class AnalysisView : public juce::Component,
                     private juce::Timer
{
public:
    explicit AnalysisView (SoundManagerProcessor&);
    ~AnalysisView() override;

    void setMode (const juce::String& mode);
    const juce::String& getMode() const noexcept { return mode; }
    static juce::StringArray modes() { return { "rta", "waterfall", "meters", "loudness", "correlation" }; }
    static juce::String modeLabel (const juce::String& m);

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void paintRta (juce::Graphics&, juce::Rectangle<float>);
    void paintWaterfall (juce::Graphics&, juce::Rectangle<float>);
    void paintMeters (juce::Graphics&, juce::Rectangle<float>);
    void paintHistory (juce::Graphics&, juce::Rectangle<float>, const std::deque<float>&, float lo, float hi, const juce::String& unit);
    void drawFrequencyGrid (juce::Graphics&, juce::Rectangle<float>);
    void pushWaterfallRow (const smix::AudioAnalyzer::RtaFrame&);

    SoundManagerProcessor& processor;
    juce::String mode { "rta" };
    smix::AudioAnalyzer::RtaFrame current, peakHold, smoothed;
    smix::AudioAnalyzer::MeterFrame meter;
    float peakHoldL = -120, peakHoldR = -120;
    std::deque<float> lufsHistory, correlationHistory;
    juce::Image waterfall;
    bool haveFrame = false;
};
