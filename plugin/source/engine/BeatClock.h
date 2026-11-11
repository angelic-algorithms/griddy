#pragma once

#include "TransportState.h"

namespace griddy
{

/** How often the collage changes, in musical terms. Order matches the "switchRate" parameter. */
enum class SwitchRate
{
    oneBeat = 0,
    twoBeats,
    oneBar,
    twoBars,
    fourBars
};

/** Turns DAW transport info into "where are we musically, right now" for the video thread.

    The audio thread only reports at the end of each block, and the video
    thread runs on its own 30 fps clock. So we extrapolate: take the last
    reported position and add however much time has passed since, at the
    current tempo. When the DAW is stopped we keep free running at the
    project tempo so the preview stays alive, and snap back to the DAW's
    position the moment it starts playing again.
*/
class BeatClock
{
public:
    /** Call once per video frame. nowSeconds is any monotonic clock (Time::getMillisecondCounterHiRes / 1000). */
    void update (const TransportSnapshot& t, double nowSeconds) noexcept;

    /** Current position in quarter notes. */
    double getPositionInQuarterNotes() const noexcept { return position; }

    /** Length of one bar in quarter notes, from the time signature (7/8 is 3.5). */
    double getBarLengthInQuarterNotes() const noexcept { return barLength; }

    /** Length of one switch period in quarter notes. */
    double getSwitchLengthInQuarterNotes (SwitchRate rate) const noexcept;

    /** How many switch periods have elapsed since the song start.
        Same song position always gives the same step, so a second take
        lines up with the first. */
    long long getStepIndex (SwitchRate rate) const noexcept;

    /** 0..1 progress through the current switch period, handy for UI meters. */
    double getStepPhase (SwitchRate rate) const noexcept;

    bool isPlaying() const noexcept { return playing; }

private:
    double position = 0.0;
    double barLength = 4.0;
    double bpm = 120.0;
    double lastReportedPpq = 0.0;
    double lastUpdateSeconds = -1.0;
    long long lastCounter = -1;
    double counterArrivalSeconds = 0.0;
    bool playing = false;
};

} // namespace griddy
