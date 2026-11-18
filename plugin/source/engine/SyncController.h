#pragma once

#include "ClipSource.h"
#include "HostClock.h"
#include "SeqLock.h"
#include "SpscRing.h"
#include "sync/HostPositionClipSource.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
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
    uint8_t gap = 0;      // no clip plays here: the picture goes black (status is held)
    int16_t pass = -1;    // which recorded pass of the take, when the position matches one
    uint32_t serial = 0;
};

/** How a slot's take changed, for griddy.log. */
struct TakeChange
{
    enum Path : uint8_t { aligned, fingerprint, embedded, prior, silence, gapStart, gapEnd, timeline };
    int32_t oldTake = -1, newTake = -1;
    int16_t oldPass = -1, newPass = -1;
    double hostSeconds = 0.0;
    Path path = aligned;
    static const char* pathName (Path p);
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

    /** Where the take sat on the host timeline: one range per recorded pass. */
    struct Range { double hostStart = 0.0, hostEnd = 0.0, takeAtHostStart = 0.0; };
    std::vector<Range> ranges;

    /** False when the take recorded no sound (Live's Monitor was off): it can never match, so it never goes black
        for not matching. */
    bool hasSound = true;

    /** When recording began, seconds since the epoch: tells apart takes that started at the same host time. */
    double createdAt = 0.0;

    /** A film take: filmed later, while Live played a recording that was already there. Empty for a take recorded with
        its sound. It is that file's video wherever the file plays, found by path, not by where the take started. */
    std::string filmPath;
    double filmBorn = 0.0;   // the file's creation time, to find it again when its path changed
    double filmLead = 0.0;   // seconds into the file where the take's second 0 is
    bool filmCurrent = false; // kept or in review: chosen over the file's other film takes
};

/** One clip on a Griddy track in Live's arrangement, as the Live script reports it. Host seconds throughout. */
struct TimelineClip
{
    double hostStart = 0.0, hostEnd = 0.0;
    bool active = true;     // plays sound: not deactivated, gain above zero
    bool lane = false;      // on a take lane, not the main lane: never plays, only helps tell which take a file is
    int file = -1;          // which recorded file, numbered per track; -1 for a clip with no file
    bool originKnown = false;
    double origin = 0.0;    // host time where the file's first sample would sit; unknown for a looped clip
    double fileBorn = 0.0;  // when the file was created, seconds since the epoch; 0 when unknown
    bool selected = false;  // the clip clicked in Live: Griddy's Film films for its file
    int bar = 0;            // the bar the clip starts in, 1 based, when selected
};

/** Every clip on one slot's track, with what the whole track does. */
struct SlotTimeline
{
    std::vector<TimelineClip> clips;
    std::vector<std::string> files; // each file number's path, from a script that says; empty from an older one
    bool audible = true;     // the track is not muted, and not silenced by another track's solo
    bool arrangement = true; // the track plays the arrangement, not a Session clip
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

    /** Live's arrangement for this slot, or null when the Live script is not running. While set, it decides where the
        picture is black and, for a clip whose file is known to be a take, which take shows and where, to the frame. */
    void setTimeline (std::shared_ptr<const SlotTimeline> t);

    /** Which take a recorded file is. Worked out from where the file's clips sit and when it was created. */
    struct FileBinding
    {
        int file = -1; int32_t takeId = -1; double lead = 0.0; bool leadKnown = false;
        bool film = false; // takeId is a film take, bound by the file's path
        // For a film take: the take bound by time, shown where the film does not cover the file; -1 for none (black).
        int32_t underTakeId = -1; double underLead = 0.0; bool underLeadKnown = false;
    };
    static std::vector<FileBinding> bindFiles (const SlotTimeline& t, const std::vector<TakeProfile>& takes);

    /** The bindings last worked out, for showing which recordings have a video. Any thread. */
    std::vector<FileBinding> currentBindings() const;

    /** Computes the target for this moment. wallNow is steadyNowSeconds(). */
    SlotTarget update (double wallNow);

    /** Comp detection and black gaps (on unless GRIDDY_NO_COMP). */
    void setCompEnabled (bool on) noexcept { compOn = on; }

    /** Black gaps and silent piece detection need the slot's own track audio: off for a slot no Griddy owns,
        and for a slot that is armed or recording. */
    void setGapsAllowed (bool on) noexcept { gapsAllowed = on; }

    /** Takes the oldest take change not yet logged. Any one thread. */
    bool popChange (TakeChange& c) { return changes.read (&c, 1) == 1; }

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
    static int passOf (const TakeProfile& t, double offset);
    void note (TakeChange::Path path, int32_t oldTake, int16_t oldPass, int32_t newTake, int16_t newPass, double hostNow);
    const TakeProfile* findTake (int32_t id, bool isTag) const;
    double envelopeAt (const TakeProfile& t, double seconds) const;

    const HostClock& host;
    ClipSource* embedded;
    ClipSource* fingerprint;
    HostPositionClipSource* prior;

    mutable std::mutex takeLock;
    std::vector<TakeProfile> takes;
    bool takesChanged = false; // guarded by takeLock; update() starts over from the new takes
    std::shared_ptr<const SlotTimeline> timeline; // guarded by takeLock
    bool timelineChanged = false;                 // guarded by takeLock
    std::shared_ptr<const SlotTimeline> localTimeline;
    std::vector<FileBinding> bindings;
    mutable std::mutex bindingsLock;
    std::vector<FileBinding> publishedBindings;

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

    std::atomic<bool> compOn { true }, gapsAllowed { true };
    uint32_t lastAlignedSerial = 0;
    bool alignedActive = false;
    int32_t alignedTake = -1;
    double alignedOffset = 0.0;
    double digitalSilentSince = -1.0, signalSince = -1.0;
    bool inGap = false;
    uint32_t lastMatchSerial = 0;
    bool matchedNow = false;      // the last check that heard something found a take playing where it was recorded
    double mismatchSince = -1.0;  // wall time sound began matching no take
    double movedMatchAt = -1.0;   // wall time of the last fingerprint lock on a take away from where it was recorded
    int32_t loggedTake = -1;
    int16_t loggedPass = -1;
    SpscRing<TakeChange> changes { 256 };
};

} // namespace griddy
