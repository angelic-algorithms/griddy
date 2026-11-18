#pragma once

#include <juce_graphics/juce_graphics.h>

#include <deque>

namespace griddy
{

/** Decode ahead cache for one take: decoded frames by index, bounded, with the first frames pinned.

    The first `pinned` frames of a take are decoded once and kept, so a loop wrap (or a
    duplicate clip starting) shows its first frame at once instead of waiting for a decode:
    that is the loop point pre roll. Everything else is a sliding window around the playhead.

    Owned and used by one thread (the slot's decode thread). The renderer never touches it; it is
    handed finished frames by SlotPlayer.
*/
class FrameQueue
{
public:
    FrameQueue (int windowFrames, int pinnedFrames) : window (windowFrames), pinned (pinnedFrames) {}

    bool has (int index) const { return find (index) != nullptr; }

    juce::Image get (int index) const
    {
        const auto* e = find (index);
        return e != nullptr ? e->image : juce::Image();
    }

    void put (int index, juce::Image image)
    {
        if (has (index))
            return;

        entries.push_back ({ index, std::move (image) });
    }

    /** Drops frames outside [centre - behind, centre + window), except the pinned ones. */
    void trim (int centre, int behind = 3)
    {
        for (auto it = entries.begin(); it != entries.end();)
        {
            const auto outside = it->index < centre - behind || it->index >= centre + window + behind;
            if (outside && it->index >= pinned)
                it = entries.erase (it);
            else
                ++it;
        }
    }

    void clear() { entries.clear(); }
    int size() const { return (int) entries.size(); }
    int windowSize() const noexcept { return window; }
    int pinnedCount() const noexcept { return pinned; }

private:
    struct Entry { int index; juce::Image image; };

    const Entry* find (int index) const
    {
        for (const auto& e : entries)
            if (e.index == index)
                return &e;
        return nullptr;
    }

    int window, pinned;
    std::deque<Entry> entries;
};

} // namespace griddy
