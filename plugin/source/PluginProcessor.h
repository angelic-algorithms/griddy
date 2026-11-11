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
class GriddyProcessor final : public juce::AudioProcessor
{
public:
    GriddyProcessor();
    ~GriddyProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
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

private:
    // Declared first, so it is destroyed last: the hub outlives every instance that uses it.
    juce::SharedResourcePointer<Hub> hub;
    Member* member = nullptr;

    juce::AudioProcessorValueTreeState parameters;
    TransportState transport;
    int64_t blockCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GriddyProcessor)
};

} // namespace griddy
