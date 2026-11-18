#include "LiveTimeline.h"

namespace griddy
{

juce::File liveTimelineFile()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support").getChildFile ("Griddy").getChildFile ("live-timeline.json");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Griddy").getChildFile ("live-timeline.json");
   #endif
}

juce::File liveRequestsFile()
{
    return liveTimelineFile().getSiblingFile ("live-requests.json");
}

juce::String liveRequestsJson (const std::vector<LiveRequest>& requests)
{
    juce::Array<juce::var> list;
    for (const auto& r : requests)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", (juce::int64) r.id);
        o->setProperty ("slot", r.slot + 1);
        o->setProperty ("track", r.track);
        o->setProperty ("action", r.action);
        o->setProperty ("at", r.at);
        list.add (juce::var (o));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("requests", list);
    return juce::JSON::toString (juce::var (root), true);
}

bool parseLiveTimeline (const juce::String& json, LiveTimeline& out)
{
    const auto v = juce::JSON::parse (json);
    if (! v.isObject() || (int) v.getProperty ("version", 0) != 1)
        return false;

    out = {};
    out.written = (double) v.getProperty ("written", 0.0);

    if (const auto* tracks = v.getProperty ("tracks", {}).getArray())
        for (const auto& t : *tracks)
        {
            LiveTimelineTrack track;
            track.name = t.getProperty ("name", {}).toString();
            const auto slot = t.getProperty ("slot", {}).toString().trim().getIntValue();
            track.slot = slot >= 1 ? slot - 1 : -1;
            track.timeline.audible = (bool) t.getProperty ("audible", true);
            track.timeline.arrangement = (bool) t.getProperty ("arrangement", true);

            if (const auto* clips = t.getProperty ("clips", {}).getArray())
                for (const auto& c : *clips)
                {
                    TimelineClip clip;
                    clip.hostStart = (double) c.getProperty ("start", 0.0);
                    clip.hostEnd = (double) c.getProperty ("end", 0.0);
                    clip.active = (bool) c.getProperty ("active", true);
                    clip.lane = (bool) c.getProperty ("lane", false);
                    clip.file = (int) c.getProperty ("file", -1);
                    const auto origin = c.getProperty ("origin", {});
                    clip.originKnown = origin.isDouble() || origin.isInt() || origin.isInt64();
                    clip.origin = clip.originKnown ? (double) origin : 0.0;
                    clip.fileBorn = (double) c.getProperty ("born", 0.0);
                    if (clip.hostEnd > clip.hostStart)
                        track.timeline.clips.push_back (clip);
                }

            out.tracks.push_back (std::move (track));
        }

    return true;
}

static bool same (double a, double b) { return std::abs (a - b) < 1.0e-9; }

bool operator== (const SlotTimeline& a, const SlotTimeline& b)
{
    if (a.audible != b.audible || a.arrangement != b.arrangement || a.clips.size() != b.clips.size())
        return false;

    for (size_t i = 0; i < a.clips.size(); ++i)
    {
        const auto& x = a.clips[i];
        const auto& y = b.clips[i];
        if (! same (x.hostStart, y.hostStart) || ! same (x.hostEnd, y.hostEnd) || x.active != y.active || x.lane != y.lane
            || x.file != y.file || x.originKnown != y.originKnown || ! same (x.origin, y.origin) || ! same (x.fileBorn, y.fileBorn))
            return false;
    }

    return true;
}

} // namespace griddy
