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

static int wrapIndex (long long step, int n)
{
    // C++ % keeps the sign of the left side, so a count in (negative position) needs this.
    const auto m = step % (long long) n;
    return (int) (m < 0 ? m + n : m);
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

std::vector<Cell> computeLayout (LayoutMode mode, int numSlots, long long stepIndex)
{
    numSlots = juce::jlimit (1, 16, numSlots);
    const juce::Rectangle<float> full { 0.0f, 0.0f, 1.0f, 1.0f };

    std::vector<int> all;
    for (int i = 0; i < numSlots; ++i)
        all.push_back (i);

    switch (mode)
    {
        case LayoutMode::grid:
            return gridCells (all, full);

        case LayoutMode::cut:
            return { { wrapIndex (stepIndex, numSlots), full } };

        case LayoutMode::spotlight:
        {
            const auto featured = wrapIndex (stepIndex, numSlots);

            if (numSlots == 1)
                return { { featured, full } };

            std::vector<Cell> cells;
            auto area = full;
            const auto main = area.removeFromLeft (0.75f);
            cells.push_back ({ featured, main });

            // Everyone else stacks down the right hand strip, in their usual order after the featured one.
            const auto others = numSlots - 1;
            const auto h = area.getHeight() / (float) others;

            for (int i = 1; i < numSlots; ++i)
                cells.push_back ({ (featured + i) % numSlots,
                                   { area.getX(), area.getY() + (float) (i - 1) * h, area.getWidth(), h } });

            return cells;
        }
    }

    return gridCells (all, full);
}

} // namespace griddy
