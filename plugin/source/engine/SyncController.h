#pragma once

#include "ClipSource.h"
#include "HostClock.h"
#include "SeqLock.h"
#include "sync/HostPositionClipSource.h"

#include <memory>
#include <mutex>
#include <vector>

namespace griddy
{

enum class SyncStatus : uint8_t
{
    noTake = 0,   // the slot has no take
    stopped,      // the DAW is stopped; the picture holds
    prior,        // placed by the host position only; nothing confirmed it yet
    locked,       // placed by a source that heard the audio
    held,         // the take ended or its audio stopped; last frame is held
    lost          // audio is flowing but matches nothing: warped, effected, bounced or another sound
};

/** What the player needs to know: which take, how far into it, and how sure we are. */
struct SlotTarget
{
    int32_t takeId = -1;
    double takeSeconds = 0.0;
    SyncKind kind = SyncKind::none;
    SyncStatus status = SyncStatus::noTake;
    uint8_t playing = 0;
    uint8_t warpSuspected = 0;
    uint8_t predictedWrap = 0;
    uint32_t serial = 0;
};

/** What the controller knows about a take. */
struct TakeProfile
{
    int32_t takeId = -1;
    double hostStartSeconds = 0.0;
    double durationSeconds = 0.0;
    double recordedBpm = 120.0;
    /** Reference loudness every envelopeStep seconds (RMS), for telling "the clip ended" from "a quiet passage". */
    std::shared_ptr<const std::vector<float>> envelope;
    double envelopeStep = 0.1;
};

/** Per slot: combines the three sources into one steady answer.

    Ranking, best first: embedded sync, fingerprint, host position. A fix keeps placing the
    clip until a better or newer one replaces it, so the picture rides through silence. A new
    fix that disagrees with the current placement has to repeat before it is believed, so a single
    false match cannot throw the picture around. The video clock is the host clock plus the
    placement, which is why stops, loops and seeks need no special handling.

    Runs on the sync worker (or the render thread); never on the audio thread.
*/
class SyncController
{
public:
    SyncController (const HostClock& host, ClipSource* embedded, ClipSource* fingerprint, HostPositionClipSource* prior);

    void setTakes (std::vector<TakeProfile> takes);

    /** Computes the target for this moment. wallNow is steadyNowSeconds(). */
    SlotTarget update (double wallNow);

    /** Seconds between the first audio that could be matched and the first locked fix (for tests and status). */
    double lastLockLatencySeconds() const noexcept { return lockLatency; }
    int numJumps() const noexcept { return jumps; }

private:
    struct Placement
    {
        bool valid = false;
        int32_t takeId = -1;
        double offset = 0.0; // takeSeconds - hostSeconds
        SyncKind kind = SyncKind::none;
    };

    bool accept (const ClipFix& f, const TakeProfile* take, double hostNow);
    const TakeProfile* findTake (int32_t id, bool isTag) const;
    double envelopeAt (const TakeProfile& t, double seconds) const;

    const HostClock& host;
    ClipSource* embedded;
    ClipSource* fingerprint;
    HostPositionClipSource* prior;

    mutable std::mutex takeLock;
    std::vector<TakeProfile> takes;

    Placement placement;
    uint32_t lastEmbeddedSerial = 0, lastFingerprintSerial = 0;
    Placement pending;
    int pendingCount = 0;

    double silentSince = -1.0;
    bool wasPlaying = false;
    double firstAudioWall = -1.0;
    double lockLatency = -1.0;
    int jumps = 0;
    uint32_t serial = 0;
    bool lost = false;
    double lostSince = -1.0;
    double lastBlockWall = -1.0, smoothOffset = 0.0;
    bool offsetValid = false;
};

} // namespace griddy
