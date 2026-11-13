#include "TakeHistory.h"

#include <algorithm>

namespace griddy
{

TakeHistory::TakeHistory (juce::File r) : rootFolder (std::move (r))
{
    rootFolder.createDirectory();
}

juce::File TakeHistory::slotFolder (int slot) const
{
    return rootFolder.getChildFile ("slot" + juce::String (slot + 1));
}

int32_t TakeHistory::nextTakeId()
{
    // Persist the counter so ids (and the 8 bit embedded tags derived from them) are never reused within 256 takes.
    const auto f = rootFolder.getChildFile ("counter.txt");
    const auto next = f.existsAsFile() ? f.loadFileAsString().getIntValue() + 1 : 1;
    f.replaceWithText (juce::String (next));
    return next;
}

std::vector<TakeHistory::Entry> TakeHistory::scan (int slot) const
{
    std::vector<Entry> out;

    for (const auto& dir : slotFolder (slot).findChildFiles (juce::File::findDirectories, false, "*.gtake"))
    {
        Entry e;
        if (readTakeMeta (dir, e.meta))
        {
            e.folder = dir;
            out.push_back (std::move (e));
        }
    }

    std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b) { return a.meta.takeId > b.meta.takeId; });
    return out;
}

bool TakeHistory::save (const Entry& e) const
{
    return writeTakeMeta (e.folder, e.meta);
}

TakeHistory::Entry TakeHistory::beginTake (int slot, TakeMeta initial)
{
    const std::scoped_lock sl (lock);

    Entry e;
    e.meta = std::move (initial);
    e.meta.slot = slot;
    e.meta.takeId = nextTakeId();
    e.meta.embeddedTag = e.meta.takeId & 0xff;
    e.meta.state = TakeMeta::State::recording;
    e.meta.createdAt = (double) juce::Time::currentTimeMillis() / 1000.0;
    e.folder = slotFolder (slot).getChildFile (juce::String (e.meta.takeId) + ".gtake");
    e.folder.getChildFile ("frames").createDirectory();
    e.folder.getChildFile ("proxy").createDirectory();
    save (e);
    return e;
}

bool TakeHistory::finishTake (const Entry& finished)
{
    const std::scoped_lock sl (lock);

    // A take already waiting in review is superseded by a newer one: it can no longer be reviewed.
    for (auto& other : scan (finished.meta.slot))
    {
        if (other.meta.takeId != finished.meta.takeId && other.meta.state == TakeMeta::State::review)
            discard (other);
    }

    Entry e = finished;
    e.meta.state = TakeMeta::State::review;
    return save (e);
}

bool TakeHistory::keep (int slot, int32_t takeId)
{
    const std::scoped_lock sl (lock);
    auto all = scan (slot);
    Entry* chosen = nullptr;

    for (auto& e : all)
        if (e.meta.takeId == takeId)
            chosen = &e;

    if (chosen == nullptr || (chosen->meta.state != TakeMeta::State::review && chosen->meta.state != TakeMeta::State::archived))
        return false;

    for (auto& e : all)
        if (e.meta.state == TakeMeta::State::kept)
        {
            e.meta.state = TakeMeta::State::archived;
            save (e);
        }

    chosen->meta.state = TakeMeta::State::kept;
    return save (*chosen);
}

bool TakeHistory::restore (int slot, int32_t takeId)
{
    // Restoring an archived take is exactly keeping it: the current active take is archived in turn.
    return keep (slot, takeId);
}

int TakeHistory::archiveAll (int slot)
{
    const std::scoped_lock sl (lock);
    int changed = 0;

    for (auto& e : scan (slot))
        if (e.meta.state == TakeMeta::State::kept || e.meta.state == TakeMeta::State::review)
        {
            e.meta.state = TakeMeta::State::archived;
            changed += save (e) ? 1 : 0;
        }

    return changed;
}

bool TakeHistory::retake (int slot, int32_t takeId)
{
    const std::scoped_lock sl (lock);

    for (auto& e : scan (slot))
    {
        if (e.meta.takeId == takeId && e.meta.state == TakeMeta::State::review)
        {
            // The discarded take was never kept, so its media goes. The old active take is untouched.
            return discard (e);
        }
    }

    return false;
}

bool TakeHistory::setSlip (int slot, int32_t takeId, double slipSeconds)
{
    const std::scoped_lock sl (lock);

    for (auto& e : scan (slot))
        if (e.meta.takeId == takeId)
        {
            e.meta.slipSeconds = slipSeconds;
            return save (e);
        }

    return false;
}

bool TakeHistory::discard (const Entry& e)
{
    // Out of the way at once: scan() only lists *.gtake folders. purgeDiscarded() deletes it later.
    const auto target = e.folder.getSiblingFile (juce::String (e.meta.takeId) + ".discarded").getNonexistentSibling (false);
    return e.folder.moveFileTo (target) || e.folder.deleteRecursively();
}

void TakeHistory::purgeDiscarded (const juce::File& rootFolder)
{
    for (const auto& slot : rootFolder.findChildFiles (juce::File::findDirectories, false, "slot*"))
        for (const auto& dir : slot.findChildFiles (juce::File::findDirectories, false, "*.discarded"))
            dir.deleteRecursively();
}

std::optional<TakeHistory::Entry> TakeHistory::active (int slot) const
{
    const std::scoped_lock sl (lock);
    for (auto& e : scan (slot))
        if (e.meta.state == TakeMeta::State::kept)
            return e;
    return std::nullopt;
}

std::optional<TakeHistory::Entry> TakeHistory::inReview (int slot) const
{
    const std::scoped_lock sl (lock);
    for (auto& e : scan (slot))
        if (e.meta.state == TakeMeta::State::review)
            return e;
    return std::nullopt;
}

std::vector<TakeHistory::Entry> TakeHistory::listed (int slot) const
{
    const std::scoped_lock sl (lock);
    std::vector<Entry> out;

    for (auto& e : scan (slot))
        if (e.meta.state == TakeMeta::State::kept || e.meta.state == TakeMeta::State::archived || e.meta.state == TakeMeta::State::review)
            if ((int) out.size() < maxListed)
                out.push_back (std::move (e));

    return out;
}

std::vector<TakeHistory::Entry> TakeHistory::playable (int slot) const
{
    auto all = listed (slot);
    auto rank = [] (const Entry& e)
    {
        switch (e.meta.state)
        {
            case TakeMeta::State::review: return 0;
            case TakeMeta::State::kept:   return 1;
            default:                      return 2;
        }
    };

    std::stable_sort (all.begin(), all.end(), [&] (const Entry& a, const Entry& b) { return rank (a) < rank (b); });
    return all;
}

} // namespace griddy
