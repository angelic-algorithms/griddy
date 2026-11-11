// macOS writer for the exported grid: H.264 picture and AAC sound in an MP4, through AVAssetWriter. Compiled with ARC.
#include "GridExport.h"

#if JUCE_MAC

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <thread>

namespace griddy
{
namespace
{
class AppleBackend final : public ExportBackend
{
public:
    bool open (const ExportSettings& s, juce::String& error) override
    {
        cfg = s;
        NSURL* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: s.file.getFullPathName().toRawUTF8()]];
        NSError* err = nil;
        writer = [[AVAssetWriter alloc] initWithURL: url fileType: AVFileTypeMPEG4 error: &err];
        if (writer == nil)
        {
            error = juce::String::fromUTF8 (err.localizedDescription.UTF8String ?: "could not create the writer");
            return false;
        }

        NSDictionary* vset = @{ AVVideoCodecKey: AVVideoCodecTypeH264,
                                AVVideoWidthKey: @(s.width),
                                AVVideoHeightKey: @(s.height),
                                AVVideoCompressionPropertiesKey: @{ AVVideoAverageBitRateKey: @((long) s.width * s.height * 4),
                                                                    AVVideoMaxKeyFrameIntervalKey: @(s.fps * 2) } };
        video = [AVAssetWriterInput assetWriterInputWithMediaType: AVMediaTypeVideo outputSettings: vset];
        video.expectsMediaDataInRealTime = YES;

        NSDictionary* pset = @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                (id) kCVPixelBufferWidthKey: @(s.width),
                                (id) kCVPixelBufferHeightKey: @(s.height) };
        adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput: video sourcePixelBufferAttributes: pset];

        AudioChannelLayout layout = {};
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
        NSDictionary* aset = @{ AVFormatIDKey: @(kAudioFormatMPEG4AAC),
                                AVSampleRateKey: @(s.sampleRate),
                                AVNumberOfChannelsKey: @2,
                                AVEncoderBitRateKey: @192000,
                                AVChannelLayoutKey: [NSData dataWithBytes: &layout length: sizeof (layout)] };
        audio = [AVAssetWriterInput assetWriterInputWithMediaType: AVMediaTypeAudio outputSettings: aset];
        audio.expectsMediaDataInRealTime = YES;

        if (! [writer canAddInput: video] || ! [writer canAddInput: audio])
        {
            error = "the writer refused the video or audio settings";
            return false;
        }

        [writer addInput: video];
        [writer addInput: audio];

        if (! [writer startWriting])
        {
            error = juce::String::fromUTF8 (writer.error.localizedDescription.UTF8String ?: "startWriting failed");
            return false;
        }

        [writer startSessionAtSourceTime: kCMTimeZero];
        started = true;
        return true;
    }

    bool writeVideo (const uint8_t* bgra, int stride, double pts) override
    {
        if (! started || ! waitReady (video))
            return false;

        const auto t = CMTimeMakeWithSeconds (std::max (0.0, pts), 600);
        if (CMTIME_COMPARE_INLINE (t, <=, lastVideo))
            return false;

        CVPixelBufferRef pb = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer (kCFAllocatorDefault, adaptor.pixelBufferPool, &pb) != kCVReturnSuccess || pb == nullptr)
            return false;

        CVPixelBufferLockBaseAddress (pb, 0);
        auto* dst = (uint8_t*) CVPixelBufferGetBaseAddress (pb);
        const auto dstStride = CVPixelBufferGetBytesPerRow (pb);
        for (int y = 0; y < cfg.height; ++y)
            std::memcpy (dst + (size_t) y * dstStride, bgra + (size_t) y * (size_t) stride, (size_t) cfg.width * 4);
        CVPixelBufferUnlockBaseAddress (pb, 0);

        const auto ok = [adaptor appendPixelBuffer: pb withPresentationTime: t];
        CVPixelBufferRelease (pb);
        if (ok)
            lastVideo = t;
        return ok;
    }

    bool writeAudio (const int16_t* stereo, int frames, int64_t firstFrame) override
    {
        if (! started || frames <= 0 || ! waitReady (audio))
            return false;

        AudioStreamBasicDescription asbd = {};
        asbd.mSampleRate = cfg.sampleRate;
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        asbd.mBitsPerChannel = 16;
        asbd.mChannelsPerFrame = 2;
        asbd.mBytesPerFrame = 4;
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerPacket = 4;

        if (audioFormat == nullptr)
            if (CMAudioFormatDescriptionCreate (kCFAllocatorDefault, &asbd, 0, nullptr, 0, nullptr, nullptr, &audioFormat) != noErr)
                return false;

        const size_t size = (size_t) frames * 4;
        CMBlockBufferRef block = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock (kCFAllocatorDefault, nullptr, size, kCFAllocatorDefault, nullptr, 0, size, kCMBlockBufferAssureMemoryNowFlag, &block) != noErr)
            return false;

        CMBlockBufferReplaceDataBytes (stereo, block, 0, size);
        CMSampleBufferRef sb = nullptr;
        const auto pts = CMTimeMake (firstFrame, (int32_t) cfg.sampleRate);
        const auto status = CMAudioSampleBufferCreateReadyWithPacketDescriptions (kCFAllocatorDefault, block, audioFormat, frames, pts, nullptr, &sb);
        CFRelease (block);
        if (status != noErr || sb == nullptr)
            return false;

        const auto ok = [audio appendSampleBuffer: sb];
        CFRelease (sb);
        return ok;
    }

    bool close (juce::String& error) override
    {
        if (! started)
            return true;

        started = false;
        if (audioFormat != nullptr)
        {
            CFRelease (audioFormat);
            audioFormat = nullptr;
        }

        [video markAsFinished];
        [audio markAsFinished];

        dispatch_semaphore_t done = dispatch_semaphore_create (0);
        [writer finishWritingWithCompletionHandler: ^{ dispatch_semaphore_signal (done); }];
        if (dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0)
        {
            error = "finishing the file timed out";
            return false;
        }

        if (writer.status != AVAssetWriterStatusCompleted)
        {
            error = juce::String::fromUTF8 (writer.error.localizedDescription.UTF8String ?: "the writer did not finish");
            return false;
        }

        return true;
    }

private:
    static bool waitReady (AVAssetWriterInput* in)
    {
        for (int i = 0; i < 1000 && ! in.isReadyForMoreMediaData; ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        return in.isReadyForMoreMediaData;
    }

    ExportSettings cfg;
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* video = nil;
    AVAssetWriterInput* audio = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
    CMAudioFormatDescriptionRef audioFormat = nullptr;
    CMTime lastVideo = kCMTimeNegativeInfinity;
    bool started = false;
};
} // namespace

std::unique_ptr<ExportBackend> createExportBackend() { return std::make_unique<AppleBackend>(); }
bool gridExportAvailable() { return true; }

MediaInfo probeMedia (const juce::File& file)
{
    MediaInfo info;
    NSURL* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()]];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL: url options: nil];
    NSArray<AVAssetTrack*>* vt = [asset tracksWithMediaType: AVMediaTypeVideo];
    NSArray<AVAssetTrack*>* at = [asset tracksWithMediaType: AVMediaTypeAudio];
    if (vt.count == 0)
    {
        info.problem = "no video track";
        return info;
    }

    const auto size = vt[0].naturalSize;
    info.width = (int) size.width;
    info.height = (int) size.height;
    info.videoSeconds = CMTimeGetSeconds (vt[0].timeRange.duration);
    info.fileSeconds = CMTimeGetSeconds (asset.duration);
    info.videoCodec = "h264";

    if (at.count > 0)
    {
        info.audioSeconds = CMTimeGetSeconds (at[0].timeRange.duration);
        info.audioChannels = 2;
        info.audioCodec = "aac";
    }

    NSError* err = nil;
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset: asset error: &err];
    if (reader != nil)
    {
        AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack: vt[0] outputSettings: nil];
        [reader addOutput: out];
        if ([reader startReading])
            while (CMSampleBufferRef sb = [out copyNextSampleBuffer])
            {
                if (CMSampleBufferGetNumSamples (sb) > 0 && CMSampleBufferGetTotalSampleSize (sb) > 0)
                    info.videoFrames += (int) CMSampleBufferGetNumSamples (sb);
                CFRelease (sb);
            }
    }

    info.ok = info.width > 0 && info.audioChannels > 0;
    return info;
}

} // namespace griddy

#endif
