#pragma once

#include "Compositor.h"

#include <memory>

namespace griddy
{

/** The same collage as Compositor, drawn with OpenGL. Compositor stays the reference: the harness
    renders both and compares pixels.

    One texture per slot, uploaded only when that slot's frame changes. Each tile is one textured quad
    with the cover crop done in texture coordinates, so scaling is the GPU's bilinear filter. The result
    is read back into an ARGB image because the UI preview and the exporter both want pixels.

    render() blocks the calling thread while the GL thread draws. available() is false when no GL context could be made
    (a headless CI box with no GL), and the engine then uses the CPU compositor.
*/
class GpuCompositor
{
public:
    GpuCompositor();
    ~GpuCompositor();

    /** Makes the GL context. Call from the message thread (it needs a native window). */
    bool initialise (int width, int height);
    bool available() const noexcept;
    juce::String description() const;

    /** Same contract as Compositor::render. Slots with no frame get a plain dark tile (no text). */
    void render (juce::Image& output, const std::vector<Cell>& cells, const Compositor::FrameForSlot& frameForSlot);

    /** Milliseconds spent drawing and reading back in the last render, for benchmarks. */
    double lastDrawMs() const noexcept;
    double lastReadbackMs() const noexcept;

    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace griddy
