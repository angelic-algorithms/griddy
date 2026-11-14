#pragma once

#include "SeqLock.h"

#include <chrono>
#include <cmath>

namespace griddy
{

/** What the audio thread publishes about the host timeline once per block. */
struct HostState
{
    int64_t hostSample = 0;     // host time in samples of the first sample of the last block
    double sampleRate = 48000.0;
    double wallSeconds = 0.0;   // steady clock when the block arrived
    int32_t blockSize = 0;
    float level = 0.0f;         // peak of the block, for silence detection
    uint8_t playing = 0;
    uint8_t pad[3] = {};
};

inline double steadyNowSeconds()
{
    using namespace std::chrono;
    return duration<double> (steady_clock::now().time_since_epoch()).count();
}

/** Wait free host timeline. The audio thread calls feed(); any other thread asks where the
    host is "right now", extrapolated from the last block with the wall clock. */
class HostClock
{
public:
    void feed (const float* mono, int numSamples, int64_t hostSample, double sampleRate, bool playing) noexcept
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = std::fmax (peak, std::fabs (mono[i]));

        HostState s;
        s.hostSample = hostSample;
        s.sampleRate = sampleRate;
        s.wallSeconds = steadyNowSeconds();
        s.blockSize = numSamples;
        s.level = peak;
        s.playing = playing ? 1 : 0;
        state.write (s);
    }

    HostState read() const noexcept { return state.read(); }

    /** Host time in seconds at the wall time now. Frozen while stopped. */
    double hostSeconds (double wallNow) const noexcept
    {
        const auto s = state.read();
        const auto base = (double) s.hostSample / s.sampleRate;
        if (! s.playing)
            return base;

        // Never extrapolate far past the block: a stalled audio thread must not run the clock away.
        const auto since = std::fmin (std::fmax (wallNow - s.wallSeconds, 0.0), 0.25);
        return base + since;
    }

private:
    SeqLock<HostState> state;
};

} // namespace griddy
