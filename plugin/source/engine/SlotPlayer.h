#pragma once

#include "FrameQueue.h"
#include "FrameStore.h"
#include "SyncController.h"
#include "TakeHistory.h"
#include "VideoDecoder.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace griddy
{

/** What the renderer shows for one slot right now. Immutable once published. */
struct ShownFrame
{
    std::shared_ptr<const VideoFrame> frame; // null when there is nothing to show
    int32_t takeId = -1;
    int frameIndex = -1;
    double frameTakeSeconds = 0.0;  // take time of this picture, after latency and slip
    double targetSeconds = 0.0;     // take time the sync asked for when this was published
    double publishWall = 0.0;
    SyncStatus status = SyncStatus::noTake;
    SyncKind kind = SyncKind::none;
    bool playing = false;
    bool predictedWrap = false;
    bool warpSuspected = false;
};

struct SlotPlayerStats
{
    uint64_t decodes = 0;
    double decodeMsTotal = 0.0;
    double decodeMsMax = 0.0;
    uint64_t synchronousDecodes = 0;  // frames the player had to decode on demand, because the cache did not have them
    uint64_t publishes = 0;
    int openTakes = 0;
    int cachedFrames = 0;
};

/** One thread per slot: asks the SyncController where the clip is, keeps the right frames decoded
    ahead of that, and publishes the frame to show.

    The player never waits for the renderer and the renderer never waits for the player: the
    shown frame is swapped under a lock held for a pointer copy. If a frame is not ready the
    previous one simply stays up (hold the last frame, never stutter).
*/
class SlotPlayer final : private juce::Thread
{
public:
    struct Config
    {
        int aheadFrames = 15;       // half a second at 30 fps
        int pinnedFrames = 15;      // loop point pre roll
        bool useProxy = true;
        int maxDecodesPerTick = 3;
        int maxOpenTakes = 2;       // the shown take and one other; the rest are closed to keep memory flat
        int tickMs = 2;
    };

    SlotPlayer (SyncController& controller, Config config);
    ~SlotPlayer() override;

    /** Message thread. The takes this slot can show, in the order the controller knows them. */
    void setTakes (std::vector<TakeHistory::Entry> entries);

    void start();
    void stop();

    std::shared_ptr<const ShownFrame> shown() const;
    SlotPlayerStats stats() const;

private:
    struct Playback
    {
        TakeHistory::Entry entry;
        std::unique_ptr<VideoDecoder> decoder;
        std::unique_ptr<FrameQueue> cache;
        int pinnedDone = 0;
        double lastUsed = 0.0;
    };

    void run() override;
    Playback* playbackFor (int32_t takeId, double now);
    static int frameIndexFor (const TakeMeta& meta, double videoSeconds);
    juce::Image decodeInto (Playback& p, int index, bool synchronous);

    SyncController& controller;
    Config config;

    std::mutex takesLock;
    std::vector<TakeHistory::Entry> takes;
    std::atomic<uint32_t> takesVersion { 0 };
    uint32_t seenVersion = 0;
    std::vector<TakeHistory::Entry> localTakes;

    std::map<int32_t, std::unique_ptr<Playback>> open;

    mutable std::mutex shownLock;
    std::shared_ptr<const ShownFrame> current;

    mutable std::mutex statsLock;
    SlotPlayerStats st;
    uint64_t serial = 0;
    int lastIndex = -1;
    int32_t lastTake = -1;
    SyncStatus lastStatus = SyncStatus::noTake;
};

} // namespace griddy
