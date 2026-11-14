#pragma once

#include "engine/FrameStore.h"

namespace griddy
{

/** Something that produces video frames: a camera, a capture card, a test pattern.

    A source pushes each new frame into the shared FrameStore under its id,
    from whatever thread its platform API calls back on. The engine never
    waits on a source; it just takes the latest frame when it composites.
*/
class CaptureSource
{
public:
    explicit CaptureSource (FrameStore& s) : store (s) {}
    virtual ~CaptureSource() = default;

    virtual juce::String getId() const = 0;
    virtual juce::String getName() const = 0;

    /** Returns false and fills error if the device couldn't start. */
    virtual bool start (juce::String& error) = 0;
    virtual void stop() = 0;

protected:
    FrameStore& store;
};

/** A source the UI can offer in a slot's dropdown. */
struct CaptureDeviceInfo
{
    juce::String id;    // stable across launches where the platform allows (AVFoundation uniqueID)
    juce::String name;  // what the user sees
    juce::String kind;  // "camera", "test"
};

/** Lists every source available right now: real cameras and capture cards. Test patterns come first, but only
    when testPatternsEnabled(). */
std::vector<CaptureDeviceInfo> listCaptureDevices();

/** Test patterns are for the harness, the smoke test and CI only: set GRIDDY_TEST_PATTERNS to get them. */
bool testPatternsEnabled();

/** The camera a new slot starts with: the FaceTime HD camera, else the first camera, else none (empty). */
juce::String defaultCameraId();

/** Creates a source for an id from listCaptureDevices(), or nullptr if it's gone. */
std::unique_ptr<CaptureSource> createCaptureSource (const juce::String& id, FrameStore& store);

/** Whether this process (the DAW, when we're a plugin) is allowed to open cameras.
    On macOS, opening a camera in a host that lacks camera permission makes the
    OS kill the whole DAW, so we must check before touching AVFoundation.
    reason explains a "no" in words the UI can show. */
bool hostAllowsCameraAccess (juce::String& reason);

} // namespace griddy
