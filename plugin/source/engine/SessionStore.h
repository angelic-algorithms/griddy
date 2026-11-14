#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace griddy
{

/** Which session folder a project's saved state opens.

    A plugin cannot see the project file, so a template, a new set made from it, a saved project and a copy
    of that project all hand Griddy the same kind of state. What tells them apart is the takes: the state
    remembers which takes its session folder held when it was saved, and the folder on disk says what it
    holds now.

      * Same takes on disk as when saved: this is the project that wrote them. It opens its own session.
      * Different takes on disk: someone else wrote into that folder after this state was saved (another set
        made from the same template, a copy of the project, or takes recorded and never saved). This state gets
        a new session holding only the takes it knew, linked from the old folder, so neither project can
        replace or delete the other's takes.
      * A state saved before take lists existed (stateVersion 2 or older) cannot be told apart from a template,
        so it starts a new, empty session.

    Nothing here ever deletes or moves a session folder. */
struct SessionChoice
{
    enum class Kind { reopened, fresh, forked };

    juce::String id;
    Kind kind = Kind::fresh;
    int linkedTakes = 0;
    juce::String reason;
};

namespace session
{
    /** Ids of the takes in a session folder (slotN/<id>.gtake), sorted. Discarded takes do not count. */
    std::vector<int32_t> takeIdsOnDisk (const juce::File& sessionFolder);

    juce::String encodeTakeIds (const std::vector<int32_t>& ids);
    std::vector<int32_t> decodeTakeIds (const juce::String& text);

    /** Decides which session a saved state opens, creating the new one when it forks. sessionsRoot is <takes folder>/Sessions.
        savedTakes is empty for a state saved before take lists existed. */
    SessionChoice choose (const juce::File& sessionsRoot, const juce::String& savedId,
                          const std::optional<std::vector<int32_t>>& savedTakes);

    /** Makes `to` a copy of the take folder `from`. Media files are hard linked where the disk allows, so this is instant
        and takes no space; take.json is copied, because edits replace it. */
    bool linkTake (const juce::File& from, const juce::File& to);
}

} // namespace griddy
