#include "BeatClock.h"

#include <algorithm>
#include <cmath>

namespace griddy
{

void BeatClock::update (const TransportSnapshot& t, double nowSeconds) noexcept
{
    bpm = t.bpm > 0.0 ? t.bpm : 120.0;

    const auto den = t.timeSigDenominator > 0 ? t.timeSigDenominator : 4;
    const auto num = t.timeSigNumerator > 0 ? t.timeSigNumerator : 4;
    barLength = (double) num * 4.0 / (double) den;

    const auto quarterNotesPerSecond = bpm / 60.0;
    const auto elapsed = lastUpdateSeconds < 0.0 ? 0.0 : std::max (0.0, nowSeconds - lastUpdateSeconds);
    lastUpdateSeconds = nowSeconds;

    if (t.isPlaying && t.hostProvidedPosition)
    {
        // A fresh audio block arrived: remember when, so we can extrapolate from it.
        if (t.updateCounter != lastCounter)
        {
            lastCounter = t.updateCounter;
            lastReportedPpq = t.ppqPosition;
            counterArrivalSeconds = nowSeconds;
        }

        // Audio blocks are a few ms apart, video frames 33 ms. Extrapolating
        // from the last block keeps switches tight to the beat, but we cap it
        // so a stalled audio thread can't run the clock away.
        const auto sinceBlock = std::clamp (nowSeconds - counterArrivalSeconds, 0.0, 0.25);
        position = lastReportedPpq + sinceBlock * quarterNotesPerSecond;
        playing = true;
        return;
    }

    // Stopped (or no play head): free run at the project tempo.
    if (playing && t.hostProvidedPosition)
        position = t.ppqPosition; // just stopped: start free running from where the DAW stopped

    playing = false;
    position += elapsed * quarterNotesPerSecond;
}

} // namespace griddy
