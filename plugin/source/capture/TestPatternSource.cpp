#include "TestPatternSource.h"
#include "../engine/HostClock.h"

namespace griddy
{

TestPatternSource::TestPatternSource (FrameStore& s, int patternNumber, int w, int h, double framesPerSecond)
    : CaptureSource (s),
      juce::Thread ("Griddy test pattern " + juce::String (patternNumber)),
      number (patternNumber), width (w), height (h), fps (framesPerSecond),
      // Spread the hues so four patterns side by side are easy to tell apart.
      colour (juce::Colour::fromHSV (std::fmod (0.13f + 0.27f * (float) patternNumber, 1.0f), 0.65f, 0.75f, 1.0f))
{
}

TestPatternSource::~TestPatternSource()
{
    stop();
}

bool TestPatternSource::start (juce::String&)
{
    startThread();
    return true;
}

void TestPatternSource::stop()
{
    stopThread (1000);
    store.remove (getId());
}

juce::Image TestPatternSource::renderFrame (double t) const
{
    juce::Image img (juce::Image::ARGB, width, height, true, juce::SoftwareImageType());
    juce::Graphics g (img);

    g.fillAll (colour);

    // A white bar sweeping left to right once per second shows motion and dropped frames.
    const auto phase = (float) std::fmod (t, 1.0);
    g.setColour (juce::Colours::white.withAlpha (0.8f));
    g.fillRect (phase * (float) width - 10.0f, 0.0f, 20.0f, (float) height);

    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.setFont (juce::FontOptions ((float) height * 0.3f, juce::Font::bold));
    g.drawText (juce::String (number), img.getBounds(), juce::Justification::centred);

    g.setFont (juce::FontOptions ((float) height * 0.08f));
    g.drawText (juce::String (t, 2) + " s", img.getBounds().reduced (12), juce::Justification::bottomRight);

    return img;
}

void TestPatternSource::run()
{
    const auto start = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    auto next = start;

    while (! threadShouldExit())
    {
        const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        store.push (getId(), renderFrame (now - start), steadyNowSeconds()); // the clock the recorder uses

        next += 1.0 / fps;
        const auto waitMs = (int) ((next - juce::Time::getMillisecondCounterHiRes() / 1000.0) * 1000.0);

        if (waitMs > 0)
            wait (waitMs);
        else
            next = juce::Time::getMillisecondCounterHiRes() / 1000.0; // fell behind: don't try to catch up
    }
}

} // namespace griddy
