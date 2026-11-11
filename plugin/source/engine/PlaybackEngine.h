#pragma once

#include "Compositor.h"
#include "GpuCompositor.h"
#include "SlotPlayer.h"
#include "SyncController.h"
#include "TakeHistory.h"
#include "TakeRecorder.h"
#include "sync/EmbeddedSync.h"
#include "sync/Fingerprint.h"
#include "sync/HostPositionClipSource.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace griddy
{

/** Output sizes. The grid layouts work on any of them; the single frame template is the cheapest. */
enum class Canvas
{
    hd720,     // 1280 x 720, the minimum
    hd1080,    // 1920 x 1080, the target
    square,    // 1080 x 1080 (1:1)
    portrait   // 1080 x 1920 (9:16)
};

inline juce::Point<int> canvasSize (Canvas c)
{
    switch (c)
    {
        case Canvas::hd720:    return { 1280, 720 };
        case Canvas::hd1080:   return { 1920, 1080 };
        case Canvas::square:   return { 1080, 1080 };
        case Canvas::portrait: return { 1080, 1920 };
    }
    return { 1280, 720 };
}

struct SlotDriftStats
{
    uint64_t samples = 0;
    double meanMs = 0.0, p50Ms = 0.0, p99Ms = 0.0, maxMs = 0.0;
    uint64_t skippedFrames = 0; // renders where the picture advanced by a different number of frames than the clock did
};

struct RenderStats
{
    uint64_t frames = 0;
    uint64_t lateFrames = 0;      // renders that started more than a frame period after they were due
    double renderMsMean = 0.0, renderMsMax = 0.0;
};

/** The whole recording and playback engine as a library: N slots, each fed its own audio stream,
    one composited canvas out.

    Threads (see docs/engine-design.md section 6):
      audio thread   feedSlotAudio() and TakeRecorder::feedAudio(), lock free, no allocation
      sync worker    one thread running every slot's fingerprint and embedded sync analysis
      slot players   one thread per slot: sync target, decode ahead, publish the frame to show
      render thread  30 fps: composite the shown frames into the canvas
      control        reloadSlot(), history edits
*/
class PlaybackEngine
{
public:
    struct Config
    {
        int numSlots = 4;
        double sampleRate = 48000.0;
        Canvas canvas = Canvas::hd720;
        juce::File takesRoot;
        SlotPlayer::Config player;
        bool runRenderThread = true;
    };

    explicit PlaybackEngine (Config config);
    ~PlaybackEngine();

    /** Message thread. Tries to switch compositing to OpenGL; returns false and keeps the CPU path if no GL context can be made. */
    bool enableGpuCompositor();
    bool usingGpu() const noexcept { return gpu.available(); }

    void start();
    void stop();

    TakeHistory& history() noexcept { return takeHistory; }
    int numSlots() const noexcept { return config.numSlots; }

    /** Audio thread. mono is that slot's audio, hostSample the host time of mono[0]. */
    void feedSlotAudio (int slot, const float* mono, int numSamples, int64_t hostSample, bool playing) noexcept;

    /** Rebuilds a slot from its takes on disk: fingerprint indexes, profiles, the player's takes.
        Slow (it reads and analyses the audio), so call it from a worker, not the message thread. */
    void reloadSlot (int slot);

    /** Renders one composite now and returns it. The render thread calls this at 30 fps. */
    juce::Image renderOnce();

    /** The latest composite, as a copy-on-write image handle. */
    juce::Image latestOutput() const;

    std::shared_ptr<const ShownFrame> shown (int slot) const { return slots[(size_t) slot]->player.shown(); }
    SlotPlayerStats playerStats (int slot) const { return slots[(size_t) slot]->player.stats(); }
    SlotDriftStats driftStats (int slot) const;
    RenderStats renderStats() const;
    double lockLatencySeconds (int slot) const { return slots[(size_t) slot]->controller.lastLockLatencySeconds(); }

    /** True when fingerprint matching reports audio flowing that matches nothing. */
    bool slotLooksWarped (int slot) const { return slots[(size_t) slot]->fingerprint.noMatchWhileActive(); }

    SyncController& controller (int slot) { return slots[(size_t) slot]->controller; }

private:
    struct Slot
    {
        Slot (double sr, const SlotPlayer::Config& pc);

        HostClock clock;
        embedded::EmbeddedSyncClipSource embedded;
        fingerprint::FingerprintClipSource fingerprint;
        HostPositionClipSource prior;
        SyncController controller;
        SlotPlayer player;

        std::map<int32_t, std::shared_ptr<const fingerprint::Index>> indexCache;
        // Drift as a histogram (0.25 ms bins up to 500 ms), so a long run uses constant memory.
        std::vector<uint32_t> driftBins = std::vector<uint32_t> (2001, 0);
        uint64_t driftCount = 0;
        double driftSum = 0.0, driftMax = 0.0;
        int lastShownIndex = -1;
        int32_t lastShownTake = -1;
        double lastShownTarget = 0.0;
        uint64_t skipped = 0;
    };

    void syncLoop();
    void renderLoop();

    Config config;
    TakeHistory takeHistory;
    std::vector<std::unique_ptr<Slot>> slots;
    Compositor compositor;
    GpuCompositor gpu;

    std::atomic<bool> running { false };
    std::unique_ptr<juce::Thread> syncThread, renderThread;

    mutable std::mutex outputLock;
    juce::Image output;

    mutable std::mutex statsLock;
    RenderStats render;
    double renderMsSum = 0.0;
};

} // namespace griddy
