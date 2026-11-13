#include "SyncController.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace griddy
{

static constexpr double jumpTolerance = 0.030;   // seconds: smaller differences are just refined
static constexpr float silenceLevel = 0.002f;    // peak below this counts as silence
static constexpr double silenceBeforeHold = 0.25; // seconds of silence, where the take is not silent, before holding
static constexpr float digitalSilence = 1.0e-5f;  // about -100 dBFS: nothing plays on the track at all
static constexpr double gapAfter = 0.150;          // seconds of digital silence before a gap goes black
static constexpr double gapLeave = 0.025;          // seconds of sound that end a gap
static constexpr double loudReference = 4.0 * silenceLevel; // a take's own audio is clearly not silent above this

const char* TakeChange::pathName (Path p)
{
    switch (p)
    {
        case aligned:     return "comp detection";
        case fingerprint: return "fingerprint";
        case embedded:    return "sync signal";
        case prior:       return "host position";
        case silence:     return "silent piece";
        case gapStart:    return "gap starts";
        case gapEnd:      return "gap ends";
    }
    return "";
}

int SyncController::passOf (const TakeProfile& t, double offset)
{
    for (size_t i = 0; i < t.ranges.size(); ++i)
        if (std::abs ((t.ranges[i].takeAtHostStart - t.ranges[i].hostStart) - offset) < 0.05)
            return (int) i;
    return -1;
}

void SyncController::note (TakeChange::Path path, int32_t oldTake, int16_t oldPass, int32_t newTake, int16_t newPass, double hostNow)
{
    TakeChange c;
    c.path = path;
    c.oldTake = oldTake;
    c.oldPass = oldPass;
    c.newTake = newTake;
    c.newPass = newPass;
    c.hostSeconds = hostNow;
    changes.write (&c, 1);
}

SyncController::SyncController (const HostClock& h, ClipSource* e, ClipSource* f, HostPositionClipSource* p)
    : host (h), embedded (e), fingerprint (f), prior (p)
{
}

void SyncController::setTakes (std::vector<TakeProfile> t)
{
    // Called from a worker while the player thread runs update(). The placement belongs to update(), so it is
    // reset there, never here: resetting it here raced with update() writing a placement for the old takes.
    const std::scoped_lock sl (takeLock);
    takes = std::move (t);
    takesChanged = true;
}

const TakeProfile* SyncController::findTake (int32_t id, bool isTag) const
{
    for (const auto& t : takes)
        if (isTag ? (t.takeId & 0xff) == id : t.takeId == id)
            return &t;

    return nullptr;
}

double SyncController::envelopeAt (const TakeProfile& t, double seconds) const
{
    if (t.envelope == nullptr || t.envelope->empty() || seconds < 0.0)
        return 0.0;

    const auto i = (size_t) (seconds / t.envelopeStep);
    return i < t.envelope->size() ? (double) (*t.envelope)[i] : 0.0;
}

bool SyncController::accept (const ClipFix& f, const TakeProfile* take, double hostNow)
{
    if (take == nullptr || ! f.locked)
        return false;

    // Fixes are stamped with the host time they were true at; carry the placement as an offset.
    Placement p;
    p.valid = true;
    p.takeId = take->takeId;
    p.offset = f.takeSeconds - f.hostSeconds;
    p.kind = f.kind;

    if (! placement.valid)
    {
        placement = p;
        return true;
    }

    const auto sameTake = placement.takeId == p.takeId;
    const auto close = sameTake && std::abs (placement.offset - p.offset) < jumpTolerance;

    // A better source always gets to replace a weaker one when it is consistent, and immediately
    // when the weaker one was only the host position prior.
    if (close || placement.kind == SyncKind::hostPosition)
    {
        placement = p;
        pendingCount = 0;
        return true;
    }

    // A disagreeing fix has to repeat before it is believed.
    const auto samePending = pending.valid && pending.takeId == p.takeId && std::abs (pending.offset - p.offset) < jumpTolerance;
    pending = p;
    pendingCount = samePending ? pendingCount + 1 : 1;

    // Embedded frames are CRC protected, so one is enough; the others need to repeat.
    if (f.kind == SyncKind::embedded || pendingCount >= 2)
    {
        placement = p;
        pendingCount = 0;
        ++jumps;
        (void) hostNow;
        return true;
    }

    return false;
}

SlotTarget SyncController::update (double wallNow)
{
    SlotTarget out;
    const auto hs = host.read();
    out.playing = hs.playing;

    // Host time now. Audio blocks arrive in bursts, so extrapolating from the newest block alone
    // jitters by a block length. Instead, track the offset between host time and the wall clock
    // with a slow filter and jump straight to a new value only after a seek or a start.
    double hostNow = (double) hs.hostSample / hs.sampleRate;
    if (hs.playing)
    {
        if (hs.wallSeconds != lastBlockWall)
        {
            lastBlockWall = hs.wallSeconds;
            const auto sample = (double) hs.hostSample / hs.sampleRate - hs.wallSeconds;

            if (! offsetValid || std::abs (sample - smoothOffset) > 0.080)
                smoothOffset = sample;
            else
                smoothOffset += 0.02 * (sample - smoothOffset);

            offsetValid = true;
        }

        hostNow = std::min (wallNow + smoothOffset, (double) hs.hostSample / hs.sampleRate + 0.25);
    }
    else
    {
        offsetValid = false;
    }

    std::vector<TakeProfile> localTakes;
    bool fresh = false;
    {
        const std::scoped_lock sl (takeLock);
        localTakes = takes;
        fresh = std::exchange (takesChanged, false);
    }

    if (fresh)
    {
        placement = {};
        pending = {};
        pendingCount = 0;
        lost = false;
        alignedActive = false;
        alignedTake = -1;
    }

    const auto comp = compOn.load (std::memory_order_relaxed);
    auto changePath = TakeChange::fingerprint;

    if (localTakes.empty())
    {
        out.status = SyncStatus::noTake;
        return out;
    }

    auto lookup = [&] (int32_t id, bool isTag) -> const TakeProfile*
    {
        for (const auto& t : localTakes)
            if (isTag ? (t.takeId & 0xff) == id : t.takeId == id)
                return &t;
        return nullptr;
    };

    // Track the first moment audio is flowing, to report lock latency.
    if (hs.playing && hs.level > silenceLevel && firstAudioWall < 0.0)
        firstAudioWall = wallNow;
    if (! hs.playing)
        firstAudioWall = -1.0;

    // Take in new fixes, best source first.
    if (embedded != nullptr)
    {
        const auto f = embedded->latest();
        if (f.serial != lastEmbeddedSerial)
        {
            lastEmbeddedSerial = f.serial;
            const auto before = placement.takeId;
            if (accept (f, lookup (f.takeId, true), hostNow) && lockLatency < 0.0 && firstAudioWall >= 0.0)
                lockLatency = wallNow - firstAudioWall;
            if (placement.takeId != before)
                changePath = TakeChange::embedded;
        }
    }

    // Comp detection: of several takes recorded over this stretch, which one plays. It already waited out its own
    // hysteresis, so a change is taken at once, as a hard cut.
    if (comp && fingerprint != nullptr)
    {
        const auto a = fingerprint->aligned();
        if (a.serial != lastAlignedSerial)
        {
            lastAlignedSerial = a.serial;
            alignedActive = a.locked && lookup (a.takeId, false) != nullptr;

            if (alignedActive)
            {
                alignedTake = a.takeId;
                alignedOffset = a.takeSeconds - a.hostSeconds;

                if (! (placement.valid && placement.kind == SyncKind::embedded))
                {
                    const auto same = placement.valid && placement.takeId == a.takeId && std::abs (placement.offset - alignedOffset) < jumpTolerance;
                    if (! same || placement.kind == SyncKind::hostPosition)
                    {
                        placement.valid = true;
                        placement.takeId = a.takeId;
                        placement.offset = alignedOffset;
                        placement.kind = SyncKind::fingerprint;
                        pending = {};
                        pendingCount = 0;
                        changePath = TakeChange::aligned;
                    }
                }
            }
        }
    }

    if (fingerprint != nullptr)
    {
        const auto f = fingerprint->latest();
        if (f.serial != lastFingerprintSerial)
        {
            lastFingerprintSerial = f.serial;

            // While comp detection knows which take plays here, the fingerprint may only refine that take's position:
            // its votes mix every take that sounds alike, so on its own it could flip between them.
            const auto agreesWithComp = ! alignedActive
                                     || (f.takeId == alignedTake && std::abs ((f.takeSeconds - f.hostSeconds) - alignedOffset) < jumpTolerance);

            // Never let the weaker source overwrite a recent embedded placement.
            if (! (placement.valid && placement.kind == SyncKind::embedded) && agreesWithComp)
                if (accept (f, lookup (f.takeId, false), hostNow) && lockLatency < 0.0 && firstAudioWall >= 0.0)
                    lockLatency = wallNow - firstAudioWall;

            if (! f.locked && hs.playing)
            {
                if (! lost)
                {
                    lost = true;
                    lostSince = wallNow;
                }
            }
            else if (f.locked)
            {
                lost = false;
            }
        }
    }

    // Host position prior: used only while nothing has been heard yet.
    const TakeProfile* active = &localTakes.front();
    if (! placement.valid && prior != nullptr)
    {
        // The prior's takes are set a moment before ours, so only use a fix for a take we know.
        const auto f = prior->latest();
        if (f.locked && lookup (f.takeId, false) != nullptr)
        {
            Placement p;
            p.valid = true;
            p.takeId = f.takeId;
            p.offset = f.takeSeconds - f.hostSeconds;
            p.kind = SyncKind::hostPosition;
            placement = p;
            changePath = TakeChange::prior;
        }
    }

    // Digital silence on the track, timed for black gaps and silent pieces.
    if (hs.playing && hs.level < digitalSilence)
    {
        if (digitalSilentSince < 0.0)
            digitalSilentSince = wallNow;
        signalSince = -1.0;
    }
    else
    {
        digitalSilentSince = -1.0;
        if (signalSince < 0.0)
            signalSince = wallNow;
    }

    const auto gapsOn = comp && hs.playing && gapsAllowed.load (std::memory_order_relaxed);
    const auto longSilence = gapsOn && digitalSilentSince >= 0.0 && wallNow - digitalSilentSince >= gapAfter;

    // A gap ends once sound has lasted gapLeave, which is also long enough for comp detection to say whose sound it is.
    const auto stayInGap = inGap && gapsOn && signalSince >= 0.0 && wallNow - signalSince < gapLeave;

    auto covering = [&] (const TakeProfile& t, double& takeSecondsHere) -> bool
    {
        for (const auto& r : t.ranges)
            if (hostNow >= r.hostStart && hostNow < r.hostEnd)
            {
                takeSecondsHere = r.takeAtHostStart + (hostNow - r.hostStart);
                return true;
            }
        return false;
    };

    // A piece that is silent on the track where one take's own audio was silent too: that take is playing (a singer
    // resting in one take of a comp). Switch to it rather than going black. When the current take is silent here too,
    // or two takes are, the current choice stays.
    if (longSilence && ! inGap)
    {
        const TakeProfile* currentTake = placement.valid ? lookup (placement.takeId, false) : nullptr;
        const auto currentQuiet = currentTake != nullptr && envelopeAt (*currentTake, hostNow + placement.offset) <= silenceLevel
                               && hostNow + placement.offset < currentTake->durationSeconds;

        if (! currentQuiet)
        {
            for (const auto& t : localTakes)
            {
                double ts = 0.0;
                if (covering (t, ts) && envelopeAt (t, ts) <= silenceLevel && ts >= 0.0 && ts < t.durationSeconds
                    && ! (placement.valid && placement.kind == SyncKind::embedded))
                {
                    placement.valid = true;
                    placement.takeId = t.takeId;
                    placement.offset = ts - hostNow;
                    placement.kind = placement.kind == SyncKind::none ? SyncKind::hostPosition : placement.kind;
                    changePath = TakeChange::silence;
                    break; // the take in review or kept comes first, then the newest archived one
                }
            }
        }
    }

    bool anyCovers = false;
    for (const auto& t : localTakes)
    {
        double ts = 0.0;
        anyCovers = anyCovers || covering (t, ts);
    }

    if (! placement.valid)
    {
        out.takeId = active->takeId;
        out.status = hs.playing ? SyncStatus::held : SyncStatus::stopped;
        out.kind = SyncKind::none;

        // Nothing placed yet and nothing was recorded here: no clip plays.
        const auto gapNow = (longSilence && ! anyCovers) || stayInGap;
        if (gapNow != inGap)
        {
            inGap = gapNow;
            note (gapNow ? TakeChange::gapStart : TakeChange::gapEnd, loggedTake, loggedPass, -1, -1, hostNow);
            loggedTake = -1;
            loggedPass = -1;
        }
        out.gap = inGap ? 1 : 0;

        out.serial = ++serial;
        return out;
    }

    const auto* take = lookup (placement.takeId, false);
    if (take == nullptr)
        take = active;

    auto pos = hostNow + placement.offset;
    out.takeId = take->takeId;
    out.kind = placement.kind;

    // Past the end of the take: if sound keeps arriving, assume the clip repeats (a duplicate or a
    // loop of the clip) and wrap at once, so a loop point never shows a stale frame. Otherwise hold.
    const auto D = take->durationSeconds;
    if (D > 0.0 && pos >= D)
    {
        if (hs.playing && hs.level > silenceLevel)
        {
            pos = std::fmod (pos, D);
            out.predictedWrap = 1;
        }
        else
        {
            pos = D;
            out.status = SyncStatus::held;
        }
    }

    // Audio has stopped while the take's own audio has not: the clip was trimmed or split here.
    if (hs.playing && hs.level <= silenceLevel && envelopeAt (*take, pos) > 4.0 * silenceLevel)
    {
        if (silentSince < 0.0)
            silentSince = wallNow;
        if (wallNow - silentSince > silenceBeforeHold)
            out.status = SyncStatus::held;
    }
    else
    {
        silentSince = -1.0;
    }

    if (pos < 0.0)
    {
        pos = 0.0;
        out.status = SyncStatus::held;
    }

    // Black gaps. Digital silence where no take was recorded, or where the take this slot heard has sound of its own,
    // means no clip plays: the picture goes black instead of holding a frame. Never for a take whose own audio is
    // silent there, and never on a placement only the host position made (Griddy recorded with no audio in Live).
    {
        const auto unclamped = hostNow + placement.offset;
        const auto refQuiet = unclamped >= 0.0 && unclamped < take->durationSeconds && envelopeAt (*take, unclamped) <= silenceLevel;

        const auto heard = placement.kind == SyncKind::fingerprint || placement.kind == SyncKind::embedded;
        const auto refLoud = heard && unclamped >= 0.0 && unclamped < take->durationSeconds && envelopeAt (*take, unclamped) > loudReference;
        const auto gapNow = (longSilence && ! refQuiet && (! anyCovers || refLoud)) || stayInGap;

        if (gapNow != inGap)
        {
            inGap = gapNow;
            note (gapNow ? TakeChange::gapStart : TakeChange::gapEnd, loggedTake, loggedPass, gapNow ? -1 : take->takeId,
                  gapNow ? (int16_t) -1 : (int16_t) passOf (*take, placement.offset), hostNow);
            if (! gapNow)
            {
                loggedTake = take->takeId;
                loggedPass = (int16_t) passOf (*take, placement.offset);
            }
            else
            {
                loggedTake = -1;
                loggedPass = -1;
            }
        }

        if (inGap)
        {
            out.gap = 1;
            out.status = SyncStatus::held;
        }
    }

    out.pass = (int16_t) passOf (*take, placement.offset);
    if (! inGap && hs.playing && (take->takeId != loggedTake || out.pass != loggedPass))
    {
        note (changePath, loggedTake, loggedPass, take->takeId, out.pass, hostNow);
        loggedTake = take->takeId;
        loggedPass = out.pass;
    }

    out.takeSeconds = pos;
    out.warpSuspected = lost && hs.playing ? 1 : 0;

    if (out.status != SyncStatus::held)
    {
        if (! hs.playing)
            out.status = SyncStatus::stopped;
        else if (lost && wallNow - lostSince > 1.0)
            out.status = SyncStatus::lost;
        else if (placement.kind == SyncKind::hostPosition)
            out.status = SyncStatus::prior;
        else
            out.status = SyncStatus::locked;
    }

    wasPlaying = hs.playing;
    out.serial = ++serial;
    return out;
}

} // namespace griddy
