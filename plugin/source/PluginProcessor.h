#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "engine/Hub.h"
#include "engine/VideoEngine.h"

namespace griddy
{

namespace ParamIDs
{
    inline constexpr auto slot       = "slot";
    inline constexpr auto master     = "master";
    inline constexpr auto numSlots   = "numSlots";
    inline constexpr auto recordArm  = "recordArm";
    inline constexpr auto recordMode = "recordMode";
}

/** The plugin as the DAW sees it.

    Audio passes straight through untouched: Griddy sits on any track purely
    to hear the DAW's transport (tempo, position, play and record) and to
    save its settings with the project. All the video work happens in
    VideoEngine on its own thread.
*/
class GriddyProcessor final : public juce::AudioProcessor, private juce::Timer
{
public:
    GriddyProcessor();
    ~GriddyProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void updateTrackProperties (const TrackProperties& properties) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Griddy"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }
    VideoEngine& getEngine() noexcept { return hub->video(); }
    Hub& getHub() noexcept { return *hub; }
    Member* getMember() noexcept { return member; }
    TransportSnapshot getTransport() const noexcept { return transport.read(); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Message thread: the UI picked a camera for a slot. An empty source on this track's own slot means the user
        turned its camera off, which is saved, so the slot stays off rather than getting the default camera. */
    void cameraPicked (int slot, const juce::String& source);

    /** Saved with the state. 2: Record when defaults to DAW plays and older states are moved to it.
        3: the state lists the takes its session held, so a template or a copy of a project gets a session of its own.
        4: Record when is gone; every state, whatever its recordMode, records on arm plus play. */
    static constexpr int currentStateVersion = 4;

private:
    // Declared first, so it is destroyed last: the hub outlives every instance that uses it.
    juce::SharedResourcePointer<Hub> hub;
    Member* member = nullptr;

    juce::AudioProcessorValueTreeState parameters;
    TransportState transport;
    int64_t blockCounter = 0;

    // A new Griddy gives its slot the default camera once the hub has given it a slot, unless a saved set says
    // which camera it had (or that it was off).
    void timerCallback() override;
    bool cameraDecided = false, cameraOff = false;
    int cameraTries = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GriddyProcessor)
};

} // namespace griddy
