#include "Compositor.h"

#include <algorithm>
#include <cstring>

namespace griddy
{

juce::AffineTransform Compositor::coverTransform (juce::Rectangle<int> source, juce::Rectangle<float> target)
{
    if (source.isEmpty() || target.isEmpty())
        return {};

    // "Cover" uses the larger of the two scale factors, so the frame overflows
    // the cell on one axis and we crop the overflow equally on both sides.
    const auto scale = juce::jmax (target.getWidth() / (float) source.getWidth(),
                                   target.getHeight() / (float) source.getHeight());

    const auto scaledW = (float) source.getWidth() * scale;
    const auto scaledH = (float) source.getHeight() * scale;

    return juce::AffineTransform::translation ((float) -source.getX(), (float) -source.getY())
               .scaled (scale)
               .translated (target.getX() + (target.getWidth() - scaledW) * 0.5f,
                            target.getY() + (target.getHeight() - scaledH) * 0.5f);
}

namespace
{
// Bilinear "cover" blit with 16.16 fixed point, straight into the output pixels. It replaces
// Graphics::drawImageTransformed on the hot path: no clip stack, no per pixel transform maths.
void blitCover (juce::Image& out, const juce::Image& src, juce::Rectangle<int> cell)
{
    cell = cell.getIntersection (out.getBounds());
    if (cell.isEmpty() || ! src.isValid())
        return;

    const juce::Image::BitmapData dst (out, cell.getX(), cell.getY(), cell.getWidth(), cell.getHeight(), juce::Image::BitmapData::writeOnly);
    const juce::Image::BitmapData in (src, juce::Image::BitmapData::readOnly);
    const auto cw = cell.getWidth(), ch = cell.getHeight();
    const auto sw = in.width, sh = in.height;

    // Source rectangle (in source pixels) that covers the cell without stretching.
    const double scale = std::max ((double) cw / sw, (double) ch / sh);
    const double viewW = cw / scale, viewH = ch / scale;
    const double x0 = (sw - viewW) * 0.5, y0 = (sh - viewH) * 0.5;

    // 1:1 copy when the proxy already matches the tile: plain row copies.
    if (cw == sw && ch == sh)
    {
        for (int y = 0; y < ch; ++y)
            std::memcpy (dst.getLinePointer (y), in.getLinePointer (y), (size_t) cw * 4);
        return;
    }

    const auto step = (int64_t) (65536.0 / scale);
    const auto maxX = (int64_t) (sw - 1) << 16, maxY = (int64_t) (sh - 1) << 16;

    std::vector<int32_t> xs ((size_t) cw), xf ((size_t) cw);
    for (int x = 0; x < cw; ++x)
    {
        auto fx = (int64_t) ((x0 + (x + 0.5) / scale - 0.5) * 65536.0);
        fx = std::clamp<int64_t> (fx, 0, maxX);
        xs[(size_t) x] = (int32_t) (fx >> 16);
        xf[(size_t) x] = (int32_t) ((fx & 0xffff) >> 8); // 0..255
    }
    (void) step;

    for (int y = 0; y < ch; ++y)
    {
        auto fy = (int64_t) ((y0 + (y + 0.5) / scale - 0.5) * 65536.0);
        fy = std::clamp<int64_t> (fy, 0, maxY);
        const auto iy = (int) (fy >> 16), wy = (int) ((fy & 0xffff) >> 8);
        const auto iy1 = std::min (iy + 1, sh - 1);
        const auto* r0 = in.getLinePointer (iy);
        const auto* r1 = in.getLinePointer (iy1);
        auto* o = reinterpret_cast<uint32_t*> (dst.getLinePointer (y));

        for (int x = 0; x < cw; ++x)
        {
            const auto ix = xs[(size_t) x], wx = xf[(size_t) x];
            const auto ix1 = std::min (ix + 1, sw - 1);
            uint32_t a00, a01, a10, a11;
            std::memcpy (&a00, r0 + ix * 4, 4);
            std::memcpy (&a01, r0 + ix1 * 4, 4);
            std::memcpy (&a10, r1 + ix * 4, 4);
            std::memcpy (&a11, r1 + ix1 * 4, 4);

            // Two channels at a time: 0x00ff00ff masks hold R,B then A,G.
            auto lerp2 = [] (uint32_t a, uint32_t b, uint32_t w) -> uint32_t
            {
                const auto ag = ((a >> 8) & 0x00ff00ffu) * (256 - w) + ((b >> 8) & 0x00ff00ffu) * w;
                const auto rb = (a & 0x00ff00ffu) * (256 - w) + (b & 0x00ff00ffu) * w;
                return (ag & 0xff00ff00u) | ((rb >> 8) & 0x00ff00ffu);
            };

            const auto top = lerp2 (a00, a01, (uint32_t) wx);
            const auto bot = lerp2 (a10, a11, (uint32_t) wx);
            o[x] = lerp2 (top, bot, (uint32_t) wy);
        }
    }
}
} // namespace

void Compositor::render (juce::Image& output, const std::vector<Cell>& cells, const FrameForSlot& frameForSlot) const
{
    jassert (output.isValid());

    {
        const juce::Image::BitmapData bd (output, juce::Image::BitmapData::writeOnly);
        const uint32_t bg = 0xff0b0b0fu;
        for (int y = 0; y < bd.height; ++y)
        {
            auto* row = reinterpret_cast<uint32_t*> (bd.getLinePointer (y));
            std::fill (row, row + bd.width, bg);
        }
    }

    juce::Graphics g (output);
    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);

    const auto outW = (float) output.getWidth();
    const auto outH = (float) output.getHeight();
    const auto halfGap = gap * 0.5f;

    for (const auto& cell : cells)
    {
        // Normalised (0..1) cell to pixels, then shrink by half the gap on every side
        // so neighbouring cells end up exactly one gap apart.
        auto area = juce::Rectangle<float> (cell.area.getX() * outW, cell.area.getY() * outH,
                                            cell.area.getWidth() * outW, cell.area.getHeight() * outH)
                        .reduced (cells.size() > 1 ? halfGap : 0.0f);

        if (area.isEmpty())
            continue;

        if (auto frame = frameForSlot (cell.slot); frame != nullptr && frame->image.isValid())
        {
            blitCover (output, frame->image, area.toNearestInt());
        }
        else
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (area.toNearestInt());
            g.setColour (juce::Colour (0xff1c1c24));
            g.fillRect (area);
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.setFont (juce::FontOptions (juce::jmax (12.0f, area.getHeight() * 0.08f)));
            g.drawText ("Slot " + juce::String (cell.slot + 1) + ": no camera", area, juce::Justification::centred);
        }
    }
}

} // namespace griddy
