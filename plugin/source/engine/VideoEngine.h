#pragma once

#include "BeatClock.h"
#include "CollageLayout.h"
#include "Compositor.h"
#include "FrameStore.h"
#include "Recorder.h"
#include "TransportState.h"
#include "capture/CaptureSource.h"

#include <array>

namespace griddy
{

/** When a take starts and stops. Order matches the "recordMode" parameter. */
enum class RecordMode
{
    followDawRecord = 0, // record while armed and the DAW is recording (the usual case)
    followDawPlay,       // record while armed and the DAW is playing, even if it isn't recording audio
    manual               // record whenever armed
};

/** Live settings the engine reads every frame. They point at the plugin's
    parameter values, so automation and the UI change them without locks. */
struct EngineControls
{
    std::atomic<float>* layout = nullptr;
    std::atomic<float>* switchRate = nullptr;
    std::atomic<float>* numSlots = nullptr;
    std::atomic<float>* recordArm = nullptr;
    std::atomic<float>* recordMode = nullptr;
};

/** Everything video: owns the cameras, runs the 30 fps compositing loop,
    feeds the recorder and produces JPEG previews for the UI.

    Threads involved:
      audio thread    -> writes TransportState (never touches anything here directly)
      capture threads -> push frames into the FrameStore
      engine thread   -> this class's run(): composite, record, preview
      message thread  -> UI calls like setSlotSource() and getStatus()
*/
class VideoEngine final : private juce::Thread
{
public:
    static constexpr int maxSlots = 9;
    static constexpr int outputWidth = 1280;
    static constexpr int outputHeight = 720;
    static constexpr double fps = 30.0;
    static constexpr int previewWidth = 640;
    static constexpr int previewHeight = 360;

    VideoEngine (TransportState& transport, EngineControls controls);
    ~VideoEngine() override;

    void startEngine();
    void stopEngine();

    // ---- message thread API (called from the UI bridge) ----
    std::vector<CaptureDeviceInfo> getDevices() const { return listCaptureDevices(); }

    /** Puts a source in a slot. An empty id clears the slot. Returns an error message, or empty on success. */
    juce::String setSlotSource (int slot, const juce::String& sourceId);
    std::array<juce::String, maxSlots> getSlotSources() const;

    void setOutputFolder (const juce::File& folder);
    juce::File getOutputFolder() const;

    /** A JSON friendly status object for the UI. */
    juce::var getStatus() const;

    /** Latest preview JPEG and its serial number; serial 0 means none yet. */
    int64_t getPreviewSerial() const noexcept { return previewSerial.load(); }
    juce::MemoryBlock getPreviewJpeg() const;

private:
    void run() override;
    void renderOneFrame (double now);
    void updateRecording (bool shouldRecord, const TransportSnapshot& t);
    void encodePreview (const juce::Image& frame);
    void releaseUnusedSources();

    TransportState& transport;
    EngineControls controls;

    FrameStore frameStore;
    BeatClock beatClock;
    Compositor compositor;
    juce::Image output { juce::Image::ARGB, outputWidth, outputHeight, true, juce::SoftwareImageType() };

    // Slots and sources: changed on the message thread, read on the engine thread.
    mutable std::mutex slotLock;
    std::array<juce::String, maxSlots> slotSources;
    std::map<juce::String, std::unique_ptr<CaptureSource>> sources;

    // Recording state lives on the engine thread only, except the bits mirrored for status.
    std::unique_ptr<Recorder> recorder;
    int64_t takeFrameIndex = 0;
    juce::File outputFolder;
    mutable std::mutex statusLock;
    juce::String lastTakePath, lastError, recorderFormat;
    std::atomic<bool> recording { false };
    std::atomic<double> measuredFps { 0.0 };

    // Preview
    mutable std::mutex previewLock;
    juce::MemoryBlock previewJpeg;
    std::atomic<int64_t> previewSerial { 0 };
    juce::Image previewImage { juce::Image::ARGB, previewWidth, previewHeight, true, juce::SoftwareImageType() };
    juce::JPEGImageFormat previewFormat;
    int64_t framesRendered = 0;

    juce::String cameraBlockedReason;
    bool camerasAllowed = false;
};

} // namespace griddy
