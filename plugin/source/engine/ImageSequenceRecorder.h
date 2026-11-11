#pragma once

#include "Recorder.h"

namespace griddy
{

/** Portable fallback: a folder of numbered JPEGs plus take.json.

    Works everywhere JUCE does, needs no codecs, and ffmpeg turns it into a
    video in one command (see the README). On macOS the AVFoundation
    recorder is used instead.
*/
class ImageSequenceRecorder final : public Recorder
{
public:
    bool start (const juce::File& baseFile, const TakeInfo& info, juce::String& error) override;
    void writeFrame (const juce::Image& frame, int64_t frameIndex) override;
    juce::File finish() override;
    juce::String getFormatName() const override { return "JPEG sequence"; }

    int64_t getFramesWritten() const noexcept { return framesWritten; }

private:
    juce::File folder;
    TakeInfo takeInfo;
    int64_t framesWritten = 0;
    juce::JPEGImageFormat jpeg;
};

} // namespace griddy
