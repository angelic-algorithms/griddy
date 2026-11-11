#pragma once

#include "TakeFormat.h"
#include "VideoDecoder.h"

namespace griddy
{

/** True when this build has FFmpeg. Everything else in this header does nothing without it. */
bool ffmpegAvailable();

/** Writes take.mov next to the take's media: the intraframe pictures (the JPEGs are remuxed, not re-encoded, so no quality
    loss and it is fast) and the reference audio, which is the audio carrying the Griddy sync signal when the take was
    recorded with it. Put that file on a track in Live's Arrangement View and the sync decoder locks to it.

    Picture time stamps are scene time plus the video offset. If that would start before zero, both streams are delayed by the same
    amount, so the pair stays aligned. A DAW that plays the file sees audio and picture in sync.
*/
bool exportTakeMov (const TakeMeta& meta, const juce::File& takeFolder, const juce::File& movFile, juce::String& error);

/** Decodes the video stream of take.mov (or any file handed to open as takeFolder/take.mov). Software decode through FFmpeg. */
std::unique_ptr<VideoDecoder> createFfmpegDecoder();

} // namespace griddy
