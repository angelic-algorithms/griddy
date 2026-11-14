#pragma once

#include <juce_graphics/juce_graphics.h>
#include <map>
#include <memory>
#include <mutex>

namespace griddy
{

struct VideoFrame
{
    juce::Image image;               // ARGB software image
    double captureTimeSeconds = 0.0; // steadyNowSeconds() at capture, the clock the recorder and engine use
    int64_t serial = 0;              // bumps per frame from this source
};

/** Latest frame from each video source, keyed by source id.

    Cameras deliver frames on their own threads at their own rate. The
    compositor only ever wants "the newest frame from camera X", so this is a
    one slot mailbox per source rather than a queue: old frames are simply
    replaced, which is what keeps latency low when the machine is busy.
*/
class FrameStore
{
public:
    void push (const juce::String& sourceId, juce::Image image, double captureTimeSeconds);

    /** Returns nullptr if that source hasn't produced a frame yet. */
    std::shared_ptr<const VideoFrame> latest (const juce::String& sourceId) const;

    void remove (const juce::String& sourceId);

private:
    mutable std::mutex lock;
    std::map<juce::String, std::shared_ptr<const VideoFrame>> frames;
};

} // namespace griddy
