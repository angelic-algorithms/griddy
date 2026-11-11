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

double BeatClock::getSwitchLengthInQuarterNotes (SwitchRate rate) const noexcept
{
    switch (rate)
    {
        case SwitchRate::oneBeat:  return 1.0;
        case SwitchRate::twoBeats: return 2.0;
        case SwitchRate::oneBar:   return barLength;
        case SwitchRate::twoBars:  return barLength * 2.0;
        case SwitchRate::fourBars: return barLength * 4.0;
    }

    return barLength;
}

long long BeatClock::getStepIndex (SwitchRate rate) const noexcept
{
    const auto len = getSwitchLengthInQuarterNotes (rate);
    // A tiny epsilon so a position of exactly 3.9999999 from float error still counts as beat 4.
    return (long long) std::floor ((position + 1.0e-6) / len);
}

double BeatClock::getStepPhase (SwitchRate rate) const noexcept
{
    const auto len = getSwitchLengthInQuarterNotes (rate);
    const auto p = std::fmod (position, len) / len;
    return p < 0.0 ? p + 1.0 : p;
}

} // namespace griddy
