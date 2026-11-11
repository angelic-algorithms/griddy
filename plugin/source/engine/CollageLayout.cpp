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

static std::vector<Cell> rowOf (std::vector<Cell> cells, int first, int count, int slotOffset, juce::Rectangle<float> r)
{
    for (int i = 0; i < count; ++i)
        cells.push_back ({ slotOffset + i, { r.getX() + r.getWidth() * (float) i / (float) count, r.getY(), r.getWidth() / (float) count, r.getHeight() } });
    (void) first;
    return cells;
}

static std::vector<Cell> columnOf (std::vector<Cell> cells, int count, int slotOffset, juce::Rectangle<float> r)
{
    for (int i = 0; i < count; ++i)
        cells.push_back ({ slotOffset + i, { r.getX(), r.getY() + r.getHeight() * (float) i / (float) count, r.getWidth(), r.getHeight() / (float) count } });
    return cells;
}

std::vector<Cell> computeLayout (int numSlots, Template shape)
{
    numSlots = juce::jlimit (1, 16, numSlots);

    if (numSlots == 1 || shape == Template::grid)
    {
        std::vector<int> all;
        for (int i = 0; i < numSlots; ++i)
            all.push_back (i);

        return gridCells (all, { 0.0f, 0.0f, 1.0f, 1.0f });
    }

    constexpr float hero = 0.64f; // the large cell's share of the frame
    std::vector<Cell> cells;

    switch (shape)
    {
        case Template::columns:
            return rowOf (cells, 0, numSlots, 0, { 0, 0, 1, 1 });

        case Template::stack:
            return columnOf (cells, numSlots, 0, { 0, 0, 1, 1 });

        case Template::heroLeft:
            cells.push_back ({ 0, { 0, 0, hero, 1 } });
            return columnOf (cells, numSlots - 1, 1, { hero, 0, 1.0f - hero, 1 });

        case Template::heroTop:
            cells.push_back ({ 0, { 0, 0, 1, hero } });
            return rowOf (cells, 0, numSlots - 1, 1, { 0, hero, 1, 1.0f - hero });

        case Template::twoRows:
        {
            const auto top = (numSlots + 1) / 2;
            cells = rowOf (cells, 0, top, 0, { 0, 0, 1, 0.5f });
            return rowOf (cells, 0, numSlots - top, top, { 0, 0.5f, 1, 0.5f });
        }

        case Template::grid:
            break;
    }

    return cells;
}

} // namespace griddy
