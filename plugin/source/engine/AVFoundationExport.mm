// macOS take.mov writer. The JPEG frames go into the movie as they are (passthrough, no re-encode), the reference audio as
// 16 bit PCM. Same layout as the FFmpeg exporter in MovTake.cpp. Compiled with ARC.
#include "MovTake.h"

#if JUCE_MAC

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace griddy
{
namespace
{
CMSampleBufferRef makeVideoSample (const void* bytes, size_t size, CMVideoFormatDescriptionRef fd, CMTime pts, CMTime duration)
{
    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock (kCFAllocatorDefault, nullptr, size, kCFAllocatorDefault, nullptr, 0, size, kCMBlockBufferAssureMemoryNowFlag, &block) != noErr)
        return nullptr;

    CMBlockBufferReplaceDataBytes (bytes, block, 0, size);
    CMSampleTimingInfo timing { duration, pts, pts };
    CMSampleBufferRef sb = nullptr;
    const size_t sizes[] = { size };
    CMSampleBufferCreate (kCFAllocatorDefault, block, true, nullptr, nullptr, fd, 1, 1, &timing, 1, sizes, &sb);
    CFRelease (block);
    return sb;
}

CMSampleBufferRef makeAudioSample (const int16_t* pcm, int frames, double sampleRate, CMAudioFormatDescriptionRef fd, int64_t firstFrame)
{
    const size_t size = (size_t) frames * 2;
    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock (kCFAllocatorDefault, nullptr, size, kCFAllocatorDefault, nullptr, 0, size, kCMBlockBufferAssureMemoryNowFlag, &block) != noErr)
        return nullptr;

    CMBlockBufferReplaceDataBytes (pcm, block, 0, size);
    CMSampleBufferRef sb = nullptr;
    const auto pts = CMTimeMake (firstFrame, (int32_t) sampleRate);
    CMAudioSampleBufferCreateReadyWithPacketDescriptions (kCFAllocatorDefault, block, fd, frames, pts, nullptr, &sb);
    CFRelease (block);
    return sb;
}
} // namespace

bool exportTakeMovApple (const TakeMeta& meta, const juce::File& takeFolder, const juce::File& movFile, juce::String& error)
{
    @autoreleasepool
    {
        if (meta.numFrames <= 0 || meta.frameTimes.size() < (size_t) meta.numFrames)
        {
            error = "take has no frames";
            return false;
        }

        std::vector<float> audio;
        double sr = 0.0;
        if (! readWav (takeFolder.getChildFile ("audio.wav"), audio, sr) || sr <= 0.0)
        {
            error = "no audio.wav";
            return false;
        }

        const auto tmp = movFile.getSiblingFile (movFile.getFileName() + ".part.mov");
        tmp.deleteFile();

        NSError* nserr = nil;
        NSURL* url = [NSURL fileURLWithPath: [NSString stringWithUTF8String: tmp.getFullPathName().toRawUTF8()]];
        AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL: url fileType: AVFileTypeQuickTimeMovie error: &nserr];
        if (writer == nil)
        {
            error = "AVAssetWriter could not be created: " + juce::String::fromUTF8 ([[nserr description] UTF8String] != nullptr ? [[nserr description] UTF8String] : "no NSError")
                    + " (path " + tmp.getFullPathName() + ")";
            return false;
        }

        CMVideoFormatDescriptionRef vfd = nullptr;
        CMVideoFormatDescriptionCreate (kCFAllocatorDefault, kCMVideoCodecType_JPEG, meta.width, meta.height, nullptr, &vfd);

        AudioStreamBasicDescription asbd {};
        asbd.mSampleRate = sr;
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        asbd.mBitsPerChannel = 16;
        asbd.mChannelsPerFrame = 1;
        asbd.mBytesPerFrame = 2;
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerPacket = 2;
        CMAudioFormatDescriptionRef afd = nullptr;
        CMAudioFormatDescriptionCreate (kCFAllocatorDefault, &asbd, 0, nullptr, 0, nullptr, nullptr, &afd);

        AVAssetWriterInput* vin = [AVAssetWriterInput assetWriterInputWithMediaType: AVMediaTypeVideo outputSettings: nil sourceFormatHint: vfd];
        NSDictionary* aset = @{ AVFormatIDKey: @(kAudioFormatLinearPCM), AVSampleRateKey: @(sr), AVNumberOfChannelsKey: @1,
                                AVLinearPCMBitDepthKey: @16, AVLinearPCMIsFloatKey: @NO, AVLinearPCMIsBigEndianKey: @NO,
                                AVLinearPCMIsNonInterleaved: @NO };
        AVAssetWriterInput* ain = [AVAssetWriterInput assetWriterInputWithMediaType: AVMediaTypeAudio outputSettings: aset sourceFormatHint: afd];
        vin.expectsMediaDataInRealTime = NO;
        ain.expectsMediaDataInRealTime = NO;

        if (! [writer canAddInput: vin] || ! [writer canAddInput: ain])
        {
            error = "AVAssetWriter refused the inputs";
            return false;
        }

        [writer addInput: vin];
        [writer addInput: ain];

        if (! [writer startWriting])
        {
            error = "startWriting failed: " + juce::String::fromUTF8 ([[writer.error description] UTF8String] != nullptr ? [[writer.error description] UTF8String] : "no NSError");
            return false;
        }

        const auto lead = std::max (0.0, -(meta.frameTimes.front() + meta.videoOffsetSeconds()));
        const auto leadSamples = (int64_t) std::llround (lead * sr);
        [writer startSessionAtSourceTime: kCMTimeZero];

        auto videoTime = [&] (int i) { return meta.frameTimes[(size_t) i] + meta.videoOffsetSeconds() + lead; };
        auto audioTime = [&] (int64_t pos) { return (double) (pos + leadSamples) / sr; };

        int vi = 0;
        int64_t aPos = 0, lastPts = -1;
        const auto totalAudio = (int64_t) audio.size();
        const int chunk = 2048;
        std::vector<int16_t> pcm ((size_t) chunk);
        bool ok = true;

        while (ok && (vi < meta.numFrames || aPos < totalAudio))
        {
            const bool wantVideo = vi < meta.numFrames && (aPos >= totalAudio || videoTime (vi) <= audioTime (aPos));
            AVAssetWriterInput* input = wantVideo ? vin : ain;

            if (! input.isReadyForMoreMediaData)
            {
                [NSThread sleepForTimeInterval: 0.001];
                if (writer.status == AVAssetWriterStatusFailed)
                    ok = false;
                continue;
            }

            if (wantVideo)
            {
                juce::MemoryBlock jpeg;
                if (! takeFolder.getChildFile ("frames").getChildFile (juce::String::formatted ("%06d.jpg", vi)).loadFileAsData (jpeg) || jpeg.isEmpty())
                {
                    error = "missing frame " + juce::String (vi);
                    ok = false;
                    break;
                }

                auto pts = (int64_t) std::llround (videoTime (vi) * 30000.0);
                if (pts <= lastPts)
                    pts = lastPts + 1;
                lastPts = pts;

                CMSampleBufferRef sb = makeVideoSample (jpeg.getData(), jpeg.getSize(), vfd, CMTimeMake (pts, 30000), CMTimeMake (1000, 30000));
                ok = sb != nullptr && [vin appendSampleBuffer: sb];
                if (sb != nullptr)
                    CFRelease (sb);
                ++vi;
            }
            else
            {
                const auto n = (int) std::min<int64_t> (chunk, totalAudio - aPos);
                for (int i = 0; i < n; ++i)
                    pcm[(size_t) i] = (int16_t) std::clamp (std::lround (audio[(size_t) (aPos + i)] * 32767.0f), -32768l, 32767l);

                CMSampleBufferRef sb = makeAudioSample (pcm.data(), n, sr, afd, aPos + leadSamples);
                ok = sb != nullptr && [ain appendSampleBuffer: sb];
                if (sb != nullptr)
                    CFRelease (sb);
                aPos += n;
            }
        }

        [vin markAsFinished];
        [ain markAsFinished];

        dispatch_semaphore_t done = dispatch_semaphore_create (0);
        [writer finishWritingWithCompletionHandler: ^{ dispatch_semaphore_signal (done); }];
        dispatch_semaphore_wait (done, DISPATCH_TIME_FOREVER);

        CFRelease (vfd);
        CFRelease (afd);

        if (! ok || writer.status != AVAssetWriterStatusCompleted)
        {
            if (writer.error != nil)
                error += " " + juce::String::fromUTF8 ([[writer.error description] UTF8String]);
            error += " (status " + juce::String ((int) writer.status) + ", video frames appended " + juce::String (vi) + ", audio samples " + juce::String ((int) aPos) + ")";
            tmp.deleteFile();
            return false;
        }

        movFile.deleteFile();
        if (! tmp.moveFileTo (movFile))
        {
            error = "could not move " + tmp.getFullPathName() + " to " + movFile.getFullPathName() + " (tmp exists: " + juce::String (tmp.existsAsFile() ? "yes" : "no") + ")";
            return false;
        }

        return true;
    }
}

} // namespace griddy

#endif
