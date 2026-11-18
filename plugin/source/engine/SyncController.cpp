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
        case timeline:    return "Live's arrangement";
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

void SyncController::setTimeline (std::shared_ptr<const SlotTimeline> t)
{
    const std::scoped_lock sl (takeLock);
    timeline = std::move (t);
    timelineChanged = true;
}

// Where a take's first second sits on the host timeline (a take recorded while looping has passes before its last).
static double takeZero (const TakeProfile& t)
{
    return t.ranges.empty() ? t.hostStartSeconds : t.ranges.front().hostStart - t.ranges.front().takeAtHostStart;
}

std::vector<SyncController::FileBinding> SyncController::bindFiles (const SlotTimeline& tl, const std::vector<TakeProfile>& takes)
{
    // Griddy starts its take with Live's recording or a moment later (the script arms it on the next tick), so a
    // take begins between just before and a little after its file. Clips of a file that were not moved, on the main
    // lane or a take lane, show where the file started.
    constexpr double earliestLead = -0.1, latestLead = 1.5;
    constexpr double bornWithin = 30.0; // seconds between the file's creation and the take's

    std::vector<int> files;
    for (const auto& c : tl.clips)
        if (c.file >= 0 && std::find (files.begin(), files.end(), c.file) == files.end())
            files.push_back (c.file);

    struct Candidate { const TakeProfile* take; double lead; bool leadKnown; };
    std::vector<std::vector<Candidate>> found (files.size());
    std::vector<double> born (files.size(), 0.0);

    for (size_t f = 0; f < files.size(); ++f)
        for (const auto& t : takes)
        {
            bool have = false;
            double lead = 0.0;
            for (const auto& c : tl.clips)
            {
                if (c.file != files[f])
                    continue;
                born[f] = std::max (born[f], c.fileBorn);
                if (! c.originKnown)
                    continue;
                const auto l = takeZero (t) - c.origin;
                if (l >= earliestLead && l <= latestLead && (! have || std::abs (l) < std::abs (lead)))
                {
                    have = true;
                    lead = l;
                }
            }
            if (have)
                found[f].push_back ({ &t, lead, true });
        }

    // A take that is the only fit for one file is not another file's: Live writes one file per recording.
    auto taken = [&] (const TakeProfile* t, size_t except)
    {
        for (size_t g = 0; g < files.size(); ++g)
            if (g != except && found[g].size() == 1 && found[g].front().take == t)
                return true;
        return false;
    };
    for (bool changed = true; changed;)
    {
        changed = false;
        for (size_t f = 0; f < files.size(); ++f)
            if (found[f].size() > 1)
            {
                const auto before = found[f].size();
                found[f].erase (std::remove_if (found[f].begin(), found[f].end(), [&] (const Candidate& c) { return taken (c.take, f); }),
                                found[f].end());
                changed = changed || found[f].size() != before;
            }
    }

    std::vector<FileBinding> out;
    for (size_t f = 0; f < files.size(); ++f)
    {
        // Several takes started where this file did (each take begun from the same spot), or none did because every
        // clip of the file was moved: the one created closest to the file's creation, if it stands out.
        if (found[f].size() != 1 && born[f] > 0.0)
        {
            std::vector<Candidate> pool = found[f];
            if (pool.empty())
                for (const auto& t : takes)
                    if (! taken (&t, f))
                        pool.push_back ({ &t, 0.0, false });

            const Candidate* best = nullptr;
            double bestGap = bornWithin, second = bornWithin;
            for (const auto& c : pool)
            {
                if (c.take->createdAt <= 0.0)
                    continue;
                const auto gap = std::abs (c.take->createdAt - born[f]);
                if (gap < bestGap)
                {
                    second = bestGap;
                    bestGap = gap;
                    best = &c;
                }
                else
                {
                    second = std::min (second, gap);
                }
            }

            auto chosen = best != nullptr && second - bestGap > 2.0 ? std::vector<Candidate> { *best } : std::vector<Candidate> {};
            found[f] = std::move (chosen);
        }

        if (found[f].size() == 1)
            out.push_back ({ files[f], found[f].front().take->takeId, found[f].front().lead, found[f].front().leadKnown });
    }

    return out;
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
    bool fresh = false, rebind = false;
    {
        const std::scoped_lock sl (takeLock);
        localTakes = takes;
        fresh = std::exchange (takesChanged, false);
        rebind = std::exchange (timelineChanged, false);
        localTimeline = timeline;
    }

    if (fresh || rebind)
        bindings = localTimeline != nullptr ? bindFiles (*localTimeline, localTakes) : std::vector<FileBinding> {};

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

            if (f.locked)
                if (const auto* t = lookup (f.takeId, false); t != nullptr)
                {
                    bool atRecorded = false;
                    for (const auto& r : t->ranges)
                        if (f.hostSeconds >= r.hostStart && f.hostSeconds < r.hostEnd
                            && std::abs ((f.takeSeconds - f.hostSeconds) - (r.takeAtHostStart - r.hostStart)) < 0.05)
                            atRecorded = true;
                    if (! atRecorded)
                        movedMatchAt = wallNow;
                }

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

    // Live's arrangement, while the Live script reports it: the clip under the playhead decides. No clip, a deactivated
    // one or a silenced track is black at once; a clip whose file is a known take shows that take, placed by where the
    // clip's file starts, so a cut switches on the frame. A clip whose file is no known take leaves the take and its
    // position to the sources that listen.
    const auto tlOn = localTimeline != nullptr && localTimeline->arrangement;
    const TimelineClip* here = nullptr;
    if (tlOn)
        for (const auto& c : localTimeline->clips)
            if (! c.lane && hostNow >= c.hostStart && hostNow < c.hostEnd)
                here = &c;
    const auto tlGap = tlOn && (here == nullptr || ! here->active || ! localTimeline->audible);
    auto timelinePlaced = false;

    if (tlOn && here != nullptr && here->active)
    {
        const FileBinding* b = nullptr;
        for (const auto& x : bindings)
            if (x.file == here->file)
                b = &x;

        if (const auto* t = b != nullptr ? lookup (b->takeId, false) : nullptr; t != nullptr)
        {
            const auto home = t->ranges.empty() ? -t->hostStartSeconds
                                                : t->ranges.front().takeAtHostStart - t->ranges.front().hostStart;
            auto offset = home;
            auto exact = false;
            if (here->originKnown && b->leadKnown)
            {
                offset = -(here->origin + b->lead);
                exact = true;
            }

            // Where the file's start is not known exactly, a source that heard this take keeps its own position.
            const auto heard = placement.valid && placement.takeId == t->takeId && placement.kind != SyncKind::timeline
                            && placement.kind != SyncKind::hostPosition;
            if (exact || ! heard)
            {
                if (! (placement.valid && placement.takeId == t->takeId && placement.kind == SyncKind::timeline
                       && std::abs (placement.offset - offset) < 1.0e-6))
                {
                    placement = { true, t->takeId, offset, SyncKind::timeline };
                    pending = {};
                    pendingCount = 0;
                    changePath = TakeChange::timeline;
                }
            }
            timelinePlaced = true;
        }
        else if (placement.kind == SyncKind::timeline)
        {
            placement = {}; // a clip of a file that is no known take: the take placed for the last clip is not it
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

    // Digital silence on the track, timed for black gaps.
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

    // Sound that is no take playing where it was recorded: a deleted or deactivated stretch with the mic monitored
    // through the track, or room sound. Only judged where every take recorded here has sound of its own.
    if (fingerprint != nullptr)
    {
        const auto m = fingerprint->audioMatch();
        if (m.serial != lastMatchSerial)
        {
            lastMatchSerial = m.serial;
            if (m.heard && m.corr >= 0.5f)
            {
                matchedNow = true;
                mismatchSince = -1.0;
            }
            else if (m.heard && m.corr < 0.3f)
            {
                matchedNow = false;
                if (mismatchSince < 0.0)
                    mismatchSince = wallNow;
            }
        }
    }
    if (! hs.playing)
        mismatchSince = -1.0;

    bool soundless = false;
    for (const auto& t : localTakes)
        for (const auto& r : t.ranges)
            soundless = soundless || (! t.hasSound && hostNow >= r.hostStart && hostNow < r.hostEnd);

    const auto movedMatch = movedMatchAt >= 0.0 && wallNow - movedMatchAt < 0.6;
    const auto judgeMatch = gapsOn && ! soundless;
    const auto longMismatch = judgeMatch && ! movedMatch && mismatchSince >= 0.0 && wallNow - mismatchSince >= gapAfter;

    // A gap ends once sound has lasted gapLeave, which is also long enough for comp detection to say whose sound it is,
    // and, where takes are judged by their sound, once that sound is a take.
    const auto stayInGap = inGap && gapsOn
                        && (digitalSilentSince >= 0.0 || (signalSince >= 0.0 && wallNow - signalSince < gapLeave)
                            || (judgeMatch && ! matchedNow && ! movedMatch));

    // With Live's arrangement known, a gap is exactly where no clip plays.
    const auto gapNow = tlOn && gapsOn ? tlGap : (longSilence || longMismatch || stayInGap);

    if (! placement.valid)
    {
        out.takeId = active->takeId;
        out.status = hs.playing ? SyncStatus::held : SyncStatus::stopped;
        out.kind = SyncKind::none;

        if (fingerprint != nullptr)
            fingerprint->setShown (-1, 0.0);

        // Nothing placed yet: digital silence, or sound that is no take, means no clip plays.
        if (gapNow != inGap)
        {
            inGap = gapNow;
            if (gapNow)
                matchedNow = false; // leaving the gap needs fresh evidence that a take plays
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
        if (hs.playing && hs.level > silenceLevel && ! timelinePlaced)
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
    if (hs.playing && hs.level <= silenceLevel && envelopeAt (*take, pos) > 4.0 * silenceLevel && ! timelinePlaced)
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

    // Black gaps. A playing clip always has some sound (even a quiet stretch of a recording has its noise floor); no
    // clip at all is digital silence. So digital silence for gapAfter means no clip plays here: black, whatever take
    // was placed and however it was placed. The picture comes back once sound has lasted gapLeave.
    {
        if (gapNow != inGap)
        {
            inGap = gapNow;
            if (gapNow)
                matchedNow = false; // leaving the gap needs fresh evidence that a take plays
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
    if (fingerprint != nullptr)
        fingerprint->setShown (take->takeId, placement.offset);
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
        else if (placement.kind == SyncKind::timeline)
            out.status = SyncStatus::locked;
        else
            out.status = SyncStatus::locked;
    }

    wasPlaying = hs.playing;
    out.serial = ++serial;
    return out;
}

} // namespace griddy
