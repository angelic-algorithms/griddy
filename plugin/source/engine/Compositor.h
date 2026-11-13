#pragma once

#include "CollageLayout.h"
#include "FrameStore.h"

#include <functional>

namespace griddy
{

/** Draws the collage: one output image built from the latest frame of each slot's camera.

    This is the single source of truth for what Griddy shows. The preview in
    the UI and the recorded file both come from this image, so what you see
    is exactly what gets recorded.
*/
class Compositor
{
public:
    /** Gives the frame to draw for a slot, or nullptr if that slot has no camera or no frame yet. */
    using FrameForSlot = std::function<std::shared_ptr<const VideoFrame> (int slot)>;

    /** Renders into `output`, which must be an ARGB image of the final size. */
    void render (juce::Image& output, const std::vector<Cell>& cells, const FrameForSlot& frameForSlot) const;

    /** Pixels of dark gap between cells, at the output resolution. */
    void setGapPixels (float g) noexcept { gap = g; }

    /** Framed: the gap also runs around the outside edge, and holds for a single cell too (the grid's frame). */
    void setFramed (bool f) noexcept { framed = f; }

    /** Rounded cell corners, in output pixels. 0 is square. */
    void setCornerRadius (float r) noexcept { radius = r; }

    /** The colour behind and between the cells, as 0xAARRGGBB. */
    void setBackground (uint32_t argb) noexcept { background = argb; }

    /** Scale and crop so the frame fills the cell without stretching (like CSS object-fit: cover). */
    static juce::AffineTransform coverTransform (juce::Rectangle<int> source, juce::Rectangle<float> target);

private:
    float gap = 4.0f;
    float radius = 0.0f;
    bool framed = false;
    uint32_t background = 0xff0b0b0fu;
};

} // namespace griddy
