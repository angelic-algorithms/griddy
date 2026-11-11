#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace griddy
{

juce::AudioProcessorValueTreeState::ParameterLayout GriddyProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { ParamIDs::numSlots, 1 }, "Cameras",
                                                     1, VideoEngine::maxSlots, 4));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::recordArm, 1 }, "Record arm", false));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::recordMode, 1 }, "Record when",
                                                        StringArray { "DAW records", "DAW plays", "Armed" }, 0));
    return layout;
}

GriddyProcessor::GriddyProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "GriddyState", createParameterLayout())
{
    EngineControls controls;
    controls.numSlots   = parameters.getRawParameterValue (ParamIDs::numSlots);
    controls.recordArm  = parameters.getRawParameterValue (ParamIDs::recordArm);
    controls.recordMode = parameters.getRawParameterValue (ParamIDs::recordMode);

    engine = std::make_unique<VideoEngine> (transport, controls);

    // A fresh instance starts with test patterns so there's something on screen
    // before any camera is picked (and in hosts that block cameras).
    for (int slot = 0; slot < 4; ++slot)
        engine->setSlotSource (slot, "test:" + juce::String (slot + 1));

    engine->startEngine();
}

GriddyProcessor::~GriddyProcessor()
{
    engine->stopEngine();
}

void GriddyProcessor::prepareToPlay (double, int) {}
void GriddyProcessor::releaseResources() {}

bool GriddyProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Pass through: input and output must match, mono or stereo.
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
        && out == layouts.getMainInputChannelSet();
}

void GriddyProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channels that have no matching input (standard JUCE hygiene).
    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // The one job of the audio thread: read the DAW's transport and publish it.
    // No allocation, no locks: TransportState::write is wait free.
    TransportSnapshot s;
    s.updateCounter = ++blockCounter;

    if (auto* head = getPlayHead())
    {
        if (const auto pos = head->getPosition())
        {
            s.hostProvidedPosition = true;
            s.bpm = pos->getBpm().orFallback (120.0);
            s.ppqPosition = pos->getPpqPosition().orFallback (0.0);
            s.ppqLastBarStart = pos->getPpqPositionOfLastBarStart().orFallback (0.0);
            s.isPlaying = pos->getIsPlaying();
            s.isRecording = pos->getIsRecording();

            if (const auto sig = pos->getTimeSignature())
            {
                s.timeSigNumerator = sig->numerator;
                s.timeSigDenominator = sig->denominator;
            }
        }
    }

    transport.write (s);
}

juce::AudioProcessorEditor* GriddyProcessor::createEditor()
{
    return new GriddyEditor (*this);
}

//==============================================================================
// Saved with the DAW project: parameters plus which camera is in which slot
// and where takes go. Camera ids are AVFoundation unique ids, so the same
// webcam comes back in the same slot next time you open the project.
void GriddyProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();

    juce::ValueTree slots ("Slots");
    const auto ids = engine->getSlotSources();

    for (int i = 0; i < VideoEngine::maxSlots; ++i)
        slots.appendChild (juce::ValueTree ("Slot", { { "index", i }, { "source", ids[(size_t) i] } }), nullptr);

    state.removeChild (state.getChildWithName ("Slots"), nullptr);
    state.appendChild (slots, nullptr);
    state.setProperty ("outputFolder", engine->getOutputFolder().getFullPathName(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void GriddyProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    const auto slots = state.getChildWithName ("Slots");

    for (const auto& slot : slots)
    {
        const auto index = (int) slot.getProperty ("index", -1);
        const auto source = slot.getProperty ("source").toString();

        // If a camera from last session is unplugged, the slot just shows "no camera".
        if (juce::isPositiveAndBelow (index, VideoEngine::maxSlots))
            engine->setSlotSource (index, source);
    }

    if (const auto folder = state.getProperty ("outputFolder").toString(); juce::File::isAbsolutePath (folder))
        engine->setOutputFolder (juce::File (folder));

    parameters.replaceState (state);
}

} // namespace griddy

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new griddy::GriddyProcessor();
}
