#include "CollageLayout.h"

#include <cmath>

namespace griddy
{

juce::Point<int> gridShapeFor (int numTiles)
{
    numTiles = juce::jmax (1, numTiles);
    const auto cols = (int) std::ceil (std::sqrt ((double) numTiles));
    const auto rows = (numTiles + cols - 1) / cols;
    return { cols, rows };
}

static std::vector<Cell> gridCells (const std::vector<int>& slots, juce::Rectangle<float> bounds)
{
    std::vector<Cell> cells;
    const auto n = (int) slots.size();

    if (n == 0)
        return cells;

    const auto shape = gridShapeFor (n);
    const auto cellW = bounds.getWidth() / (float) shape.x;
    const auto cellH = bounds.getHeight() / (float) shape.y;

    for (int i = 0; i < n; ++i)
    {
        const auto row = i / shape.x;
        const auto col = i % shape.x;

        // Centre a partly filled last row, e.g. 3 cameras in a 2x2 grid put the third in the middle.
        const auto inThisRow = juce::jmin (shape.x, n - row * shape.x);
        const auto rowOffset = (float) (shape.x - inThisRow) * cellW * 0.5f;

        cells.push_back ({ slots[(size_t) i],
                           { bounds.getX() + rowOffset + (float) col * cellW,
                             bounds.getY() + (float) row * cellH,
                             cellW, cellH } });
    }

    return cells;
}

std::vector<Cell> computeLayout (int numSlots)
{
    numSlots = juce::jlimit (1, 16, numSlots);

    std::vector<int> all;
    for (int i = 0; i < numSlots; ++i)
        all.push_back (i);

    return gridCells (all, { 0.0f, 0.0f, 1.0f, 1.0f });
}

} // namespace griddy
