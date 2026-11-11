#pragma once

#include "PlaybackEngine.h"
#include "TransportState.h"
#include "VideoEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace griddy
{

/** Connects the plugin to the recording and playback engine.

    One Griddy instance has one audio input, so it can hear exactly one slot: the target slot.
    That slot is recorded and sync-analysed from the real audio. The other slots are placed by
    the host position alone (the "prior" sync source) until a hub lets other instances feed them.

    Threads:
      audio thread    processBlock(): mixes to mono, feeds the engine and the recorder, decides when to record.
                      No allocation, no locks, no file access.
      control thread  starts and stops takes, passes camera frames to the recorder, draws the preview.
      worker          stops a take (writes files) and rebuilds a slot's sync data after a take.
*/
class EngineHost final : private juce::Thread
{
public:
    static constexpr int maxSlots = VideoEngine::maxSlots;

    EngineHost (VideoEngine& video, EngineControls controls);
    ~EngineHost() override;

    /** Message thread, before audio runs (or between runs). */
    void prepare (double sampleRate, int maxBlockSize);

    /** Which project's takes this instance plays. Rebuilds the engine if it changes. */
    void setSession (const juce::String& sessionId);
    juce::String getSession() const;

    /** Audio thread. */
    void processBlock (const juce::AudioBuffer<float>& buffer, const TransportSnapshot& transport) noexcept;

    // ---- message thread ----
    void setTargetSlot (int slot) noexcept { target = juce::jlimit (0, maxSlots - 1, slot); }
    int getTargetSlot() const noexcept { return target.load(); }

    bool keepTake (int slot, int takeId);
    bool retakeTake (int slot, int takeId);
    bool restoreTake (int slot, int takeId);
    bool setSlip (int slot, int takeId, double seconds);

    juce::var getStatus() const;
    juce::MemoryBlock getPreviewJpeg() const;
    int64_t getPreviewSerial() const noexcept { return previewSerial.load(); }

    /** Test hook: runs the control loop work once, on the calling thread. */
    bool isRecording() const noexcept;

private:
    void run() override;
    void controlTick();
    void buildEngine();
    void destroyEngine();
    void drawPreview();
    void finishTake (int slot);
    void reloadLater (int slot);
    juce::var takesToVar (int slot) const;

    VideoEngine& video;
    EngineControls controls;

    mutable std::mutex engineLock;            // guards the engine pointer for non audio threads
    std::unique_ptr<PlaybackEngine> engine;
    std::unique_ptr<TakeRecorder> recorder;
    std::atomic<PlaybackEngine*> audioEngine { nullptr };
    std::atomic<TakeRecorder*> audioRecorder { nullptr };

    double sampleRate = 48000.0;
    int maxBlock = 1024;
    juce::String sessionId;

    std::vector<float> mono, silence;

    std::atomic<int> target { 0 };
    std::atomic<int> activeSlots { 4 };

    // Audio thread -> control thread.
    std::atomic<bool> wantRecording { false };
    std::atomic<int64_t> requestSample { 0 };
    std::atomic<double> requestPpq { 0.0 }, requestBpm { 120.0 };
    std::atomic<int> requestNum { 4 }, requestDen { 4 };
    std::atomic<bool> workerBusy { false };
    std::atomic<int> recordingSlot { -1 };
    std::atomic<bool> playingNow { false };

    uint64_t lastCameraSerial = 0;
    uint64_t droppedFrames = 0, droppedAudio = 0;

    juce::ThreadPool worker { 1 };

    Compositor compositor;
    juce::Image output { juce::Image::ARGB, 1280, 720, true, juce::SoftwareImageType() };
    juce::Image previewImage { juce::Image::ARGB, VideoEngine::previewWidth, VideoEngine::previewHeight, true, juce::SoftwareImageType() };
    juce::JPEGImageFormat previewFormat;
    mutable std::mutex previewLock;
    juce::MemoryBlock previewJpeg;
    std::atomic<int64_t> previewSerial { 0 };
    int tick = 0;

    mutable std::mutex statusLock;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineHost)
};

} // namespace griddy
