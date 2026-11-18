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
    int armed = -1;       // Live has the track armed: 1 or 0, -1 from an older script that did not say
    bool recordFromKnown = false;
    double recordFrom = 0.0; // the beat this track's recording in Live began at, while Live records it
    SlotTimeline timeline;
};

/** What the Live script writes about the Griddy tracks in Live's arrangement. */
struct LiveTimeline
{
    double written = 0.0; // seconds since the epoch; the script rewrites the file every second while Live runs
    bool recording = false; // Live's record button (Arrangement or Session) is on
    std::vector<LiveTimelineTrack> tracks;
};

/** Where the Live script writes it: ~/Library/Application Support/Griddy/live-timeline.json on a Mac. */
juce::File liveTimelineFile();

/** False when the text is not a timeline this version understands. */
bool parseLiveTimeline (const juce::String& json, LiveTimeline& out);

bool operator== (const SlotTimeline& a, const SlotTimeline& b);

/** A press of Griddy's REC or Arm, for the Live script to do in Live. */
struct LiveRequest
{
    int64_t id = 0;       // increasing, also across restarts, so the script does each request once
    int slot = -1;        // 0 based
    juce::String track;   // the track's name, for a script that cannot read the slot
    juce::String action;  // "record", "stop", "arm" or "disarm"
    double at = 0.0;      // seconds since the epoch; the script drops a request older than a few seconds
};

/** Where Griddy writes requests for the Live script, next to the arrangement file. */
juce::File liveRequestsFile();

/** The file's text for the most recent requests. */
juce::String liveRequestsJson (const std::vector<LiveRequest>& requests);

} // namespace griddy
