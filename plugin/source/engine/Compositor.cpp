#include "Compositor.h"

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

void Compositor::render (juce::Image& output, const std::vector<Cell>& cells, const FrameForSlot& frameForSlot) const
{
    jassert (output.isValid());

    juce::Graphics g (output);
    // Medium quality is bilinear: good enough for video and much cheaper than high.
    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
    g.fillAll (juce::Colour (0xff0b0b0f));

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

        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (area.toNearestInt());

        if (auto frame = frameForSlot (cell.slot); frame != nullptr && frame->image.isValid())
        {
            g.drawImageTransformed (frame->image, coverTransform (frame->image.getBounds(), area));
        }
        else
        {
            g.setColour (juce::Colour (0xff1c1c24));
            g.fillRect (area);
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.setFont (juce::FontOptions (juce::jmax (12.0f, area.getHeight() * 0.08f)));
            g.drawText ("Slot " + juce::String (cell.slot + 1) + ": no camera", area, juce::Justification::centred);
        }
    }
}

} // namespace griddy
