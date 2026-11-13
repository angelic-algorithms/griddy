#include "CollageLayout.h"

#include <array>
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

namespace
{
struct PresetShape
{
    Preset preset;
    int cols, rows;
    std::vector<std::array<int, 4>> cells; // column, row (1 based), column span, row span; empty means a plain grid
};

const std::vector<PresetShape>& presetShapes()
{
    static const std::vector<PresetShape> shapes {
        { { "1x1", "1\u00d71", 1 }, 1, 1, {} },
        { { "1x2", "1\u00d72", 2 }, 2, 1, {} },
        { { "2x1", "2\u00d71", 2 }, 1, 2, {} },
        { { "1p2", "1+2", 3 }, 3, 2, { { 1, 1, 2, 2 }, { 3, 1, 1, 1 }, { 3, 2, 1, 1 } } },
        { { "3s", "Stack 3", 3 }, 1, 3, {} },
        { { "2x2", "2\u00d72", 4 }, 2, 2, {} },
        { { "1p3", "1+3", 4 }, 3, 3, { { 1, 1, 3, 2 }, { 1, 3, 1, 1 }, { 2, 3, 1, 1 }, { 3, 3, 1, 1 } } },
        { { "2p3", "2+3", 5 }, 6, 2, { { 1, 1, 3, 1 }, { 4, 1, 3, 1 }, { 1, 2, 2, 1 }, { 3, 2, 2, 1 }, { 5, 2, 2, 1 } } },
        { { "1p5", "1+5", 6 }, 3, 3, { { 1, 1, 2, 2 }, { 3, 1, 1, 1 }, { 3, 2, 1, 1 }, { 1, 3, 1, 1 }, { 2, 3, 1, 1 }, { 3, 3, 1, 1 } } },
        { { "3x2", "3\u00d72", 6 }, 3, 2, {} },
        { { "3p4", "3+4", 7 }, 12, 2, { { 1, 1, 4, 1 }, { 5, 1, 4, 1 }, { 9, 1, 4, 1 }, { 1, 2, 3, 1 }, { 4, 2, 3, 1 }, { 7, 2, 3, 1 }, { 10, 2, 3, 1 } } },
        { { "4x2", "4\u00d72", 8 }, 4, 2, {} },
        { { "3x3", "3\u00d73", 9 }, 3, 3, {} },
    };
    jassert ((int) shapes.size() == numPresets);
    return shapes;
}
} // namespace

const Preset& presetAt (int index)
{
    return presetShapes()[(size_t) juce::jlimit (0, numPresets - 1, index)].preset;
}

int presetIndex (const juce::String& id)
{
    for (int i = 0; i < numPresets; ++i)
        if (id == presetShapes()[(size_t) i].preset.id)
            return i;
    return -1;
}

int defaultPresetFor (int panels)
{
    static const char* ids[] = { "1x1", "1x2", "1p2", "2x2", "2p3", "3x2", "3p4", "4x2", "3x3" };
    return presetIndex (ids[juce::jlimit (1, 9, panels) - 1]);
}

int presetForTemplate (Template shape, int numSlots)
{
    numSlots = juce::jlimit (1, 9, numSlots);
    const char* id = nullptr;

    switch (shape)
    {
        case Template::columns:  id = numSlots == 2 ? "1x2" : nullptr; break;
        case Template::stack:    id = numSlots == 2 ? "2x1" : numSlots == 3 ? "3s" : nullptr; break;
        case Template::heroLeft: id = numSlots == 3 ? "1p2" : numSlots == 6 ? "1p5" : nullptr; break;
        case Template::heroTop:  id = numSlots == 4 ? "1p3" : nullptr; break;
        case Template::twoRows:  id = numSlots == 5 ? "2p3" : numSlots == 7 ? "3p4" : nullptr; break;
        case Template::grid:     break;
    }

    return id != nullptr ? presetIndex (id) : defaultPresetFor (numSlots);
}

std::vector<Cell> presetLayout (int index)
{
    const auto& shape = presetShapes()[(size_t) juce::jlimit (0, numPresets - 1, index)];
    const auto cw = 1.0f / (float) shape.cols;
    const auto rh = 1.0f / (float) shape.rows;
    std::vector<Cell> cells;

    if (shape.cells.empty())
    {
        for (int r = 0; r < shape.rows; ++r)
            for (int c = 0; c < shape.cols; ++c)
                cells.push_back ({ (int) cells.size(), { (float) c * cw, (float) r * rh, cw, rh } });
        return cells;
    }

    for (const auto& c : shape.cells)
        cells.push_back ({ (int) cells.size(), { (float) (c[0] - 1) * cw, (float) (c[1] - 1) * rh, (float) c[2] * cw, (float) c[3] * rh } });

    return cells;
}

juce::Rectangle<float> framedCellArea (juce::Rectangle<float> n, float width, float height, float gap)
{
    // Shrink the whole picture by half a gap, then every cell by half a gap: the edge and every seam end up one gap wide.
    const auto half = gap * 0.5f;
    const auto inner = juce::Rectangle<float> (0, 0, width, height).reduced (half);
    return juce::Rectangle<float> (inner.getX() + n.getX() * inner.getWidth(), inner.getY() + n.getY() * inner.getHeight(),
                                   n.getWidth() * inner.getWidth(), n.getHeight() * inner.getHeight())
        .reduced (half);
}

} // namespace griddy
