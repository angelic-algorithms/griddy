#pragma once

#include <juce_graphics/juce_graphics.h>
#include <vector>

namespace griddy
{

/** Order matches the "layout" parameter. */
enum class LayoutMode
{
    grid = 0,   // every camera tiled, like a video call
    cut,        // one camera full frame, cutting to the next on each switch
    spotlight   // one big camera plus the rest in a side strip, rotating on each switch
};

/** One rectangle of the collage and which slot (camera) fills it. */
struct Cell
{
    int slot = 0;
    juce::Rectangle<float> area; // normalised: 0..1 in both directions, so it works at any output size
};

/** Pure function from (mode, how many slots, which step we're on) to cells.

    Keeping this free of cameras and threads makes it trivial to unit test
    and to mirror in the web UI if we ever want the UI to draw guides.
*/
std::vector<Cell> computeLayout (LayoutMode mode, int numSlots, long long stepIndex);

/** Grid shape for n tiles: columns = ceil(sqrt(n)), rows = ceil(n / columns). */
juce::Point<int> gridShapeFor (int numTiles);

} // namespace griddy
