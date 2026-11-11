#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace griddy
{

juce::AudioProcessorValueTreeState::ParameterLayout GriddyProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterInt> (ParameterID { ParamIDs::slot, 1 }, "Slot", 1, Hub::maxSlots, 1));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::master, 1 }, "Master", false));

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
    MemberControls controls;
    controls.slot       = parameters.getRawParameterValue (ParamIDs::slot);
    controls.cameras    = parameters.getRawParameterValue (ParamIDs::numSlots);
    controls.master     = parameters.getRawParameterValue (ParamIDs::master);
    controls.recordArm  = parameters.getRawParameterValue (ParamIDs::recordArm);
    controls.recordMode = parameters.getRawParameterValue (ParamIDs::recordMode);

    // The hub may need to move this instance to a free slot; the parameter is shown on the message thread.
    controls.setSlotParameter = [this] (int slot)
    {
        if (auto* p = parameters.getParameter (ParamIDs::slot))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) (slot + 1)));
    };

    controls.setMasterParameter = [this] (bool on)
    {
        if (auto* p = parameters.getParameter (ParamIDs::master))
            p->setValueNotifyingHost (on ? 1.0f : 0.0f);
    };

    member = hub->addMember (controls);
}

GriddyProcessor::~GriddyProcessor()
{
    hub->removeMember (member);
}

void GriddyProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hub->prepare (sampleRate, samplesPerBlock);
}
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
            s.isLooping = pos->getIsLooping();
            s.timeInSamples = pos->getTimeInSamples().orFallback (0);

            if (const auto sig = pos->getTimeSignature())
            {
                s.timeSigNumerator = sig->numerator;
                s.timeSigDenominator = sig->denominator;
            }
        }
    }

    transport.write (s);

    // Hand the same block to the recorder and the playback engine.
    hub->processBlock (*member, buffer, s);
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

    const auto slot = member->slot.load();
    const auto sources = hub->video().getSlotSources();
    state.setProperty ("source", slot >= 0 ? sources[(size_t) slot] : juce::String(), nullptr);
    state.setProperty ("outputFolder", hub->video().getOutputFolder().getFullPathName(), nullptr);
    state.setProperty ("session", hub->getSession(), nullptr);
    state.setProperty ("layout", juce::JSON::toString (hub->layoutToVar(), true), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void GriddyProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    parameters.replaceState (state);

    if (const auto folder = state.getProperty ("outputFolder").toString(); juce::File::isAbsolutePath (folder))
        hub->video().setOutputFolder (juce::File (folder));

    if (const auto layout = state.getProperty ("layout").toString(); layout.isNotEmpty())
        hub->applyLayoutVar (juce::JSON::parse (layout));

    // All instances of one project carry the same session id. The first one to load decides it.
    hub->setSession (state.getProperty ("session").toString());

    // If a camera from last session is unplugged, the slot just shows "no camera".
    const auto source = state.getProperty ("source").toString();
    const auto slot = (int) std::lround (parameters.getRawParameterValue (ParamIDs::slot)->load()) - 1;
    if (source.isNotEmpty() && juce::isPositiveAndBelow (slot, Hub::maxSlots))
        hub->video().setSlotSource (slot, source);
}

} // namespace griddy

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new griddy::GriddyProcessor();
}
