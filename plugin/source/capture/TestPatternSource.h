#pragma once

#include "CaptureSource.h"

namespace griddy
{

/** A fake camera: a coloured frame with a moving bar, a big label and a running clock.

    It lets you build and test Griddy anywhere (CI, Linux, a host that blocks
    cameras), and it makes timing bugs obvious because the clock and bar are
    drawn into every frame.
*/
class TestPatternSource final : public CaptureSource, private juce::Thread
{
public:
    TestPatternSource (FrameStore& store, int patternNumber, int width = 640, int height = 360, double fps = 30.0);
    ~TestPatternSource() override;

    juce::String getId() const override   { return idFor (number); }
    juce::String getName() const override { return "Test pattern " + juce::String (number); }

    bool start (juce::String& error) override;
    void stop() override;

    /** Renders one frame. Public so tests can check it without a thread. */
    juce::Image renderFrame (double timeSeconds) const;

    static juce::String idFor (int n) { return "test:" + juce::String (n); }

private:
    void run() override;

    int number, width, height;
    double fps;
    juce::Colour colour;
};

} // namespace griddy
