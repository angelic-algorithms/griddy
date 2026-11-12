#pragma once

#include "TakeFormat.h"

#include <mutex>
#include <optional>
#include <vector>

namespace griddy
{

/** The takes on disk, per slot, and the review workflow around them.

    State machine of one take:
        recording -> review      (recording stopped)
        review    -> kept        (Keep: becomes the slot's active take, the old active take is archived)
        review    -> discarded   (Retake: the media is deleted, the old active take stays active)
        archived  -> kept        (Restore: swaps with the current active take)

    Nothing that was ever kept is deleted. The list shown to the user is the newest five
    takes that were not discarded; older ones stay on disk.

    Replacement rule: the slot plays what its audio is. The newest take that is in review or
    kept is the one a fresh slot shows, and every take still in history is a candidate the
    fingerprint matcher can recognise, so an old clip that is still on the timeline keeps its old video.
*/
class TakeHistory
{
public:
    struct Entry
    {
        TakeMeta meta;
        juce::File folder;
    };

    static constexpr int maxListed = 5;

    explicit TakeHistory (juce::File rootFolder);

    /** Creates the folder for a new take and returns its metadata (state recording). */
    Entry beginTake (int slot, TakeMeta initial);

    /** Called when recording stops; moves the take to review and saves its metadata. */
    bool finishTake (const Entry& finished);

    bool keep (int slot, int32_t takeId);
    bool retake (int slot, int32_t takeId);

    /** Deletes the media of discarded takes. finishTake() and retake() only rename a discarded take's folder,
        which is instant, because deleting tens of thousands of frame files can take many seconds.
        Call this off the path that makes a new take playable. */
    static void purgeDiscarded (const juce::File& rootFolder);
    bool restore (int slot, int32_t takeId);
    bool setSlip (int slot, int32_t takeId, double slipSeconds);

    /** The kept take, if any. */
    std::optional<Entry> active (int slot) const;

    /** The take in review, if any. */
    std::optional<Entry> inReview (int slot) const;

    /** Newest first, at most maxListed, never including discarded takes. */
    std::vector<Entry> listed (int slot) const;

    /** What the slot should be able to play and recognise: the review take, the active take, then the archived ones. */
    std::vector<Entry> playable (int slot) const;

    juce::File slotFolder (int slot) const;
    const juce::File& root() const noexcept { return rootFolder; }

private:
    std::vector<Entry> scan (int slot) const;
    bool save (const Entry& e) const;
    static bool discard (const Entry& e);
    int32_t nextTakeId();

    juce::File rootFolder;
    mutable std::mutex lock;
};

} // namespace griddy
