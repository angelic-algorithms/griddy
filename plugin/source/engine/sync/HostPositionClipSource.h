#pragma once

#include "../ClipSource.h"
#include "../HostClock.h"

#include <mutex>
#include <vector>

namespace griddy
{

/** Where a take sat on the host timeline when it was recorded. */
struct TakeSpan
{
    int32_t takeId = -1;
    double hostStartSeconds = 0.0;
    double durationSeconds = 0.0;
};

/** The weakest and most basic source: assume the clip still sits where it was recorded.
    Exactly right for an unmoved clip, wrong the moment the clip is moved. It also gives the
    other sources an instant first guess, so the picture is never blind after a seek. */
class HostPositionClipSource final : public ClipSource
{
public:
    SyncKind kind() const noexcept override { return SyncKind::hostPosition; }

    void feed (const float*, int numSamples, int64_t hostSample, bool) noexcept override
    {
        lastHostSample.store (hostSample + numSamples, std::memory_order_relaxed);
    }

    void process() override {}

    /** The newest take is first. */
    void setTakes (std::vector<TakeSpan> t)
    {
        const std::scoped_lock sl (lock);
        takes = std::move (t);
    }

    void setSampleRate (double sr) noexcept { sampleRate = sr; }

    ClipFix latest() const noexcept override
    {
        ClipFix f;
        const std::scoped_lock sl (lock);

        if (takes.empty())
            return f;

        const auto& t = takes.front();
        f.hostSeconds = (double) lastHostSample.load (std::memory_order_relaxed) / sampleRate;
        f.takeId = t.takeId;
        f.takeSeconds = f.hostSeconds - t.hostStartSeconds;
        f.kind = SyncKind::hostPosition;
        f.confidence = 0.3f;
        f.locked = f.takeSeconds >= 0.0 && f.takeSeconds <= t.durationSeconds + 0.05;
        f.serial = 1;
        return f;
    }

private:
    mutable std::mutex lock;
    std::vector<TakeSpan> takes;
    std::atomic<int64_t> lastHostSample { 0 };
    double sampleRate = 48000.0;
};

} // namespace griddy
