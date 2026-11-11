#pragma once

#include "TakeFormat.h"

#include <juce_graphics/juce_graphics.h>

#include <memory>

namespace griddy
{

/** Decodes the frames of one take, one at a time, in any order.

    Every frame of a take is an intraframe, so decode(i) never depends on another frame and
    seeking costs the same as playing forward. That is the whole reason takes are intraframe.

    Implementations:
      JpegTakeDecoder   portable, used by tests, the harness, Linux CI and as the fallback (this file)
      VideoToolbox      macOS, hardware decode of ProRes Proxy or all intraframe H.264 .mov. Not written yet.
      MediaFoundation   Windows, hardware decode through D3D11. Not written yet.
    The two hardware backends are named by the design and cannot be built or measured without those platforms.
*/
class VideoDecoder
{
public:
    virtual ~VideoDecoder() = default;

    /** useProxy picks the tile sized copy of the frames, which is what 9 slot playback wants. */
    virtual bool open (const TakeMeta& meta, const juce::File& takeFolder, bool useProxy) = 0;
    virtual int numFrames() const = 0;

    /** Decodes one frame. Returns an invalid Image on failure. Called from one thread at a time. */
    virtual juce::Image decode (int index) = 0;

    virtual const char* name() const = 0;
};

std::unique_ptr<VideoDecoder> createJpegTakeDecoder();

/** The best decoder available on this platform. Today that is the JPEG one everywhere. */
std::unique_ptr<VideoDecoder> createDefaultDecoder();

} // namespace griddy
