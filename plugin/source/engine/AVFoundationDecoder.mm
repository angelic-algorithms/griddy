// macOS decoder for take.mov. AVAssetReader hands the compressed samples to VideoToolbox when the codec
// has a hardware decoder (H.264, HEVC, ProRes) and to Apple's software decoders otherwise (Photo JPEG).
// Either way the work is Apple's code, which is the point: no FFmpeg needed on a Mac.
//
// Compiled with ARC (see CMakeLists.txt).
#include <cstdlib>
#include <cstdio>
#include "VideoDecoder.h"

#if JUCE_MAC

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <vector>

namespace griddy
{
namespace
{
class AVFoundationDecoder final : public VideoDecoder
{
public:
    bool open (const TakeMeta& meta, const juce::File& takeFolder, bool useProxy) override
    {
        @autoreleasepool
        {
            const auto path = takeFolder.getChildFile ("take.mov").getFullPathName();
            NSURL* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: path.toRawUTF8()]];
            asset = [AVURLAsset URLAssetWithURL: url options: nil];

            NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType: AVMediaTypeVideo];
            if (tracks.count == 0)
                return false;

            track = tracks.firstObject;
            outW = useProxy ? meta.proxyWidth : (int) track.naturalSize.width;
            outH = useProxy ? meta.proxyHeight : (int) track.naturalSize.height;

            // Frame times from the container: read the compressed samples without decoding them.
            NSError* error = nil;
            AVAssetReader* scan = [[AVAssetReader alloc] initWithAsset: asset error: &error];
            if (scan == nil)
                return false;

            AVAssetReaderTrackOutput* out = [[AVAssetReaderTrackOutput alloc] initWithTrack: track outputSettings: nil];
            [scan addOutput: out];
            if (! [scan startReading])
                return false;

            while (CMSampleBufferRef sb = [out copyNextSampleBuffer])
            {
                stamps.push_back (CMTimeGetSeconds (CMSampleBufferGetPresentationTimeStamp (sb)));
                CFRelease (sb);
            }

            if (std::getenv ("GRIDDY_DEBUG_DECODER") != nullptr)
            {
                fprintf (stderr, "[avf] %d samples, track duration %.3f, nominal fps %.2f, size %.0fx%.0f\n", (int) stamps.size(),
                         CMTimeGetSeconds (track.timeRange.duration), (double) track.nominalFrameRate, track.naturalSize.width, track.naturalSize.height);
                for (size_t i = 0; i < stamps.size() && i < 8; ++i)
                    fprintf (stderr, "[avf]  stamp %zu = %.4f\n", i, stamps[i]);
                for (size_t i = stamps.size() > 4 ? stamps.size() - 4 : 0; i < stamps.size(); ++i)
                    fprintf (stderr, "[avf]  stamp %zu = %.4f\n", i, stamps[i]);
            }

            std::sort (stamps.begin(), stamps.end());
            return ! stamps.empty();
        }
    }

    int numFrames() const override { return (int) stamps.size(); }

    juce::Image decode (int index) override
    {
        if (index < 0 || index >= (int) stamps.size())
            return {};

        @autoreleasepool
        {
            // Sequential reads continue the open reader. Anything else restarts it at the wanted frame:
            // every frame is an intraframe, so a restart costs the same wherever it lands.
            if (reader == nil || index != lastIndex + 1)
                if (! startReader (index))
                    return {};

            lastIndex = index;
            CMSampleBufferRef sb = [output copyNextSampleBuffer];

            if (sb == nullptr)
            {
                if (! startReader (index))
                    return {};
                sb = [output copyNextSampleBuffer];
                if (sb == nullptr)
                    return {};
            }

            auto img = toImage (sb);
            CFRelease (sb);
            return img;
        }
    }

    const char* name() const override { return "AVFoundation (VideoToolbox where available)"; }

private:
    bool startReader (int index)
    {
        NSError* error = nil;
        reader = [[AVAssetReader alloc] initWithAsset: asset error: &error];
        if (reader == nil)
            return false;

        NSDictionary* settings = @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) };
        output = [[AVAssetReaderTrackOutput alloc] initWithTrack: track outputSettings: settings];
        output.alwaysCopiesSampleData = NO;
        [reader addOutput: output];

        const auto start = CMTimeMakeWithSeconds (stamps[(size_t) index] - 0.001, 90000);
        reader.timeRange = CMTimeRangeMake (start, CMTimeMakeWithSeconds (3600.0, 90000));
        return [reader startReading];
    }

    juce::Image toImage (CMSampleBufferRef sb)
    {
        CVImageBufferRef pb = CMSampleBufferGetImageBuffer (sb);
        if (pb == nullptr)
            return {};

        CVPixelBufferLockBaseAddress (pb, kCVPixelBufferLock_ReadOnly);
        const auto w = (int) CVPixelBufferGetWidth (pb), h = (int) CVPixelBufferGetHeight (pb);
        const auto* base = static_cast<const uint8_t*> (CVPixelBufferGetBaseAddress (pb));
        const auto stride = (int) CVPixelBufferGetBytesPerRow (pb);

        juce::Image full (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
        {
            const juce::Image::BitmapData bd (full, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < h; ++y)
                std::memcpy (bd.getLinePointer (y), base + (size_t) y * (size_t) stride, (size_t) w * 4);
        }
        CVPixelBufferUnlockBaseAddress (pb, kCVPixelBufferLock_ReadOnly);

        if (w == outW && h == outH)
            return full;

        return full.rescaled (outW, outH, juce::Graphics::mediumResamplingQuality).convertedToFormat (juce::Image::ARGB);
    }

    AVURLAsset* asset = nil;
    AVAssetTrack* track = nil;
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    std::vector<double> stamps;
    int outW = 0, outH = 0, lastIndex = -10;
};
} // namespace

std::unique_ptr<VideoDecoder> createPlatformDecoder() { return std::make_unique<AVFoundationDecoder>(); }

} // namespace griddy

#endif
