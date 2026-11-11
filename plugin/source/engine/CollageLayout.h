#pragma once

#include <juce_graphics/juce_graphics.h>
#include <vector>

namespace griddy
{

/** One rectangle of the collage and which slot (camera) fills it. */
struct Cell
{
    int slot = 0;
    juce::Rectangle<float> area; // normalised: 0..1 in both directions, so it works at any output size
};

/** Pure function from slot count to grid cells. A single slot fills the frame (1x1).

    Keeping this free of cameras and threads makes it trivial to unit test.
*/
std::vector<Cell> computeLayout (int numSlots);

/** Grid shape for n tiles: columns = ceil(sqrt(n)), rows = ceil(n / columns). */
juce::Point<int> gridShapeFor (int numTiles);

} // namespace griddy
