#include "SlotPlayer.h"

#include <algorithm>
#include <cmath>

namespace griddy
{

SlotPlayer::SlotPlayer (SyncController& c, Config cfg) : juce::Thread ("Griddy slot player"), controller (c), config (cfg) {}

SlotPlayer::~SlotPlayer() { stop(); }

void SlotPlayer::setTakes (std::vector<TakeHistory::Entry> entries)
{
    {
        const std::scoped_lock sl (takesLock);
        takes = std::move (entries);
    }
    ++takesVersion;
}

void SlotPlayer::start()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::high);
}

void SlotPlayer::stop() { stopThread (3000); }

std::shared_ptr<const ShownFrame> SlotPlayer::shown() const
{
    const std::scoped_lock sl (shownLock);
    return current;
}

SlotPlayerStats SlotPlayer::stats() const
{
    const std::scoped_lock sl (statsLock);
    return st;
}

int SlotPlayer::frameIndexFor (const TakeMeta& m, double t)
{
    const auto n = m.numFrames;
    if (n <= 0)
        return -1;

    if (m.frameTimes.size() == (size_t) n)
    {
        const auto& ft = m.frameTimes;
        const auto it = std::lower_bound (ft.begin(), ft.end(), t);
        const auto hi = (int) (it - ft.begin());
        if (hi <= 0)
            return 0;
        if (hi >= n)
            return n - 1;
        return std::abs (ft[(size_t) hi] - t) < std::abs (ft[(size_t) hi - 1] - t) ? hi : hi - 1;
    }

    return std::clamp ((int) std::lround (t * m.fps), 0, n - 1);
}

SlotPlayer::Playback* SlotPlayer::playbackFor (int32_t takeId, double now)
{
    if (auto it = open.find (takeId); it != open.end())
    {
        it->second->lastUsed = now;
        return it->second.get();
    }

    const TakeHistory::Entry* entry = nullptr;
    for (const auto& e : localTakes)
        if (e.meta.takeId == takeId)
            entry = &e;

    if (entry == nullptr)
        return nullptr;

    auto p = std::make_unique<Playback>();
    p->entry = *entry;
    p->decoder = createDefaultDecoder();

    if (! p->decoder->open (entry->meta, entry->folder, config.useProxy))
        return nullptr;

    p->cache = std::make_unique<FrameQueue> (config.aheadFrames, config.pinnedFrames);
    p->lastUsed = now;

    // Keep memory flat: close the least recently used take when too many are open.
    while ((int) open.size() >= config.maxOpenTakes)
    {
        auto victim = open.begin();
        for (auto it = open.begin(); it != open.end(); ++it)
            if (it->second->lastUsed < victim->second->lastUsed)
                victim = it;
        open.erase (victim);
    }

    auto* raw = p.get();
    open[takeId] = std::move (p);
    return raw;
}

juce::Image SlotPlayer::decodeInto (Playback& p, int index, bool synchronous)
{
    if (p.cache->has (index))
        return p.cache->get (index);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto img = p.decoder->decode (index);
    const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;

    if (img.isValid())
        p.cache->put (index, img);

    const std::scoped_lock sl (statsLock);
    ++st.decodes;
    st.decodeMsTotal += ms;
    st.decodeMsMax = std::max (st.decodeMsMax, ms);
    if (synchronous)
        ++st.synchronousDecodes;
    return img;
}

void SlotPlayer::run()
{
    while (! threadShouldExit())
    {
        const auto now = steadyNowSeconds();

        if (const auto v = takesVersion.load(); v != seenVersion)
        {
            seenVersion = v;
            {
                const std::scoped_lock sl (takesLock);
                localTakes = takes;
            }

            // Takes may have changed on disk (slip edits, a take discarded): reopen what is still listed.
            open.clear();
        }

        const auto tgt = controller.update (now);

        if (tgt.status == SyncStatus::noTake || tgt.takeId < 0)
        {
            if (lastStatus != SyncStatus::noTake)
            {
                const std::scoped_lock sl (shownLock);
                current.reset();
                lastStatus = SyncStatus::noTake;
            }

            wait (config.tickMs * 2);
            continue;
        }

        auto* pb = playbackFor (tgt.takeId, now);
        if (pb == nullptr)
        {
            wait (config.tickMs);
            continue;
        }

        const auto& meta = pb->entry.meta;
        const auto videoSeconds = tgt.takeSeconds - meta.videoOffsetSeconds();
        const auto idx = frameIndexFor (meta, videoSeconds);

        if (idx >= 0)
        {
            const auto changed = idx != lastIndex || tgt.takeId != lastTake || tgt.status != lastStatus;

            if (changed)
            {
                const auto hadIt = pb->cache->has (idx);
                auto img = decodeInto (*pb, idx, ! hadIt);

                if (img.isValid())
                {
                    auto f = std::make_shared<VideoFrame>();
                    f->image = img;
                    f->captureTimeSeconds = now;
                    f->serial = (int64_t) ++serial;

                    auto s = std::make_shared<ShownFrame>();
                    s->frame = f;
                    s->takeId = tgt.takeId;
                    s->frameIndex = idx;
                    s->frameTakeSeconds = meta.frameTimes.size() == (size_t) meta.numFrames ? meta.frameTimes[(size_t) idx] + meta.videoOffsetSeconds()
                                                                                              : (double) idx / meta.fps + meta.videoOffsetSeconds();
                    s->targetSeconds = tgt.takeSeconds;
                    s->publishWall = now;
                    s->status = tgt.status;
                    s->kind = tgt.kind;
                    s->playing = tgt.playing != 0;
                    s->predictedWrap = tgt.predictedWrap != 0;
                    s->warpSuspected = tgt.warpSuspected != 0;

                    {
                        const std::scoped_lock sl (shownLock);
                        current = s;
                    }

                    lastIndex = idx;
                    lastTake = tgt.takeId;
                    lastStatus = tgt.status;

                    const std::scoped_lock sl (statsLock);
                    ++st.publishes;
                }
            }
            else if (current != nullptr)
            {
                // Same frame: refresh the target and time so the renderer can measure drift against it.
                const std::scoped_lock sl (shownLock);
                auto s = std::make_shared<ShownFrame> (*current);
                s->targetSeconds = tgt.takeSeconds;
                s->publishWall = now;
                s->playing = tgt.playing != 0;
                current = s;
            }

            // Decode ahead, nearest first, a few per tick so one slow disk read cannot starve the others.
            int budget = config.maxDecodesPerTick;
            if (tgt.playing != 0)
            {
                for (int k = 1; k <= config.aheadFrames && budget > 0; ++k)
                {
                    const auto next = idx + k;
                    if (next < meta.numFrames && ! pb->cache->has (next))
                    {
                        decodeInto (*pb, next, false);
                        --budget;
                    }
                }
            }

            // Loop point pre roll: the first frames stay decoded so a wrap never waits.
            while (budget > 0 && pb->pinnedDone < std::min (config.pinnedFrames, meta.numFrames))
            {
                decodeInto (*pb, pb->pinnedDone++, false);
                --budget;
            }

            pb->cache->trim (idx);

            const std::scoped_lock sl (statsLock);
            st.openTakes = (int) open.size();
            st.cachedFrames = pb->cache->size();
        }

        wait (config.tickMs);
    }
}

} // namespace griddy
