#include "VideoDecoder.h"

#include "MovTake.h"

namespace griddy
{

namespace
{
class JpegTakeDecoder final : public VideoDecoder
{
public:
    bool open (const TakeMeta& meta, const juce::File& takeFolder, bool useProxy) override
    {
        dir = takeFolder.getChildFile (useProxy ? "proxy" : "frames");
        count = meta.numFrames;
        return dir.isDirectory() && count > 0;
    }

    int numFrames() const override { return count; }

    juce::Image decode (int index) override
    {
        if (index < 0 || index >= count)
            return {};

        juce::FileInputStream in (dir.getChildFile (juce::String::formatted ("%06d.jpg", index)));
        if (in.failedToOpen())
            return {};

        auto img = format.decodeImage (in);
        if (! img.isValid())
            return {};

        // Software ARGB images draw fastest in the CPU compositor.
        return img.convertedToFormat (juce::Image::ARGB);
    }

    const char* name() const override { return "JPEG intraframe (portable)"; }

private:
    juce::File dir;
    int count = 0;
    juce::JPEGImageFormat format;
};
} // namespace

std::unique_ptr<VideoDecoder> createJpegTakeDecoder() { return std::make_unique<JpegTakeDecoder>(); }
#if ! JUCE_MAC
std::unique_ptr<VideoDecoder> createPlatformDecoder() { return nullptr; }
#endif

std::unique_ptr<VideoDecoder> createDecoder (DecoderKind kind, const juce::File& takeFolder)
{
    switch (kind)
    {
        case DecoderKind::jpeg:     return createJpegTakeDecoder();
        case DecoderKind::ffmpeg:   return createFfmpegDecoder();
        case DecoderKind::platform: return createPlatformDecoder();
        case DecoderKind::automatic: break;
    }

    if (takeFolder.getChildFile ("frames").isDirectory())
        return createJpegTakeDecoder();

    if (auto d = createPlatformDecoder())
        return d;

    if (auto d = createFfmpegDecoder())
        return d;

    return createJpegTakeDecoder();
}

std::unique_ptr<VideoDecoder> createDefaultDecoder() { return createJpegTakeDecoder(); }

} // namespace griddy
