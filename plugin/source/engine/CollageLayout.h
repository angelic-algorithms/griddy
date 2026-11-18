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

/** The shapes a grid can take. Every one of them fills the whole frame, except grid, which centres a short last row.
    Cell 0 is the large one in the hero shapes, so swapping cells chooses who gets the spotlight. */
enum class Template
{
    grid,      // rows and columns as square as possible
    columns,   // one row, every slot side by side
    stack,     // one column, every slot on top of the next
    heroLeft,  // slot in cell 0 on the left (about two thirds wide), the rest stacked on the right
    heroTop,   // slot in cell 0 on top (about two thirds tall), the rest side by side below
    twoRows    // two rows with half the slots each
};

constexpr int numTemplates = 6;

/** Pure function from slot count to grid cells. A single slot fills the frame (1x1).

    Keeping this free of cameras and threads makes it trivial to unit test.
*/
std::vector<Cell> computeLayout (int numSlots, Template shape = Template::grid);

/** Grid shape for n tiles: columns = ceil(sqrt(n)), rows = ceil(n / columns). */
juce::Point<int> gridShapeFor (int numTiles);

/** The grid templates the UI offers, in the order it shows them (the Overdub Cam design). Each one has a fixed
    number of panels and is drawn on a small grid of columns and rows; a panel can span several of them. */
struct Preset
{
    const char* id;    // stable name, saved with the project
    const char* label; // what the template button says
    int panels;
};

constexpr int numPresets = 13;

/** The preset at an index, clamped into range. */
const Preset& presetAt (int index);

/** Index of a preset by id, or -1. */
int presetIndex (const juce::String& id);

/** The preset a slot count falls back to when the chosen one has a different number of panels. */
int defaultPresetFor (int panels);

/** The closest preset to a layout saved before presets existed. */
int presetForTemplate (Template shape, int numSlots);

/** Normalised cells of a preset, cell i holding slot i. */
std::vector<Cell> presetLayout (int index);

/** Where a normalised cell lands in a framed picture of the given size: the frame keeps `gap` pixels around the
    edge and between cells (like CSS grid with padding and gap both equal to the frame thickness). */
juce::Rectangle<float> framedCellArea (juce::Rectangle<float> normalised, float width, float height, float gap);

} // namespace griddy
