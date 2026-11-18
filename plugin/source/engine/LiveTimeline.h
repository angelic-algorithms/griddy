#pragma once

#include "SyncController.h"

#include <juce_core/juce_core.h>

namespace griddy
{

/** One Griddy track as the Live script (live/Griddy) reports it. */
struct LiveTimelineTrack
{
    int slot = -1;        // 0 based, from the Griddy's Slot parameter; -1 when it could not be read
    juce::String name;    // the track's name in Live
    SlotTimeline timeline;
};

/** What the Live script writes about the Griddy tracks in Live's arrangement. */
struct LiveTimeline
{
    double written = 0.0; // seconds since the epoch; the script rewrites the file every second while Live runs
    std::vector<LiveTimelineTrack> tracks;
};

/** Where the Live script writes it: ~/Library/Application Support/Griddy/live-timeline.json on a Mac. */
juce::File liveTimelineFile();

/** False when the text is not a timeline this version understands. */
bool parseLiveTimeline (const juce::String& json, LiveTimeline& out);

bool operator== (const SlotTimeline& a, const SlotTimeline& b);

} // namespace griddy
