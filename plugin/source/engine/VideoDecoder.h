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
      JpegTakeDecoder   portable, reads the per frame JPEGs a take is recorded as (this file)
      FfmpegDecoder     software decode of take.mov and any file FFmpeg reads (MovTake.cpp)
      AVFoundation      macOS, hardware decode through VideoToolbox (AVFoundationDecoder.mm)
    A Windows Media Foundation backend is not written.
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

enum class DecoderKind { automatic, jpeg, ffmpeg, platform };

/** Makes a decoder of the given kind, or nullptr if this build does not have it.
    automatic: the JPEG frames when the take has them (the fastest path for intraframe stills), otherwise take.mov
    through the platform decoder (hardware where there is one), otherwise FFmpeg. */
std::unique_ptr<VideoDecoder> createDecoder (DecoderKind kind, const juce::File& takeFolder);

/** Hardware backed decoder for take.mov, or nullptr. macOS: AVFoundation (VideoToolbox underneath). */
std::unique_ptr<VideoDecoder> createPlatformDecoder();

std::unique_ptr<VideoDecoder> createDefaultDecoder();

} // namespace griddy
