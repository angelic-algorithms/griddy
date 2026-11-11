// macOS only. Compiled with ARC (see CMakeLists.txt), so no manual retain/release.
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include "Recorder.h"

namespace griddy
{

/** Writes the collage straight to an H.264 QuickTime file with AVAssetWriter.

    The idea: AVAssetWriter owns a pool of CVPixelBuffers in BGRA. For each
    frame we borrow a buffer, copy our image rows into it, and append it with
    a presentation time of frameIndex / fps. JUCE's ARGB pixels are stored as
    B, G, R, A bytes on Apple CPUs, which is exactly kCVPixelFormatType_32BGRA,
    so the copy is a plain memcpy per row with no colour conversion.
*/
class AVFoundationRecorder final : public Recorder
{
public:
    ~AVFoundationRecorder() override
    {
        if (writer != nil)
            finish();
    }

    bool start (const juce::File& baseFile, const TakeInfo& takeInfo, juce::String& error) override
    {
        info = takeInfo;
        file = baseFile.withFileExtension ("mov");
        file.deleteFile();
        framesWritten = 0;
        framesDropped = 0;

        NSError* err = nil;
        NSURL* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()]];
        writer = [[AVAssetWriter alloc] initWithURL: url fileType: AVFileTypeQuickTimeMovie error: &err];

        if (writer == nil)
        {
            error = "AVAssetWriter failed: " + juce::String ([[err localizedDescription] UTF8String]);
            return false;
        }

        const auto fps = juce::roundToInt (info.fps);
        // Roughly 0.1 bits per pixel per frame: about 2.8 Mbps at 720p30, 6 Mbps at 1080p30.
        const auto bitrate = (int) ((double) info.width * info.height * info.fps * 0.1);

        NSDictionary* compression = @{ AVVideoAverageBitRateKey: @(bitrate),
                                       AVVideoExpectedSourceFrameRateKey: @(fps),
                                       AVVideoMaxKeyFrameIntervalKey: @(fps * 2) };

        NSDictionary* settings = @{ AVVideoCodecKey: AVVideoCodecTypeH264,
                                    AVVideoWidthKey: @(info.width),
                                    AVVideoHeightKey: @(info.height),
                                    AVVideoCompressionPropertiesKey: compression };

        input = [[AVAssetWriterInput alloc] initWithMediaType: AVMediaTypeVideo outputSettings: settings];
        input.expectsMediaDataInRealTime = YES;

        NSDictionary* bufferAttributes = @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                            (id) kCVPixelBufferWidthKey: @(info.width),
                                            (id) kCVPixelBufferHeightKey: @(info.height) };

        adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput: input
                                                              sourcePixelBufferAttributes: bufferAttributes];

        if (! [writer canAddInput: input])
        {
            error = "AVAssetWriter can't take a video input with these settings";
            writer = nil;
            return false;
        }

        [writer addInput: input];

        if (! [writer startWriting])
        {
            error = "AVAssetWriter couldn't start: " + juce::String ([[[writer error] localizedDescription] UTF8String]);
            writer = nil;
            return false;
        }

        [writer startSessionAtSourceTime: kCMTimeZero];
        return true;
    }

    void writeFrame (const juce::Image& frameIn, int64_t frameIndex) override
    {
        if (writer == nil || writer.status != AVAssetWriterStatusWriting)
            return;

        // The encoder is behind. Dropping a frame keeps us real time; the
        // timestamps of later frames are still correct, so sync is preserved.
        if (! input.readyForMoreMediaData)
        {
            ++framesDropped;
            return;
        }

        CVPixelBufferPoolRef pool = adaptor.pixelBufferPool;
        CVPixelBufferRef buffer = nullptr;

        if (pool == nullptr || CVPixelBufferPoolCreatePixelBuffer (nullptr, pool, &buffer) != kCVReturnSuccess)
        {
            ++framesDropped;
            return;
        }

        const auto frame = frameIn.getFormat() == juce::Image::ARGB ? frameIn
                                                                    : frameIn.convertedToFormat (juce::Image::ARGB);
        const auto w = juce::jmin (frame.getWidth(), info.width);
        const auto h = juce::jmin (frame.getHeight(), info.height);

        CVPixelBufferLockBaseAddress (buffer, 0);
        {
            auto* dst = static_cast<uint8_t*> (CVPixelBufferGetBaseAddress (buffer));
            const auto dstStride = CVPixelBufferGetBytesPerRow (buffer);
            const juce::Image::BitmapData src (frame, juce::Image::BitmapData::readOnly);

            for (int y = 0; y < h; ++y)
                std::memcpy (dst + (size_t) y * dstStride, src.getLinePointer (y), (size_t) w * 4);
        }
        CVPixelBufferUnlockBaseAddress (buffer, 0);

        const auto time = CMTimeMake ((int64_t) frameIndex, (int32_t) juce::roundToInt (info.fps));

        if ([adaptor appendPixelBuffer: buffer withPresentationTime: time])
            ++framesWritten;
        else
            ++framesDropped;

        CVPixelBufferRelease (buffer);
    }

    juce::File finish() override
    {
        if (writer == nil)
            return file;

        if (writer.status == AVAssetWriterStatusWriting)
        {
            [input markAsFinished];

            // finishWriting is asynchronous. We're on the engine thread, never
            // the audio or UI thread, so waiting here is fine and makes the
            // file complete by the time we tell the UI about it.
            dispatch_semaphore_t done = dispatch_semaphore_create (0);
            [writer finishWritingWithCompletionHandler: ^{ dispatch_semaphore_signal (done); }];
            dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, (int64_t) 10 * NSEC_PER_SEC));
        }

        writeTakeSidecar (file.withFileExtension ("json"), info, file, framesWritten);

        writer = nil;
        input = nil;
        adaptor = nil;
        return file;
    }

    juce::String getFormatName() const override { return "H.264 QuickTime"; }

private:
    TakeInfo info;
    juce::File file;
    int64_t framesWritten = 0, framesDropped = 0;

    AVAssetWriter* writer = nil;
    AVAssetWriterInput* input = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
};

std::unique_ptr<Recorder> createAVFoundationRecorder()
{
    return std::make_unique<AVFoundationRecorder>();
}

} // namespace griddy
