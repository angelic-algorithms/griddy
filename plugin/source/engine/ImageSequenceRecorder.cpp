#include "ImageSequenceRecorder.h"

namespace griddy
{

bool writeTakeSidecar (const juce::File& jsonFile, const TakeInfo& info, const juce::File& video, int64_t framesWritten)
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("app", "Griddy");
    obj->setProperty ("video", video.getFileName());
    obj->setProperty ("startPpq", info.startPpq);
    // Bars and beats as the DAW shows them (1 based), assuming the time signature never changed.
    const auto barLen = (double) info.timeSigNumerator * 4.0 / (double) info.timeSigDenominator;
    obj->setProperty ("startBar", (int) std::floor (info.startPpq / barLen) + 1);
    obj->setProperty ("bpm", info.bpm);
    obj->setProperty ("timeSignature", juce::String (info.timeSigNumerator) + "/" + juce::String (info.timeSigDenominator));
    obj->setProperty ("fps", info.fps);
    obj->setProperty ("width", info.width);
    obj->setProperty ("height", info.height);
    obj->setProperty ("frames", (juce::int64) framesWritten);
    obj->setProperty ("durationSeconds", (double) framesWritten / info.fps);

    return jsonFile.replaceWithText (juce::JSON::toString (juce::var (obj.release())));
}

bool ImageSequenceRecorder::start (const juce::File& baseFile, const TakeInfo& info, juce::String& error)
{
    folder = baseFile;
    takeInfo = info;
    framesWritten = 0;

    if (const auto r = folder.createDirectory(); r.failed())
    {
        error = "Couldn't create " + folder.getFullPathName() + ": " + r.getErrorMessage();
        return false;
    }

    jpeg.setQuality (0.9f);
    return true;
}

void ImageSequenceRecorder::writeFrame (const juce::Image& frame, int64_t frameIndex)
{
    // Six digits is 9 hours at 30 fps, plenty for one take.
    const auto file = folder.getChildFile ("frame_" + juce::String (frameIndex).paddedLeft ('0', 6) + ".jpg");
    file.deleteFile();

    if (juce::FileOutputStream out (file); out.openedOk())
        if (jpeg.writeImageToStream (frame, out))
            ++framesWritten;
}

juce::File ImageSequenceRecorder::finish()
{
    writeTakeSidecar (folder.getChildFile ("take.json"), takeInfo, folder, framesWritten);
    return folder;
}

} // namespace griddy
