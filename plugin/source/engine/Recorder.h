#pragma once

#include <juce_graphics/juce_graphics.h>
#include <memory>

namespace griddy
{

/** Where a take sits in the song, written next to the video so it can be lined up in the DAW. */
struct TakeInfo
{
    double startPpq = 0.0;     // song position (quarter notes) of the first frame
    double bpm = 120.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    double fps = 30.0;
    int width = 1280;
    int height = 720;
};

/** Writes composited frames to disk. One instance per take.

    All calls come from the video engine thread, never the audio thread.
*/
class Recorder
{
public:
    virtual ~Recorder() = default;

    /** baseFile has no extension; each recorder adds its own (.mov, a folder of JPEGs, ...). */
    virtual bool start (const juce::File& baseFile, const TakeInfo& info, juce::String& error) = 0;

    /** frameIndex counts from 0 at the start of the take; time = frameIndex / fps. */
    virtual void writeFrame (const juce::Image& frame, int64_t frameIndex) = 0;

    /** Finishes the file and returns what was written. */
    virtual juce::File finish() = 0;

    virtual juce::String getFormatName() const = 0;
};

/** Best recorder for this platform: H.264 .mov via AVFoundation on macOS, a JPEG sequence elsewhere. */
std::unique_ptr<Recorder> createRecorder();

/** Writes the take's sidecar JSON (song position, tempo, fps) next to the video. */
bool writeTakeSidecar (const juce::File& jsonFile, const TakeInfo& info, const juce::File& video, int64_t framesWritten);

} // namespace griddy
