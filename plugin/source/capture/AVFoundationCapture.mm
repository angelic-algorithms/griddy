// macOS only. Compiled with ARC.
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include "CaptureSource.h"

#include <functional>

// ObjC class names are global to the whole DAW process, and two plugins that
// define the same name will clash. A long, Griddy specific name avoids that.
@interface GriddyParkAngelFrameDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic) std::function<void (CVPixelBufferRef)> onFrame;
@end

@implementation GriddyParkAngelFrameDelegate
- (void) captureOutput: (AVCaptureOutput*) output
 didOutputSampleBuffer: (CMSampleBufferRef) sampleBuffer
        fromConnection: (AVCaptureConnection*) connection
{
    juce::ignoreUnused (output, connection);

    if (auto pixels = CMSampleBufferGetImageBuffer (sampleBuffer); pixels != nullptr && _onFrame)
        _onFrame (pixels);
}
@end

namespace griddy
{

static NSArray<AVCaptureDevice*>* discoverDevices()
{
    NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray arrayWithObject: AVCaptureDeviceTypeBuiltInWideAngleCamera];

    if (@available (macOS 14.0, *))
    {
        // External covers USB webcams and UVC HDMI capture cards (Elgato, etc).
        // Continuity Camera is an iPhone near the Mac, signed in to the same Apple ID.
        [types addObject: AVCaptureDeviceTypeExternal];
        [types addObject: AVCaptureDeviceTypeContinuityCamera];
    }
    else
    {
        JUCE_BEGIN_IGNORE_DEPRECATION_WARNINGS
        [types addObject: AVCaptureDeviceTypeExternalUnknown];
        JUCE_END_IGNORE_DEPRECATION_WARNINGS
    }

    auto* session = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes: types
                                                                           mediaType: AVMediaTypeVideo
                                                                            position: AVCaptureDevicePositionUnspecified];
    return [session devices];
}

std::vector<CaptureDeviceInfo> listAVFoundationDevices()
{
    std::vector<CaptureDeviceInfo> result;

    for (AVCaptureDevice* device in discoverDevices())
        result.push_back ({ "av:" + juce::String ([[device uniqueID] UTF8String]),
                            juce::String ([[device localizedName] UTF8String]),
                            "camera" });

    return result;
}

/** One camera or capture card through AVFoundation.

    AVCaptureVideoDataOutput hands us every frame as a CVPixelBuffer on a
    serial queue we own. We ask for BGRA so a frame can be copied straight
    into a juce::Image row by row, then dropped into the FrameStore.
    alwaysDiscardsLateVideoFrames means if we're slow, AVFoundation throws
    stale frames away instead of queueing them, which keeps latency flat.
*/
class AVFoundationSource final : public CaptureSource
{
public:
    AVFoundationSource (FrameStore& s, AVCaptureDevice* d)
        : CaptureSource (s), device (d),
          id ("av:" + juce::String ([[d uniqueID] UTF8String])),
          name ([[d localizedName] UTF8String])
    {
        queue = dispatch_queue_create ("com.parkangel.griddy.capture", DISPATCH_QUEUE_SERIAL);
    }

    ~AVFoundationSource() override { stop(); }

    juce::String getId() const override   { return id; }
    juce::String getName() const override { return name; }

    bool start (juce::String& error) override
    {
        NSError* err = nil;
        auto* input = [AVCaptureDeviceInput deviceInputWithDevice: device error: &err];

        if (input == nil)
        {
            error = "Couldn't open " + name + ": " + juce::String ([[err localizedDescription] UTF8String]);
            return false;
        }

        session = [[AVCaptureSession alloc] init];
        [session beginConfiguration];

        // 720p is plenty for a collage cell and keeps four cameras cheap. Fall back if the device can't.
        if ([session canSetSessionPreset: AVCaptureSessionPreset1280x720])
            session.sessionPreset = AVCaptureSessionPreset1280x720;
        else
            session.sessionPreset = AVCaptureSessionPresetHigh;

        if (! [session canAddInput: input])
        {
            error = name + " is busy or can't be used by this session";
            session = nil;
            return false;
        }

        [session addInput: input];

        output = [[AVCaptureVideoDataOutput alloc] init];
        output.videoSettings = @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) };
        output.alwaysDiscardsLateVideoFrames = YES;

        delegate = [[GriddyParkAngelFrameDelegate alloc] init];
        delegate.onFrame = [this] (CVPixelBufferRef pixels) { handleFrame (pixels); };
        [output setSampleBufferDelegate: delegate queue: queue];

        if (! [session canAddOutput: output])
        {
            error = "Couldn't read frames from " + name;
            session = nil;
            return false;
        }

        [session addOutput: output];
        [session commitConfiguration];

        // startRunning blocks for a moment while the camera powers up, so keep it off the caller's thread.
        AVCaptureSession* s = session;
        dispatch_async (queue, ^{ [s startRunning]; });
        return true;
    }

    void stop() override
    {
        if (session == nil)
            return;

        [output setSampleBufferDelegate: nil queue: nil];
        delegate.onFrame = nullptr;

        AVCaptureSession* s = session;
        // Run on the capture queue and wait, so no frame callback can still be in flight when we're destroyed.
        dispatch_sync (queue, ^{ [s stopRunning]; });

        session = nil;
        output = nil;
        delegate = nil;
        store.remove (id);
    }

private:
    void handleFrame (CVPixelBufferRef pixels)
    {
        if (CVPixelBufferGetPixelFormatType (pixels) != kCVPixelFormatType_32BGRA)
            return;

        CVPixelBufferLockBaseAddress (pixels, kCVPixelBufferLock_ReadOnly);

        const auto w = (int) CVPixelBufferGetWidth (pixels);
        const auto h = (int) CVPixelBufferGetHeight (pixels);
        const auto srcStride = CVPixelBufferGetBytesPerRow (pixels);
        const auto* src = static_cast<const uint8_t*> (CVPixelBufferGetBaseAddress (pixels));

        // Camera frames are opaque, so BGRA with alpha 255 is already valid premultiplied ARGB.
        juce::Image image (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());

        {
            juce::Image::BitmapData dst (image, juce::Image::BitmapData::writeOnly);

            for (int y = 0; y < h; ++y)
                std::memcpy (dst.getLinePointer (y), src + (size_t) y * srcStride, (size_t) w * 4);
        }

        CVPixelBufferUnlockBaseAddress (pixels, kCVPixelBufferLock_ReadOnly);

        store.push (id, std::move (image), juce::Time::getMillisecondCounterHiRes() / 1000.0);
    }

    AVCaptureDevice* device = nil;
    AVCaptureSession* session = nil;
    AVCaptureVideoDataOutput* output = nil;
    GriddyParkAngelFrameDelegate* delegate = nil;
    dispatch_queue_t queue;
    juce::String id, name;
};

std::unique_ptr<CaptureSource> createAVFoundationSource (const juce::String& uniqueId, FrameStore& store)
{
    if (AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID: [NSString stringWithUTF8String: uniqueId.toRawUTF8()]])
        return std::make_unique<AVFoundationSource> (store, device);

    return nullptr;
}

} // namespace griddy
