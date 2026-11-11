#include "SyncController.h"

#include <algorithm>
#include <cmath>

namespace griddy
{

static constexpr double jumpTolerance = 0.030;   // seconds: smaller differences are just refined
static constexpr float silenceLevel = 0.002f;    // peak below this counts as silence
static constexpr double silenceBeforeHold = 0.25; // seconds of silence, where the take is not silent, before holding

SyncController::SyncController (const HostClock& h, ClipSource* e, ClipSource* f, HostPositionClipSource* p)
    : host (h), embedded (e), fingerprint (f), prior (p)
{
}

void SyncController::setTakes (std::vector<TakeProfile> t)
{
    const std::scoped_lock sl (takeLock);
    takes = std::move (t);
    placement = {};
    pending = {};
    pendingCount = 0;
    lost = false;
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
    {
        const std::scoped_lock sl (takeLock);
        localTakes = takes;
    }

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
            if (accept (f, lookup (f.takeId, true), hostNow) && lockLatency < 0.0 && firstAudioWall >= 0.0)
                lockLatency = wallNow - firstAudioWall;
        }
    }

    if (fingerprint != nullptr)
    {
        const auto f = fingerprint->latest();
        if (f.serial != lastFingerprintSerial)
        {
            lastFingerprintSerial = f.serial;

            // Never let the weaker source overwrite a recent embedded placement.
            if (! (placement.valid && placement.kind == SyncKind::embedded))
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
        const auto f = prior->latest();
        if (f.locked)
        {
            Placement p;
            p.valid = true;
            p.takeId = active->takeId;
            p.offset = f.takeSeconds - f.hostSeconds;
            p.kind = SyncKind::hostPosition;
            placement = p;
        }
    }

    if (! placement.valid)
    {
        out.takeId = active->takeId;
        out.status = hs.playing ? SyncStatus::held : SyncStatus::stopped;
        out.kind = SyncKind::none;
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
