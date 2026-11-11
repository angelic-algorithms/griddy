// macOS take.mov writer. The JPEG frames go into the movie as they are (passthrough, no re-encode), the reference audio as
// 16 bit PCM. Same layout as the FFmpeg exporter in MovTake.cpp. Compiled with ARC.
#include "MovTake.h"

#if JUCE_MAC

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
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
        error = "stage 1 (inputs)";
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

        error = "stage 2 (create writer)";
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

        error = "stage 3 (start writing)";
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


        error = "stage 4 (append samples)";

        // The documented pattern: each input pulls its own samples when the writer is ready for more. The writer
        // decides the interleave, so there is no ordering for us to get wrong.
        struct Shared
        {
            std::atomic<bool> ok { true };
            std::atomic<int> vi { 0 };
            std::atomic<int64_t> aPos { 0 };
            std::mutex errorLock;
            juce::String problem;
            int64_t lastPts = -1;
            std::vector<int16_t> pcm;
        };
        auto shp = std::make_shared<Shared>();
        Shared* const sh = shp.get();
        const auto totalAudio = (int64_t) audio.size();
        const int chunk = 2048;

        sh->pcm.resize ((size_t) chunk);

        dispatch_group_t group = dispatch_group_create();
        dispatch_queue_t vq = dispatch_queue_create ("griddy.export.video", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_t aq = dispatch_queue_create ("griddy.export.audio", DISPATCH_QUEUE_SERIAL);

        dispatch_group_enter (group);
        [vin requestMediaDataWhenReadyOnQueue: vq usingBlock: ^{
            (void) shp;
            while (vin.isReadyForMoreMediaData)
            {
                const int i = sh->vi.load();
                if (i >= meta.numFrames || ! sh->ok.load())
                {
                    [vin markAsFinished];
                    dispatch_group_leave (group);
                    return;
                }

                juce::MemoryBlock jpeg;
                if (! takeFolder.getChildFile ("frames").getChildFile (juce::String::formatted ("%06d.jpg", i)).loadFileAsData (jpeg) || jpeg.isEmpty())
                {
                    const std::scoped_lock sl (sh->errorLock);
                    sh->problem = "missing frame " + juce::String (i);
                    sh->ok = false;
                    continue;
                }

                auto pts = (int64_t) std::llround ((meta.frameTimes[(size_t) i] + meta.videoOffsetSeconds() + lead) * 30000.0);
                if (pts <= sh->lastPts)
                    pts = sh->lastPts + 1;
                sh->lastPts = pts;

                CMSampleBufferRef sb = makeVideoSample (jpeg.getData(), jpeg.getSize(), vfd, CMTimeMake (pts, 30000), CMTimeMake (1000, 30000));
                const bool appended = sb != nullptr && [vin appendSampleBuffer: sb];
                if (sb != nullptr)
                    CFRelease (sb);

                if (! appended)
                {
                    const std::scoped_lock sl (sh->errorLock);
                    sh->problem = "video append failed at frame " + juce::String (i);
                    sh->ok = false;
                }

                sh->vi = i + 1;
            }
        }];

        dispatch_group_enter (group);
        [ain requestMediaDataWhenReadyOnQueue: aq usingBlock: ^{
            (void) shp;
            while (ain.isReadyForMoreMediaData)
            {
                const auto pos = sh->aPos.load();
                if (pos >= totalAudio || ! sh->ok.load())
                {
                    [ain markAsFinished];
                    dispatch_group_leave (group);
                    return;
                }

                const auto n = (int) std::min<int64_t> (chunk, totalAudio - pos);
                for (int k = 0; k < n; ++k)
                    sh->pcm[(size_t) k] = (int16_t) std::clamp (std::lround (audio[(size_t) (pos + k)] * 32767.0f), -32768l, 32767l);

                CMSampleBufferRef sb = makeAudioSample (sh->pcm.data(), n, sr, afd, pos + leadSamples);
                const bool appended = sb != nullptr && [ain appendSampleBuffer: sb];
                if (sb != nullptr)
                    CFRelease (sb);

                if (! appended)
                {
                    const std::scoped_lock sl (sh->errorLock);
                    sh->problem = "audio append failed at sample " + juce::String ((int) pos);
                    sh->ok = false;
                }

                sh->aPos = pos + n;
            }
        }];

        // Never wait forever: a stuck writer must not hang the app.
        if (dispatch_group_wait (group, dispatch_time (DISPATCH_TIME_NOW, (int64_t) (60.0 * NSEC_PER_SEC))) != 0)
        {
            sh->ok = false;
            [writer cancelWriting];
            CFRelease (vfd);
            CFRelease (afd);
            error = "timed out appending (video frames " + juce::String (sh->vi.load()) + " of " + juce::String (meta.numFrames) + ", audio " + juce::String ((int) sh->aPos.load()) + " of " + juce::String ((int) totalAudio) + ", writer status " + juce::String ((int) writer.status) + ")";
            tmp.deleteFile();
            return false;
        }

        if (! sh->problem.isEmpty())
            error = sh->problem;

        if (error.startsWith ("stage"))
            error = "stage 5 (finish)";

        dispatch_semaphore_t done = dispatch_semaphore_create (0);
        [writer finishWritingWithCompletionHandler: ^{ dispatch_semaphore_signal (done); }];
        dispatch_semaphore_wait (done, DISPATCH_TIME_FOREVER);

        CFRelease (vfd);
        CFRelease (afd);

        if (! sh->ok.load() || writer.status != AVAssetWriterStatusCompleted)
        {
            if (writer.error != nil)
                error += " " + juce::String::fromUTF8 ([[writer.error description] UTF8String]);
            error += " (status " + juce::String ((int) writer.status) + ", video frames appended " + juce::String (sh->vi.load()) + ", audio samples " + juce::String ((int) sh->aPos.load()) + ")";
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
