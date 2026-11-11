#pragma once

#include "PluginProcessor.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace griddy
{

/** The plugin window: one WebView that fills it, running the React UI from ui/.

    How the two sides talk:
      * Parameters (layout, switch rate, cameras, record arm, record mode) use
        JUCE's relays. The UI and DAW automation stay in sync both ways, and
        undo and automation work like any native knob.
      * Native functions are async calls from JS into C++ that return a value:
        getDevices, getSlots, setSlotSource, getStatus, revealOutputFolder,
        chooseOutputFolder.
      * Events are pushed from C++ to JS on a timer: "transport" (30 Hz),
        "preview" (when a new frame is ready) and "status" (twice a second).
      * The preview image itself is fetched by the UI from the resource
        provider at /preview.jpg. Sending a JPEG as a resource avoids base64
        encoding it into a JSON event.
*/
class GriddyEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit GriddyEditor (GriddyProcessor&);
    ~GriddyEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    juce::WebBrowserComponent::Options makeBrowserOptions();

    static juce::var devicesToVar (const std::vector<CaptureDeviceInfo>&);
    juce::var slotsToVar() const;
    juce::var transportToVar() const;

    GriddyProcessor& processor;
    VideoEngine& engine;

    juce::WebComboBoxRelay layoutRelay { ParamIDs::layout };
    juce::WebComboBoxRelay switchRateRelay { ParamIDs::switchRate };
    juce::WebSliderRelay numSlotsRelay { ParamIDs::numSlots };
    juce::WebToggleButtonRelay recordArmRelay { ParamIDs::recordArm };
    juce::WebComboBoxRelay recordModeRelay { ParamIDs::recordMode };

    // Declared after the relays because its options reference them.
    std::unique_ptr<juce::WebBrowserComponent> browser;

    juce::WebComboBoxParameterAttachment layoutAttachment;
    juce::WebComboBoxParameterAttachment switchRateAttachment;
    juce::WebSliderParameterAttachment numSlotsAttachment;
    juce::WebToggleButtonParameterAttachment recordArmAttachment;
    juce::WebComboBoxParameterAttachment recordModeAttachment;

    std::unique_ptr<juce::FileChooser> folderChooser;
    int64_t lastPreviewSerial = 0;
    int ticks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GriddyEditor)
};

} // namespace griddy
