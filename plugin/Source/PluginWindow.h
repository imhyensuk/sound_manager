#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/** Floating window showing a hosted plugin's own editor. Owned by the processor. */
class PluginWindow : public juce::DocumentWindow
{
public:
    explicit PluginWindow (juce::AudioProcessor& p)
        : juce::DocumentWindow (p.getName(), juce::Colours::darkgrey, juce::DocumentWindow::closeButton), processor (p)
    {
        setUsingNativeTitleBar (true);
        if (auto* editor = p.createEditorIfNeeded())
            setContentOwned (editor, true);
        else
            setContentOwned (new juce::GenericAudioProcessorEditor (p), true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~PluginWindow() override { clearContentComponent(); }

    void closeButtonPressed() override { setVisible (false); }

    juce::AudioProcessor& processor;
};
